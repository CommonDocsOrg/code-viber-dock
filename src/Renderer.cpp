#include "Renderer.h"

#include <iterator>

namespace {

#define RETURN_IF_FAILED(expr)            \
    do {                                  \
        const HRESULT hr_ = (expr);       \
        if (FAILED(hr_)) return hr_;      \
    } while (0)

HRESULT CreateD3DDevice(D3D_DRIVER_TYPE type, UINT flags, ID3D11Device** device) {
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1,
    };
    return D3D11CreateDevice(nullptr, type, nullptr, flags, levels, static_cast<UINT>(std::size(levels)),
                             D3D11_SDK_VERSION, device, nullptr, nullptr);
}

}  // namespace

HRESULT Renderer::Initialize(HWND hwnd, bool softwareOnly) {
    hwnd_ = hwnd;
    softwareOnly_ = softwareOnly;

    D2D1_FACTORY_OPTIONS options{};
#ifdef _DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                   d2dFactory_.put_void());
    if (FAILED(hr) && options.debugLevel != D2D1_DEBUG_LEVEL_NONE) {
        // Debug layer not installed (Graphics Tools optional feature): run without it.
        options.debugLevel = D2D1_DEBUG_LEVEL_NONE;
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                               d2dFactory_.put_void());
    }
    RETURN_IF_FAILED(hr);
    RETURN_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(dwrite_.put())));
    return CreateDeviceResources();
}

void Renderer::Shutdown() {
    ReleaseDeviceResources();
    dwrite_ = nullptr;
    d2dFactory_ = nullptr;
    hwnd_ = nullptr;
}

HRESULT Renderer::RecreateDevice() {
    ReleaseDeviceResources();
    return CreateDeviceResources();
}

HRESULT Renderer::CreateDeviceResources() {
    // BGRA support is mandatory for D2D interop.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;

    // "software" config: go straight to WARP. The hardware path maps the GPU vendor's user-mode
    // driver into the process (tens of MB of private bytes); the dock draws rarely, so WARP is fine.
    usingWarp_ = softwareOnly_;
    HRESULT hr = CreateD3DDevice(softwareOnly_ ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, flags,
                                 d3dDevice_.put());
    if (FAILED(hr) && !softwareOnly_) {
        usingWarp_ = true;
        hr = CreateD3DDevice(D3D_DRIVER_TYPE_WARP, flags, d3dDevice_.put());
    }
    RETURN_IF_FAILED(hr);

    RETURN_IF_FAILED(d3dDevice_->QueryInterface(IID_PPV_ARGS(dxgiDevice_.put())));
    RETURN_IF_FAILED(d2dFactory_->CreateDevice(dxgiDevice_.get(), d2dDevice_.put()));

    // Creating the DComp device from the D2D device lets IDCompositionSurface::BeginDraw
    // hand out an ID2D1DeviceContext directly.
    RETURN_IF_FAILED(DCompositionCreateDevice2(d2dDevice_.get(), IID_PPV_ARGS(dcomp_.put())));
    RETURN_IF_FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, target_.put()));
    RETURN_IF_FAILED(dcomp_->CreateVisual(root_.put()));
    RETURN_IF_FAILED(target_->SetRoot(root_.get()));
    return S_OK;
}

void Renderer::ReleaseDeviceResources() {
    if (target_) target_->SetRoot(nullptr);
    root_ = nullptr;
    target_ = nullptr;
    dcomp_ = nullptr;
    d2dDevice_ = nullptr;
    dxgiDevice_ = nullptr;
    d3dDevice_ = nullptr;
}

bool Renderer::IsDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR || hr == D2DERR_RECREATE_TARGET;
}

HRESULT Renderer::CheckDevice() const {
    if (!d3dDevice_ || !dcomp_) return DXGI_ERROR_DEVICE_REMOVED;
    HRESULT hr = d3dDevice_->GetDeviceRemovedReason();
    if (FAILED(hr)) return IsDeviceLost(hr) ? hr : DXGI_ERROR_DEVICE_REMOVED;
    return S_OK;
}

HRESULT Renderer::CreateLayer(Layer& layer) {
    layer.Reset();
    if (!dcomp_ || !root_) return E_UNEXPECTED;
    RETURN_IF_FAILED(dcomp_->CreateVisual(layer.visual.put()));
    // Offsets are pixel-aligned, so unscaled content stays crisp; linear filtering only
    // matters once visuals get scale transforms (magnification, later phases).
    layer.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    return root_->AddVisual(layer.visual.get(), TRUE, nullptr);
}

HRESULT Renderer::DrawLayer(Layer& layer, UINT width, UINT height, const DrawFn& draw) {
    if (!layer.visual || !dcomp_) return E_UNEXPECTED;

    if (width == 0 || height == 0) {
        layer.surface = nullptr;
        layer.width = layer.height = 0;
        return layer.visual->SetContent(nullptr);
    }

    if (!layer.surface || layer.width != width || layer.height != height) {
        layer.surface = nullptr;
        RETURN_IF_FAILED(dcomp_->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                               DXGI_ALPHA_MODE_PREMULTIPLIED, layer.surface.put()));
        RETURN_IF_FAILED(layer.visual->SetContent(layer.surface.get()));
        layer.width = width;
        layer.height = height;
    }

    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    RETURN_IF_FAILED(layer.surface->BeginDraw(nullptr, IID_PPV_ARGS(dc.put()), &offset));

    // The surface may live inside a larger atlas: the update rect starts at `offset`.
    dc->SetDpi(96.0f, 96.0f);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType needs an opaque background
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    draw(dc.get());

    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc = nullptr;
    return layer.surface->EndDraw();
}

void Renderer::SetOffset(Layer& layer, float x, float y) {
    if (!layer.visual) return;
    layer.visual->SetOffsetX(x);
    layer.visual->SetOffsetY(y);
}

HRESULT Renderer::Commit() {
    if (!dcomp_) return E_UNEXPECTED;
    return dcomp_->Commit();
}
