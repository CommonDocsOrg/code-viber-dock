#include "ScriptPopup.h"

#include <cmath>

void ScriptPopup::Init(HINSTANCE instance, Renderer* renderer, Callbacks callbacks) {
    PopupWindow::Init(instance, renderer);
    callbacks_ = std::move(callbacks);
    SetClosedCallback([this] {
        if (callbacks_.closed) callbacks_.closed();
    });
}

bool ScriptPopup::Show(float w, float h, const Style& style, const PopupAnchor& anchor) {
    style_ = style;
    w_ = w;
    h_ = h;
    Frame frame;
    frame.scale = style.scale;
    frame.acrylic = style.acrylic;
    frame.cornerRadiusDip = style.cornerRadiusDip;
    frame.monitor = style.monitor;
    return ShowAt(frame, anchor);
}

void ScriptPopup::Repaint() {
    if (!OnScreen()) return;
    if (SUCCEEDED(Redraw())) Commit();
}

SIZE ScriptPopup::Measure() {
    const float s = Scale();
    return SIZE{std::max(1L, std::lround(w_ * s)), std::max(1L, std::lround(h_ * s))};
}

void ScriptPopup::Paint(ID2D1DeviceContext* dc) {
    PaintPanel(dc, style_.tint, style_.border);
    if (callbacks_.paint) callbacks_.paint(dc, w_, h_, Scale());
}

int ScriptPopup::HitAt(POINT, int* part) const {
    if (part) *part = 0;
    return 0;  // the whole popup: the script hit-tests by itself
}

void ScriptPopup::OnClick(int, int, Button button) {
    if (!callbacks_.click) return;
    const DWORD pos = GetMessagePos();
    const RECT r = ScreenRect();
    const float s = Scale() > 0.0f ? Scale() : 1.0f;
    const float x = (static_cast<float>(static_cast<short>(LOWORD(pos))) - static_cast<float>(r.left)) / s;
    const float y = (static_cast<float>(static_cast<short>(HIWORD(pos))) - static_cast<float>(r.top)) / s;
    callbacks_.click(x, y, button == Button::Middle ? 1 : button == Button::Right ? 2 : 0);
}
