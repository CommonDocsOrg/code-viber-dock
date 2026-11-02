#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>

#include <winrt/base.h>

#include <cmath>
#include <functional>
#include <optional>

#include "Config.h"  // Easing

// A scalar visual property driven by DWM: either a static value or an eased curve from the value
// it had when it was last retargeted. We keep our own copy of the curve so a new target can start
// from the current (interpolated) value, which makes retargeting mid-flight smooth. Value() is the
// exact closed form of the cubic segments Renderer::Drive hands to DirectComposition, so CPU-side
// followers (the acrylic backdrop window, partial-reversal maths) see what DWM shows.
struct Tween {
    float from = 0.0f;
    float to = 0.0f;
    double start = 0.0;     // Renderer::Now() seconds
    double duration = 0.0;  // 0: static
    bool valid = false;     // false: the property has not been set (next Drive sets it statically)
    Easing easing = Easing::EaseOut;

    // Spring: an ease-out to to + kSpringOvershoot * (to - from) over the first kSpringRise of
    // the duration (zero velocity at the peak), then a smoothstep back to `to` over the rest.
    static constexpr double kSpringOvershoot = 0.12;
    static constexpr double kSpringRise = 0.6;

    float Value(double now) const {
        if (!valid || duration <= 0.0) return to;
        const double u = (now - start) / duration;
        if (u >= 1.0) return to;
        if (u <= 0.0) return from;
        const double s = now - start;
        const double d = static_cast<double>(to) - static_cast<double>(from);
        switch (easing) {
            case Easing::Linear:
                return static_cast<float>(from + d * u);
            case Easing::Spring: {
                const double ov = kSpringOvershoot * d;
                const double t1 = kSpringRise * duration;
                if (s < t1) {
                    const double v = 1.0 - s / t1;
                    return static_cast<float>(from + (d + ov) * (1.0 - v * v * v));
                }
                const double w = (s - t1) / (duration - t1);
                return static_cast<float>(to + ov - ov * w * w * (3.0 - 2.0 * w));
            }
            case Easing::EaseOut:
            default: {
                const double v = 1.0 - u;
                return from + (to - from) * static_cast<float>(1.0 - v * v * v);
            }
        }
    }
    bool Running(double now) const { return valid && duration > 0.0 && now < start + duration; }
};

// One DirectComposition visual with an optional D2D-drawn surface as content.
// Positional changes go through the visual (offsets / transforms); the surface is redrawn
// only when its content changes.
struct Layer {
    winrt::com_ptr<IDCompositionVisual2> visual;
    winrt::com_ptr<IDCompositionVisual2> parent;  // null: child of the root
    winrt::com_ptr<IDCompositionSurface> surface;
    UINT width = 0;
    UINT height = 0;
    // Animation currently driving the visual's OffsetY / OffsetX (a launch bounce; null: static).
    winrt::com_ptr<IDCompositionAnimation> offsetYAnimation;
    // Optional placement transform (Renderer::EnsurePlacement): uniform scale about a centre,
    // then a translation. Both have animatable properties.
    winrt::com_ptr<IDCompositionScaleTransform> scale;
    winrt::com_ptr<IDCompositionTranslateTransform> translate;
    // Optional clip (Renderer::SetClip), in the visual's local coordinates.
    winrt::com_ptr<IDCompositionRectangleClip> clip;

    void Reset() {
        offsetYAnimation = nullptr;
        scale = nullptr;
        translate = nullptr;
        clip = nullptr;
        surface = nullptr;
        visual = nullptr;
        parent = nullptr;
        width = height = 0;
    }
};

// Owns the D3D11 / D2D / DComp device chain and the visual tree root.
// All coordinates are physical pixels relative to the window's client area.
class Renderer {
public:
    using DrawFn = std::function<void(ID2D1DeviceContext*)>;

    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer() { Shutdown(); }

    // Creates device-independent factories and the device chain bound to hwnd. With
    // `softwareOnly` the D3D11 device is always WARP (also when the device is recreated).
    HRESULT Initialize(HWND hwnd, bool softwareOnly = false);
    void Shutdown();

    // Releases every device-dependent resource and builds a fresh device chain.
    // Callers must reset and recreate their Layers afterwards.
    HRESULT RecreateDevice();

    // True for HRESULTs that mean "the GPU device is gone, rebuild everything".
    static bool IsDeviceLost(HRESULT hr);
    // Cheap probe of the D3D device; returns a device-lost HRESULT or S_OK.
    HRESULT CheckDevice() const;

    // Creates a visual and adds it in front of the existing children of `parent` (the root
    // when null).
    HRESULT CreateLayer(Layer& layer, Layer* parent = nullptr);
    // (Re)creates the layer surface if the size changed, then draws into it.
    // The device context is in pixel units with the surface origin at (0,0) and already cleared.
    HRESULT DrawLayer(Layer& layer, UINT width, UINT height, const DrawFn& draw);
    // Shows src's surface in dst too (one surface, two visuals).
    HRESULT ShareContent(Layer& dst, const Layer& src);
    void SetOffset(Layer& layer, float x, float y);
    void SetStaticTransform(Layer& layer, const D2D1_MATRIX_3X2_F& m);
    // Gives the layer a scale-about-(cx, cy) + translate transform (created once). Drive
    // layer.scale / layer.translate properties afterwards.
    HRESULT EnsurePlacement(Layer& layer, float cx, float cy);
    // Clips the layer (and its children) to `rect`, in its local coordinates.
    HRESULT SetClip(Layer& layer, const D2D1_RECT_F& rect);
    void SetVisible(Layer& layer, bool visible);

    // Animations: a fresh IDCompositionAnimation to fill with AddCubic/AddRepeat/End segments,
    // then attach it to a layer property. DWM runs it after the next Commit(); we stay idle.
    winrt::com_ptr<IDCompositionAnimation> CreateAnimation();
    // Drives the layer's OffsetY (OffsetX with `alongX`; added on top of its placement transform)
    // with `animation`.
    HRESULT AnimateOffset(Layer& layer, IDCompositionAnimation* animation, bool alongX);
    // Detaches that animation and sets the static value.
    void StopOffsetAnimation(Layer& layer, float value, bool alongX);
    HRESULT AnimateOffsetY(Layer& layer, IDCompositionAnimation* animation) { return AnimateOffset(layer, animation, false); }
    void StopOffsetYAnimation(Layer& layer, float value = 0.0f) { StopOffsetAnimation(layer, value, false); }
    // Drives the layer's opacity (IDCompositionVisual3) with `animation` / sets it statically.
    HRESULT AnimateOpacity(Layer& layer, IDCompositionAnimation* animation);
    void StopOpacityAnimation(Layer& layer, float value = 1.0f);

    // Moves a Tween-tracked property towards `target`: an eased curve over `duration` seconds
    // starting at the property's current value, or a static set when `duration` <= 0, when the
    // tween has never been set, or when it is already (within `epsilon`) at the target. A target
    // equal to the running animation's target leaves that animation alone. `set` is called with
    // either a float or an IDCompositionAnimation* (use a generic lambda). `easing` defaults to
    // SetDefaultEasing's; every curve reaches `target` exactly at `duration`.
    template <class Setter>
    void Drive(Tween& t, float target, double duration, double now, Setter&& set, float epsilon = 0.01f,
               std::optional<Easing> easing = std::nullopt) {
        if (t.valid && std::fabs(target - t.to) < epsilon) return;
        const float current = t.Value(now);
        if (!t.valid || duration <= 0.0 || std::fabs(target - current) < epsilon) {
            set(target);
            t = Tween{target, target, now, 0.0, true};
            return;
        }
        winrt::com_ptr<IDCompositionAnimation> anim = CreateAnimation();
        if (!anim) {
            set(target);
            t = Tween{target, target, now, 0.0, true};
            return;
        }
        const Easing e = easing.value_or(defaultEasing_);
        const float d = target - current;
        const float D = static_cast<float>(duration);
        switch (e) {
            case Easing::Linear:
                anim->AddCubic(0.0, current, d / D, 0.0f, 0.0f);
                break;
            case Easing::Spring: {
                // Ease-out to the peak (target + ov) over t1, then smoothstep back over t2:
                // p(s) = peak - 3 ov s^2 / t2^2 + 2 ov s^3 / t2^3 (zero velocity at both ends).
                const float ov = static_cast<float>(Tween::kSpringOvershoot) * d;
                const double t1 = Tween::kSpringRise * duration;
                const float T1 = static_cast<float>(t1);
                const float T2 = static_cast<float>(duration - t1);
                const float d1 = d + ov;
                anim->AddCubic(0.0, current, 3.0f * d1 / T1, -3.0f * d1 / (T1 * T1), d1 / (T1 * T1 * T1));
                anim->AddCubic(t1, target + ov, 0.0f, -3.0f * ov / (T2 * T2), 2.0f * ov / (T2 * T2 * T2));
                break;
            }
            case Easing::EaseOut:
            default:
                // v(s) = current + d * (1 - (1 - s/D)^3) = current + 3d/D s - 3d/D^2 s^2 + d/D^3 s^3
                anim->AddCubic(0.0, current, 3.0f * d / D, -3.0f * d / (D * D), d / (D * D * D));
                break;
        }
        anim->End(duration, target);
        set(anim.get());
        t = Tween{current, target, now, duration, true, e};
    }

    // The easing Drive uses when none is given (settle animations). Tracking retargets pass
    // Easing::EaseOut explicitly.
    void SetDefaultEasing(Easing easing) { defaultEasing_ = easing; }
    Easing DefaultEasing() const { return defaultEasing_; }
    // For opacity: the default without overshoot (DWM clamps opacity to 0..1, so a spring would
    // only distort the timing). Spring -> EaseOut.
    Easing FadeEasing() const { return defaultEasing_ == Easing::Spring ? Easing::EaseOut : defaultEasing_; }

    // Monotonic seconds (QueryPerformanceCounter) for Tween bookkeeping.
    static double Now();

    // The root visual (the whole dock) and its IDCompositionVisual3 view (opacity).
    IDCompositionVisual2* Root() const { return root_.get(); }
    IDCompositionVisual3* Root3() const { return root3_.get(); }

    // Removes the layer's visual from the tree and releases it.
    void DestroyLayer(Layer& layer);

    // A composition target for another top-level window of this process (the preview popup) on
    // the same device: `root` gets a fresh visual set as the target's root; build layers under it
    // with CreateLayer(layer, &root). Commit() commits both trees. Release the target (and the
    // layers) before RecreateDevice / Shutdown. A window can have only one target.
    HRESULT CreateWindowTarget(HWND hwnd, winrt::com_ptr<IDCompositionTarget>& target, Layer& root);

    // A device context that is not bound to any surface, for creating device resources
    // (bitmaps) outside of a draw. Valid until the device is recreated or shut down.
    ID2D1DeviceContext* ResourceContext();

    HRESULT Commit();

    // The DirectComposition device (3D transform effects); null before Initialize.
    IDCompositionDesktopDevice* DComp() const { return dcomp_.get(); }
    IDWriteFactory* DWrite() const { return dwrite_.get(); }
    ID2D1Factory1* D2DFactory() const { return d2dFactory_.get(); }
    bool UsingWarp() const { return usingWarp_; }

private:
    HRESULT CreateDeviceResources();
    void ReleaseDeviceResources();

    HWND hwnd_ = nullptr;
    bool usingWarp_ = false;
    bool softwareOnly_ = false;
    Easing defaultEasing_ = Easing::EaseOut;

    // Device independent.
    winrt::com_ptr<ID2D1Factory1> d2dFactory_;
    winrt::com_ptr<IDWriteFactory> dwrite_;

    // Device dependent.
    winrt::com_ptr<ID3D11Device> d3dDevice_;
    winrt::com_ptr<IDXGIDevice> dxgiDevice_;
    winrt::com_ptr<ID2D1Device> d2dDevice_;
    winrt::com_ptr<IDCompositionDesktopDevice> dcomp_;
    winrt::com_ptr<IDCompositionTarget> target_;
    winrt::com_ptr<IDCompositionVisual2> root_;
    winrt::com_ptr<IDCompositionVisual3> root3_;
    winrt::com_ptr<ID2D1DeviceContext> resourceDc_;
};
