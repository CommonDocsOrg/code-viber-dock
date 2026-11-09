// Round 12 phase 12G: Quick setup on the first run and the three tips on the dock, App's part (the
// windows are QuickSetup.cpp and CoachMarks.cpp). App.cpp keeps only the hooks: the entry points,
// the dispatch of DockCommand 180 - 183 / QueryState 317 - 320 and the three gestures' one-line calls.

#include "App.h"

#include <algorithm>

#include "ContextMenu.h"
#include "Presets.h"
#include "SettingsIds.h"
#include "Theme.h"

namespace {

constexpr wchar_t kSetupLabel[] = L"Quick setup";
constexpr wchar_t kSetupText[] = L"Quick setup is open: the desktop shows your choices.";

}  // namespace

size_t App::FirstDockIndex() const {
    for (size_t i = 0; i < config_.docks.size(); ++i) {
        if (config_.docks[i].IsDock()) return i;
    }
    return SIZE_MAX;
}

void App::OpenQuickSetup(bool firstRun) {
    if (shutDown_) return;
    if (QuickSetupOpen()) {
        quickSetup_->Activate();
        return;
    }
    if (modalDepth_ > 0 || FirstDockIndex() == SIZE_MAX) {
        MessageBeep(MB_ICONWARNING);  // a dock menu / dialog is open (its dock must stay)
        return;
    }
    // One preview at a time: an open chooser goes (its Apply would be a second template), so does
    // customize mode (its edit bar and drawer would edit the dock under Quick setup's rows).
    if (presets_ && presets_->Hwnd()) DestroyWindow(presets_->Hwnd());
    if (editDock_) LeaveEdit();
    // A template previewed elsewhere is kept (it was on screen; Quick setup's Cancel must not take it too).
    if (previewing_) KeepBar();
    ShowTip(-1);
    if (firstRun) {
        // The first start's dock is the Basic layout with the seeded pins, exactly what the welcome
        // dialog's Skip gave; it is where Cancel comes back to (the preview starts after it), and not
        // a change of its own: the timeline starts here (the save is flushed by StartPreview).
        firstRun_ = false;
        const size_t basic = Presets::IndexOf(L"basic");
        if (basic != SIZE_MAX && !previewing_ && ApplyPreset(basic, FirstDockIndex(), true, false, nullptr, false)) {
            pendingLabel_.clear();
            lastState_ = SnapshotText();
            lastPlacement_ = PlacementOf(config_);
        }
    }
    QuickSetupHost host;
    host.config = &config_;
    host.change = [this](QuickSetupChange& c) { return ApplySetupChange(c); };
    host.finish = [this](bool start, bool hide, bool tips) { return FinishQuickSetup(start, hide, tips); };
    host.cancel = [this] { return CancelQuickSetup(); };
    host.destroyed = [this] {
        if (hwnd_) PostMessageW(hwnd_, kQuickSetupClosed, 0, 0);
    };
    quickSetup_ = std::make_unique<QuickSetup>(std::move(host), firstRun, AutostartOn());
    if (!quickSetup_->Create(instance_, hwnd_)) {
        quickSetup_.reset();
        MessageBeep(MB_ICONWARNING);
        return;
    }
    // Everything from here is a Keep / Cancel preview: nothing is saved until Finish.
    EnsureSetupPreview();
    UpdateTimelineUi();
}

void App::EnsureSetupPreview() {
    const size_t first = FirstDockIndex();
    StartPreview(kSetupLabel, kSetupText, quickSetup_ ? quickSetup_->Hwnd() : nullptr, first == SIZE_MAX ? 0 : first);
}

bool App::RestoreSetupDock(size_t cfg) {
    // The preview's snapshot is the config as Quick setup found it (or as a restarted preview did).
    Config before;
    if (cfg >= config_.docks.size() || previewBefore_.empty() || before.LoadJson(previewBefore_) != Config::LoadResult::Ok) return false;
    const auto it = std::find_if(before.docks.begin(), before.docks.end(), [&](const DockConfig& d) { return d.id == config_.docks[cfg].id; });
    if (it == before.docks.end() || !it->IsDock()) return false;  // a dock added here: nothing to go back to
    config_.docks[cfg] = *it;
    if (config_.docks[cfg].Has(ItemKind::Tray)) config_.tray.enabled = true;  // as ApplyLayoutTo
    config_.Sanitize();
    config_.ResolveDockThemes();
    SyncDocks();
    docks_.ForConfig(cfg, [](DockWindow& w) {
        w.ApplyConfig();
        w.ReloadPins();
    });
    RequestSave();
    RefreshSettings();
    FlashDock(cfg);
    return true;
}

bool App::ApplySetupChange(QuickSetupChange& c) {
    if (shutDown_ || !QuickSetupOpen()) return false;
    if (modalDepth_ > 0) return false;  // a dock menu / dialog is open: its dock must stay
    EnsureSetupPreview();
    const HWND host = quickSetup_->Hwnd();
    const size_t cfg = c.dock.empty() ? SIZE_MAX : DockIndexOf(c.dock);
    const bool dock = cfg < config_.docks.size() && config_.docks[cfg].IsDock();
    bool done = false;
    using Kind = QuickSetupChange::Kind;
    switch (c.kind) {
        case Kind::Layout:
            if (!dock) break;
            done = c.value < 0 ? RestoreSetupDock(cfg) : ApplyPreset(static_cast<size_t>(c.value), cfg, true, true, host, false);
            break;
        case Kind::Rename:
            done = dock && RenameDock(cfg, c.text);
            break;
        case Kind::Display:
            if (!dock) break;
            config_.docks[cfg].monitor = c.value;
            OnSettingsChanged(SettingsScope::Appearance, cfg, nullptr);  // Sync makes the windows of the new display
            done = true;
            break;
        case Kind::Edge:
            if (!dock || c.value < 0 || c.value > 3) break;
            config_.docks[cfg].edge = static_cast<DockEdge>(c.value);
            OnSettingsChanged(SettingsScope::Appearance, cfg, nullptr);
            done = true;
            break;
        case Kind::AddDock: {
            // A Basic dock (without the first dock's pins: they stay there) on the next free edge of
            // the primary display, so it does not sit on top of the first one.
            const size_t basic = Presets::IndexOf(L"basic");
            const size_t made = basic == SIZE_MAX ? SIZE_MAX : AddPresetDock(basic, false, true, host);
            if (made == SIZE_MAX) break;
            DockConfig& d = config_.docks[made];
            bool used[4] = {};
            for (size_t i = 0; i < config_.docks.size(); ++i) {
                const DockConfig& o = config_.docks[i];
                if (i == made || !o.IsDock() || !o.enabled || o.Floating() || o.shape != DockShape::Bar) continue;
                const int m = o.monitor == DockConfig::kMonitorPrimary ? 0 : o.monitor;
                if (m == 0 || o.Each()) used[static_cast<int>(o.edge) & 3] = true;
            }
            const DockEdge order[] = {DockEdge::Bottom, DockEdge::Top, DockEdge::Left, DockEdge::Right};
            const auto freeEdge = std::find_if(std::begin(order), std::end(order), [&](DockEdge e) { return !used[static_cast<int>(e)]; });
            if (freeEdge != std::end(order) && *freeEdge != d.edge && !d.Floating()) {
                d.edge = *freeEdge;
                OnSettingsChanged(SettingsScope::Appearance, made, nullptr);
            }
            c.text = config_.docks[made].id;
            done = true;
            break;
        }
        case Kind::RemoveDock:
            done = dock && EditDocks(SettingsDockEdit::Remove, cfg);
            break;
        case Kind::SameLook: {
            std::vector<size_t> list;
            for (const std::wstring& id : c.docks) {
                const size_t i = DockIndexOf(id);
                if (i < config_.docks.size() && config_.docks[i].IsDock()) list.push_back(i);
            }
            if (list.empty()) break;
            if (c.value) {
                // The first dock's look becomes the app look ("Same as the first dock"), the docks use it.
                const DockConfig& first = config_.docks[list.front()];
                if (first.OwnLook()) {
                    config_.themePreset = first.themePreset;
                    config_.themeOverrides = first.themeOverrides;
                    config_.theme = first.theme;
                }
                for (size_t i : list) Config::UseAppLook(config_.docks[i]);
                config_.ResolveTheme();
            } else {
                // Each its own look, starting from the app's (nothing changes on screen yet).
                for (size_t i : list) {
                    if (!config_.docks[i].OwnLook()) config_.GiveOwnLook(config_.docks[i]);
                }
                config_.ResolveDockThemes();
            }
            OnSettingsChanged(SettingsScope::Appearance, list.front(), nullptr);
            done = true;
            break;
        }
        case Kind::Look:
            if (!c.dock.empty() && !dock) break;
            done = ApplyLook(static_cast<size_t>(std::max(0, c.value)), dock ? cfg : SIZE_MAX, true, host);
            break;
        case Kind::Quick: {
            if (c.quick < 0 || c.quick >= static_cast<int>(Theme::Quick::Count) || (!c.dock.empty() && !dock)) break;
            DockConfig* d = dock ? &config_.docks[cfg] : nullptr;
            if (d && !d->OwnLook()) config_.GiveOwnLook(*d);
            ThemeConfig& t = d ? d->theme : config_.theme;
            uint64_t& mask = d ? d->themeOverrides : config_.themeOverrides;
            const ThemeConfig preset = config_.PresetOf(d);
            if (Theme::SetDerived(static_cast<Theme::Quick>(c.quick), t, mask, preset, c.value)) {
                if (d) {
                    config_.ResolveDockThemes();
                } else {
                    config_.ResolveTheme();
                }
                OnSettingsChanged(SettingsScope::Appearance, d ? cfg : 0, nullptr);
            }
            done = true;
            break;
        }
        case Kind::AutoHide: {
            const size_t first = FirstDockIndex();
            if (first == SIZE_MAX || c.value < 0 || c.value > 2) break;
            SetAutoHide(first, static_cast<AutoHideMode>(c.value));
            RefreshSettings();
            done = true;
            break;
        }
        case Kind::Animation: {
            // Full = speed 1 with Reduce motion following Windows; Reduced = Reduce motion on (an
            // animation speed of 0 becomes 1, or it would still be off); Off = speed 0.
            AnimationConfig& a = config_.animation;
            if (c.value == 0) {
                a.speed = 1.0;
                a.reduceMotion = ReduceMotion::System;
            } else if (c.value == 1) {
                if (a.speed == 0.0) a.speed = 1.0;
                a.reduceMotion = ReduceMotion::On;
            } else {
                a.speed = 0.0;
            }
            OnSettingsChanged(SettingsScope::Appearance, 0, nullptr);
            done = true;
            break;
        }
    }
    // A layout or look applied named the preview after itself: it is Quick setup's again.
    EnsureSetupPreview();
    if (quickSetup_) quickSetup_->Refresh();
    return done;
}

bool App::FinishQuickSetup(bool startWithWindows, bool hideTaskbar, bool showTips) {
    if (shutDown_) return false;
    EnsureSetupPreview();
    // Held until now (they touch Explorer and the registry); part of the one entry.
    if (hideTaskbar != config_.hideTaskbar) {
        config_.hideTaskbar = hideTaskbar;
        SetTaskbarEnabled(hideTaskbar);
        RequestSave();
    }
    if (startWithWindows != AutostartOn()) {
        if (SetAutostartOn(startWithWindows)) {
            config_.startWithWindows = startWithWindows;
        } else {
            MessageBeep(MB_ICONWARNING);
        }
        RequestSave();
    } else if (config_.startWithWindows != startWithWindows) {
        config_.startWithWindows = startWithWindows;  // the file follows the Run key
        RequestSave();
    }
    // Keep: everything since Quick setup opened is one undo entry "Quick setup", saved now.
    previewLabel_ = kSetupLabel;
    previewHost_ = nullptr;
    KeepBar();
    RefreshSettings();
    if (showTips) StartTips(false);
    return true;
}

bool App::CancelQuickSetup() {
    if (!previewing_) return true;  // ended elsewhere (an Undo, a Keep): nothing to put back
    previewHost_ = nullptr;
    return CancelBar();
}

bool App::QuickSetupCommand(UINT command, LPARAM lp) {
    const LPARAM arg = lp & 0xFFFF;
    switch (command) {
        case ContextMenu::kQuickSetup:
            OpenQuickSetup(arg == 1);
            return QuickSetupOpen();
        case ContextMenu::kShowTips:
            if (arg == 0xFFFF) {
                ShowTip(-1);
                return true;
            }
            if (arg == 0xFFFE || arg == 0xFFFD) {
                StartTips(arg == 0xFFFD);
                return tipShown_ >= 0;
            }
            if (arg >= SettingsUiConfig::kTipCount) return false;
            ShowTip(static_cast<int>(arg));
            return tipShown_ == static_cast<int>(arg);
        case ContextMenu::kSetupNext:
            return QuickSetupOpen() && quickSetup_->Next();
        case ContextMenu::kSetupFinish:
            return QuickSetupOpen() && quickSetup_->Finish(arg == 1);
        default:
            return false;
    }
}

LRESULT App::QuickSetupState(WPARAM wp, LPARAM lp) {
    const LPARAM arg = lp & 0xFFFF;
    switch (wp) {
        case 317:  // Quick setup's step 0 - 3 (LOWORD 1: its dock rows, 2: the first-run variant 1 / 0); -2 closed
            if (!QuickSetupOpen()) return -2;
            if (arg == 1) return quickSetup_->Rows();
            if (arg == 2) return quickSetup_->FirstRun() ? 1 : 0;
            return quickSetup_->Step();
        case 318:  // the tip shown (0 - 2; -1 none)
            return tipShown_ >= 0 && tips_.Hwnd() ? tipShown_ : -1;
        case 319:  // settings.tipsShown
            return config_.settings.tipsShown;
        case 320:  // Quick setup's held choices: bit 0 Start with Windows, bit 1 Hide the taskbar; -2 closed
            return QuickSetupOpen() ? quickSetup_->HeldChoices() : -2;
        default:
            return -1;
    }
}

// --- the tips --------------------------------------------------------------------------------------

int App::NextUnseenTip(int from) const {
    for (int i = std::max(0, from); i < SettingsUiConfig::kTipCount; ++i) {
        if (!(config_.settings.tipsShown & (1 << i))) return i;
    }
    return -1;
}

void App::MarkTips(int bits) {
    const int now = (config_.settings.tipsShown | bits) & ((1 << SettingsUiConfig::kTipCount) - 1);
    if (now == config_.settings.tipsShown) return;
    config_.settings.tipsShown = now;
    RequestSave();  // UI state: no undo entry (the snapshot leaves it out)
}

void App::ShowTip(int index) {
    if (index < 0 || index >= SettingsUiConfig::kTipCount || shutDown_) {
        tips_.Close();
        tipShown_ = -1;
        return;
    }
    tipShown_ = index;
    PlaceTip();
    if (!tips_.Hwnd()) tipShown_ = -1;
}

void App::StartTips(bool all) {
    // Show the tips again: all three from the first. Show me around: the ones not seen and not
    // done yet (every one done: all of them again, as asked).
    if (all || NextUnseenTip(0) < 0) {
        if (config_.settings.tipsShown != 0) {
            config_.settings.tipsShown = 0;
            RequestSave();
        }
    }
    ShowTip(NextUnseenTip(0));
}

void App::OnTipCommand(int id) {
    if (tipShown_ < 0) return;
    if (id == SettingsIds::kTipSkip) {
        MarkTips((1 << SettingsUiConfig::kTipCount) - 1);  // skipped: none of them again
        ShowTip(-1);
    } else if (id == SettingsIds::kTipNext) {
        const int shown = tipShown_;
        MarkTips(1 << shown);
        ShowTip(NextUnseenTip(shown + 1));
    }
}

void App::NoteTipGesture(int tip) {
    if (tip < 0 || tip >= SettingsUiConfig::kTipCount) return;
    MarkTips(1 << tip);
    if (tipShown_ == tip) ShowTip(NextUnseenTip(tip + 1));  // the tip on screen was just done
}

void App::PlaceTip() {
    if (tipShown_ < 0 || shutDown_) return;
    const size_t first = FirstDockIndex();
    DockWindow* d = first == SIZE_MAX ? nullptr : docks_.FirstOf(first);
    if (!d) d = docks_.First();
    RECT bar{};
    DockEdge edge = DockEdge::Bottom;
    bool floating = false;
    UINT dpi = GetDpiForSystem();
    if (d) {
        bar = d->RestBarRect();
        const size_t cfg = d->ConfigIndex();
        if (cfg < config_.docks.size()) {
            edge = config_.docks[cfg].edge;
            floating = config_.docks[cfg].Floating();
        }
        dpi = d->Dpi();
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(d ? MonitorFromRect(&bar, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    // "Done" when no tip after this one is left to show.
    const bool last = NextUnseenTip(tipShown_ + 1) < 0;
    tips_.Show(instance_, tipShown_, SettingsUiConfig::kTipCount, last, bar, mi.rcWork, edge, floating, dpi);
}
