// The notification centre element (`notifications` item, round 4 phase 9b): a bell with the unread
// count of every app in a bubble (theme badgeFill / badgeText), in one non-magnifying widget slot
// an icon square; a click opens the centre popup (the dock's NotificationPopup). The count comes
// from the dock's NotificationService (ElementHost::UnreadNotifications); the element redraws only
// when it changes (DockEvent::Notifications). While the service is denied the bell is dimmed.

#include <algorithm>
#include <cmath>
#include <string>

#include <winrt/base.h>

#include "DockElement.h"
#include "Theme.h"

namespace {

class NotificationsElement final : public DockElement {
public:
    NotificationsElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal) : DockElement(host, entry) {
        key_.id = L"kind:notifications" + (ordinal ? L"#" + std::to_wstring(ordinal) : std::wstring());
    }

    void AppendSlots(SlotBuilder& out) override {
        DockItem& item = out.Add(key_);
        if (item.kind != DockItem::Kind::Notifications) {
            item.kind = DockItem::Kind::Notifications;
            item.dirty = true;
        }
        item.pinned = false;
        item.displayName = L"Notifications";
    }

    bool Update() override { return Refresh(); }
    bool OnEvent(DockEvent e) override { return e == DockEvent::Notifications && Refresh(); }

    ElementSize Measure(const LayoutContext& ctx) override {
        Refresh();
        const float s = std::max(16.0f, std::round(ctx.iconPx));
        size_ = s;
        return {s, s};
    }

    HRESULT Render(Renderer& r, Layer& content, const LayoutContext& ctx) override {
        const ThemeConfig& t = host_.DockTheme();
        const UINT px = static_cast<UINT>(size_);
        const int n = count_;
        const bool dim = denied_;
        return r.DrawLayer(content, px, px, [&](ID2D1DeviceContext* dc) {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc->CreateSolidColorBrush(Theme::ToColor(t.labelText, dim ? 0.45f : 0.95f), brush.put()))) return;
            winrt::com_ptr<ID2D1Factory> factory;
            dc->GetFactory(factory.put());
            const float w = size_, g = w * 0.56f, cx = w / 2.0f, cy = w / 2.0f;
            // The bell (the shape of the Notifications widget's).
            const float top = cy - g * 0.42f, rim = cy + g * 0.26f, half = g * 0.4f;
            winrt::com_ptr<ID2D1PathGeometry> path;
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if (factory && SUCCEEDED(factory->CreatePathGeometry(path.put())) && SUCCEEDED(path->Open(sink.put()))) {
                sink->BeginFigure(D2D1::Point2F(cx - half, rim), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - half * 0.72f, rim - g * 0.12f),
                                                    D2D1::Point2F(cx - half * 0.78f, top + g * 0.1f),
                                                    D2D1::Point2F(cx - half * 0.55f, top + g * 0.02f)));
                sink->AddQuadraticBezier(
                    D2D1::QuadraticBezierSegment(D2D1::Point2F(cx, top - g * 0.12f), D2D1::Point2F(cx + half * 0.55f, top + g * 0.02f)));
                sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + half * 0.78f, top + g * 0.1f),
                                                    D2D1::Point2F(cx + half * 0.72f, rim - g * 0.12f), D2D1::Point2F(cx + half, rim)));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                if (SUCCEEDED(sink->Close())) dc->FillGeometry(path.get(), brush.get());
            }
            const float lip = g * 0.08f;
            dc->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(cx - half * 1.05f, rim - g * 0.02f, cx + half * 1.05f, rim - g * 0.02f + lip), lip / 2.0f,
                                  lip / 2.0f),
                brush.get());
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, rim + g * 0.13f), g * 0.1f, g * 0.08f), brush.get());
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, top - g * 0.04f), g * 0.05f, g * 0.05f), brush.get());
            if (n <= 0) return;
            // The count: a bubble at the bell's top right, like an app badge.
            const std::wstring text = n > 99 ? L"99+" : std::to_wstring(n);
            const float h = std::max(std::round(w * t.badgeSize), std::round(12.0f * ctx.scale));
            winrt::com_ptr<IDWriteTextFormat> f;
            if (!ctx.dwrite ||
                FAILED(ctx.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                                    DWRITE_FONT_STRETCH_NORMAL, h * 0.66f, L"", f.put()))) {
                return;
            }
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            float textW = 0.0f;
            winrt::com_ptr<IDWriteTextLayout> layout;
            if (SUCCEEDED(ctx.dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), f.get(), 1000.0f, h,
                                                       layout.put()))) {
                DWRITE_TEXT_METRICS m{};
                if (SUCCEEDED(layout->GetMetrics(&m))) textW = m.widthIncludingTrailingWhitespace;
            }
            const float bw = std::min(w, std::max(h, std::ceil(textW + h * 0.62f)));
            const float bx = std::min(w - bw, std::round(cx + g * 0.38f - h / 2.0f));
            const float by = std::max(0.0f, std::round(cy - g * 0.38f - h / 2.0f));
            brush->SetColor(Theme::ToColor(t.badgeFill));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + bw, by + h), h / 2.0f, h / 2.0f), brush.get());
            brush->SetColor(Theme::ToColor(t.badgeText));
            dc->DrawText(text.c_str(), static_cast<UINT32>(text.size()), f.get(), D2D1::RectF(bx, by - h * 0.04f, bx + bw, by + h),
                         brush.get());
        });
    }

    void OnClick(int slot, bool pressClosedPopup) override {
        if (!pressClosedPopup) host_.ShowClickPopup(slot);  // the centre
    }
    RightClickAction OnRightClick(int) const override { return RightClickAction::DockMenu; }
    ClickPopup Popup() const override { return ClickPopup::Notifications; }
    float PopupAnchor(const DockItem& slot, const LayoutContext&) const override { return slot.x + slot.slotW / 2.0f; }

private:
    // Re-reads the count; true if what the bell shows changed.
    bool Refresh() {
        const int n = static_cast<int>(host_.UnreadNotifications());
        const bool denied = host_.NotificationsDenied();
        if (n == count_ && denied == denied_) return false;
        count_ = n;
        denied_ = denied;
        return true;
    }

    AppKey key_;
    float size_ = 48.0f;
    int count_ = -1;
    bool denied_ = false;
};

}  // namespace

std::unique_ptr<DockElement> MakeNotificationsElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal) {
    return std::make_unique<NotificationsElement>(host, entry, ordinal);
}
