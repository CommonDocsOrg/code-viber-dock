// Round 9 phase 9C: the predefined dock setups (see Presets.h).

#include "Presets.h"

#include <algorithm>
#include <cwchar>
#include <iterator>

#include "JsonUtil.h"
#include "Theme.h"
#include "WidgetPackage.h"
#include "resource.h"

namespace Presets {

namespace {

using namespace JsonUtil;

// In list order (DockCommand 153's index). Round 11 phase 11C: "win7" after "windows" (sidebar,
// performance and dashboard moved from 4 - 6 to 5 - 7).
constexpr int kResources[] = {IDR_PRESET_BASIC,   IDR_PRESET_MINIMAL,     IDR_PRESET_MODERN,
                              IDR_PRESET_WINDOWS, IDR_PRESET_WIN7,        IDR_PRESET_SIDEBAR,
                              IDR_PRESET_PERFORMANCE, IDR_PRESET_DASHBOARD};

void Log(int resource, const std::wstring& text) {
    OutputDebugStringW((L"WinDock: preset resource " + std::to_wstring(resource) + L" skipped: " + text + L"\n").c_str());
}

bool ReadResource(int resource, std::string& out) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(resource), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL mem = res ? LoadResource(module, res) : nullptr;
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data) return false;
    out.assign(static_cast<const char*>(data), SizeofResource(module, res));
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool IsMarker(const DockItemEntry& e) { return CompareStringOrdinal(e.kind.c_str(), -1, kPinsKind, -1, TRUE) == CSTR_EQUAL; }

// One preset from its resource; false (with a log line) when it does not validate.
bool LoadOne(int resource, Preset& p) {
    std::string bytes;
    if (!ReadResource(resource, bytes)) {
        Log(resource, L"not found");
        return false;
    }
    try {
        json::JsonObject root{nullptr};
        if (!json::JsonObject::TryParse(Utf8ToWide(bytes), root) || !root) {
            Log(resource, L"malformed JSON");
            return false;
        }
        ReadString(root, L"id", p.id);
        ReadString(root, L"name", p.name);
        ReadString(root, L"tagline", p.tagline);
        ReadString(root, L"description", p.description);
        if (p.id.empty() || p.name.empty()) {
            Log(resource, L"no id or name");
            return false;
        }
        // The dock object with the items in it: one docks[] entry for the config reader.
        json::JsonObject dock = json::JsonObject::Parse(L"{}");
        if (const json::JsonObject d = ReadObject(root, L"dock")) dock = json::JsonObject::Parse(d.Stringify());
        const json::JsonArray items = ReadArray(root, L"items");
        if (!items) {
            Log(resource, L"\"" + p.id + L"\" has no items array");
            return false;
        }
        dock.SetNamedValue(L"items", items);
        DockConfig d;
        if (!ReadDockJson(std::wstring(dock.Stringify()), d)) {
            Log(resource, L"\"" + p.id + L"\": the dock object could not be read");
            return false;
        }
        // Clamped like a loaded config (Sanitize also sorts the items into group order).
        Config tmp;
        tmp.docks = {std::move(d)};
        tmp.Sanitize();
        p.dock = std::move(tmp.docks.front());
        p.dock.id = p.id;
        if (p.dock.items.empty()) {
            Log(resource, L"\"" + p.id + L"\" has no items");
            return false;
        }
        int markers = 0, running = 0;
        for (const DockItemEntry& e : p.dock.items) {
            if (IsMarker(e)) {
                ++markers;
                continue;
            }
            const ItemKind k = e.Kind();
            if (k == ItemKind::Unknown) {
                Log(resource, L"\"" + p.id + L"\": unknown item kind \"" + e.kind + L"\"");
                return false;
            }
            if (k == ItemKind::Running) ++running;
            if (k == ItemKind::Widget) {
                std::wstring error;
                if (!Widgets::Load(e.id, error)) {
                    Log(resource, L"\"" + p.id + L"\": widget \"" + e.id + L"\" is not installed (" + error + L")");
                    return false;
                }
            }
        }
        if (markers > 1 || running > 1) {
            Log(resource, L"\"" + p.id + L"\": more than one pins marker or running item");
            return false;
        }
        if (!p.dock.themePreset.empty() && !Theme::FindBuiltIn(p.dock.themePreset)) {
            Log(resource, L"\"" + p.id + L"\": unknown theme preset \"" + p.dock.themePreset + L"\"");
            return false;
        }
        p.hasPins = markers == 1;
        return true;
    } catch (...) {
        Log(resource, L"read failed");
        return false;
    }
}

std::vector<DockItemEntry> BuildItems(const Preset& p, const Config& config, const std::vector<DockItemEntry>& pins, bool keepPins) {
    std::vector<DockItemEntry> out;
    for (const DockItemEntry& e : p.dock.items) {
        if (IsMarker(e)) {
            if (!keepPins) continue;
            for (DockItemEntry pin : pins) {
                pin.anchor = ItemAnchor::None;  // they stay at the marker
                pin.offset = 0;
                out.push_back(std::move(pin));
            }
            continue;
        }
        DockItemEntry item = e;
        if (item.Kind() == ItemKind::Clock) item.settings = ClockSettingsJson(ClockOf(config, e));
        if (item.Kind() == ItemKind::Weather) item.settings = WeatherSettingsJson(WeatherOf(config, e));
        out.push_back(std::move(item));
    }
    return out;
}

}  // namespace

const std::vector<Preset>& All() {
    static const std::vector<Preset> presets = [] {
        std::vector<Preset> list;
        for (int r : kResources) {
            Preset p;
            if (!LoadOne(r, p)) continue;
            if (std::any_of(list.begin(), list.end(), [&](const Preset& q) { return _wcsicmp(q.id.c_str(), p.id.c_str()) == 0; })) {
                Log(r, L"\"" + p.id + L"\": the id is taken");
                continue;
            }
            list.push_back(std::move(p));
        }
        return list;
    }();
    return presets;
}

size_t IndexOf(const std::wstring& id) {
    const std::vector<Preset>& all = All();
    for (size_t i = 0; i < all.size(); ++i) {
        if (_wcsicmp(all[i].id.c_str(), id.c_str()) == 0) return i;
    }
    return SIZE_MAX;
}

std::vector<std::wstring> ItemNames(const Preset& p, bool keepPins) {
    std::vector<std::wstring> names;
    for (const DockItemEntry& e : p.dock.items) {
        if (IsMarker(e)) {
            if (keepPins) names.push_back(L"Your pinned apps and folders");
            continue;
        }
        switch (e.Kind()) {
            case ItemKind::Running: names.push_back(L"Running apps"); break;
            case ItemKind::Separator: names.push_back(L"Separator"); break;
            case ItemKind::Clock: names.push_back(L"Clock"); break;
            case ItemKind::Weather: names.push_back(L"Weather"); break;
            case ItemKind::Notifications: names.push_back(L"Notifications"); break;
            case ItemKind::Tray: names.push_back(L"System tray"); break;
            case ItemKind::RecycleBin: names.push_back(L"Recycle Bin"); break;
            case ItemKind::Widget: names.push_back(Widgets::DisplayName(e.id)); break;
            default: names.push_back(e.kind); break;
        }
    }
    return names;
}

DockConfig Apply(const Preset& p, const Config& config, const DockConfig& target, const std::vector<DockItemEntry>& pins,
                 bool keepPins) {
    if (!target.IsDock()) return target;
    DockConfig d = p.dock;
    d.id = target.id;
    d.color = target.color;
    d.monitor = target.monitor;
    d.items = BuildItems(p, config, pins, keepPins);
    return d;
}

DockConfig MakeNew(const Preset& p, const Config& config, const std::vector<DockItemEntry>& pins) {
    DockConfig d = p.dock;
    d.id = config.UniqueDockId(p.id);
    d.color = 0;
    d.monitor = DockConfig::kMonitorPrimary;
    d.items = BuildItems(p, config, pins, !pins.empty());
    return d;
}

std::vector<DockItemEntry> PinsOf(const DockConfig& dock) { return dock.IsDock() ? dock.Pins() : std::vector<DockItemEntry>{}; }

}  // namespace Presets
