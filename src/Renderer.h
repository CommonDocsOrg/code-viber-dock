#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>

#include <winrt/base.h>

#include <functional>

// One DirectComposition visual with an optional D2D-drawn surface as content.
// Positional changes go through the visual (SetOffset); the surface is redrawn
// only when its content changes.
struct Layer {
    winrt::com_ptr<IDCompositionVisual2> visual;
    winrt::com_ptr<IDCompositionSurface> surface;
    UINT width = 0;
    UINT height = 0;

    void Reset() {
        surface = nullptr;
        visual = nullptr;
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

    // Creates a visual and appends it on top of the root's children.
    HRESULT CreateLayer(Layer& layer);
    // (Re)creates the layer surface if the size changed, then draws into it.
    // The device context is in pixel units with the surface origin at (0,0) and already cleared.
    HRESULT DrawLayer(Layer& layer, UINT width, UINT height, const DrawFn& draw);
    void SetOffset(Layer& layer, float x, float y);

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
};
