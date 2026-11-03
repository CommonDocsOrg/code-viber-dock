#include "AudioService.h"

#include <mmreg.h>
#include <propsys.h>
#include <propvarutil.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {

// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h), defined here so no GUID library is
// needed.
constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// Our own volume changes carry this context (IAudioEndpointVolume::SetMasterVolumeLevelScalar).
constexpr GUID kEventContext = {0x3c1b5d2e, 0x7a41, 0x4f0e, {0x9a, 0x66, 0x2d, 0x51, 0x0b, 0x8e, 0x4c, 0x17}};

// IPolicyConfig: what Windows' sound settings use to change the default endpoint. Undocumented;
// the same vtable since Windows 7 (EarTrumpet, SoundSwitch and others depend on it). Only
// SetDefaultEndpoint is called.
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR id, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
constexpr CLSID kPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

std::wstring DeviceId(IMMDevice* d) {
    LPWSTR id = nullptr;
    if (!d || FAILED(d->GetId(&id)) || !id) return {};
    std::wstring s = id;
    CoTaskMemFree(id);
    return s;
}

std::wstring DeviceName(IMMDevice* d) {
    winrt::com_ptr<IPropertyStore> props;
    if (!d || FAILED(d->OpenPropertyStore(STGM_READ, props.put()))) return {};
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = v.pwszVal;
    PropVariantClear(&v);
    return name;
}

}  // namespace

// The callbacks Core Audio calls on its own threads: they only post (coalesced) to the owner.
struct AudioService::Callbacks : winrt::implements<Callbacks, IMMNotificationClient, IAudioEndpointVolumeCallback> {
    explicit Callbacks(HWND owner) : owner_(owner) {}
    void Detach() { owner_.store(nullptr); }

    // Bits: 1 levels, 2 the default devices or the device list.
    LONG Take() { return pending_.exchange(0); }

    void Post(LONG bits) {
        if (pending_.fetch_or(bits) != 0) return;  // a message is in the queue already
        HWND h = owner_.load();
        if (!h || !PostMessageW(h, kMessage, 0, 0)) pending_.store(0);
    }

    // IAudioEndpointVolumeCallback
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) noexcept override {
        Post(1);
        return S_OK;
    }
    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) noexcept override {
        Post(2);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) noexcept override {
        Post(2);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) noexcept override {
        Post(2);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole role, LPCWSTR) noexcept override {
        if (role == eConsole) Post(2);  // one per role: the console one stands for all
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) noexcept override { return S_OK; }

private:
    std::atomic<HWND> owner_;
    std::atomic<LONG> pending_{0};
};

AudioService::AudioService() = default;

AudioService::~AudioService() { Stop(); }

bool AudioService::Start(HWND owner) {
    if (Active()) return true;
    owner_ = owner;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(enumerator_.put())))) {
        enumerator_ = nullptr;
        return false;
    }
    callbacks_ = winrt::make_self<Callbacks>(owner_);
    enumerator_->RegisterEndpointNotificationCallback(callbacks_.get());
    BindEndpoints();
    ReadDevices();
    return true;
}

void AudioService::Stop() {
    if (!enumerator_) return;
    UnbindEndpoints();
    if (callbacks_) {
        enumerator_->UnregisterEndpointNotificationCallback(callbacks_.get());
        callbacks_->Detach();
    }
    callbacks_ = nullptr;
    enumerator_ = nullptr;
    state_ = State{};
}

void AudioService::UnbindEndpoints() {
    if (render_ && callbacks_) render_->UnregisterControlChangeNotify(callbacks_.get());
    if (capture_ && callbacks_) capture_->UnregisterControlChangeNotify(callbacks_.get());
    render_ = nullptr;
    capture_ = nullptr;
}

void AudioService::BindEndpoints() {
    UnbindEndpoints();
    state_.output = state_.input = false;
    state_.deviceId.clear();
    state_.deviceName.clear();
    state_.micName.clear();
    if (!enumerator_) return;
    winrt::com_ptr<IMMDevice> out, in;
    if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, out.put())) && out &&
        SUCCEEDED(out->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, render_.put_void()))) {
        render_->RegisterControlChangeNotify(callbacks_.get());
        state_.output = true;
        state_.deviceId = DeviceId(out.get());
        state_.deviceName = DeviceName(out.get());
    } else {
        render_ = nullptr;
    }
    if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eCapture, eConsole, in.put())) && in &&
        SUCCEEDED(in->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, capture_.put_void()))) {
        capture_->RegisterControlChangeNotify(callbacks_.get());
        state_.input = true;
        state_.micName = DeviceName(in.get());
    } else {
        capture_ = nullptr;
    }
    ReadLevels();
}

void AudioService::ReadLevels() {
    float v = 0.0f;
    BOOL m = FALSE;
    if (render_ && SUCCEEDED(render_->GetMasterVolumeLevelScalar(&v))) state_.volume = v;
    if (render_ && SUCCEEDED(render_->GetMute(&m))) state_.muted = m != FALSE;
    if (capture_ && SUCCEEDED(capture_->GetMasterVolumeLevelScalar(&v))) state_.micVolume = v;
    if (capture_ && SUCCEEDED(capture_->GetMute(&m))) state_.micMuted = m != FALSE;
}

void AudioService::ReadDevices() {
    state_.devices.clear();
    winrt::com_ptr<IMMDeviceCollection> all;
    if (!enumerator_ || FAILED(enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, all.put()))) return;
    UINT n = 0;
    all->GetCount(&n);
    for (UINT i = 0; i < n && i < 64; ++i) {
        winrt::com_ptr<IMMDevice> d;
        if (FAILED(all->Item(i, d.put()))) continue;
        Device dev;
        dev.id = DeviceId(d.get());
        dev.name = DeviceName(d.get());
        dev.isDefault = dev.id == state_.deviceId;
        state_.devices.push_back(std::move(dev));
    }
}

bool AudioService::HandleMessage(UINT msg, WPARAM, LPARAM) {
    if (msg != kMessage) return false;
    if (!Active() || !callbacks_) return true;  // stopped since it was posted
    const LONG bits = callbacks_->Take();
    if (bits & 2) {
        BindEndpoints();
        ReadDevices();
    } else if (bits & 1) {
        ReadLevels();
    }
    ++notifications_;
    if (changed_) changed_();
    return true;
}

bool AudioService::SetVolume(float volume, std::wstring& error) {
    if (!render_) {
        error = L"audio: there is no output device";
        return false;
    }
    if (!std::isfinite(volume)) volume = 0.0f;
    volume = std::clamp(volume, 0.0f, 1.0f);
    if (FAILED(render_->SetMasterVolumeLevelScalar(volume, &kEventContext))) {
        error = L"audio: the volume could not be set";
        return false;
    }
    state_.volume = volume;  // the notification follows (and re-reads it)
    return true;
}

bool AudioService::SetMuted(bool muted, std::wstring& error) {
    if (!render_ || FAILED(render_->SetMute(muted ? TRUE : FALSE, &kEventContext))) {
        error = L"audio: the output could not be muted / unmuted";
        return false;
    }
    state_.muted = muted;
    return true;
}

bool AudioService::SetMicMuted(bool muted, std::wstring& error) {
    if (!capture_ || FAILED(capture_->SetMute(muted ? TRUE : FALSE, &kEventContext))) {
        error = L"audio: there is no microphone to mute";
        return false;
    }
    state_.micMuted = muted;
    return true;
}

bool AudioService::SetDevice(const std::wstring& id, std::wstring& error) {
    if (std::none_of(state_.devices.begin(), state_.devices.end(), [&](const Device& d) { return d.id == id; })) {
        error = L"audio.setDevice: not an active output device";
        return false;
    }
    winrt::com_ptr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), policy.put_void())) ||
        FAILED(policy->SetDefaultEndpoint(id.c_str(), eConsole)) ||
        FAILED(policy->SetDefaultEndpoint(id.c_str(), eMultimedia))) {
        error = L"audio.setDevice: Windows refused to change the default output";
        return false;
    }
    return true;  // OnDefaultDeviceChanged rebinds
}
