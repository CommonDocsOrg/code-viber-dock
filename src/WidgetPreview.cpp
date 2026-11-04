#include "WidgetPreview.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "JsonUtil.h"
#include "WidgetInstance.h"
#include "WidgetRuntime.h"

namespace {

// The preview instance's dock: nothing to redraw, no popups, no editor, no notifications.
class StubClient final : public WidgetInstance::Client {
public:
    void WidgetChanged(WidgetInstance&) override {}
    void WidgetPopupRequest(WidgetInstance&) override {}
    bool WidgetPopupShown(const WidgetInstance&) const override { return false; }
    bool WidgetPassiveShown(const WidgetInstance&) const override { return false; }
    bool WidgetActivateNotification(uint32_t) override { return false; }
    bool WidgetOpenCentre(WidgetInstance&) override { return false; }
    EditorPlace WidgetEditorPlace(const WidgetInstance&) const override { return EditorPlace{}; }
    void WidgetEditorChanged(WidgetInstance&, bool) override {}
};

double NowMs() {
    LARGE_INTEGER f{}, c{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

}  // namespace

ScriptDockInfo WidgetPreview::DockInfoFor(const Config& config, float scale) {
    const ThemeConfig& t = config.theme;
    ScriptDockInfo info;
    info.axis = "horizontal";
    info.edge = "bottom";
    info.dockId = "preview";
    info.scale = scale > 0.0f ? scale : 1.0f;
    info.iconSize = 48.0f;
    info.reduceMotion = true;  // one frame: no sweeps or flips that would start half-way
    info.visible = true;
    info.metric = config.weather.units == Units::Metric;
    info.text = t.clockText;
    info.dateText = t.clockDateText;
    info.accent = t.accent;
    info.background = t.background;
    info.labelBackground = t.labelBackground;
    info.labelText = t.labelText;
    info.separator = t.separator;
    info.border = t.barBorder;
    info.preview = true;
    return info;
}

uint64_t WidgetPreview::ThemeStamp(const Config& config) {
    const ThemeConfig& t = config.theme;
    const uint32_t parts[] = {t.clockText, t.clockDateText, t.accent, t.background, t.labelBackground, t.labelText,
                              t.separator, t.barBorder, config.weather.units == Units::Metric ? 1u : 2u};
    uint64_t h = 1469598103934665603ull;
    for (uint32_t p : parts) {
        for (int b = 0; b < 4; ++b) h = (h ^ ((p >> (8 * b)) & 0xFF)) * 1099511628211ull;
    }
    return h;
}

bool WidgetPreview::EnsureFactories() {
    if (d2d_ && dwrite_ && wic_) return true;
    D2D1_FACTORY_OPTIONS options{};
    if (!d2d_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options, d2d_.put_void()))) {
        return false;
    }
    if (!dwrite_ && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                               reinterpret_cast<IUnknown**>(dwrite_.put())))) {
        return false;
    }
    if (!wic_ && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.put())))) {
        return false;
    }
    return true;
}

void WidgetPreview::Release() {
    d2d_ = nullptr;
    dwrite_ = nullptr;
    wic_ = nullptr;
}

void WidgetPreview::Report(const std::wstring& id, const Result& r) {
    if (r.bitmap) {
        // A widget that works again may report a new failure later.
        reported_.erase(id);
    } else if (reported_.insert(id).second) {
        std::wstring line = r.error;
        if (line.size() > 600) line = line.substr(0, 600) + L"…";
        OutputDebugStringW((L"WinDock preview " + id + L": " + line + L"\n").c_str());
    }
    // Test hook: every render to a file (tests read the failure text and the cost from it).
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"WINDOCK_PREVIEW_LOG", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t ms[32];
    swprintf_s(ms, L"%.2f", r.ms);
    std::wstring text = r.bitmap ? L"ok\t" + id + L"\t" + std::to_wstring(r.px.cx) + L"x" + std::to_wstring(r.px.cy) + L"\t" + ms
                                 : L"fail\t" + id + L"\t" + r.error;
    for (wchar_t& c : text) {
        if (c == L'\r' || c == L'\n') c = L' ';
    }
    const std::string bytes = JsonUtil::WideToUtf8(text) + "\r\n";
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(f);
}

WidgetPreview::Result WidgetPreview::Render(const std::wstring& id, const ScriptDockInfo& dock, const WeatherConfig& place,
                                            UINT fitW, UINT fitH, float maxZoom) {
    Result r;
    const double t0 = NowMs();
    ++renders_;
    auto fail = [&](std::wstring why) {
        if (r.bitmap) DeleteObject(r.bitmap);
        r.bitmap = nullptr;
        r.px = SIZE{};
        r.error = std::move(why);
        ++failures_;
    };
    if (!EnsureFactories()) {
        fail(L"Direct2D / WIC could not be started");
    } else {
        StubClient client;
        ScriptDockInfo info = dock;
        info.preview = true;
        ++alive_;
        {
            WidgetInstance w(runtime_, client, d2d_.get(), dwrite_.get(), id, L"preview#0", L"", info, place);
            UINT pw = 0, ph = 0;
            if (w.GetState() == WidgetInstance::State::Ok) {
                LayoutContext ctx;
                ctx.axis = DockAxis::Horizontal;
                ctx.scale = info.scale;
                ctx.iconPx = std::round(48.0f * info.scale);
                ctx.dwrite = dwrite_.get();
                w.Measure(ctx);
                w.PixelSize(pw, ph);
                fitW = std::max(1u, fitW);
                fitH = std::max(1u, fitH);
                const float k = pw > 0 && ph > 0 ? std::min({static_cast<float>(fitW) / pw, static_cast<float>(fitH) / ph, std::max(1.0f, maxZoom)})
                                                 : 1.0f;
                if (w.GetState() == WidgetInstance::State::Ok && std::fabs(k - 1.0f) > 0.01f) {
                    // Not the box's size: the same layout at another scale (measured again; the
                    // canvas is vector, so a larger one stays sharp).
                    ctx.scale = info.scale * k;
                    ctx.iconPx = std::round(48.0f * ctx.scale);
                    w.Measure(ctx);
                    w.PixelSize(pw, ph);
                }
                pw = std::min(pw, fitW);
                ph = std::min(ph, fitH);
            }
            if (w.GetState() != WidgetInstance::State::Ok) {
                fail(w.ErrorText().empty() ? L"the widget could not be loaded" : w.ErrorText());
            } else if (pw == 0 || ph == 0) {
                fail(L"the widget measured an empty box");
            } else {
                winrt::com_ptr<IWICBitmap> bmp;
                winrt::com_ptr<ID2D1RenderTarget> rt;
                winrt::com_ptr<ID2D1DeviceContext> dc;
                HRESULT hr = wic_->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bmp.put());
                if (SUCCEEDED(hr)) {
                    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                        96.0f, 96.0f);
                    hr = d2d_->CreateWicBitmapRenderTarget(bmp.get(), props, rt.put());
                }
                if (SUCCEEDED(hr)) hr = rt->QueryInterface(IID_PPV_ARGS(dc.put()));
                if (SUCCEEDED(hr)) {
                    dc->BeginDraw();
                    dc->SetTransform(D2D1::Matrix3x2F::Identity());
                    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // no ClearType on a transparent target
                    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                    w.DrawInto(dc.get());
                    hr = dc->EndDraw();
                }
                if (FAILED(hr)) {
                    fail(L"the preview could not be drawn (Direct2D error)");
                } else if (w.GetState() != WidgetInstance::State::Ok) {
                    fail(w.ErrorText().empty() ? L"draw() failed" : w.ErrorText());
                } else {
                    // The pixels into a top-down 32-bit DIB section (premultiplied BGRA: GdiAlphaBlend).
                    BITMAPINFO bi{};
                    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                    bi.bmiHeader.biWidth = static_cast<LONG>(pw);
                    bi.bmiHeader.biHeight = -static_cast<LONG>(ph);
                    bi.bmiHeader.biPlanes = 1;
                    bi.bmiHeader.biBitCount = 32;
                    bi.bmiHeader.biCompression = BI_RGB;
                    void* bits = nullptr;
                    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                    const UINT stride = pw * 4;
                    if (dib && bits && SUCCEEDED(bmp->CopyPixels(nullptr, stride, stride * ph, static_cast<BYTE*>(bits)))) {
                        GdiFlush();
                        r.bitmap = dib;
                        r.px = SIZE{static_cast<LONG>(pw), static_cast<LONG>(ph)};
                    } else {
                        if (dib) DeleteObject(dib);
                        fail(L"the preview bitmap could not be made");
                    }
                }
            }
        }  // the instance (and its script runtime) goes here
        --alive_;
    }
    r.ms = NowMs() - t0;
    Report(id, r);
    return r;
}
