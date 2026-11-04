// The `perf` provider's sampler (round 6 phase 6D): see PerfService.h.
#include <winsock2.h>
#include <ws2ipdef.h>

#include "PerfService.h"

#include <iphlpapi.h>
#include <netioapi.h>
#include <winternl.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <powerbase.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <string_view>
#include <type_traits>

#include "JsonUtil.h"

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double QpcMs() {
    static const double freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / freq;
}

// ms since 1970 (UTC).
double WallMs() {
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<double>(t - 116444736000000000ULL) / 10000.0;
}

// The smallest of 1, 2, 2.5, 5 x 10^k that is >= v (v > 0).
double Nice(double v) {
    if (!(v > 0.0) || !std::isfinite(v)) return 1.0;
    const double p = std::pow(10.0, std::floor(std::log10(v)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        if (m * p >= v * (1.0 - 1e-12)) return m * p;
    }
    return 10.0 * p;
}

std::wstring Widen(const char* s, size_t max, bool utf8) {
    const size_t n = strnlen(s, max);
    if (!n) return {};
    const UINT cp = utf8 ? CP_UTF8 : CP_ACP;
    const int len = MultiByteToWideChar(cp, utf8 ? MB_ERR_INVALID_CHARS : 0, s, static_cast<int>(n), nullptr, 0);
    if (len <= 0) return utf8 ? Widen(s, max, false) : std::wstring();
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(cp, utf8 ? MB_ERR_INVALID_CHARS : 0, s, static_cast<int>(n), w.data(), len);
    return w;
}

std::string Narrow(std::wstring_view s) { return JsonUtil::WideToUtf8(std::wstring(s)); }

// A key part from a display name: lower case a-z 0-9, runs of anything else as one '-'.
std::string Slug(std::wstring_view s) {
    std::string out;
    for (wchar_t c : s) {
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) {
            out += static_cast<char>(c);
        } else if (c >= L'A' && c <= L'Z') {
            out += static_cast<char>(c - L'A' + L'a');
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.size() > 32) out.resize(32);
    return out.empty() ? std::string("interface") : out;
}

using Metric = PerfService::Metric;
using Kind = PerfService::Kind;
using Catalog = PerfService::Catalog;

Metric Make(std::wstring name, std::wstring unit, std::wstring group, Kind kind, double min, double max, bool widen = false) {
    Metric m;
    m.name = std::move(name);
    m.unit = std::move(unit);
    m.group = std::move(group);
    m.kind = kind;
    m.min = min;
    m.max = max;
    m.widen = widen;
    return m;
}

// Catalog serials are unique in the process (a restarted service never repeats one).
std::atomic<uint64_t> g_catalogSerial{0};
// Samples posted and not yet received (the worker skips one while too many wait).
std::atomic<int> g_inFlight{0};

// Builds a sample's catalog: keys added in the same order as last time reuse the last catalog
// (no strings built); a new key, a missing one or a value outside a widening range makes a new one.
class Builder {
public:
    std::vector<double> values;

    void Begin() {
        values.clear();
        next_.clear();
        pos_ = 0;
        changed_ = false;
    }

    // A borrowed callable that makes a key's Metric (called only when the catalog changes): one
    // non-template Add instead of one instantiation per call site.
    class MetaRef {
    public:
        template <class F>
        MetaRef(F&& f) : obj_(&f), call_([](void* o) { return (*static_cast<std::remove_reference_t<F>*>(o))(); }) {}
        Metric operator()() const { return call_(obj_); }

    private:
        void* obj_;
        Metric (*call_)(void*);
    };

    void Add(std::string_view key, double value, MetaRef meta) {
        if (!std::isfinite(value)) return;  // absent this time
        const Catalog* cur = current_.get();
        const Metric* old = nullptr;
        const bool atPos = cur && pos_ < cur->metrics.size() && cur->metrics[pos_].key == key;
        if (atPos) {
            old = &cur->metrics[pos_];
        } else if (cur) {
            if (indexed_ != cur) {
                index_.clear();  // a partial index (an exception while building it); its keys are cur's
                for (size_t i = 0; i < cur->metrics.size(); ++i) index_.emplace(cur->metrics[i].key, i);
                indexed_ = cur;
            }
            if (auto it = index_.find(key); it != index_.end()) old = &cur->metrics[it->second];
        }
        const bool reuse = old && (!old->widen || (value >= old->min && value <= old->max));
        if (!changed_ && reuse && atPos) {
            values.push_back(value);
            ++pos_;
            return;
        }
        if (!changed_) {
            changed_ = true;
            if (cur) next_.assign(cur->metrics.begin(), cur->metrics.begin() + static_cast<ptrdiff_t>(pos_));
        }
        if (reuse) {
            next_.push_back(*old);
        } else {
            Metric m = meta();
            m.key = std::string(key);
            if (m.widen) {
                m.min = std::min(m.min, value);
                m.max = std::max(m.max, value);
            }
            next_.push_back(std::move(m));
        }
        values.push_back(value);
        ++pos_;
    }

    // The catalog of this sample (the same pointer as before when nothing changed).
    std::shared_ptr<const Catalog> Finish() {
        const Catalog* cur = current_.get();
        if (!changed_ && cur && pos_ == cur->metrics.size()) return current_;
        if (!changed_ && cur) next_.assign(cur->metrics.begin(), cur->metrics.begin() + static_cast<ptrdiff_t>(pos_));
        auto c = std::make_shared<Catalog>();
        c->serial = ++g_catalogSerial;
        c->metrics = std::move(next_);
        next_.clear();
        // The index's keys view the old catalog's strings, which die with it (the UI thread may drop
        // the last reference at any time after this): clear it while they are still alive. MSVC's
        // clear() hashes every key when the table is sparse (size < buckets / 8), so clearing views
        // of freed strings later picks the wrong buckets and leaves them pointing at freed nodes.
        index_.clear();
        indexed_ = nullptr;
        current_ = std::move(c);
        return current_;
    }

private:
    // The catalog of the last sample; the only one index_ ever views (it keeps it alive).
    std::shared_ptr<const Catalog> current_;
    std::vector<Metric> next_;
    // key -> position in current_->metrics, built on the first key out of order since current_
    // changed. Declared after current_, so destroyed before it.
    std::unordered_map<std::string_view, size_t> index_;
    const Catalog* indexed_ = nullptr;  // the catalog index_ was completed for (null: none)
    size_t pos_ = 0;
    bool changed_ = false;
};

// --- HWiNFO's shared memory (HWiNFO SDK, "hwisenssm2.h"; packed, stable since v6) ---------------
#pragma pack(push, 1)
struct HwHeader {
    DWORD signature;  // 'HWiS' (0x53695748); 'DEAD' (0x44414544): sharing stopped
    DWORD version;
    DWORD revision;
    __time64_t pollTime;
    DWORD sensorOffset;
    DWORD sensorSize;
    DWORD sensorCount;
    DWORD readingOffset;
    DWORD readingSize;
    DWORD readingCount;
};
struct HwSensor {
    DWORD id;
    DWORD instance;
    char nameOrig[128];
    char nameUser[128];
    // version 2+: char utfNameUser[128] (UTF-8)
};
struct HwReading {
    DWORD type;  // 0 none, 1 temp, 2 volt, 3 fan, 4 current, 5 power, 6 clock, 7 usage, 8 other
    DWORD sensorIndex;
    DWORD readingId;
    char labelOrig[128];
    char labelUser[128];
    char unit[16];
    double value;
    double valueMin;
    double valueMax;
    double valueAvg;
    // version 2+: char utfLabelUser[128], char utfUnit[16] (UTF-8)
};
#pragma pack(pop)
constexpr DWORD kHwSignature = 0x53695748;
constexpr DWORD kHwDead = 0x44414544;
constexpr int64_t kHwStaleSeconds = 10;
constexpr double kHwRetryMs = 5000.0;

// A gauge range for a HWiNFO reading from its own min / max (rounded outwards).
void HwRange(Kind kind, double lo, double hi, double& outLo, double& outHi) {
    if (!std::isfinite(lo)) lo = 0.0;
    if (!std::isfinite(hi)) hi = lo;
    switch (kind) {
        case Kind::Percent:
            outLo = std::min(0.0, lo);
            outHi = hi > 100.0 ? Nice(hi) : 100.0;
            return;
        case Kind::Temperature:
            outLo = lo < 0.0 ? -Nice(-lo) : 0.0;
            outHi = std::max(100.0, std::ceil(hi * 1.1 / 10.0) * 10.0);
            return;
        case Kind::Fan:
            outLo = 0.0;
            outHi = std::max(1000.0, std::ceil(hi * 1.25 / 500.0) * 500.0);
            return;
        case Kind::Bool:
            outLo = 0.0;
            outHi = 1.0;
            return;
        default:
            outLo = lo < 0.0 ? -Nice(-lo * 1.2) : 0.0;
            outHi = hi > 0.0 ? Nice(hi * 1.2) : 1.0;
            return;
    }
}

class HwReader {
public:
    ~HwReader() { Close(); }

    // Adds the readings (up to kMaxHwinfo) and fills `info`. `force`: rebuild the sensor names.
    void Read(Builder& b, PerfService::HwInfo& info, double now) {
        if (!view_ && now < nextTry_) return;
        if (!view_ && !Open()) {
            nextTry_ = now + kHwRetryMs;
            Absent(info, L"HWiNFO is not running, or its Shared Memory Support is off");
            return;
        }
        HwHeader h{};
        if (size_ < sizeof(h)) return Fail(info, now, L"HWiNFO's shared memory is too small");
        // HWiNFO's own lock (if it exists) keeps a half-written update out; never wait long.
        const bool locked = mutex_ && WaitForSingleObject(mutex_, 20) == WAIT_OBJECT_0;
        memcpy(&h, view_, sizeof(h));
        if (h.signature == kHwDead) {
            if (locked) ReleaseMutex(mutex_);
            return Fail(info, now, L"HWiNFO stopped sharing its sensors");
        }
        if (h.signature != kHwSignature) {
            if (locked) ReleaseMutex(mutex_);
            return Fail(info, now, L"HWiNFO's shared memory has an unknown format");
        }
        const int64_t age = static_cast<int64_t>(_time64(nullptr)) - static_cast<int64_t>(h.pollTime);
        if (age > kHwStaleSeconds) {
            if (locked) ReleaseMutex(mutex_);
            return Fail(info, now,
                        L"HWiNFO's sensor data is " + std::to_wstring(age) +
                            L" s old (HWiNFO was closed, or the free version stopped sharing after 12 hours: restart it)");
        }
        const uint64_t sensorEnd = static_cast<uint64_t>(h.sensorOffset) + static_cast<uint64_t>(h.sensorSize) * h.sensorCount;
        const uint64_t readingEnd = static_cast<uint64_t>(h.readingOffset) + static_cast<uint64_t>(h.readingSize) * h.readingCount;
        if (h.sensorSize < sizeof(HwSensor) || h.readingSize < sizeof(HwReading) || sensorEnd > size_ || readingEnd > size_) {
            if (locked) ReleaseMutex(mutex_);
            return Fail(info, now, L"HWiNFO's shared memory has an unexpected layout");
        }
        const bool utf = h.version >= 2;
        const bool utfSensor = utf && h.sensorSize >= sizeof(HwSensor) + 128;
        const bool utfReading = utf && h.readingSize >= sizeof(HwReading) + 128 + 16;
        // The sensor names: when the sensor list changed, and every 5 s with the poll time.
        const uint32_t sensorCount = std::min<uint32_t>(h.sensorCount, 1024);
        const bool rebuild = names_.size() != sensorCount || now >= nextInfo_ || !info.available ||
                             info.version != h.version || info.revision != h.revision;
        if (rebuild) {
            names_.clear();
            for (uint32_t i = 0; i < sensorCount; ++i) {
                const BYTE* p = view_ + h.sensorOffset + static_cast<size_t>(i) * h.sensorSize;
                HwSensor s{};
                memcpy(&s, p, sizeof(s));
                std::wstring name;
                if (utfSensor) name = Widen(reinterpret_cast<const char*>(p + sizeof(HwSensor)), 128, true);
                if (name.empty()) name = Widen(s.nameUser, sizeof(s.nameUser), false);
                if (name.empty()) name = Widen(s.nameOrig, sizeof(s.nameOrig), false);
                names_.push_back(std::move(name));
            }
        }
        const uint32_t count = std::min<uint32_t>(h.readingCount, static_cast<uint32_t>(PerfService::kMaxHwinfo));
        char key[48];
        for (uint32_t i = 0; i < count; ++i) {
            const BYTE* p = view_ + h.readingOffset + static_cast<size_t>(i) * h.readingSize;
            HwReading r{};
            memcpy(&r, p, sizeof(r));
            if (r.type == 0) continue;
            snprintf(key, sizeof(key), "hwinfo.%lu.%lu", static_cast<unsigned long>(r.sensorIndex),
                     static_cast<unsigned long>(r.readingId));
            b.Add(key, r.value, [&] {
                std::wstring label, unit;
                if (utfReading) {
                    label = Widen(reinterpret_cast<const char*>(p + sizeof(HwReading)), 128, true);
                    unit = Widen(reinterpret_cast<const char*>(p + sizeof(HwReading) + 128), 16, true);
                }
                if (label.empty()) label = Widen(r.labelUser, sizeof(r.labelUser), false);
                if (label.empty()) label = Widen(r.labelOrig, sizeof(r.labelOrig), false);
                if (unit.empty()) unit = Widen(r.unit, sizeof(r.unit), false);
                const std::wstring sensor = r.sensorIndex < names_.size() ? names_[r.sensorIndex] : L"Sensor " + std::to_wstring(r.sensorIndex);
                Kind kind = Kind::Number;
                switch (r.type) {
                    case 1: kind = Kind::Temperature; break;
                    case 2: kind = Kind::Voltage; break;
                    case 3: kind = Kind::Fan; break;
                    case 4: kind = Kind::Current; break;
                    case 5: kind = Kind::Power; break;
                    case 6: kind = Kind::Clock; break;
                    case 7: kind = Kind::Percent; break;
                    default: kind = unit == L"Yes/No" ? Kind::Bool : Kind::Number; break;
                }
                double lo = 0.0, hi = 100.0;
                HwRange(kind, std::min(r.valueMin, r.value), std::max(r.valueMax, r.value), lo, hi);
                Metric m = Make(sensor + L" \x00B7 " + label, unit, sensor, kind, lo, hi, true);
                m.hwinfo = true;
                return m;
            });
        }
        if (locked) ReleaseMutex(mutex_);
        if (rebuild) {
            nextInfo_ = now + 5000.0;
            info.available = true;
            info.reason.clear();
            info.version = h.version;
            info.revision = h.revision;
            info.pollTime = static_cast<int64_t>(h.pollTime) * 1000;
            info.readings = h.readingCount;
            info.sensors.clear();
            for (uint32_t i = 0; i < names_.size(); ++i) info.sensors.emplace_back(i, names_[i]);
        }
    }

private:
    bool Open() {
        map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM2");
        if (!map_) map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, L"HWiNFO_SENS_SM2");
        if (!map_) return false;
        view_ = static_cast<const BYTE*>(MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0));
        MEMORY_BASIC_INFORMATION mbi{};
        if (!view_ || !VirtualQuery(view_, &mbi, sizeof(mbi))) {
            Close();
            return false;
        }
        size_ = mbi.RegionSize;
        mutex_ = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\HWiNFO_SM2_MUTEX");
        names_.clear();
        nextInfo_ = 0.0;
        return true;
    }
    void Close() {
        if (view_) UnmapViewOfFile(view_);
        if (map_) CloseHandle(map_);
        if (mutex_) CloseHandle(mutex_);
        view_ = nullptr;
        map_ = nullptr;
        mutex_ = nullptr;
        size_ = 0;
        names_.clear();
    }
    void Absent(PerfService::HwInfo& info, std::wstring reason) {
        info = PerfService::HwInfo{};
        info.reason = std::move(reason);
    }
    void Fail(PerfService::HwInfo& info, double now, std::wstring reason) {
        Close();
        nextTry_ = now + kHwRetryMs;
        Absent(info, std::move(reason));
    }

    HANDLE map_ = nullptr;
    HANDLE mutex_ = nullptr;
    const BYTE* view_ = nullptr;
    size_t size_ = 0;
    double nextTry_ = 0.0;
    double nextInfo_ = 0.0;
    std::vector<std::wstring> names_;
};

// --- the sampler (one per worker thread; ListOnce runs one on the caller's thread) --------------

using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

// CallNtPowerInformation(ProcessorInformation)'s element (documented, not in the SDK headers).
struct ProcessorPowerInfo {
    ULONG number, maxMhz, currentMhz, mhzLimit, maxIdleState, currentIdleState;
};

struct EngineTypes {
    const wchar_t* prefix;
    const char* key;
    const wchar_t* name;
};
constexpr EngineTypes kEngines[] = {
    {L"3D", "gpu.engine.3D", L"GPU 3D engine"},
    {L"Copy", "gpu.engine.Copy", L"GPU copy engine"},
    {L"VideoDecode", "gpu.engine.VideoDecode", L"GPU video decode"},
    {L"VideoEncode", "gpu.engine.VideoEncode", L"GPU video encode"},
    {L"Compute", "gpu.engine.Compute", L"GPU compute"},
};

class Sampler {
public:
    explicit Sampler(bool listing = false) : listing_(listing) {}
    ~Sampler() {
        if (query_) PdhCloseQuery(query_);
    }

    void Open() {
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            ntqsi_ = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
        }
        if (PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS) {
            auto add = [&](const wchar_t* path) {
                PDH_HCOUNTER c = nullptr;
                return PdhAddEnglishCounterW(query_, path, 0, &c) == ERROR_SUCCESS ? c : nullptr;
            };
            cpuPerf_ = add(L"\\Processor Information(_Total)\\% Processor Performance");
            cpuFreq_ = add(L"\\Processor Information(_Total)\\Processor Frequency");
            gpuEngine_ = add(L"\\GPU Engine(*)\\Utilization Percentage");
            gpuMemory_ = add(L"\\GPU Adapter Memory(*)\\Dedicated Usage");
            diskTime_ = add(L"\\PhysicalDisk(*)\\% Disk Time");
            diskRead_ = add(L"\\PhysicalDisk(*)\\Disk Read Bytes/sec");
            diskWrite_ = add(L"\\PhysicalDisk(*)\\Disk Write Bytes/sec");
        } else {
            query_ = nullptr;
        }
        // The first hardware adapter's dedicated memory (read once).
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d{};
                const bool ok = SUCCEEDED(adapter->GetDesc1(&d));
                adapter->Release();
                if (ok && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > 0) {
                    gpuMemoryTotal_ = static_cast<double>(d.DedicatedVideoMemory);
                    break;
                }
            }
            factory->Release();
        }
    }

    // The first readings (the deltas and the PDH rates need a previous sample).
    void Prime() {
        if (query_) PdhCollectQueryData(query_);
        ReadCpu(nullptr);
        ReadNet(nullptr, 0.0);
        last_ = QpcMs();
    }

    void Sample(Builder& b, PerfService::HwInfo& hw) {
        const double now = QpcMs();
        const double dt = (now - last_) / 1000.0;
        last_ = now;
        b.Begin();
        const bool pdh = query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS;
        ReadCpu(&b);
        ReadCpuFrequency(b, pdh);
        ReadMemory(b);  // processes and threads first, then memory

        if (pdh) ReadGpu(b);
        if (pdh) ReadDisks(b);
        ReadNet(&b, dt);
        ReadPower(b);
        hwinfo_.Read(b, hw, now);
    }

private:
    // --- CPU ---
    void ReadCpu(Builder* b) {
        if (!ntqsi_) return;
        cur_.resize(256);
        ULONG len = 0;
        if (ntqsi_(SystemProcessorPerformanceInformation, cur_.data(),
                   static_cast<ULONG>(cur_.size() * sizeof(cur_[0])), &len) < 0) {
            return;
        }
        cur_.resize(len / sizeof(cur_[0]));
        if (b && prev_.size() == cur_.size()) {
            double idleAll = 0.0, totalAll = 0.0;
            const size_t n = cur_.size();
            cores_.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const double idle = static_cast<double>(cur_[i].IdleTime.QuadPart - prev_[i].IdleTime.QuadPart);
                const double total = static_cast<double>(cur_[i].KernelTime.QuadPart - prev_[i].KernelTime.QuadPart +
                                                         cur_[i].UserTime.QuadPart - prev_[i].UserTime.QuadPart);
                idleAll += idle;
                totalAll += total;
                cores_[i] = total > 0.0 ? std::clamp(100.0 * (1.0 - idle / total), 0.0, 100.0) : (listing_ ? 0.0 : kNaN);
            }
            const double cpu = totalAll > 0.0 ? std::clamp(100.0 * (1.0 - idleAll / totalAll), 0.0, 100.0) : (listing_ ? 0.0 : kNaN);
            b->Add("cpu.total", cpu, [] { return Make(L"CPU total", L"%", L"CPU", Kind::Percent, 0, 100); });
            coreCount_ = std::min(n, PerfService::kMaxCores);  // added after the processes (AddCores)
        }
        prev_.swap(cur_);
    }

    // The per-core keys, after the CPU's other keys (so lists show the totals first).
    void AddCores(Builder& b) {
        char key[32];
        for (size_t i = 0; i < coreCount_ && i < cores_.size(); ++i) {
            snprintf(key, sizeof(key), "cpu.core.%zu", i);
            b.Add(key, cores_[i], [i] { return Make(L"CPU core " + std::to_wstring(i), L"%", L"CPU", Kind::Percent, 0, 100); });
        }
        coreCount_ = 0;
    }

    void ReadCpuFrequency(Builder& b, bool pdh) {
        double mhz = kNaN, base = kNaN;
        PDH_FMT_COUNTERVALUE perf{}, freq{};
        if (pdh && cpuPerf_ && cpuFreq_ &&
            PdhGetFormattedCounterValue(cpuPerf_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &perf) == ERROR_SUCCESS &&
            PdhGetFormattedCounterValue(cpuFreq_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &freq) == ERROR_SUCCESS &&
            perf.CStatus == PDH_CSTATUS_VALID_DATA && freq.CStatus == PDH_CSTATUS_VALID_DATA && freq.doubleValue > 0) {
            base = freq.doubleValue;
            mhz = base * perf.doubleValue / 100.0;
        } else if (!cpuPerf_ || !cpuFreq_ || listing_) {
            // CallNtPowerInformation: what Windows reports (often the base clock).
            ProcessorPowerInfo info[256]{};
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            const ULONG n = std::min<ULONG>(si.dwNumberOfProcessors, 256);
            if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, info, n * sizeof(ProcessorPowerInfo)) == 0 && n) {
                double sum = 0.0, maxMhz = 0.0;
                for (ULONG i = 0; i < n; ++i) {
                    sum += info[i].currentMhz;
                    maxMhz = std::max(maxMhz, static_cast<double>(info[i].maxMhz));
                }
                mhz = sum / n;
                base = maxMhz;
            }
        }
        if (std::isfinite(base) && base > 0) cpuBase_ = base;
        b.Add("cpu.frequency", mhz, [&] {
            return Make(L"CPU frequency", L"MHz", L"CPU", Kind::Clock, 0, Nice(std::max(cpuBase_, mhz) * 1.5), true);
        });
    }

    // --- memory, processes ---
    void ReadMemory(Builder& b) {
        PERFORMANCE_INFORMATION pi{};
        pi.cb = sizeof(pi);
        const bool perf = GetPerformanceInfo(&pi, sizeof(pi)) != FALSE;
        if (perf) {
            const double procs = static_cast<double>(pi.ProcessCount), threads = static_cast<double>(pi.ThreadCount);
            b.Add("cpu.processes", procs, [&] { return Make(L"Processes", L"", L"CPU", Kind::Number, 0, Nice(procs * 1.5), true); });
            b.Add("cpu.threads", threads, [&] { return Make(L"Threads", L"", L"CPU", Kind::Number, 0, Nice(threads * 1.5), true); });
        }
        AddCores(b);
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            const double total = static_cast<double>(ms.ullTotalPhys), avail = static_cast<double>(ms.ullAvailPhys);
            const double used = total - avail;
            b.Add("memory.used", used, [&] { return Make(L"Memory used", L"B", L"Memory", Kind::Bytes, 0, total, true); });
            b.Add("memory.available", avail, [&] { return Make(L"Memory available", L"B", L"Memory", Kind::Bytes, 0, total, true); });
            b.Add("memory.total", total, [&] { return Make(L"Memory total", L"B", L"Memory", Kind::Bytes, 0, total, true); });
            b.Add("memory.percent", total > 0 ? 100.0 * used / total : kNaN,
                  [] { return Make(L"Memory used (percent)", L"%", L"Memory", Kind::Percent, 0, 100); });
        }
        if (perf) {
            const double page = static_cast<double>(pi.PageSize);
            const double committed = static_cast<double>(pi.CommitTotal) * page, limit = static_cast<double>(pi.CommitLimit) * page;
            b.Add("memory.committed", committed, [&] { return Make(L"Memory committed", L"B", L"Memory", Kind::Bytes, 0, limit, true); });
            b.Add("memory.commitLimit", limit, [&] { return Make(L"Commit limit", L"B", L"Memory", Kind::Bytes, 0, limit, true); });
        }
    }

    // A PDH wildcard counter's values (false when it has none).
    bool Array(PDH_HCOUNTER c, DWORD& count, PDH_FMT_COUNTERVALUE_ITEM_W*& items, DWORD format = PDH_FMT_DOUBLE) {
        count = 0;
        items = nullptr;
        if (!c) return false;
        DWORD size = static_cast<DWORD>(pdhBuffer_.size());
        PDH_STATUS st = PdhGetFormattedCounterArrayW(
            c, format, &size, &count, pdhBuffer_.empty() ? nullptr : reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(pdhBuffer_.data()));
        if (st == PDH_MORE_DATA) {
            pdhBuffer_.resize(size + 4096);
            size = static_cast<DWORD>(pdhBuffer_.size());
            st = PdhGetFormattedCounterArrayW(c, format, &size, &count, reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(pdhBuffer_.data()));
        }
        if (st != ERROR_SUCCESS || !count) return false;
        items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(pdhBuffer_.data());
        return true;
    }
    bool Valid(const PDH_FMT_COUNTERVALUE& v) const {
        return v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA || listing_;
    }
    double Value(const PDH_FMT_COUNTERVALUE& v) const {
        const bool ok = v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA;
        return ok && std::isfinite(v.doubleValue) ? v.doubleValue : 0.0;
    }

    // --- GPU ---
    void ReadGpu(Builder& b) {
        DWORD count = 0;
        PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
        if (Array(gpuEngine_, count, items)) {
            // Each engine (adapter + physical GPU + engine) summed over the processes using it.
            engines_.clear();
            for (DWORD i = 0; i < count; ++i) {
                if (!Valid(items[i].FmtValue) || !items[i].szName) continue;
                const std::wstring_view name(items[i].szName);
                const size_t luid = name.find(L"luid_");
                const size_t type = name.find(L"_engtype_");
                if (luid == std::wstring_view::npos || type == std::wstring_view::npos || type < luid) continue;
                const std::wstring_view engine = name.substr(luid, type - luid);
                const std::wstring_view kind = name.substr(type + 9);
                auto it = std::find_if(engines_.begin(), engines_.end(), [&](const Engine& e) { return e.id == engine; });
                if (it == engines_.end()) {
                    Engine e;
                    e.id.assign(engine);
                    e.type = -1;
                    for (size_t k = 0; k < std::size(kEngines); ++k) {
                        if (kind.rfind(kEngines[k].prefix, 0) == 0) e.type = static_cast<int>(k);
                    }
                    engines_.push_back(std::move(e));
                    it = engines_.end() - 1;
                }
                it->busy += Value(items[i].FmtValue);
            }
            if (!engines_.empty()) {
                double types[std::size(kEngines)];
                bool seen[std::size(kEngines)]{};
                std::fill(std::begin(types), std::end(types), 0.0);
                double busiest = 0.0;
                for (const Engine& e : engines_) {
                    const double v = std::clamp(e.busy, 0.0, 100.0);
                    busiest = std::max(busiest, v);
                    if (e.type >= 0) {
                        types[e.type] = std::max(types[e.type], v);
                        seen[e.type] = true;
                    }
                }
                b.Add("gpu.total", seen[0] ? types[0] : busiest, [] { return Make(L"GPU (3D)", L"%", L"GPU", Kind::Percent, 0, 100); });
                b.Add("gpu.max", busiest, [] { return Make(L"GPU (busiest engine)", L"%", L"GPU", Kind::Percent, 0, 100); });
                for (size_t k = 0; k < std::size(kEngines); ++k) {
                    if (!seen[k]) continue;
                    b.Add(kEngines[k].key, types[k], [k] { return Make(kEngines[k].name, L"%", L"GPU", Kind::Percent, 0, 100); });
                }
            }
        }
        if (Array(gpuMemory_, count, items, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100)) {
            double used = 0.0;
            bool any = false;
            for (DWORD i = 0; i < count; ++i) {
                if (!Valid(items[i].FmtValue)) continue;
                used += Value(items[i].FmtValue);
                any = true;
            }
            if (any) {
                const double total = gpuMemoryTotal_;
                b.Add("gpu.memory.used", used, [&] {
                    return Make(L"GPU memory used", L"B", L"GPU", Kind::Bytes, 0, total > 0 ? total : Nice(used * 2), true);
                });
            }
        }
        if (gpuMemoryTotal_ > 0) {
            const double total = gpuMemoryTotal_;
            b.Add("gpu.memory.total", total, [&] { return Make(L"GPU memory total", L"B", L"GPU", Kind::Bytes, 0, total, true); });
        }
    }

    // --- disks ---
    void ReadDisks(Builder& b) {
        disks_.clear();  // instances "_Total", "0 C:", "1 D: E:"
        auto collect = [&](PDH_HCOUNTER c, int what) {
            DWORD count = 0;
            PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
            if (!Array(c, count, items, what == 0 ? PDH_FMT_DOUBLE : PDH_FMT_DOUBLE | PDH_FMT_NOCAP100)) return;
            for (DWORD i = 0; i < count; ++i) {
                if (!Valid(items[i].FmtValue) || !items[i].szName) continue;
                const std::wstring_view name(items[i].szName);
                auto it = std::find_if(disks_.begin(), disks_.end(), [&](const DiskRow& d) { return d.name == name; });
                if (it == disks_.end()) {
                    disks_.push_back(DiskRow{std::wstring(name), kNaN, kNaN, kNaN});
                    it = disks_.end() - 1;
                }
                const double v = Value(items[i].FmtValue);
                (what == 0 ? it->active : what == 1 ? it->read : it->write) = what == 0 ? std::clamp(v, 0.0, 100.0) : std::max(0.0, v);
            }
        };
        collect(diskTime_, 0);
        collect(diskRead_, 1);
        collect(diskWrite_, 2);
        auto total = std::find_if(disks_.begin(), disks_.end(), [](const DiskRow& d) { return d.name == L"_Total"; });
        if (total != disks_.end()) {
            b.Add("disk.total", total->active, [] { return Make(L"Disk active time", L"%", L"Disk", Kind::Percent, 0, 100); });
            b.Add("disk.read", total->read, [] { return Make(L"Disk read", L"B/s", L"Disk", Kind::BytesPerSecond, 0, 1e9); });
            b.Add("disk.write", total->write, [] { return Make(L"Disk write", L"B/s", L"Disk", Kind::BytesPerSecond, 0, 1e9); });
        }
        size_t n = 0;
        char key[48];
        for (const DiskRow& d : disks_) {
            if (d.name == L"_Total" || n >= PerfService::kMaxDisks) continue;
            const int number = _wtoi(d.name.c_str());
            if (d.name.empty() || d.name[0] < L'0' || d.name[0] > L'9') continue;
            ++n;
            const size_t space = d.name.find(L' ');
            const std::wstring letters = space == std::wstring::npos ? std::wstring() : d.name.substr(space + 1);
            const std::wstring title = L"Disk " + std::to_wstring(number) + (letters.empty() ? L"" : L" (" + letters + L")");
            snprintf(key, sizeof(key), "disk.%d.active", number);
            b.Add(key, d.active, [&] { return Make(title + L" active time", L"%", L"Disk", Kind::Percent, 0, 100); });
            snprintf(key, sizeof(key), "disk.%d.read", number);
            b.Add(key, d.read, [&] { return Make(title + L" read", L"B/s", L"Disk", Kind::BytesPerSecond, 0, 1e9); });
            snprintf(key, sizeof(key), "disk.%d.write", number);
            b.Add(key, d.write, [&] { return Make(title + L" write", L"B/s", L"Disk", Kind::BytesPerSecond, 0, 1e9); });
        }
    }

    // --- network: hardware interfaces that are up (filter / loopback / tunnel interfaces left out:
    // their traffic is counted on the hardware one too) ---
    void ReadNet(Builder* b, double dt) {
        MIB_IF_TABLE2* table = nullptr;
        if (GetIfTable2(&table) != NO_ERROR || !table) return;
        rows_.clear();
        bool anyHardware = false;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& r = table->Table[i];
            if (r.InterfaceAndOperStatusFlags.FilterInterface || r.Type == IF_TYPE_SOFTWARE_LOOPBACK || r.Type == IF_TYPE_TUNNEL ||
                r.OperStatus != IfOperStatusUp) {
                continue;
            }
            const bool hw = r.InterfaceAndOperStatusFlags.HardwareInterface != FALSE;
            anyHardware = anyHardware || hw;
            rows_.push_back(&r);
        }
        double down = 0.0, up = 0.0, linkDown = 0.0, linkUp = 0.0;
        bool any = false;
        size_t perInterface = 0;
        char key[96];
        std::vector<NetPrev> next;
        next.reserve(rows_.size());
        for (const MIB_IF_ROW2* r : rows_) {
            if (anyHardware && !r->InterfaceAndOperStatusFlags.HardwareInterface) continue;
            const uint64_t in = r->InOctets, out = r->OutOctets;
            next.push_back(NetPrev{r->InterfaceLuid.Value, in, out});
            auto it = std::find_if(net_.begin(), net_.end(), [&](const NetPrev& p) { return p.luid == r->InterfaceLuid.Value; });
            if (!b || it == net_.end()) continue;
            const double d = dt > 0.001 ? static_cast<double>(in >= it->in ? in - it->in : 0) / dt : 0.0;
            const double u = dt > 0.001 ? static_cast<double>(out >= it->out ? out - it->out : 0) / dt : 0.0;
            down += d;
            up += u;
            linkDown += static_cast<double>(r->ReceiveLinkSpeed) / 8.0;
            linkUp += static_cast<double>(r->TransmitLinkSpeed) / 8.0;
            any = true;
            if (perInterface < PerfService::kMaxInterfaces) {
                ++perInterface;
                const std::wstring alias = r->Alias;
                std::string slug = Slug(alias);
                if (std::find(slugs_.begin(), slugs_.begin() + static_cast<ptrdiff_t>(perInterface - 1), slug) !=
                    slugs_.begin() + static_cast<ptrdiff_t>(perInterface - 1)) {
                    slug += "-" + std::to_string(perInterface);  // two interfaces with one alias
                }
                if (slugs_.size() < perInterface) slugs_.resize(perInterface);
                slugs_[perInterface - 1] = slug;
                const double ld = static_cast<double>(r->ReceiveLinkSpeed) / 8.0, lu = static_cast<double>(r->TransmitLinkSpeed) / 8.0;
                snprintf(key, sizeof(key), "network.%s.down", slug.c_str());
                b->Add(key, d, [&] { return Make(alias + L" download", L"B/s", L"Network", Kind::BytesPerSecond, 0, ld > 0 && ld < 1e11 ? ld : 125e6); });
                snprintf(key, sizeof(key), "network.%s.up", slug.c_str());
                b->Add(key, u, [&] { return Make(alias + L" upload", L"B/s", L"Network", Kind::BytesPerSecond, 0, lu > 0 && lu < 1e11 ? lu : 125e6); });
            }
        }
        FreeMibTable(table);
        net_.swap(next);
        if (b && any) {
            b->Add("network.down", down, [&] {
                return Make(L"Network download", L"B/s", L"Network", Kind::BytesPerSecond, 0, linkDown > 0 && linkDown < 1e11 ? linkDown : 125e6);
            });
            b->Add("network.up", up, [&] {
                return Make(L"Network upload", L"B/s", L"Network", Kind::BytesPerSecond, 0, linkUp > 0 && linkUp < 1e11 ? linkUp : 125e6);
            });
        }
    }

    // --- battery, uptime ---
    void ReadPower(Builder& b) {
        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255 && ps.BatteryLifePercent <= 100) {
            b.Add("battery.percent", ps.BatteryLifePercent, [] { return Make(L"Battery", L"%", L"Battery", Kind::Percent, 0, 100); });
            b.Add("battery.charging", (ps.BatteryFlag & 8) ? 1.0 : 0.0,
                  [] { return Make(L"Battery charging", L"", L"Battery", Kind::Bool, 0, 1); });
            // Round 7 phase 7C: on mains power (absent when Windows does not know) and the time left
            // (seconds; absent while unknown: charging, on mains, or just after a change).
            if (ps.ACLineStatus <= 1) {
                b.Add("battery.acLine", ps.ACLineStatus == 1 ? 1.0 : 0.0,
                      [] { return Make(L"On mains power", L"", L"Battery", Kind::Bool, 0, 1); });
            }
            if (ps.BatteryLifeTime != static_cast<DWORD>(-1)) {
                const double left = static_cast<double>(ps.BatteryLifeTime);
                b.Add("battery.timeLeft", left, [&] {
                    return Make(L"Battery time left", L"s", L"Battery", Kind::Seconds, 0, Nice(std::max(36000.0, left * 1.5)), true);
                });
            }
        }
        const double up = static_cast<double>(GetTickCount64() / 1000);
        b.Add("system.uptime", up, [&] { return Make(L"Uptime", L"s", L"System", Kind::Seconds, 0, Nice(std::max(86400.0, up * 2)), true); });
    }

    struct Engine {
        std::wstring id;
        int type = -1;  // index in kEngines, -1 another kind
        double busy = 0.0;
    };
    struct DiskRow {
        std::wstring name;
        double active, read, write;
    };
    struct NetPrev {
        uint64_t luid;
        uint64_t in, out;
    };

    bool listing_ = false;
    NtQuerySystemInformationFn ntqsi_ = nullptr;
    std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> prev_, cur_;
    std::vector<double> cores_;
    size_t coreCount_ = 0;  // cores_ to add this sample
    double cpuBase_ = 0.0;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER cpuPerf_ = nullptr, cpuFreq_ = nullptr, gpuEngine_ = nullptr, gpuMemory_ = nullptr;
    PDH_HCOUNTER diskTime_ = nullptr, diskRead_ = nullptr, diskWrite_ = nullptr;
    std::vector<BYTE> pdhBuffer_;
    std::vector<Engine> engines_;
    std::vector<DiskRow> disks_;
    std::vector<const MIB_IF_ROW2*> rows_;
    std::vector<NetPrev> net_;
    std::vector<std::string> slugs_;  // this sample's per-interface key parts
    double gpuMemoryTotal_ = 0.0;
    double last_ = 0.0;
    HwReader hwinfo_;
};

void AppendNumber(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    // The shortest text that reads back as the same double (perf.series hands out the same values).
    char buf[40];
    snprintf(buf, sizeof(buf), "%.15g", v);
    if (strtod(buf, nullptr) != v) snprintf(buf, sizeof(buf), "%.17g", v);
    out += buf;
}

// Keys are ASCII (a-z 0-9 . - and the fixed names), but escape anyway.
void AppendKey(std::string& out, const std::string& key) {
    out += '"';
    for (char c : key) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    out += '"';
}

std::string Quoted(const std::wstring& s) { return JsonUtil::WideToUtf8(JsonUtil::Quote(s)); }

}  // namespace

const char* PerfService::KindName(Kind k) {
    switch (k) {
        case Kind::Percent: return "percent";
        case Kind::Bytes: return "bytes";
        case Kind::BytesPerSecond: return "bytesPerSecond";
        case Kind::Temperature: return "temperature";
        case Kind::Voltage: return "voltage";
        case Kind::Fan: return "fan";
        case Kind::Current: return "current";
        case Kind::Power: return "power";
        case Kind::Clock: return "clock";
        case Kind::Bool: return "bool";
        case Kind::Seconds: return "seconds";
        case Kind::Number:
        default: return "number";
    }
}

struct PerfService::Shared {
    HWND owner = nullptr;
    uint64_t generation = 0;
    std::atomic<uint32_t> interval{kDefaultInterval};
    std::atomic<bool> stop{false};
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto reset: stop, or the interval changed
    ~Shared() {
        if (wake) CloseHandle(wake);
    }
};

bool PerfService::Start(HWND owner) {
    if (Active()) return true;
    auto shared = std::make_shared<Shared>();
    if (!shared->wake) return false;
    shared->owner = owner;
    shared->generation = ++generation_;
    shared->interval = interval_;
    auto* param = new std::shared_ptr<Shared>(shared);
    HANDLE t = CreateThread(nullptr, 256 * 1024, &PerfService::ThreadMain, param, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) {
        delete param;
        return false;
    }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    thread_ = t;
    shared_ = std::move(shared);
    return true;
}

void PerfService::Stop() {
    if (!thread_) return;
    shared_->stop = true;
    SetEvent(shared_->wake);
    // A sample in progress finishes first; past 2 s the thread is left to end by itself (its last
    // snapshot carries an old generation and is dropped).
    WaitForSingleObject(thread_, 2000);
    CloseHandle(thread_);
    thread_ = nullptr;
    shared_.reset();
    ++generation_;
    catalog_.reset();
    values_.clear();
    hwinfo_ = HwInfo{};
    ringOf_.clear();
    ringKeys_.clear();
    rings_.clear();
    ringFor_.clear();
    ringCatalog_ = 0;
    head_ = 0;
    time_ = 0.0;
    snapshotJsonFor_ = metricsJsonFor_ = UINT64_MAX;
    snapshotJson_.clear();
    snapshotJson_.shrink_to_fit();
    metricsJson_.clear();
    metricsJson_.shrink_to_fit();
}

void PerfService::SetInterval(uint32_t ms) {
    ms = std::clamp(ms, kMinInterval, kMaxInterval);
    if (ms == interval_) return;
    interval_ = ms;
    snapshotJsonFor_ = UINT64_MAX;  // it carries the interval
    if (shared_) {
        shared_->interval = ms;
        SetEvent(shared_->wake);
    }
}

DWORD WINAPI PerfService::ThreadMain(void* param) {
    std::shared_ptr<Shared> s = std::move(*static_cast<std::shared_ptr<Shared>*>(param));
    delete static_cast<std::shared_ptr<Shared>*>(param);
    Sampler sampler;
    sampler.Open();
    if (s->stop) return 0;
    sampler.Prime();
    Builder builder;
    HwInfo hw, sentHw;
    bool hwSent = false;
    double last = QpcMs();
    double next = last + std::min<uint32_t>(s->interval, 500);
    while (!s->stop) {
        const double now = QpcMs();
        if (now < next) {
            WaitForSingleObject(s->wake, static_cast<DWORD>(std::ceil(next - now)));
            if (s->stop) break;
            // The interval may have changed: the next sample is one new interval after the last.
            const double due = last + s->interval;
            if (due < next || next - QpcMs() > s->interval) next = std::max(due, QpcMs());
            continue;
        }
        last = now;
        try {
            sampler.Sample(builder, hw);
            auto sample = std::make_unique<Sample>();
            sample->catalog = builder.Finish();
            sample->values = builder.values;
            sample->time = std::floor(WallMs());
            if (!hwSent || !(hw == sentHw)) {
                sample->hwinfo = std::make_shared<HwInfo>(hw);
                sentHw = hw;
                hwSent = true;
            }
            if (g_inFlight.load() < 8) {
                ++g_inFlight;
                Sample* raw = sample.release();
                if (!PostMessageW(s->owner, kMessage, static_cast<WPARAM>(s->generation), reinterpret_cast<LPARAM>(raw))) {
                    --g_inFlight;
                    delete raw;
                }
            }
        } catch (...) {
            // Out of memory: skip this sample.
        }
        // A fixed rate (a late wake-up does not delay the ones after it), never two samples closer
        // than half an interval.
        const uint32_t iv = s->interval;
        next += iv;
        if (next < QpcMs() + iv / 2.0) next = QpcMs() + iv / 2.0;
    }
    return 0;
}

bool PerfService::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != kMessage) return false;
    std::unique_ptr<Sample> s(reinterpret_cast<Sample*>(lp));
    --g_inFlight;
    if (!s || !Active() || wp != static_cast<WPARAM>(generation_)) return true;  // stopped since it was posted
    TakeSample(std::move(s));
    if (changed_) changed_();
    return true;
}

void PerfService::TakeSample(std::unique_ptr<Sample> s) {
    ++snapshots_;
    time_ = s->time;
    if (s->hwinfo) hwinfo_ = *s->hwinfo;
    if (s->catalog != catalog_) catalog_ = s->catalog;
    values_ = std::move(s->values);
    if (!catalog_) return;
    // The rings: a key seen for the first time gets one (its past filled with NaN).
    if (ringCatalog_ != catalog_->serial) {
        ringCatalog_ = catalog_->serial;
        ringFor_.assign(catalog_->metrics.size(), SIZE_MAX);
        for (size_t i = 0; i < catalog_->metrics.size(); ++i) {
            const std::string& key = catalog_->metrics[i].key;
            auto it = ringOf_.find(key);
            if (it != ringOf_.end()) {
                ringFor_[i] = it->second;
                continue;
            }
            if (rings_.size() >= kMaxRings) continue;
            auto ring = std::make_unique<double[]>(kHistory);
            std::fill(ring.get(), ring.get() + kHistory, kNaN);
            ringOf_.emplace(key, rings_.size());
            ringFor_[i] = rings_.size();
            ringKeys_.push_back(key);
            rings_.push_back(std::move(ring));
        }
    }
    const size_t slot = head_ % kHistory;
    for (auto& r : rings_) r[slot] = kNaN;
    const size_t n = std::min(values_.size(), ringFor_.size());
    for (size_t i = 0; i < n; ++i) {
        if (ringFor_[i] != SIZE_MAX) rings_[ringFor_[i]][slot] = values_[i];
    }
    ++head_;
}

const std::string& PerfService::SnapshotJson() {
    if (snapshotJsonFor_ == snapshots_ && !snapshotJson_.empty()) return snapshotJson_;
    snapshotJsonFor_ = snapshots_;
    std::string& out = snapshotJson_;
    out.clear();
    out += "{\"time\":";
    AppendNumber(out, time_);
    out += ",\"interval\":";
    AppendNumber(out, interval_);
    out += ",\"serial\":";
    AppendNumber(out, static_cast<double>(snapshots_));
    out += ",\"metrics\":{";
    if (catalog_) {
        const size_t n = std::min(values_.size(), catalog_->metrics.size());
        for (size_t i = 0; i < n; ++i) {
            if (i) out += ',';
            AppendKey(out, catalog_->metrics[i].key);
            out += ':';
            AppendNumber(out, values_[i]);
        }
    }
    out += "}}";
    return out;
}

std::string PerfService::MetricsJsonOf(const Catalog& catalog) {
    std::string out = "{\"serial\":";
    AppendNumber(out, static_cast<double>(catalog.serial));
    out += ",\"list\":[";
    bool first = true;
    for (const Metric& m : catalog.metrics) {
        if (!first) out += ',';
        first = false;
        out += "{\"key\":";
        AppendKey(out, m.key);
        out += ",\"name\":" + Quoted(m.name) + ",\"unit\":" + Quoted(m.unit) + ",\"min\":";
        AppendNumber(out, m.min);
        out += ",\"max\":";
        AppendNumber(out, m.max);
        out += ",\"kind\":\"";
        out += KindName(m.kind);
        out += "\",\"group\":" + Quoted(m.group) + ",\"hwinfo\":" + (m.hwinfo ? "true" : "false") + "}";
    }
    out += "]}";
    return out;
}

const std::string& PerfService::MetricsJson() {
    const uint64_t serial = CatalogSerial();
    if (metricsJsonFor_ == serial && !metricsJson_.empty()) return metricsJson_;
    metricsJsonFor_ = serial;
    if (catalog_) {
        metricsJson_ = MetricsJsonOf(*catalog_);
    } else {
        metricsJson_ = "{\"serial\":0,\"list\":[]}";
    }
    return metricsJson_;
}

std::string PerfService::HwinfoJson() const {
    const HwInfo& h = hwinfo_;
    std::string out = std::string("{\"available\":") + (h.available ? "true" : "false") + ",\"reason\":" + Quoted(h.reason) +
                      ",\"version\":\"" + std::to_string(h.version) + "." + std::to_string(h.revision) + "\",\"pollTime\":";
    AppendNumber(out, static_cast<double>(h.pollTime));
    out += ",\"sensors\":[";
    for (size_t i = 0; i < h.sensors.size(); ++i) {
        if (i) out += ',';
        out += "{\"index\":" + std::to_string(h.sensors[i].first) + ",\"name\":" + Quoted(h.sensors[i].second) + "}";
    }
    out += "],\"readingCount\":" + std::to_string(h.readings) + "}";
    return out;
}

void PerfService::Series(const std::string& key, size_t n, std::vector<double>& out) const {
    out.clear();
    auto it = ringOf_.find(key);
    if (it == ringOf_.end() || !n) return;
    const double* ring = rings_[it->second].get();
    const size_t count = std::min({n, head_, kHistory});
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) out.push_back(ring[(head_ - count + i) % kHistory]);
}

std::vector<PerfService::Metric> PerfService::ListOnce() {
    static double at = -1e12;
    static std::vector<Metric> cached;
    const double now = QpcMs();
    if (now - at < 30000.0 && !cached.empty()) return cached;
    Sampler sampler(true);
    sampler.Open();
    sampler.Prime();
    Builder builder;
    HwInfo hw;
    sampler.Sample(builder, hw);
    const std::shared_ptr<const Catalog> c = builder.Finish();
    cached = c ? c->metrics : std::vector<Metric>{};
    at = now;
    return cached;
}
