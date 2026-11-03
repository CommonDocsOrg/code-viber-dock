#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <winrt/base.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The `audio` provider of the widget API (permission "audio"): the default output and input
// devices' volume and mute through Core Audio (IAudioEndpointVolume on the default render and
// capture endpoints), the list of active outputs, and switching the default output. Event-driven:
// IMMNotificationClient (default device / device list changes) and IAudioEndpointVolumeCallback
// (level / mute changes) arrive on Core Audio's threads and are posted to the owner window as
// kMessage (coalesced: one message in the queue at a time); on the UI thread the state is re-read
// and the changed callback runs. Nothing polls. Started by WidgetRuntime when the first widget uses
// `audio`, stopped when the last such widget goes.
class AudioService {
public:
    static constexpr UINT kMessage = WM_APP + 33;  // wParam unused; the pending bits are in the callbacks

    struct Device {
        std::wstring id;    // the endpoint id (IMMDevice::GetId)
        std::wstring name;  // its friendly name ("Speakers (Realtek Audio)")
        bool isDefault = false;
    };
    struct State {
        bool output = false;  // a default output device exists
        float volume = 0.0f;  // 0 - 1 (the scalar the Windows volume slider shows)
        bool muted = false;
        std::wstring deviceId, deviceName;
        bool input = false;   // a default input device (microphone) exists
        float micVolume = 0.0f;
        bool micMuted = false;
        std::wstring micName;
        std::vector<Device> devices;  // active outputs, in enumeration order
    };

    AudioService();
    AudioService(const AudioService&) = delete;
    AudioService& operator=(const AudioService&) = delete;
    ~AudioService();

    // Creates the device enumerator and registers the callbacks (COM must be initialised on this
    // thread). False if Core Audio is not available.
    bool Start(HWND owner);
    void Stop();
    bool Active() const { return enumerator_ != nullptr; }
    const State& Get() const { return state_; }

    bool SetVolume(float volume, std::wstring& error);
    bool SetMuted(bool muted, std::wstring& error);
    bool SetMicMuted(bool muted, std::wstring& error);
    // Makes `id` the default output (console and multimedia roles). This uses IPolicyConfig, the
    // interface Windows' own sound settings use (undocumented, stable since Windows 7).
    bool SetDevice(const std::wstring& id, std::wstring& error);

    // kMessage: re-reads what changed, then calls the changed callback. True if handled.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void SetChanged(std::function<void()> changed) { changed_ = std::move(changed); }
    // Change notifications handled since the process started (automation).
    uint64_t Notifications() const { return notifications_; }

private:
    struct Callbacks;
    // (Re)binds the default endpoints' volume interfaces and their callbacks.
    void BindEndpoints();
    void UnbindEndpoints();
    void ReadLevels();
    void ReadDevices();

    HWND owner_ = nullptr;
    winrt::com_ptr<IMMDeviceEnumerator> enumerator_;
    winrt::com_ptr<IAudioEndpointVolume> render_;
    winrt::com_ptr<IAudioEndpointVolume> capture_;
    winrt::com_ptr<Callbacks> callbacks_;
    State state_;
    std::function<void()> changed_;
    uint64_t notifications_ = 0;
};
