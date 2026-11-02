#include "IconEffects.h"

#include <d2d1effects.h>

#include <algorithm>
#include <cmath>

namespace IconEffects {

winrt::com_ptr<ID2D1CommandList> Record(ID2D1DeviceContext* dc, float size, const DrawIconFn& drawIcon) {
    if (!dc || !drawIcon || size <= 0.0f) return nullptr;
    // A context without a target (the dock's resource context) records directly; one that is
    // drawing gets a separate context on its device.
    winrt::com_ptr<ID2D1Image> target;
    dc->GetTarget(target.put());
    winrt::com_ptr<ID2D1DeviceContext> recorder;
    if (!target) {
        recorder.copy_from(dc);
    } else {
        winrt::com_ptr<ID2D1Device> device;
        dc->GetDevice(device.put());
        if (!device || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, recorder.put()))) {
            return nullptr;
        }
    }
    winrt::com_ptr<ID2D1CommandList> list;
    if (FAILED(recorder->CreateCommandList(list.put()))) return nullptr;
    recorder->SetTarget(list.get());
    recorder->SetDpi(96.0f, 96.0f);  // pixel units, like the surfaces it is drawn into
    recorder->BeginDraw();
    recorder->SetTransform(D2D1::Matrix3x2F::Identity());
    drawIcon(recorder.get(), size);
    const HRESULT hr = recorder->EndDraw();
    recorder->SetTarget(nullptr);
    if (FAILED(hr) || FAILED(list->Close())) return nullptr;
    return list;
}

UINT GlowPad(float radiusPx) { return static_cast<UINT>(std::ceil(std::max(radiusPx, 1.0f))) + 1u; }

uint32_t GlowColor(const ThemeConfig& theme, uint32_t dominant, uint32_t fallback) {
    if ((theme.glowColor >> 24) != 0) return theme.glowColor;
    if (dominant != 0) return dominant | 0xFF000000u;
    return fallback;
}

bool DrawGlow(ID2D1DeviceContext* dc, ID2D1Image* icon, float radiusPx, uint32_t argb, float strength,
              D2D1_POINT_2F at) {
    if (!dc || !icon) return false;
    winrt::com_ptr<ID2D1Effect> blur;
    winrt::com_ptr<ID2D1Effect> boost;
    winrt::com_ptr<ID2D1Effect> tint;
    if (FAILED(dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.put())) ||
        FAILED(dc->CreateEffect(CLSID_D2D1ColorMatrix, boost.put())) ||
        FAILED(dc->CreateEffect(CLSID_D2D1ColorMatrix, tint.put()))) {
        return false;
    }
    blur->SetInput(0, icon);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, std::max(0.5f, radiusPx / 3.0f));
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);

    // Both colour matrices work on premultiplied values. (Straight-alpha mode un-premultiplies the
    // fully transparent pixels, 0 / 0, and the zero coefficients times NaN turned the whole blur
    // rectangle opaque.) First the coverage: alpha = min(1, kGain x blurred alpha), colour 0, so the
    // halo stays solid near the silhouette instead of fading from half opacity at its edge.
    constexpr float kGain = 3.0f;
    boost->SetInputEffect(0, blur.get());
    boost->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, D2D1::Matrix5x4F(0, 0, 0, 0,       // R in
                                                                         0, 0, 0, 0,       // G in
                                                                         0, 0, 0, 0,       // B in
                                                                         0, 0, 0, kGain,   // A in
                                                                         0, 0, 0, 0));     // bias
    boost->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE, D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED);
    boost->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
    // Then the colour: output = A x (r a, g a, b a, a), a = strength x the colour's alpha.
    const float a = std::clamp(strength, 0.0f, 1.0f) * static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
    const float r = static_cast<float>((argb >> 16) & 0xFF) / 255.0f * a;
    const float g = static_cast<float>((argb >> 8) & 0xFF) / 255.0f * a;
    const float b = static_cast<float>(argb & 0xFF) / 255.0f * a;
    tint->SetInputEffect(0, boost.get());
    tint->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, D2D1::Matrix5x4F(0, 0, 0, 0,   // R in
                                                                        0, 0, 0, 0,   // G in
                                                                        0, 0, 0, 0,   // B in
                                                                        r, g, b, a,   // A in
                                                                        0, 0, 0, 0)); // bias
    tint->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE, D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED);
    tint->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);

    winrt::com_ptr<ID2D1Image> output;
    tint->GetOutput(output.put());
    if (!output) return false;
    dc->DrawImage(output.get(), &at);
    return true;
}

UINT ReflectionHeight(UINT size, float heightFrac) {
    return static_cast<UINT>(std::ceil(static_cast<float>(size) * std::clamp(heightFrac, 0.0f, 1.0f)));
}

void DrawReflection(ID2D1DeviceContext* dc, ID2D1Image* icon, float size, float height, float opacity,
                    D2D1_POINT_2F at) {
    if (!dc || !icon || size <= 0.0f || height <= 0.0f || opacity <= 0.0f) return;
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(0, 0, 0, std::min(opacity, 1.0f))},
                                        {1.0f, D2D1::ColorF(0, 0, 0, 0.0f)}};
    winrt::com_ptr<ID2D1GradientStopCollection> stopCollection;
    winrt::com_ptr<ID2D1LinearGradientBrush> fade;
    if (FAILED(dc->CreateGradientStopCollection(stops, 2, stopCollection.put())) ||
        FAILED(dc->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(at, D2D1::Point2F(at.x, at.y + height)), stopCollection.get(),
            fade.put()))) {
        return;
    }
    const D2D1_RECT_F bounds = D2D1::RectF(at.x, at.y, at.x + size, at.y + height);
    dc->PushLayer(D2D1::LayerParameters1(bounds, nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::Matrix3x2F::Identity(),
                                         1.0f, fade.get(), D2D1_LAYER_OPTIONS1_NONE),
                  nullptr);
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    // Icon y -> at.y + size - y: its bottom row lands at the top of the reflection.
    dc->SetTransform(D2D1::Matrix3x2F::Scale(1.0f, -1.0f) * D2D1::Matrix3x2F::Translation(at.x, at.y + size) * old);
    dc->DrawImage(icon);
    dc->SetTransform(old);
    dc->PopLayer();
}

}  // namespace IconEffects
