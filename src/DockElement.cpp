#include "DockElement.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kSeparatorSlot = 12.0f;  // a separator's slot, DIPs (its hairline is centred in it)

// app / folder / separator / recycleBin: one slot for the resolved pin (none when an earlier
// entry resolved to the same app). A pinned app that is running shows its windows (the dock
// merges the window group with the same key into it).
class PinElement final : public DockElement {
public:
    PinElement(ElementHost& host, const DockItemEntry& entry, size_t pinIndex)
        : DockElement(host, entry), pinIndex_(pinIndex) {}

    void AppendSlots(SlotBuilder& out) override {
        if (pinIndex_ >= out.Pins().size()) return;
        const AppIdentity::ResolvedPin& pin = out.Pins()[pinIndex_];
        resolved_ = pin.kind;  // an "app" entry may turn out to be a folder (path or .lnk target)
        if (out.Has(pin.key)) return;  // entries that resolve to the same app show once
        DockItem& item = out.Add(pin.key);
        if (!item.pinned || item.iconFile != pin.iconFile || item.iconIndex != pin.iconIndex ||
            item.missing != pin.missing || item.kind != pin.kind || item.customIcon != pin.customIcon) {
            item.dirty = true;
        }
        item.pinned = true;
        item.kind = pin.kind;
        item.customIcon = pin.customIcon;
        item.displayName = pin.displayName;
        item.launchPath = pin.launchPath.empty() ? std::nullopt : std::optional<std::wstring>(pin.launchPath);
        item.workingDir = pin.workingDir;
        item.targetPath = pin.targetPath;
        item.iconFile = pin.iconFile;
        item.iconIndex = pin.iconIndex;
        item.missing = pin.missing;
    }

    ElementSize Measure(const LayoutContext& ctx) override {
        // A separator's slot is narrow and its box (the hairline) fills it; it never magnifies.
        if (resolved_ == DockItem::Kind::Separator) {
            return {std::max(2.0f, std::round(kSeparatorSlot * ctx.scale)), ctx.iconPx};
        }
        return {ctx.iconPx, ctx.iconPx};
    }

    void OnClick(int slot, bool pressClosedPopup) override {
        if (resolved_ == DockItem::Kind::Folder) {
            // The folder popup; a middle click opens the folder in Explorer.
            if (!pressClosedPopup) host_.ShowClickPopup(slot);
            return;
        }
        host_.ActivateOrLaunch(slot);  // a separator does nothing
    }
    void OnMiddleClick(int slot) override { host_.LaunchSlot(slot, true); }
    ClickPopup Popup() const override {
        return resolved_ == DockItem::Kind::Folder ? ClickPopup::Folder : ClickPopup::None;
    }

private:
    size_t pinIndex_;
    DockItem::Kind resolved_ = DockItem::Kind::App;  // the resolved pin's kind (AppendSlots)
};

// running: the running apps that are not pinned, one slot each, in first-seen order (or the order
// the user dragged them into this session). Only the first `running` item expands; a dock without
// one does not show unpinned running apps.
class RunningElement final : public DockElement {
public:
    using DockElement::DockElement;

    void AppendSlots(SlotBuilder& out) override {
        if (out.runningPlaced) return;
        out.runningPlaced = true;
        std::vector<const WindowTracker::Group*> groups;
        groups.reserve(out.Snapshot().groups.size());
        for (const WindowTracker::Group& g : out.Snapshot().groups) {
            if (!out.IsPinnedApp(g.key)) groups.push_back(&g);
        }
        // After the user reordered the running apps: that order, apps not in it at the end. The
        // order then keeps only apps that are still running.
        std::vector<AppKey>& order = out.RunningOrder();
        if (!order.empty() && !groups.empty()) {
            auto rank = [&](const AppKey& key) {
                return static_cast<size_t>(std::find(order.begin(), order.end(), key) - order.begin());
            };
            std::stable_sort(groups.begin(), groups.end(), [&](const WindowTracker::Group* a, const WindowTracker::Group* b) {
                return rank(a->key) < rank(b->key);
            });
            order.clear();
            for (const WindowTracker::Group* g : groups) order.push_back(g->key);
        }
        for (const WindowTracker::Group* g : groups) {
            bool fresh = false;
            DockItem& item = out.Add(g->key, &fresh);
            if (fresh) item.displayName = AppIdentity::DisplayNameFor(g->key);
            item.pinned = false;  // was unpinned while running
            if (item.kind != DockItem::Kind::App) {
                item.kind = DockItem::Kind::App;
                item.dirty = true;
            }
        }
    }

    ElementSize Measure(const LayoutContext& ctx) override { return {ctx.iconPx, ctx.iconPx}; }
    void OnClick(int slot, bool) override { host_.ActivateOrLaunch(slot); }
    void OnMiddleClick(int slot) override { host_.LaunchSlot(slot, true); }
};

}  // namespace

SlotBuilder::SlotBuilder(const WindowTracker::Snapshot& snap, const std::vector<AppIdentity::ResolvedPin>& pins,
                         std::vector<AppKey>& runningOrder, std::vector<DockItem>& previous,
                         std::vector<DockItem>& next)
    : snap_(snap), pins_(pins), runningOrder_(runningOrder), previous_(previous), next_(next),
      taken_(previous.size(), false) {}

bool SlotBuilder::IsPinnedApp(const AppKey& key) const {
    return std::any_of(pins_.begin(), pins_.end(), [&](const AppIdentity::ResolvedPin& p) {
        return p.kind == DockItem::Kind::App && p.key == key;
    });
}

bool SlotBuilder::Has(const AppKey& key) const {
    return std::any_of(next_.begin(), next_.end(), [&](const DockItem& i) { return i.key == key; });
}

DockItem& SlotBuilder::Add(const AppKey& key, bool* fresh) {
    bool reused = false;
    for (size_t i = 0; i < previous_.size(); ++i) {
        if (!taken_[i] && previous_[i].key == key) {
            taken_[i] = true;
            next_.push_back(std::move(previous_[i]));
            reused = true;
            break;
        }
    }
    if (!reused) {
        next_.emplace_back();
        next_.back().key = key;
    }
    if (fresh) *fresh = !reused;
    DockItem& item = next_.back();
    item.element = element_;
    return item;
}

std::unique_ptr<DockElement> MakeElement(ElementHost& host, const DockItemEntry& entry, size_t pinIndex,
                                         size_t ordinal) {
    switch (entry.Kind()) {
        case ItemKind::App:
        case ItemKind::Folder:
        case ItemKind::Separator:
        case ItemKind::RecycleBin:
            return std::make_unique<PinElement>(host, entry, pinIndex);
        case ItemKind::Running:
            return std::make_unique<RunningElement>(host, entry);
        case ItemKind::Clock:
            return MakeClockElement(host, entry, ordinal);
        case ItemKind::Weather:
            return MakeWeatherElement(host, entry, ordinal);
        case ItemKind::Unknown:
        default:
            return nullptr;
    }
}
