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

// A scalar visual property driven by DWM: either a static value or a cubic ease-out from the
// value it had when it was last retargeted. We keep our own copy of the curve so a new target
// can start from the current (interpolated) value, which makes retargeting mid-flight smooth.
struct Tween {
    float from = 0.0f;
    float to = 0.0f;
    double start = 0.0;     // Renderer::Now() seconds
    double duration = 0.0;  // 0: static
    bool valid = false;     // false: the property has not been set (next Drive sets it statically)

    float Value(double now) const {
        if (!valid || duration <= 0.0) return to;
        const double u = (now - start) / duration;
        if (u >= 1.0) return to;
        if (u <= 0.0) return from;
        const double v = 1.0 - u;
        return from + (to - from) * static_cast<float>(1.0 - v * v * v);
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
    // Animation currently driving the visual's OffsetY (null: static value). While set,
    // placement changes leave OffsetY alone so the animation is not cancelled.
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
    // Drives the layer's OffsetY (added on top of its placement transform) with `animation`.
    HRESULT AnimateOffsetY(Layer& layer, IDCompositionAnimation* animation);
    // Detaches the OffsetY animation and sets the static value.
    void StopOffsetYAnimation(Layer& layer, float value = 0.0f);

    // Moves a Tween-tracked property towards `target`: a cubic ease-out over `duration` seconds
    // starting at the property's current value, or a static set when `duration` <= 0, when the
    // tween has never been set, or when it is already (within `epsilon`) at the target. A target
    // equal to the running animation's target leaves that animation alone. `set` is called with
    // either a float or an IDCompositionAnimation* (use a generic lambda).
    template <class Setter>
    void Drive(Tween& t, float target, double duration, double now, Setter&& set, float epsilon = 0.01f) {
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
        // v(s) = current + d * (1 - (1 - s/D)^3) = current + 3d/D s - 3d/D^2 s^2 + d/D^3 s^3
        const float d = target - current;
        const float D = static_cast<float>(duration);
        anim->AddCubic(0.0, current, 3.0f * d / D, -3.0f * d / (D * D), d / (D * D * D));
        anim->End(duration, target);
        set(anim.get());
        t = Tween{current, target, now, duration, true};
    }

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

    IDWriteFactory* DWrite() const { return dwrite_.get(); }
    ID2D1Factory1* D2DFactory() const { return d2dFactory_.get(); }
    bool UsingWarp() const { return usingWarp_; }

private:
    HRESULT CreateDeviceResources();
    void ReleaseDeviceResources();

    HWND hwnd_ = nullptr;
    bool usingWarp_ = false;
    bool softwareOnly_ = false;

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
