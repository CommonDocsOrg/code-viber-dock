#include "DockElement.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kSeparatorSlot = 12.0f;  // a separator's slot, DIPs (its hairline is centred in it)

// A middle click on an app's slot: what the dock's middleClick says.
void AppMiddleClick(ElementHost& host, int slot) {
    switch (host.DockCfg().middleClick) {
        case MiddleClickAction::NewInstance: host.LaunchSlot(slot, true); break;
        case MiddleClickAction::Minimize: host.SlotWindows(slot, WindowsCommand::Minimize); break;
        case MiddleClickAction::Close: host.SlotWindows(slot, WindowsCommand::Close); break;
        case MiddleClickAction::None:
        default: break;
    }
}

// The wheel over an app's slot with the dock's wheel "cycle": one window per notch (WHEEL_DELTA;
// the smaller steps of a precision wheel add up in `acc`, per slot). Towards the user (negative):
// the next window, away: the previous one. Consumed even when the app has no windows.
class WheelCycler {
public:
    bool OnWheel(ElementHost& host, int slot, int delta, bool horizontal) {
        if (horizontal || host.DockCfg().wheel != WheelAction::Cycle) return false;
        if (slot != slot_) {
            slot_ = slot;
            acc_ = 0;
        }
        acc_ += delta;
        if (acc_ >= WHEEL_DELTA || acc_ <= -WHEEL_DELTA) {
            const bool forward = acc_ < 0;
            acc_ %= WHEEL_DELTA;
            host.SlotWindows(slot, forward ? WindowsCommand::Next : WindowsCommand::Previous);
        }
        return true;
    }

private:
    int slot_ = -1;
    int acc_ = 0;
};

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
        if (pin.kind == DockItem::Kind::Group) {
            // What the composed icon shows (and the popup lists): the members.
            std::vector<DockItem::Member> members;
            members.reserve(pin.members.size());
            for (const AppIdentity::ResolvedPin& m : pin.members) {
                members.push_back(DockItem::Member{m.key, m.displayName, m.iconFile, m.iconIndex, m.missing});
            }
            if (members != item.members) {
                item.members = std::move(members);
                item.dirty = true;
            }
        } else if (!item.members.empty()) {
            item.members.clear();
        }
        // Unstacked: the pin slot stays as it is (its click launches / activates the app, its
        // dot and preview are the app's) and every window of the app follows it as a slot of
        // its own, in the app's first-seen window order.
        if (!out.unstacked || pin.kind != DockItem::Kind::App) return;
        const WindowTracker::Group* group = nullptr;
        for (const WindowTracker::Group& g : out.Snapshot().groups) {
            if (g.key == pin.key) group = &g;
        }
        if (!group) return;
        for (HWND w : group->windows) {
            DockItem& slot = out.Add(pin.key, nullptr, w);
            CopyAppData(pin, slot);
        }
    }

    // A window slot of a pinned app looks like the pin (icon, launch data) but is not a pin.
    static void CopyAppData(const AppIdentity::ResolvedPin& pin, DockItem& slot) {
        if (slot.kind != DockItem::Kind::App || slot.iconFile != pin.iconFile || slot.iconIndex != pin.iconIndex ||
            slot.customIcon != pin.customIcon) {
            slot.dirty = true;
        }
        slot.pinned = false;
        slot.kind = DockItem::Kind::App;
        slot.customIcon = pin.customIcon;
        slot.displayName = pin.displayName;
        slot.launchPath = pin.launchPath.empty() ? std::nullopt : std::optional<std::wstring>(pin.launchPath);
        slot.workingDir = pin.workingDir;
        slot.targetPath = pin.targetPath;
        slot.iconFile = pin.iconFile;
        slot.iconIndex = pin.iconIndex;
        slot.missing = false;
    }

    ElementSize Measure(const LayoutContext& ctx) override {
        // A separator's slot is narrow and its box (the hairline) fills it; it never magnifies.
        if (resolved_ == DockItem::Kind::Separator) {
            return {std::max(2.0f, std::round(kSeparatorSlot * ctx.scale)), ctx.iconPx};
        }
        return {ctx.iconPx, ctx.iconPx};
    }

    void OnClick(int slot, bool pressClosedPopup) override {
        if (resolved_ == DockItem::Kind::Folder || resolved_ == DockItem::Kind::Group) {
            // The folder popup; a middle click opens the folder in Explorer.
            if (!pressClosedPopup) host_.ShowClickPopup(slot);
            return;
        }
        host_.ActivateOrLaunch(slot);  // a separator does nothing
    }
    // Apps: the dock's middleClick action. Folders and the Recycle Bin open in Explorer.
    void OnMiddleClick(int slot) override {
        if (resolved_ == DockItem::Kind::App) {
            AppMiddleClick(host_, slot);
        } else if (resolved_ != DockItem::Kind::Group) {  // a group: nothing
            host_.LaunchSlot(slot, true);  // a separator does nothing
        }
    }
    bool OnWheel(int slot, int delta, bool horizontal) override {
        return resolved_ == DockItem::Kind::App && wheel_.OnWheel(host_, slot, delta, horizontal);
    }
    ClickPopup Popup() const override {
        return resolved_ == DockItem::Kind::Folder  ? ClickPopup::Folder
               : resolved_ == DockItem::Kind::Group ? ClickPopup::Group
                                                     : ClickPopup::None;
    }

private:
    size_t pinIndex_;
    DockItem::Kind resolved_ = DockItem::Kind::App;  // the resolved pin's kind (AppendSlots)
    WheelCycler wheel_;
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
            // Unstacked: one slot per window, the app's windows next to each other.
            const size_t slots = out.unstacked ? g->windows.size() : 1;
            std::wstring name;
            for (size_t k = 0; k < slots; ++k) {
                bool fresh = false;
                DockItem& item = out.Add(g->key, &fresh, out.unstacked ? g->windows[k] : nullptr);
                if (fresh) {
                    if (name.empty()) name = AppIdentity::DisplayNameFor(g->key);
                    item.displayName = name;
                }
                item.pinned = false;  // was unpinned while running
                if (item.kind != DockItem::Kind::App) {
                    item.kind = DockItem::Kind::App;
                    item.dirty = true;
                }
            }
        }
    }

    ElementSize Measure(const LayoutContext& ctx) override { return {ctx.iconPx, ctx.iconPx}; }
    void OnClick(int slot, bool) override { host_.ActivateOrLaunch(slot); }
    void OnMiddleClick(int slot) override { AppMiddleClick(host_, slot); }
    bool OnWheel(int slot, int delta, bool horizontal) override {
        return wheel_.OnWheel(host_, slot, delta, horizontal);
    }

private:
    WheelCycler wheel_;
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
    return std::any_of(next_.begin(), next_.end(), [&](const DockItem& i) { return !i.window && i.key == key; });
}

DockItem& SlotBuilder::Add(const AppKey& key, bool* fresh, HWND window) {
    bool reused = false;
    for (size_t i = 0; i < previous_.size(); ++i) {
        if (!taken_[i] && previous_[i].window == window && previous_[i].key == key) {
            taken_[i] = true;
            next_.push_back(std::move(previous_[i]));
            reused = true;
            break;
        }
    }
    if (!reused) {
        next_.emplace_back();
        next_.back().key = key;
        next_.back().window = window;
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
        case ItemKind::Group:
            return std::make_unique<PinElement>(host, entry, pinIndex);
        case ItemKind::Running:
            return std::make_unique<RunningElement>(host, entry);
        case ItemKind::Clock:
            return MakeClockElement(host, entry, ordinal);
        case ItemKind::Weather:
            return MakeWeatherElement(host, entry, ordinal);
        case ItemKind::Widget:
            return MakeScriptWidgetElement(host, entry, ordinal);
        case ItemKind::Tray:
            return MakeTrayElement(host, entry, ordinal);
        case ItemKind::Unknown:
        default:
            return nullptr;
    }
}
