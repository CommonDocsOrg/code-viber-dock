#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <cstddef>
#include <memory>
#include <vector>

#include "AppIdentity.h"
#include "Config.h"
#include "DockItems.h"
#include "Renderer.h"
#include "WindowTracker.h"

class WeatherService;

// The dock's contents. Every docks[].items[] entry becomes one DockElement, and the dock lays the
// elements out in config order. An element contributes 0..n slots (DockItem: the icon row's unit
// of layout, magnification, placement tweens, bounce and dot): app / folder / separator /
// recycleBin one each (an entry resolving to an app that is already on the dock: none), `running`
// one per unpinned running app, clock / weather one widget slot sized by Measure. The dock owns
// the slots, the window, layout, magnification, drags, drops, popups and auto-hide; an element
// knows its kind and config entry, measures its slots, draws non-icon content, and handles input
// on its slots. Elements are rebuilt whenever the item list changes (they hold no visuals).

// Layout axes, so that no element bakes in x or y. Main: along the bar; cross: away from the screen
// edge. Only the bottom edge exists until Phase 2 (the dock maps main -> x, cross -> y).
enum class DockAxis { Horizontal, Vertical };

struct ElementSize {
    float main = 0.0f;
    float cross = 0.0f;
    bool operator==(const ElementSize&) const = default;
};

struct LayoutContext {
    DockAxis axis = DockAxis::Horizontal;
    float scale = 1.0f;   // DPI scale (physical px per DIP)
    float iconPx = 0.0f;  // the unmagnified icon box, both axes
    IDWriteFactory* dwrite = nullptr;
};

// Something outside the dock changed that an element may show.
enum class DockEvent {
    ClockTick,    // the clock's timer (DockWindow::kClockTimerId) fired
    TimeChanged,  // WM_TIMECHANGE, resume from sleep
    WeatherData,  // the weather service's data or status changed
};

// The click popups the dock hosts (one at a time; each holds the dock shown while open).
enum class ClickPopup { None, Folder, Weather, Calendar };

// What a right click on a slot opens.
enum class RightClickAction { ItemMenu, DockMenu };

// Something to do with the windows of a slot's app (ElementHost::WindowsCommand).
enum class WindowsCommand { Minimize, Close, Next, Previous };

// What elements may ask of the dock (DockWindow implements it).
class ElementHost {
public:
    static constexpr UINT_PTR kClockTimerId = 1;  // = DockWindow::kClockTimerId

    virtual HWND Hwnd() const = 0;
    virtual const Config& Cfg() const = 0;
    // This dock's own settings (the per-dock keys: middle click, wheel, previews ...).
    virtual const DockConfig& DockCfg() const = 0;
    virtual const WeatherService& Weather() const = 0;
    // Brings the slot's app to the front (cycles / minimizes its windows), or starts it.
    virtual void ActivateOrLaunch(int slot) = 0;
    // Starts the slot's app (newInstance: even if it has windows) with launch feedback; folders and
    // the Recycle Bin open in Explorer.
    virtual void LaunchSlot(int slot, bool newInstance) = 0;
    // The slot's click popup (its element's Popup()) is open for this slot.
    virtual bool PopupShownFor(int slot) const = 0;
    // Opens the slot's click popup, centred on its element's PopupAnchor.
    virtual void ShowClickPopup(int slot) = 0;
    // Settings > Weather (the unconfigured weather widget was clicked).
    virtual void OpenWeatherSettings() = 0;
    // Minimizes / closes the slot's windows, or brings its next / previous window to the front.
    virtual void SlotWindows(int slot, WindowsCommand cmd) = 0;
    // Opens an app / file / URL the user configured (Launcher::OpenTarget; beeps on failure).
    virtual void OpenTarget(const std::wstring& target) = 0;

protected:
    ~ElementHost() = default;
};

// Rebuild state handed to every element in dock order: the window snapshot, the resolved pins, and
// the slot list being built (slots of the previous list are reused by key, keeping their visuals,
// tweens, bounce and windows).
class SlotBuilder {
public:
    SlotBuilder(const WindowTracker::Snapshot& snap, const std::vector<AppIdentity::ResolvedPin>& pins,
                std::vector<AppKey>& runningOrder, std::vector<DockItem>& previous, std::vector<DockItem>& next);

    const WindowTracker::Snapshot& Snapshot() const { return snap_; }
    const std::vector<AppIdentity::ResolvedPin>& Pins() const { return pins_; }
    // Order of the unpinned running apps after the user dragged among them (session only).
    std::vector<AppKey>& RunningOrder() { return runningOrder_; }
    // A pinned app (kind app, not a folder / separator / bin) has this key: its windows show on its pin.
    bool IsPinnedApp(const AppKey& key) const;
    // The new list has a slot with this key.
    bool Has(const AppKey& key) const;
    // Appends a slot for `key` (owned by the current element): the previous slot of that key moved
    // over, or a fresh one (*fresh = true). The reference is valid until the next Add.
    DockItem& Add(const AppKey& key, bool* fresh = nullptr);
    // The `running` element that expands into the unpinned running apps (the first one) is done.
    bool runningPlaced = false;

    // For the dock: the element Add assigns slots to, and which previous slots were reused.
    void SetElement(size_t index) { element_ = index; }
    bool Taken(size_t previousIndex) const { return taken_[previousIndex]; }

private:
    const WindowTracker::Snapshot& snap_;
    const std::vector<AppIdentity::ResolvedPin>& pins_;
    std::vector<AppKey>& runningOrder_;
    std::vector<DockItem>& previous_;
    std::vector<DockItem>& next_;
    std::vector<bool> taken_;
    size_t element_ = 0;
};

class DockElement {
public:
    DockElement(ElementHost& host, const DockItemEntry& entry) : host_(host), entry_(entry), kind_(entry.Kind()) {}
    DockElement(const DockElement&) = delete;
    DockElement& operator=(const DockElement&) = delete;
    virtual ~DockElement() = default;

    ItemKind Kind() const { return kind_; }
    const DockItemEntry& Entry() const { return entry_; }

    // --- slots ---
    // Appends this element's slots for the current state (0..n), in order.
    virtual void AppendSlots(SlotBuilder& out) = 0;

    // --- layout ---
    // Brings the content up to date before a layout (the clock's text). True if it changed (the
    // dock redraws the element's slots).
    virtual bool Update() { return false; }
    // The content box of each of its slots. Icon slots: the icon (the dock adds the gap around it).
    virtual ElementSize Measure(const LayoutContext& ctx) = 0;
    // How far beyond the box (main axis, rest layout) a press still hits the slot.
    virtual float HitSlack(const LayoutContext&) const { return 0.0f; }

    // --- rendering ---
    // Draws a slot's non-icon content (Measure's size) into `content`. Icon and separator slots are
    // drawn by the dock (icon cache, effects), so the default does nothing.
    virtual HRESULT Render(Renderer& /*renderer*/, Layer& /*content*/, const LayoutContext&) { return S_OK; }

    // --- input (`slot`: index in the dock's slot list) ---
    // Left button down on the slot. True when the press is on the slot whose click popup is open:
    // the dock closes the popup and the release must not open it again (a click toggles).
    virtual bool OnPress(int slot) { return Popup() != ClickPopup::None && host_.PopupShownFor(slot); }
    // Left click (press and release on the slot). `pressClosedPopup`: what OnPress returned.
    virtual void OnClick(int slot, bool pressClosedPopup) = 0;
    virtual void OnMiddleClick(int) {}
    // A wheel turn over the slot (WHEEL_DELTA units; horizontal: the tilt wheel). True if consumed.
    virtual bool OnWheel(int, int /*delta*/, bool /*horizontal*/) { return false; }
    virtual RightClickAction OnRightClick(int) const { return RightClickAction::ItemMenu; }

    // --- popups ---
    // The click popup this element opens (the dock hosts it).
    virtual ClickPopup Popup() const { return ClickPopup::None; }
    // Main-axis stage coordinate a slot's popup is centred on: the icon where it is drawn now.
    virtual float PopupAnchor(const DockItem& slot, const LayoutContext& ctx) const {
        return slot.targetX + ctx.iconPx / 2.0f;
    }

    // --- config, timers, outside changes ---
    // The config was re-read (sizes, theme, clock format...).
    virtual void Configure() {}
    // Starts the element's timers (the dock stops them all first: the clock's tick).
    virtual void Start() {}
    // Something outside changed. True if the content changed: the dock re-measures the element,
    // redraws only its slots, and relays the dock out if the size changed.
    virtual bool OnEvent(DockEvent) { return false; }

protected:
    ElementHost& host_;

private:
    DockItemEntry entry_;
    ItemKind kind_;
};

// The element for a config item: `pinIndex` is its position among the pins (DockConfig::PinIndexes,
// for app / folder / separator / recycleBin), `ordinal` its position among the items of its kind.
// nullptr for a kind this version does not know (such an item is not shown).
std::unique_ptr<DockElement> MakeElement(ElementHost& host, const DockItemEntry& entry, size_t pinIndex,
                                         size_t ordinal);

// WidgetElements.cpp
std::unique_ptr<DockElement> MakeClockElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal);
std::unique_ptr<DockElement> MakeWeatherElement(ElementHost& host, const DockItemEntry& entry, size_t ordinal);
