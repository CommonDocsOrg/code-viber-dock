#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The `perf` provider of the widget API (permission "perf", round 6 phase 6D): one sampler for the
// process, started by the first widget that uses the provider and stopped when the last one goes
// (WidgetRuntime::UsePerf / ReleaseProviders), like AudioService and MediaService. Nothing runs while
// no widget uses it.
//
// A worker thread of its own samples every Interval() ms (250 - 10000; the fastest interval any live
// user asked for, default 1000) and posts each snapshot to the owner window (kMessage); on the UI
// thread the service keeps it, appends it to a ring of the last kHistory samples per metric key (so
// widgets draw graphs without keeping history themselves, and scripts never touch the worker), and
// calls the changed callback (the runtime's users hear perf.onChange).
//
// Sources (all read on the worker): CPU total / per core (NtQuerySystemInformation
// SystemProcessorPerformanceInformation deltas), the CPU's effective frequency (PDH Processor
// Information "% Processor Performance" x "Processor Frequency", else CallNtPowerInformation),
// processes / threads / commit (GetPerformanceInfo), memory (GlobalMemoryStatusEx), GPU engines and
// dedicated memory (PDH "GPU Engine" / "GPU Adapter Memory", DXGI for the total), disks (PDH
// PhysicalDisk), network (GetIfTable2 deltas over the hardware interfaces that are up), battery
// (GetSystemPowerStatus), uptime, and HWiNFO's sensors through its shared memory
// (Global\HWiNFO_SENS_SM2, nothing linked; re-opened every 5 s while absent). PDH counters use
// English names (PdhAddEnglishCounter), so the locale does not matter; a source that fails leaves its
// keys out (perf.metrics() does not list them) rather than reporting zeros.
class PerfService {
public:
    static constexpr UINT kMessage = WM_APP + 37;  // wParam: generation, lParam: a Sample* (owned by the receiver)
    static constexpr uint32_t kMinInterval = 250, kMaxInterval = 10000, kDefaultInterval = 1000;
    static constexpr size_t kHistory = 600;      // samples kept per key (600 doubles each)
    static constexpr size_t kMaxCores = 64;      // cpu.core.N keys
    static constexpr size_t kMaxInterfaces = 8;  // network.<alias>.* keys
    static constexpr size_t kMaxDisks = 16;      // disk.N.* keys
    static constexpr size_t kMaxHwinfo = 512;    // hwinfo.* readings
    static constexpr size_t kMaxRings = 1024;    // distinct keys with a history (a bound on memory)

    enum class Kind : uint8_t { Percent, Bytes, BytesPerSecond, Temperature, Voltage, Fan, Current, Power, Clock, Number, Bool, Seconds };
    static const char* KindName(Kind k);

    struct Metric {
        std::string key;     // "cpu.total", "hwinfo.3.16777216"
        std::wstring name;   // "CPU total", "CPU [#0]: AMD Ryzen 7 · Core Temperatures"
        std::wstring unit;   // "%", "B", "B/s", "°C", "MHz", ""
        std::wstring group;  // "CPU", "Memory", "GPU", "Disk", "Network", "Battery", "System"; HWiNFO: the sensor's name
        Kind kind = Kind::Number;
        double min = 0.0, max = 100.0;  // a sensible range for gauges
        bool hwinfo = false;
        bool widen = false;  // the range follows the value (counts, HWiNFO readings): a value outside it widens it
    };
    struct Catalog {
        uint64_t serial = 0;  // changes whenever the key set or a range changes
        std::vector<Metric> metrics;
    };
    // HWiNFO's shared memory as last seen.
    struct HwInfo {
        bool available = false;
        std::wstring reason;  // why not: "not running or Shared Memory Support is off", "stale ..."
        uint32_t version = 0, revision = 0;
        int64_t pollTime = 0;  // ms since 1970 (UTC), 0 none
        std::vector<std::pair<uint32_t, std::wstring>> sensors;  // index, name
        uint32_t readings = 0;
        bool operator==(const HwInfo&) const = default;
    };
    struct Sample {
        std::shared_ptr<const Catalog> catalog;  // the same object while the key set stays
        std::vector<double> values;              // parallel to catalog->metrics
        double time = 0.0;                       // ms since 1970 (UTC)
        std::shared_ptr<const HwInfo> hwinfo;    // null: unchanged since the last sample
    };

    PerfService() = default;
    PerfService(const PerfService&) = delete;
    PerfService& operator=(const PerfService&) = delete;
    ~PerfService() { Stop(); }

    // Starts the worker; the first snapshot follows within half a second.
    bool Start(HWND owner);
    // Stops the worker (waits up to 2 s for it) and drops the history.
    void Stop();
    bool Active() const { return thread_ != nullptr; }
    // The sampling period (clamped to 250 - 10000 ms); the worker re-times at once.
    void SetInterval(uint32_t ms);
    uint32_t Interval() const { return interval_; }

    // kMessage: keeps the snapshot, extends the rings, calls the changed callback. True if handled.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void SetChanged(std::function<void()> changed) { changed_ = std::move(changed); }

    // --- the UI thread's view (valid while Active) ---
    // Snapshots received since the process started (also perf.get().serial; 0: none yet).
    uint64_t Snapshots() const { return snapshots_; }
    uint64_t CatalogSerial() const { return catalog_ ? catalog_->serial : 0; }
    size_t MetricCount() const { return catalog_ ? catalog_->metrics.size() : 0; }
    const std::vector<Metric>* Metrics() const { return catalog_ ? &catalog_->metrics : nullptr; }
    const HwInfo& Hwinfo() const { return hwinfo_; }
    // perf.get()'s object as JSON: { time, interval, serial, metrics: { key: value } }; built once
    // per snapshot.
    const std::string& SnapshotJson();
    // { serial, list: [ { key, name, unit, min, max, kind, group } ] }; built once per catalog.
    const std::string& MetricsJson();
    std::string HwinfoJson() const;
    // Up to `n` newest values of `key`, oldest first (NaN: no value in that sample). Empty when the
    // key has no history.
    void Series(const std::string& key, size_t n, std::vector<double>& out) const;
    size_t RingCount() const { return ringKeys_.size(); }

    // The metrics a sampler sees now, for the settings page when the service is not running: a
    // short synchronous sample (cached for 30 s).
    static std::vector<Metric> ListOnce();
    // The metric list of `catalog` as metrics() JSON (shared with ListOnce users).
    static std::string MetricsJsonOf(const Catalog& catalog);

private:
    struct Shared;  // what the worker and the UI thread share (owner, interval, the wake event)
    static DWORD WINAPI ThreadMain(void* param);
    void TakeSample(std::unique_ptr<Sample> s);

    HANDLE thread_ = nullptr;
    std::shared_ptr<Shared> shared_;
    uint64_t generation_ = 0;
    uint32_t interval_ = kDefaultInterval;
    std::function<void()> changed_;

    uint64_t snapshots_ = 0;
    double time_ = 0.0;
    std::shared_ptr<const Catalog> catalog_;
    std::vector<double> values_;
    HwInfo hwinfo_;
    std::string snapshotJson_;
    uint64_t snapshotJsonFor_ = UINT64_MAX;
    std::string metricsJson_;
    uint64_t metricsJsonFor_ = UINT64_MAX;
    // Rings: one per key ever seen in this run (up to kMaxRings), all advanced together.
    std::unordered_map<std::string, size_t> ringOf_;
    std::vector<std::string> ringKeys_;
    std::vector<std::unique_ptr<double[]>> rings_;
    std::vector<size_t> ringFor_;  // catalog index -> ring (SIZE_MAX none), rebuilt with the catalog
    uint64_t ringCatalog_ = 0;
    size_t head_ = 0;  // samples pushed in this run
};
