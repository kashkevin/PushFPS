// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "s10-stall" (24/25 Nov 2026): the capture-stall WITNESS. The owner's logs show capture stalls near every quarter
// hour (research/QUARTER_HOUR_STALLS.md): a callback 88-166 ms late, ~6 frames missing in a second (late or lost), the
// callback's 12.4 MB copy slowed by +5.5 or +8.1 ms, Present slowed at the same moments, while other threads of the process
// kept running. The log cannot say WHY. This records, always on and at next to no cost, what an ordinary (non-elevated)
// process can see of the machine once a second, in a small ring, and writes the ring from 5 s before to 3 s after a stall:
//   [stall-witness]        one line per second of the ring, WITH the image names of the busiest other processes (personal
//                          data: this family is on no allow-list; the test kit's tools leave it out unless asked);
//   [capture-stall-cause]  one summary line per stall, numbers only, no names (allow-listed for the kit).
// It records; it never changes how frames are captured, buffered or scheduled.
// Portable, header-only: the ring, the selection, the capture-side trigger, the mailbox between the capture thread and the
// sampler, the process-list budget, the per-process CPU table, the hypervisor CPUID decoding and the line formats. The
// Windows sampler is src/source/PushStallWitnessWin.cpp. Tests: tests/remoteplay/StallWitnessTests.inc.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace veyra::engine {

// One second of the machine, as the sampler saw it. Negative values mean "unknown" (a counter that could not be read).
struct PushWitnessSample {
    static constexpr std::size_t kTop = 5;
    double monoMs = -1;          // steady clock of the sampler (ms), the ring's time axis
    std::int64_t wallMs = -1;    // UTC milliseconds since 1970 (for the log only)
    double spanMs = -1;          // the interval this sample covers
    // Processors (PDH "\Processor(*)\..."): the busiest processor's DPC and interrupt share, and system totals.
    double dpcPctMax = -1, isrPctMax = -1, dpcPctTotal = -1, isrPctTotal = -1;
    int dpcCpu = -1, isrCpu = -1, cpus = -1;
    double dpcPerSec = -1, isrPerSec = -1;           // DPCs queued / interrupts per second, all processors
    double ctxPerSec = -1, syscallsPerSec = -1;      // "\System\Context Switches/sec", "\System\System Calls/sec"
    // Memory ("\Memory\..."): hard faults are page reads per second.
    double hardFaultsPerSec = -1, pagesInPerSec = -1, availMb = -1, commitMb = -1, standbyMb = -1, modifiedMb = -1;
    double diskMbps = -1, netMbps = -1;              // all disks / all network interfaces (sums; no instance names)
    // This process.
    double ownCpuPct = -1, ownFaultsPerSec = -1, ownWsMb = -1;
    // The capture path and the threads around it (from the program's own hooks).
    double captureCallbacks = -1, captureCopyMaxMs = -1, enginePickupMaxMs = -1, normalWakeMaxMs = -1;
    // Other processes: the largest CPU share of one other process, and the top few (pid + creation time; names are looked
    // up only when a stall is written).
    double topOtherCpuPct = -1;
    int topCount = 0;
    std::array<std::uint32_t, kTop> topPid{};
    std::array<std::uint64_t, kTop> topCreate{};
    std::array<double, kTop> topCpuPct{};
    std::array<std::array<char, 32>, kTop> topName{};   // image base names (PRIVATE: only in [stall-witness])
    int procsRead = -1, procsUnreadable = -1;
    bool processListSkipped = true;
    // Notifications seen during the second (PushWitnessEvent bits) and clock facts.
    std::uint32_t events = 0;
    double timeJumpMs = 0;        // wall clock moved against the steady clock by this much (a time change or a step)
    double samplerUs = -1;        // what this sample cost the sampler (wall time of the sampling code)
};

enum PushWitnessEvent : std::uint32_t {
    kWitnessDeviceArrival = 1u << 0,
    kWitnessDeviceRemoval = 1u << 1,
    kWitnessPowerSource = 1u << 2,
    kWitnessPowerScheme = 1u << 3,
    kWitnessDisplayState = 1u << 4,
    kWitnessTimeJump = 1u << 5,
};

// Fixed ring of samples: no allocation after construction.
template <std::size_t N>
class PushWitnessRing {
public:
    void push(const PushWitnessSample& s) {
        items_[next_] = s;
        next_ = (next_ + 1) % N;
        if (count_ < N) ++count_;
    }
    std::size_t size() const { return count_; }
    // The i-th oldest sample (0 = oldest).
    const PushWitnessSample& at(std::size_t i) const { return items_[(next_ + N - count_ + i) % N]; }
    // Indices (oldest first) of the samples whose time lies in [fromMs, toMs]; returns how many were written to out.
    template <std::size_t M>
    std::size_t select(double fromMs, double toMs, std::array<std::size_t, M>& out) const {
        std::size_t n = 0;
        for (std::size_t i = 0; i < count_ && n < M; ++i) {
            const double t = at(i).monoMs;
            if (t >= fromMs && t <= toMs) out[n++] = i;
        }
        return n;
    }
    void clear() { count_ = 0; next_ = 0; }

private:
    std::array<PushWitnessSample, N> items_{};
    std::size_t next_ = 0, count_ = 0;
};

// What the capture thread reports about one stall.
struct PushStallReport {
    double monoMs = 0;            // steady clock (ms) of the callback that saw it
    std::int64_t wallMs = 0;      // UTC ms
    int kind = 0;                 // 1 gap (a callback more than 2.5 intervals after the previous), 2 slow copy (no gap)
    double gapMs = 0, ptsStepMs = 0, copyMs = 0;
    double copyWallRatio = -1;    // this copy's wall time / the normal copy's
    double copyCycleRatio = -1;   // this copy's thread cycles / the normal copy's (-1 when cycles are not measured)
    std::uint64_t received = 0;
};

// Capture thread side: learns the normal copy (wall time and the thread's own cycles) and says when a callback is a stall.
// O(1) amortised, no allocation, called on the capture callback thread only.
// Review S10_STALL_REVIEW1 MUST 1: the normal copy is the MEDIAN of the last kWindow copies (re-computed every kWindow / 2
// callbacks), so a lasting change of the copy cost (HDR, another size or card) becomes the new normal within about a second
// instead of being reported as stalls for ever; the baseline is forgotten when the frame size or interval changes and when
// the source restarts (reset()). SHOULD 2: a gap is reported even within 1 s after a "copy" report (a slow copy often comes
// just before the gap); two reports of the same kind are at least 1 s apart.
class PushCopyWatch {
public:
    static constexpr double kGapIntervals = 2.5;   // the owner's logs: gaps of 2.6-5 intervals (88-166 ms at 30 Hz)
    static constexpr double kSlowCopyFactor = 3.0;
    static constexpr double kSlowCopyMinMs = 3.0;
    static constexpr std::size_t kWindow = 64;
    void reset() {
        count_ = next_ = 0;
        normalMs_ = normalCycles_ = 0;
        lastGapMs_ = lastCopyMs_ = -1e18;
        bytes_ = 0;
        intervalMs_ = 0;
    }
    // copyCycles: QueryThreadCycleTime delta over the same span as copyMs (0 = not measured); bytes: the sample's size.
    std::optional<PushStallReport> observe(double arrivalDeltaMs, double ptsDeltaMs, double copyMs, std::uint64_t copyCycles, double intervalMs,
                                           std::uint64_t received, double monoMs, std::int64_t wallMs, std::uint64_t bytes = 0) {
        std::optional<PushStallReport> out;
        if (!(copyMs >= 0)) return out;
        if ((bytes && bytes_ && bytes != bytes_) || (intervalMs > 0 && intervalMs_ > 0 && std::fabs(intervalMs - intervalMs_) > 0.05 * intervalMs_)) reset();
        if (bytes) bytes_ = bytes;
        if (intervalMs > 0) intervalMs_ = intervalMs;
        const bool learned = normalMs_ > 0;
        const bool gap = intervalMs > 0 && arrivalDeltaMs > kGapIntervals * intervalMs;
        const bool slow = learned && copyMs > std::max(kSlowCopyMinMs, kSlowCopyFactor * normalMs_);
        const bool post = gap ? monoMs - lastGapMs_ >= 1000.0 : (slow && monoMs - lastCopyMs_ >= 1000.0 && monoMs - lastGapMs_ >= 1000.0);
        if (post) {
            PushStallReport r;
            r.monoMs = monoMs;
            r.wallMs = wallMs;
            r.kind = gap ? 1 : 2;
            r.gapMs = gap ? arrivalDeltaMs : 0;
            r.ptsStepMs = ptsDeltaMs;
            r.copyMs = copyMs;
            r.copyWallRatio = learned ? copyMs / normalMs_ : -1;
            r.copyCycleRatio = (learned && copyCycles > 0 && normalCycles_ > 0) ? double(copyCycles) / normalCycles_ : -1;
            r.received = received;
            (gap ? lastGapMs_ : lastCopyMs_) = monoMs;
            out = r;
        }
        ms_[next_] = copyMs;
        cycles_[next_] = double(copyCycles);
        next_ = (next_ + 1) % kWindow;
        if (count_ < kWindow) ++count_;
        // The first baseline after 16 copies, then every half window.
        if ((!learned && count_ >= 16) || (learned && next_ % (kWindow / 2) == 0)) {
            normalMs_ = median(ms_);
            normalCycles_ = median(cycles_);
        }
        return out;
    }
    double normalMs() const { return normalMs_; }
    double normalCycles() const { return normalCycles_; }

private:
    double median(const std::array<double, kWindow>& v) const {
        std::array<double, kWindow> t{};
        std::copy(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(count_), t.begin());
        const auto mid = t.begin() + static_cast<std::ptrdiff_t>(count_ / 2);
        std::nth_element(t.begin(), mid, t.begin() + static_cast<std::ptrdiff_t>(count_));
        return *mid;
    }
    std::array<double, kWindow> ms_{}, cycles_{};
    std::size_t count_ = 0, next_ = 0;
    double normalMs_ = 0, normalCycles_ = 0, lastGapMs_ = -1e18, lastCopyMs_ = -1e18, intervalMs_ = 0;
    std::uint64_t bytes_ = 0;
};

// A single slot from the capture thread (writer) to the sampler (reader): a sequence lock over relaxed atomics, so neither
// side ever waits for the other and there is no data race. A newer report replaces an unread older one.
class PushStallMailbox {
public:
    void post(const PushStallReport& r) {
        const std::uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        monoMs_.store(r.monoMs, std::memory_order_relaxed);
        wallMs_.store(r.wallMs, std::memory_order_relaxed);
        kind_.store(r.kind, std::memory_order_relaxed);
        gapMs_.store(r.gapMs, std::memory_order_relaxed);
        ptsStepMs_.store(r.ptsStepMs, std::memory_order_relaxed);
        copyMs_.store(r.copyMs, std::memory_order_relaxed);
        wallRatio_.store(r.copyWallRatio, std::memory_order_relaxed);
        cycleRatio_.store(r.copyCycleRatio, std::memory_order_relaxed);
        received_.store(r.received, std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
        posted_.fetch_add(1, std::memory_order_relaxed);
    }
    // A report not yet taken, if one is there and could be read consistently.
    std::optional<PushStallReport> take() {
        for (int attempt = 0; attempt < 4; ++attempt) {
            const std::uint32_t before = seq_.load(std::memory_order_acquire);
            if (before == taken_) return std::nullopt;
            if (before & 1u) continue;
            PushStallReport r;
            r.monoMs = monoMs_.load(std::memory_order_relaxed);
            r.wallMs = wallMs_.load(std::memory_order_relaxed);
            r.kind = kind_.load(std::memory_order_relaxed);
            r.gapMs = gapMs_.load(std::memory_order_relaxed);
            r.ptsStepMs = ptsStepMs_.load(std::memory_order_relaxed);
            r.copyMs = copyMs_.load(std::memory_order_relaxed);
            r.copyWallRatio = wallRatio_.load(std::memory_order_relaxed);
            r.copyCycleRatio = cycleRatio_.load(std::memory_order_relaxed);
            r.received = received_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) != before) continue;
            taken_ = before;
            return r;
        }
        return std::nullopt;
    }
    std::uint64_t posted() const { return posted_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint32_t> seq_{0};
    std::uint32_t taken_ = 0;   // reader only
    std::atomic<double> monoMs_{0}, gapMs_{0}, ptsStepMs_{0}, copyMs_{0}, wallRatio_{-1}, cycleRatio_{-1};
    std::atomic<std::int64_t> wallMs_{0};
    std::atomic<int> kind_{0};
    std::atomic<std::uint64_t> received_{0}, posted_{0};
};

// A per-second maximum written by any thread and read-and-cleared by the sampler (lock-free).
class PushAtomicMax {
public:
    void note(double v) {
        double cur = value_.load(std::memory_order_relaxed);
        while (v > cur && !value_.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
        }
        count_.fetch_add(1, std::memory_order_relaxed);
    }
    // The maximum since the last call (-1 when nothing was noted) and how many values came.
    double takeMax(double* count = nullptr) {
        if (count) *count = double(count_.exchange(0, std::memory_order_relaxed));
        return value_.exchange(-1.0, std::memory_order_relaxed);
    }

private:
    std::atomic<double> value_{-1.0};
    std::atomic<std::uint64_t> count_{0};
};

// How often the process list may be read so that the sampler stays under its budget (default 0.1 % of one processor =
// 1000 us per second for everything; the process list may take 700 us of it on average). Pure.
class PushWitnessBudget {
public:
    static constexpr double kShareUs = 700.0;          // average per second allowed to the process list
    static constexpr double kHardCapUs = 20000.0;      // one pass slower than this: the list is off for kOffSeconds
    static constexpr double kOffSeconds = 600.0;
    static constexpr int kMaxInterval = 8;
    // Whether the process list runs in the sample taken at second `second` (a counter of samples).
    bool due(std::int64_t second) const { return second >= offUntil_ && second - last_ >= interval_; }
    // After a pass at `second` that cost costUs.
    void ran(std::int64_t second, double costUs) {
        last_ = second;
        cost_ = cost_ < 0 ? costUs : cost_ + 0.25 * (costUs - cost_);
        if (costUs > kHardCapUs) {
            offUntil_ = second + static_cast<std::int64_t>(kOffSeconds);
            ++disabled_;
        }
        const double need = std::ceil(cost_ / kShareUs);
        interval_ = static_cast<int>(std::clamp(need, 1.0, double(kMaxInterval)));
    }
    int interval() const { return interval_; }
    double averageCostUs() const { return cost_; }
    int timesDisabled() const { return disabled_; }
    bool off(std::int64_t second) const { return second < offUntil_; }

private:
    std::int64_t last_ = -1000000, offUntil_ = -1;
    int interval_ = 1, disabled_ = 0;
    double cost_ = -1;
};

// Per-process CPU time deltas between two passes, in fixed tables (no allocation). Two open-addressing tables keyed by
// pid swap roles each pass. A pid whose creation time changed is a new process (no delta).
class PushProcessCpuTable {
public:
    static constexpr std::size_t kCapacity = 4096;   // power of two; more processes than kCapacity/2 are not tracked
    struct Top {
        std::uint32_t pid = 0;
        std::uint64_t create = 0;
        double cpu100ns = 0;
        std::size_t index = 0;   // the caller's index of this process in its pass (to fetch its name)
    };
    void setIndex(std::size_t i) { currentIndex_ = i; }
    void begin() {
        cur_ ^= 1u;
        auto& t = tables_[cur_];
        for (auto& e : t) e.pid = kEmpty;
        used_ = 0;
        topN_ = 0;
        total100ns_ = 0;
    }
    // One process of this pass: its pid, creation time and total CPU time (kernel + user, 100 ns). Returns its delta (100 ns)
    // since the previous pass, or -1 for a process not seen there.
    // previousPass100ns: the time (same clock as create) of the previous pass; a process created after it counts with all of
    // its CPU time (it ran only inside this interval). 0 = unknown.
    double add(std::uint32_t pid, std::uint64_t create, std::uint64_t cpu100ns, std::uint32_t ownPid, std::uint64_t previousPass100ns = 0) {
        if (pid == kEmpty || used_ >= kCapacity / 2) return -1;
        auto& t = tables_[cur_];
        std::size_t i = slot(pid);
        while (t[i].pid != kEmpty) i = (i + 1) & (kCapacity - 1);
        t[i] = Entry{pid, create, cpu100ns};
        ++used_;
        const Entry* p = find(tables_[cur_ ^ 1u], pid);
        double d = -1;
        if (p && p->create == create && cpu100ns >= p->cpu) d = double(cpu100ns - p->cpu);
        else if (previousPass100ns && create > previousPass100ns) d = double(cpu100ns);   // started since the previous pass
        if (d < 0) return -1;
        total100ns_ += d;
        if (pid != ownPid) insertTop(Top{pid, create, d, currentIndex_});
        return d;
    }
    std::size_t topCount() const { return topN_; }
    const Top& top(std::size_t i) const { return top_[i]; }
    double total100ns() const { return total100ns_; }

private:
    static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;
    struct Entry {
        std::uint32_t pid = kEmpty;
        std::uint64_t create = 0;
        std::uint64_t cpu = 0;
    };
    static std::size_t slot(std::uint32_t pid) { return (std::size_t(pid) * 2654435761u) & (kCapacity - 1); }
    static const Entry* find(const std::array<Entry, kCapacity>& t, std::uint32_t pid) {
        std::size_t i = slot(pid);
        for (std::size_t n = 0; n < kCapacity && t[i].pid != kEmpty; ++n, i = (i + 1) & (kCapacity - 1))
            if (t[i].pid == pid) return &t[i];
        return nullptr;
    }
    void insertTop(const Top& c) {
        if (topN_ < PushWitnessSample::kTop) top_[topN_++] = c;
        else if (c.cpu100ns > top_[topN_ - 1].cpu100ns) top_[topN_ - 1] = c;
        else return;
        for (std::size_t i = topN_ - 1; i > 0 && top_[i].cpu100ns > top_[i - 1].cpu100ns; --i) std::swap(top_[i], top_[i - 1]);
    }
    std::array<std::array<Entry, kCapacity>, 2> tables_{};
    std::size_t cur_ = 0;
    std::size_t used_ = 0, topN_ = 0, currentIndex_ = 0;
    std::array<Top, PushWitnessSample::kTop> top_{};
    double total100ns_ = 0;
};

// Hypervisor facts from CPUID, decoded as the Hypervisor Top Level Functional Specification documents them
// (learn.microsoft.com/virtualization/hyper-v-on-windows/tlfs/feature-discovery): leaf 1 ECX bit 31 = a hypervisor is present;
// leaf 0x40000001 EAX = 0x31237648 ("Hv#1") = the Microsoft hypervisor interface; leaf 0x40000003 EBX bit 0 = privilege bit 32
// of HV_PARTITION_PRIVILEGE_MASK, CreatePartitions (this partition may create child partitions, i.e. it hosts VMs or can).
struct PushHypervisorInfo {
    bool present = false, microsoftInterface = false, createPartitions = false;
};
inline PushHypervisorInfo pushDecodeHypervisor(std::uint32_t leaf1Ecx, std::uint32_t maxHvLeaf, std::uint32_t leaf40000001Eax, std::uint32_t leaf40000003Ebx) {
    PushHypervisorInfo h;
    h.present = ((leaf1Ecx >> 31) & 1u) != 0;
    if (!h.present) return h;
    h.microsoftInterface = maxHvLeaf >= 0x40000001u && leaf40000001Eax == 0x31237648u;
    h.createPartitions = h.microsoftInterface && maxHvLeaf >= 0x40000003u && (leaf40000003Ebx & 1u) != 0;
    return h;
}

// Image names of virtual-machine worker processes (counted, never named in the allow-listed line). Case-insensitive,
// base name only (".exe" optional).
inline bool pushIsVmWorkerImage(const char* base) {
    if (!base) return false;
    char low[64]{};
    std::size_t n = 0;
    for (; base[n] && n + 1 < sizeof low; ++n) low[n] = static_cast<char>((base[n] >= 'A' && base[n] <= 'Z') ? base[n] - 'A' + 'a' : base[n]);
    if (n > 4 && std::strcmp(low + n - 4, ".exe") == 0) low[n - 4] = 0;
    static const char* const names[] = {"vmwp", "vmmem", "vmmemwsl", "vmcompute", "vmms", "wslhost", "wslservice", "virtualboxvm", "vboxheadless", "vmware-vmx", "qemu-system-x86_64"};
    for (const char* name : names)
        if (std::strcmp(low, name) == 0) return true;
    return false;
}

// Removes everything up to the last path separator and keeps at most 31 characters of an image name, printable ASCII only
// (a name is written only into the private [stall-witness] family).
inline void pushImageBaseName(const char* path, char (&out)[32]) {
    out[0] = 0;
    if (!path) return;
    const char* base = path;
    for (const char* c = path; *c; ++c)
        if (*c == '\\' || *c == '/') base = c + 1;
    std::size_t n = 0;
    for (; base[n] && n < 31; ++n) {
        const char ch = base[n];
        out[n] = (ch > 32 && ch < 127 && ch != '=' && ch != ',' && ch != ':') ? ch : '_';
    }
    out[n] = 0;
}

// One pass over the buffer NtQuerySystemInformation(SystemProcessInformation) fills (x64 layout of SYSTEM_PROCESS_INFORMATION:
// winternl.h documents NextEntryOffset (0), NumberOfThreads (4), ImageName (56, a UNICODE_STRING) and UniqueProcessId (80) and
// keeps bytes 8-55 "reserved"; there, by the layout every process tool relies on since Windows Vista, lie HardFaultCount (16),
// CreateTime (32), UserTime (40) and KernelTime (48)). No handle to any process is opened. Bounds-checked: an entry or a name
// outside the buffer ends the walk. f(pid, create100ns, cpu100ns, hardFaults, name, nameChars, index) per process.
struct PushProcessInfoLayout {
    static constexpr std::size_t kNext = 0, kHardFaults = 16, kCreate = 32, kUser = 40, kKernel = 48, kNameLength = 56, kNameBuffer = 64, kPid = 80,
                                 kMinEntry = 88;
};
template <class F>
std::size_t pushParseProcessInfo(const std::uint8_t* buffer, std::size_t length, F&& f) {
    using L = PushProcessInfoLayout;
    auto rd32 = [](const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; };
    auto rd64 = [](const std::uint8_t* p) { std::uint64_t v; std::memcpy(&v, p, 8); return v; };
    std::size_t at = 0, n = 0;
    while (buffer && at + L::kMinEntry <= length && n < 65536) {
        const std::uint8_t* e = buffer + at;
        const std::uint32_t next = rd32(e + L::kNext);
        const std::uint16_t nameBytes = static_cast<std::uint16_t>(e[L::kNameLength] | (e[L::kNameLength + 1] << 8));
        const std::uint64_t namePtr = rd64(e + L::kNameBuffer);
        const std::uint64_t base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(buffer));
        const std::uint16_t* name = nullptr;
        std::size_t chars = 0;
        if (namePtr >= base && namePtr + nameBytes <= base + length && (nameBytes % 2) == 0) {
            name = reinterpret_cast<const std::uint16_t*>(buffer + (namePtr - base));
            chars = nameBytes / 2u;
        }
        f(static_cast<std::uint32_t>(rd64(e + L::kPid)), rd64(e + L::kCreate), rd64(e + L::kUser) + rd64(e + L::kKernel), rd32(e + L::kHardFaults), name, chars, n);
        ++n;
        if (next == 0) break;
        at += next;
    }
    return n;
}
// A UTF-16 image name as printable ASCII base name (at most 31 characters; other characters become '_'). An empty name (the
// idle process) becomes "Idle".
inline void pushNarrowImageName(const std::uint16_t* name, std::size_t chars, std::array<char, 32>& out) {
    std::size_t start = 0;
    for (std::size_t i = 0; name && i < chars; ++i)
        if (name[i] == u'\\' || name[i] == u'/') start = i + 1;
    std::size_t n = 0;
    for (std::size_t i = start; name && i < chars && n < 31; ++i) {
        const std::uint16_t c = name[i];
        out[n++] = (c > 32 && c < 127 && c != '=' && c != ',' && c != ':') ? static_cast<char>(c) : '_';
    }
    if (n == 0) { std::memcpy(out.data(), "Idle", 5); return; }
    out[n] = 0;
}

namespace push_witness_detail {
inline std::string hms(std::int64_t wallMs) {
    if (wallMs < 0) return "-";
    const std::int64_t dayMs = ((wallMs % 86400000) + 86400000) % 86400000;
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", int(dayMs / 3600000), int(dayMs / 60000 % 60), int(dayMs / 1000 % 60), int(dayMs % 1000));
    return b;
}
inline double median(double* v, std::size_t n) {
    if (n == 0) return -1;
    std::sort(v, v + n);
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}
}  // namespace push_witness_detail

// The private per-second line. names[i] is the image name of top process i ("?" when it could not be read; it is the
// caller's job to resolve them only at stall time).
inline std::string pushFormatWitnessLine(std::uint64_t stall, const PushWitnessSample& s, double stallMonoMs, const char* const* names) {
    char b[1024];
    std::snprintf(b, sizeof b,
                  "stall=%llu dt=%+.1fs at=%s spanMs=%.0f dpcMax=%.1f@%d isrMax=%.1f@%d dpcTot=%.2f isrTot=%.2f dpcPerSec=%.0f isrPerSec=%.0f "
                  "ctxPerSec=%.0f syscallsPerSec=%.0f hardFaults=%.0f pagesIn=%.0f availMb=%.0f commitMb=%.0f standbyMb=%.0f modifiedMb=%.0f "
                  "diskMBps=%.1f netMBps=%.2f ownCpu=%.1f ownFaults=%.0f ownWsMb=%.0f callbacks=%.0f copyMaxMs=%.2f enginePickupMaxMs=%.2f "
                  "normalWakeMaxMs=%.2f topOtherCpu=%.1f procs=%d unreadable=%d listSkipped=%d events=0x%X timeJumpMs=%.1f samplerUs=%.0f top=",
                  static_cast<unsigned long long>(stall), (s.monoMs - stallMonoMs) / 1000.0, push_witness_detail::hms(s.wallMs).c_str(), s.spanMs,
                  s.dpcPctMax, s.dpcCpu, s.isrPctMax, s.isrCpu, s.dpcPctTotal, s.isrPctTotal, s.dpcPerSec, s.isrPerSec, s.ctxPerSec, s.syscallsPerSec,
                  s.hardFaultsPerSec, s.pagesInPerSec, s.availMb, s.commitMb, s.standbyMb, s.modifiedMb, s.diskMbps, s.netMbps, s.ownCpuPct,
                  s.ownFaultsPerSec, s.ownWsMb, s.captureCallbacks, s.captureCopyMaxMs, s.enginePickupMaxMs, s.normalWakeMaxMs, s.topOtherCpuPct,
                  s.procsRead, s.procsUnreadable, s.processListSkipped ? 1 : 0, s.events, s.timeJumpMs, s.samplerUs);
    std::string line(b);
    if (s.topCount == 0) line += "-";
    for (int i = 0; i < s.topCount && i < int(PushWitnessSample::kTop); ++i) {
        char t[64];
        std::snprintf(t, sizeof t, "%s%s:%.1f", i ? "," : "", (names && names[i] && names[i][0]) ? names[i] : "?", s.topCpuPct[std::size_t(i)]);
        line += t;
    }
    line += " (private: image names of other processes; withheld by the test kit's tools by default)";
    return line;
}

// Facts that are not per second: the hypervisor, VM worker processes counted at the stall, the process-list budget.
struct PushWitnessContext {
    PushHypervisorInfo hypervisor;
    int vmWorkers = -1;           // processes whose image is a VM worker (pushIsVmWorkerImage), counted when the stall is written
    int processListInterval = 1;  // seconds between process-list passes at that time
    int processListOff = 0;       // 1 when the budget switched the list off
    std::uint64_t merged = 0;     // further stalls reported while this one was waiting to be written
};

// The allow-listed summary: numbers only. "Max" is over the samples from 1 s before to 1.5 s after the stall, "Base" the
// median of the samples from 5 s to 1.5 s before it.
template <std::size_t N>
std::string pushFormatStallCause(std::uint64_t stall, const PushStallReport& r, const PushWitnessRing<N>& ring, const PushWitnessContext& c) {
    using push_witness_detail::median;
    std::array<std::size_t, N> idx{};
    const std::size_t n = ring.select(r.monoMs - 5200.0, r.monoMs + 3200.0, idx);
    auto peak = [&](double PushWitnessSample::*m, bool wantMin) {
        double best = -1;
        for (std::size_t k = 0; k < n; ++k) {
            const auto& s = ring.at(idx[k]);
            const double dt = s.monoMs - r.monoMs;
            const double v = s.*m;
            if (dt < -1000.0 || dt > 1500.0 || v < 0) continue;
            if (best < 0 || (wantMin ? v < best : v > best)) best = v;
        }
        return best;
    };
    auto base = [&](double PushWitnessSample::*m) {
        std::array<double, N> v{};
        std::size_t k = 0;
        for (std::size_t j = 0; j < n; ++j) {
            const auto& s = ring.at(idx[j]);
            const double dt = s.monoMs - r.monoMs;
            if (dt < -5200.0 || dt > -1500.0 || s.*m < 0) continue;
            v[k++] = s.*m;
        }
        return median(v.data(), k);
    };
    int dpcCpu = -1, isrCpu = -1, before = 0, after = 0;
    std::uint32_t events = 0;
    double timeJump = 0, samplerMax = -1, dpcBest = -1, isrBest = -1;
    for (std::size_t k = 0; k < n; ++k) {
        const auto& s = ring.at(idx[k]);
        const double dt = s.monoMs - r.monoMs;
        if (dt < 0) ++before;
        else ++after;
        events |= s.events;
        if (std::fabs(s.timeJumpMs) > std::fabs(timeJump)) timeJump = s.timeJumpMs;
        samplerMax = std::max(samplerMax, s.samplerUs);
        if (dt >= -1000.0 && dt <= 1500.0) {
            if (s.dpcPctMax > dpcBest) { dpcBest = s.dpcPctMax; dpcCpu = s.dpcCpu; }
            if (s.isrPctMax > isrBest) { isrBest = s.isrPctMax; isrCpu = s.isrCpu; }
        }
    }
    const double ranPct = (r.copyCycleRatio > 0 && r.copyWallRatio > 0) ? std::min(100.0 * r.copyCycleRatio / r.copyWallRatio, 999.0) : -1;
    const double standbyBefore = base(&PushWitnessSample::standbyMb), standbyMin = peak(&PushWitnessSample::standbyMb, true);
    char b[1400];
    std::snprintf(b, sizeof b,
                  "stall=%llu kind=%s at=%s gapMs=%.1f ptsStepMs=%.3f copyMs=%.2f copyWallRatio=%.2f copyCycleRatio=%.2f captureThreadRanPct=%.0f "
                  "dpcPct=%.1f dpcCpu=%d dpcPctBase=%.1f isrPct=%.1f isrCpu=%d isrPctBase=%.1f dpcPerSec=%.0f dpcPerSecBase=%.0f isrPerSec=%.0f "
                  "isrPerSecBase=%.0f ctxSwitches=%.0f ctxSwitchesBase=%.0f hardFaults=%.0f hardFaultsBase=%.0f availMbMin=%.0f standbyDropMb=%.0f "
                  "uiWakeMs=%.2f uiWakeMsBase=%.2f engineWakeMs=%.2f engineWakeMsBase=%.2f topOtherCpuPct=%.1f topOtherCpuPctBase=%.1f ownCpuPct=%.1f "
                  "diskMBps=%.1f netMBps=%.2f events=0x%X timeJumpMs=%.1f hvPresent=%d hvInterface=%d hvCreatePartitions=%d vmWorkers=%d "
                  "processListS=%d processListOff=%d merged=%llu samplesBefore=%d samplesAfter=%d samplerUsMax=%.0f "
                  "(system-wide counters around a capture stall, read by an ordinary process: Max over -1..+1.5 s, Base = median over -5..-1.5 s; "
                  "captureThreadRanPct = this copy's thread cycles against the normal copy's, over its wall time against the normal one: "
                  "uncalibrated (Microsoft documents that interrupt time is not charged to the interrupted thread; for DPC time it says nothing); "
                  "hvPresent is also 1 when only memory integrity / VBS runs the hypervisor; uiWakeMs = a normal-priority probe thread's longest "
                  "wake delay, -1 unless VEYRA_EXP_RP_STALL_WITNESS_DEEP=1; per-second counters cannot show a preemption of a few ms; no names)",
                  static_cast<unsigned long long>(stall), r.kind == 1 ? "gap" : "copy", push_witness_detail::hms(r.wallMs).c_str(), r.gapMs, r.ptsStepMs,
                  r.copyMs, r.copyWallRatio, r.copyCycleRatio, ranPct, dpcBest, dpcCpu, base(&PushWitnessSample::dpcPctMax), isrBest, isrCpu,
                  base(&PushWitnessSample::isrPctMax), peak(&PushWitnessSample::dpcPerSec, false), base(&PushWitnessSample::dpcPerSec),
                  peak(&PushWitnessSample::isrPerSec, false), base(&PushWitnessSample::isrPerSec), peak(&PushWitnessSample::ctxPerSec, false),
                  base(&PushWitnessSample::ctxPerSec), peak(&PushWitnessSample::hardFaultsPerSec, false), base(&PushWitnessSample::hardFaultsPerSec),
                  peak(&PushWitnessSample::availMb, true), (standbyBefore >= 0 && standbyMin >= 0) ? standbyBefore - standbyMin : -1.0,
                  peak(&PushWitnessSample::normalWakeMaxMs, false), base(&PushWitnessSample::normalWakeMaxMs),
                  peak(&PushWitnessSample::enginePickupMaxMs, false), base(&PushWitnessSample::enginePickupMaxMs),
                  peak(&PushWitnessSample::topOtherCpuPct, false), base(&PushWitnessSample::topOtherCpuPct), peak(&PushWitnessSample::ownCpuPct, false),
                  peak(&PushWitnessSample::diskMbps, false), peak(&PushWitnessSample::netMbps, false), events, timeJump,
                  c.hypervisor.present ? 1 : 0, c.hypervisor.microsoftInterface ? 1 : 0, c.hypervisor.createPartitions ? 1 : 0, c.vmWorkers,
                  c.processListInterval, c.processListOff, static_cast<unsigned long long>(c.merged), before, after, samplerMax);
    return b;
}

// The sampler-side state machine: keeps the ring, takes reports from the mailbox, and once the ring holds 3 s after a stall
// hands the lines to `sink(family, text)`. `names(sample, i)` returns the image name of a sample's i-th top process (called
// only while writing). At most one stall is pending; reports during it are counted as merged. Emission is limited to
// kMaxPerHour stalls per hour.
class PushStallWitnessCore {
public:
    static constexpr std::size_t kRing = 16;
    static constexpr double kAfterMs = 3000.0;
    static constexpr int kMaxPerHour = 40;        // gap-type stalls written per hour
    static constexpr int kMaxCopyPerHour = 8;     // copy-only reports per hour (they can never use up the gap quota)
    void addSample(const PushWitnessSample& s) { ring_.push(s); }
    const PushWitnessRing<kRing>& ring() const { return ring_; }
    void takeFrom(PushStallMailbox& box) {
        if (auto r = box.take()) {
            if (!pending_) { pending_ = *r; merged_ = 0; }
            else {
                ++merged_;
                // A gap within 3 s after a pending copy-only report is the stall itself: it takes the report's place.
                if (pending_->kind == 2 && r->kind == 1 && r->monoMs - pending_->monoMs < 3000.0) pending_ = *r;
            }
        }
    }
    bool pending() const { return pending_.has_value(); }
    // True once 3 s have passed since the pending stall (the ring then holds the seconds after it).
    bool ready(double nowMonoMs) const { return pending_ && nowMonoMs >= pending_->monoMs + kAfterMs; }
    template <class Names, class Sink>
    bool emit(double nowMonoMs, PushWitnessContext context, Names&& names, Sink&& sink) {
        if (!ready(nowMonoMs)) return false;
        const PushStallReport r = *pending_;
        pending_.reset();
        HourList& quota = r.kind == 1 ? hour_ : copyHour_;
        const std::size_t limit = std::size_t(r.kind == 1 ? kMaxPerHour : kMaxCopyPerHour);
        while (!quota.empty() && nowMonoMs - quota.front() > 3600000.0) quota.popFront();
        if (quota.size() >= limit) { ++suppressed_; return false; }
        quota.push(nowMonoMs);
        ++stalls_;
        context.merged = merged_;
        std::array<std::size_t, kRing> idx{};
        const std::size_t n = ring_.select(r.monoMs - 5200.0, r.monoMs + 3200.0, idx);
        for (std::size_t k = 0; k < n; ++k) {
            const auto& s = ring_.at(idx[k]);
            std::array<const char*, PushWitnessSample::kTop> list{};
            for (int i = 0; i < s.topCount && i < int(PushWitnessSample::kTop); ++i) list[std::size_t(i)] = names(s, i);
            sink("stall-witness", pushFormatWitnessLine(stalls_, s, r.monoMs, list.data()));
        }
        sink("capture-stall-cause", pushFormatStallCause(stalls_, r, ring_, context));
        return true;
    }
    std::uint64_t stalls() const { return stalls_; }
    std::uint64_t suppressed() const { return suppressed_; }

private:
    // Times of the stalls written in the last hour (fixed capacity).
    struct HourList {
        std::array<double, kMaxPerHour> t{};
        std::size_t head = 0, n = 0;
        bool empty() const { return n == 0; }
        std::size_t size() const { return n; }
        double front() const { return t[head]; }
        void popFront() { head = (head + 1) % t.size(); --n; }
        void push(double v) { t[(head + n) % t.size()] = v; ++n; }
    };
    PushWitnessRing<kRing> ring_;
    std::optional<PushStallReport> pending_;
    std::uint64_t merged_ = 0, stalls_ = 0, suppressed_ = 0;
    HourList hour_, copyHour_;
};

}  // namespace veyra::engine
