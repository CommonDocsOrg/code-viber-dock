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
    // Opacity (auto-hide fade) needs IDCompositionVisual3 (Windows 8.1+); without it the dock
    // still slides, it just does not fade.
    if (FAILED(root_->QueryInterface(IID_PPV_ARGS(root3_.put())))) root3_ = nullptr;
    return S_OK;
}

void Renderer::ReleaseDeviceResources() {
    if (target_) target_->SetRoot(nullptr);
    resourceDc_ = nullptr;
    root3_ = nullptr;
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

HRESULT Renderer::CreateLayer(Layer& layer, Layer* parent) {
    layer.Reset();
    if (!dcomp_ || !root_) return E_UNEXPECTED;
    IDCompositionVisual2* p = parent ? parent->visual.get() : root_.get();
    if (!p) return E_UNEXPECTED;
    RETURN_IF_FAILED(dcomp_->CreateVisual(layer.visual.put()));
    // Offsets are pixel-aligned, so unscaled content stays crisp; linear filtering matters
    // for scaled (magnified) and animated visuals.
    layer.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    // With no reference visual, insertAbove = FALSE puts the new visual in front of all its
    // siblings (TRUE would put it behind them).
    RETURN_IF_FAILED(p->AddVisual(layer.visual.get(), FALSE, nullptr));
    layer.parent.copy_from(p);
    return S_OK;
}

HRESULT Renderer::ShareContent(Layer& dst, const Layer& src) {
    if (!dst.visual) return E_UNEXPECTED;
    return dst.visual->SetContent(src.surface.get());
}

void Renderer::SetStaticTransform(Layer& layer, const D2D1_MATRIX_3X2_F& m) {
    if (layer.visual) layer.visual->SetTransform(m);
}

HRESULT Renderer::EnsurePlacement(Layer& layer, float cx, float cy) {
    if (!layer.visual || !dcomp_) return E_UNEXPECTED;
    if (!layer.scale || !layer.translate) {
        winrt::com_ptr<IDCompositionScaleTransform> scale;
        winrt::com_ptr<IDCompositionTranslateTransform> translate;
        winrt::com_ptr<IDCompositionTransform> group;
        RETURN_IF_FAILED(dcomp_->CreateScaleTransform(scale.put()));
        RETURN_IF_FAILED(dcomp_->CreateTranslateTransform(translate.put()));
        IDCompositionTransform* parts[] = {scale.get(), translate.get()};  // applied in order
        RETURN_IF_FAILED(dcomp_->CreateTransformGroup(parts, 2, group.put()));
        RETURN_IF_FAILED(layer.visual->SetTransform(group.get()));
        layer.scale = std::move(scale);
        layer.translate = std::move(translate);
    }
    layer.scale->SetCenterX(cx);
    layer.scale->SetCenterY(cy);
    return S_OK;
}

HRESULT Renderer::SetClip(Layer& layer, const D2D1_RECT_F& r) {
    if (!layer.visual || !dcomp_) return E_UNEXPECTED;
    if (!layer.clip) {
        RETURN_IF_FAILED(dcomp_->CreateRectangleClip(layer.clip.put()));
        RETURN_IF_FAILED(layer.visual->SetClip(layer.clip.get()));
    }
    layer.clip->SetLeft(r.left);
    layer.clip->SetTop(r.top);
    layer.clip->SetRight(r.right);
    layer.clip->SetBottom(r.bottom);
    return S_OK;
}

void Renderer::SetVisible(Layer& layer, bool visible) {
    if (!layer.visual) return;
    winrt::com_ptr<IDCompositionVisual3> v3;
    if (SUCCEEDED(layer.visual->QueryInterface(IID_PPV_ARGS(v3.put())))) {
        v3->SetVisible(visible ? TRUE : FALSE);
    } else {
        // No IDCompositionVisual3: park the visual far outside the window instead.
        layer.visual->SetOffsetX(visible ? 0.0f : -100000.0f);
    }
}

double Renderer::Now() {
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
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

winrt::com_ptr<IDCompositionAnimation> Renderer::CreateAnimation() {
    winrt::com_ptr<IDCompositionAnimation> animation;
    if (!dcomp_ || FAILED(dcomp_->CreateAnimation(animation.put()))) return nullptr;
    return animation;
}

HRESULT Renderer::AnimateOffsetY(Layer& layer, IDCompositionAnimation* animation) {
    if (!layer.visual || !animation) return E_INVALIDARG;
    RETURN_IF_FAILED(layer.visual->SetOffsetY(animation));
    layer.offsetYAnimation.copy_from(animation);
    return S_OK;
}

void Renderer::StopOffsetYAnimation(Layer& layer, float value) {
    layer.offsetYAnimation = nullptr;
    if (layer.visual) layer.visual->SetOffsetY(value);  // replaces the animation
}

void Renderer::DestroyLayer(Layer& layer) {
    if (layer.visual) {
        if (layer.parent) {
            layer.parent->RemoveVisual(layer.visual.get());
        } else if (root_) {
            root_->RemoveVisual(layer.visual.get());
        }
    }
    layer.Reset();
}

HRESULT Renderer::CreateWindowTarget(HWND hwnd, winrt::com_ptr<IDCompositionTarget>& target, Layer& root) {
    target = nullptr;
    root.Reset();
    if (!dcomp_) return E_UNEXPECTED;
    RETURN_IF_FAILED(dcomp_->CreateTargetForHwnd(hwnd, TRUE, target.put()));
    RETURN_IF_FAILED(dcomp_->CreateVisual(root.visual.put()));
    root.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    return target->SetRoot(root.visual.get());
}

ID2D1DeviceContext* Renderer::ResourceContext() {
    if (!resourceDc_ && d2dDevice_) {
        if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, resourceDc_.put()))) {
            resourceDc_ = nullptr;
        }
    }
    return resourceDc_.get();
}

HRESULT Renderer::Commit() {
    if (!dcomp_) return E_UNEXPECTED;
    return dcomp_->Commit();
}
