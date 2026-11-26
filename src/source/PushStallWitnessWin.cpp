// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "s10-stall": the Windows sampler of the capture-stall witness (see veyra/source/PushStallWitnessWin.h and
// veyra/engine/PushStallWitness.h). Everything here is readable by an ordinary user: no elevation, no system change, and
// (since review S10_STALL_REVIEW1 MUST 3) no handle to any other process.
// Always on while a capture card runs (the reviewer's minimal set):
//   - the capture callback's two thread-cycle reads and the copy watch (PushCopyWatch);
//   - PDH counters by their English names (PdhAddEnglishCounterW): per-processor "% DPC Time" / "% Interrupt Time", DPCs
//     queued and interrupts per second, context switches per second, page reads (hard faults) and pages input per second,
//     available / committed / standby / modified memory;
//   - this process's CPU time, faults and working set;
//   - other programs' CPU: ONE NtQuerySystemInformation(SystemProcessInformation) call a second (no handles; it also sees
//     SYSTEM services and gives image names and creation times); virtual-machine workers counted from the same names;
//   - CPUID (hypervisor), device-interface and power notifications, wall-clock jumps.
// Only with VEYRA_EXP_RP_STALL_WITNESS_DEEP=1 (environment only: not on slipstream.ini's allow-list; read once): the 10 ms normal-priority probe
// thread (a high-resolution timer re-armed below the clock tick keeps the system clock at its fastest rate while it runs)
// and the disk / network byte counters (the network one queries every adapter driver once a second).
// The sampler measures itself (samplerUs) and spaces the process call when it costs more (PushWitnessBudget).
// Unverified on Windows.
#include "veyra/source/PushStallWitnessWin.h"
#include "veyra/engine/PushStallWitness.h"
#include "veyra/engine/PushBoundedStop.h"
#include "veyra/engine/PushLegacyEnv.h"
#include "veyra/Log.h"
#include <windows.h>
#include <cfgmgr32.h>
#include <intrin.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <powersetting.h>
#include <psapi.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace veyra::source::stall_witness {
namespace {
using namespace veyra::engine;

double monoNowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::uint64_t fileTime100ns(const FILETIME& f) { return (std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
std::uint64_t systemTime100ns() {
    FILETIME ft{};
    GetSystemTimePreciseAsFileTime(&ft);
    return fileTime100ns(ft);
}
std::int64_t wallNowMs() { return static_cast<std::int64_t>((systemTime100ns() - 116444736000000000ull) / 10000ull); }

bool deepOn() {
    static const bool on = [] {
        wchar_t value[8]{};
        const DWORD n = GetEnvironmentVariableW(L"VEYRA_EXP_RP_STALL_WITNESS_DEEP", value, 8);
        return n == 1 && value[0] == L'1';
    }();
    return on;
}

// --- state shared with the capture and engine threads: atomics only ---------------------------------------------------
std::atomic<bool> g_active{false};
PushStallMailbox g_mailbox;
PushAtomicMax g_copyMax, g_pickupMax, g_probeLateMax;
std::atomic<std::uint32_t> g_events{0};
PushCopyWatch g_watch;   // capture callback thread only

// --- PDH ----------------------------------------------------------------------------------------------------------------
struct Pdh {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER dpcPct = nullptr, isrPct = nullptr, dpcRate = nullptr, isrRate = nullptr, ctx = nullptr, pageReads = nullptr, pagesIn = nullptr,
                 avail = nullptr, commit = nullptr, standbyCore = nullptr, standbyNormal = nullptr, standbyReserve = nullptr, modified = nullptr,
                 disk = nullptr, net = nullptr;
    int added = 0, wanted = 0;
    std::vector<BYTE> buffer;   // grows only when PDH asks for more (rare; not per sample once sized)
    PDH_HCOUNTER add(const wchar_t* path) {
        ++wanted;
        PDH_HCOUNTER c = nullptr;
        if (query && PdhAddEnglishCounterW(query, path, 0, &c) == ERROR_SUCCESS) {
            ++added;
            return c;
        }
        return nullptr;
    }
    bool open(bool deep) {
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) { query = nullptr; return false; }
        dpcPct = add(L"\\Processor(*)\\% DPC Time");
        isrPct = add(L"\\Processor(*)\\% Interrupt Time");
        dpcRate = add(L"\\Processor(_Total)\\DPCs Queued/sec");
        isrRate = add(L"\\Processor(_Total)\\Interrupts/sec");
        ctx = add(L"\\System\\Context Switches/sec");
        pageReads = add(L"\\Memory\\Page Reads/sec");
        pagesIn = add(L"\\Memory\\Pages Input/sec");
        avail = add(L"\\Memory\\Available MBytes");
        commit = add(L"\\Memory\\Committed Bytes");
        standbyCore = add(L"\\Memory\\Standby Cache Core Bytes");
        standbyNormal = add(L"\\Memory\\Standby Cache Normal Priority Bytes");
        standbyReserve = add(L"\\Memory\\Standby Cache Reserve Bytes");
        modified = add(L"\\Memory\\Modified Page List Bytes");
        if (deep) {
            disk = add(L"\\PhysicalDisk(_Total)\\Disk Bytes/sec");
            net = add(L"\\Network Interface(*)\\Bytes Total/sec");
        }
        buffer.resize(16384);
        PdhCollectQueryData(query);   // rates need two collections
        return true;
    }
    void close() {
        if (query) PdhCloseQuery(query);
        query = nullptr;
    }
    static double value(PDH_HCOUNTER c) {
        if (!c) return -1;
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS) return -1;
        if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) return -1;
        return v.doubleValue;
    }
    // Per-instance values of a wildcard counter: the largest processor value (and which processor), the "_Total" value, or
    // the sum of all instances (sum = true). No instance name is kept.
    bool array(PDH_HCOUNTER c, double& maxValue, int& maxIndex, double& total, bool sum) {
        maxValue = -1; maxIndex = -1; total = -1;
        if (!c) return false;
        DWORD bytes = static_cast<DWORD>(buffer.size()), items = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &items, reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data()));
        if (st == static_cast<PDH_STATUS>(PDH_MORE_DATA)) {
            buffer.resize(bytes + 1024);
            bytes = static_cast<DWORD>(buffer.size());
            st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &items, reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data()));
        }
        if (st != ERROR_SUCCESS) return false;
        const auto* list = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        double acc = 0;
        for (DWORD i = 0; i < items; ++i) {
            const auto& it = list[i];
            if (it.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            const bool isTotal = it.szName && std::wcscmp(it.szName, L"_Total") == 0;
            if (isTotal) { if (!sum) total = it.FmtValue.doubleValue; continue; }
            acc += it.FmtValue.doubleValue;
            if (it.FmtValue.doubleValue > maxValue) {
                maxValue = it.FmtValue.doubleValue;
                maxIndex = it.szName ? _wtoi(it.szName) : -1;
            }
        }
        if (sum) total = acc;
        return true;
    }
};

// --- the process list without handles: NtQuerySystemInformation(SystemProcessInformation = 5) -------------------------------
using NtQuerySystemInformationFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
NtQuerySystemInformationFn ntQuery() {
    static const NtQuerySystemInformationFn fn = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtQuerySystemInformationFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQuerySystemInformation"))) : nullptr;
    }();
    return fn;
}
constexpr LONG kStatusInfoLengthMismatch = -1073741820;   // STATUS_INFO_LENGTH_MISMATCH (0xC0000004)

// --- notifications (system thread-pool callbacks: set bits only) --------------------------------------------------------
// CM_Register_Notification and DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS are in the Windows SDK (Windows 8 and later) but not in
// mingw-w64's headers, which the Linux syntax check uses: there the notifications compile out.
#if defined(_MSC_VER)
std::atomic<double> g_registeredAtMs{0};
DWORD CALLBACK deviceCallback(HCMNOTIFICATION, PVOID, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD) {
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) g_events.fetch_or(kWitnessDeviceArrival, std::memory_order_relaxed);
    else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) g_events.fetch_or(kWitnessDeviceRemoval, std::memory_order_relaxed);
    return ERROR_SUCCESS;
}
// {5D3E9A59-E9D5-4B00-A6BD-FF34FF516548}, {245D8541-3943-4422-B025-13A784F679B7}, {6FE69556-704A-47A0-8F24-C28D936FDA47}
const GUID kAcDcSource{0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};
const GUID kSchemePersonality{0x245D8541, 0x3943, 0x4422, {0xB0, 0x25, 0x13, 0xA7, 0x84, 0xF6, 0x79, 0xB7}};
const GUID kConsoleDisplay{0x6FE69556, 0x704A, 0x47A0, {0x8F, 0x24, 0xC2, 0x8D, 0x93, 0x6F, 0xDA, 0x47}};
ULONG CALLBACK powerCallback(PVOID, ULONG type, PVOID setting) {
    // Registration delivers the current value at once: ignore the first two seconds.
    if (type != PBT_POWERSETTINGCHANGE || !setting || monoNowMs() - g_registeredAtMs.load(std::memory_order_relaxed) < 2000.0) return ERROR_SUCCESS;
    const auto* s = static_cast<const POWERBROADCAST_SETTING*>(setting);
    if (IsEqualGUID(s->PowerSetting, kAcDcSource)) g_events.fetch_or(kWitnessPowerSource, std::memory_order_relaxed);
    else if (IsEqualGUID(s->PowerSetting, kSchemePersonality)) g_events.fetch_or(kWitnessPowerScheme, std::memory_order_relaxed);
    else if (IsEqualGUID(s->PowerSetting, kConsoleDisplay)) g_events.fetch_or(kWitnessDisplayState, std::memory_order_relaxed);
    return ERROR_SUCCESS;
}
#endif

PushHypervisorInfo readHypervisor() {
    int r[4]{};
    __cpuid(r, 1);
    const auto ecx = static_cast<std::uint32_t>(r[2]);
    if (!((ecx >> 31) & 1u)) return pushDecodeHypervisor(ecx, 0, 0, 0);
    __cpuid(r, 0x40000000);
    const auto maxLeaf = static_cast<std::uint32_t>(r[0]);
    std::uint32_t iface = 0, priv = 0;
    if (maxLeaf >= 0x40000001u) { __cpuid(r, 0x40000001); iface = static_cast<std::uint32_t>(r[0]); }
    if (maxLeaf >= 0x40000003u) { __cpuid(r, 0x40000003); priv = static_cast<std::uint32_t>(r[1]); }
    return pushDecodeHypervisor(ecx, maxLeaf, iface, priv);
}

class Sampler {
public:
    void run(HANDLE stop) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        deep_ = deepOn();
        pdh_.open(deep_);
        hv_ = readHypervisor();
        procBuffer_.resize(1u << 20);
        int deviceOk = 0, powerOk = 0;
#if defined(_MSC_VER)
        CM_NOTIFY_FILTER filter{};
        filter.cbSize = sizeof filter;
        filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;
        filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        HCMNOTIFICATION device = nullptr;
        if (CM_Register_Notification(&filter, nullptr, deviceCallback, &device) != CR_SUCCESS) device = nullptr;
        g_registeredAtMs.store(monoNowMs(), std::memory_order_relaxed);
        DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS params{};
        params.Callback = powerCallback;
        params.Context = nullptr;
        std::array<HPOWERNOTIFY, 3> power{};
        const std::array<const GUID*, 3> guids{&kAcDcSource, &kSchemePersonality, &kConsoleDisplay};
        for (std::size_t i = 0; i < power.size(); ++i) {
            HPOWERNOTIFY h = nullptr;
            if (PowerSettingRegisterNotification(guids[i], DEVICE_NOTIFY_CALLBACK, reinterpret_cast<HANDLE>(&params), &h) == ERROR_SUCCESS) { power[i] = h; ++powerOk; }
        }
        deviceOk = device ? 1 : 0;
#endif
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        cpus_ = static_cast<int>(si.dwNumberOfProcessors);
        log::info("capture-stall-cause",
                  "witness=on deep=" + std::to_string(deep_ ? 1 : 0) + " hvPresent=" + std::to_string(hv_.present ? 1 : 0) +
                      " hvInterface=" + std::to_string(hv_.microsoftInterface ? 1 : 0) + " hvCreatePartitions=" + std::to_string(hv_.createPartitions ? 1 : 0) +
                      " counters=" + std::to_string(pdh_.added) + "/" + std::to_string(pdh_.wanted) + " processList=" + std::to_string(ntQuery() ? 1 : 0) +
                      " deviceNotify=" + std::to_string(deviceOk) + " powerNotify=" + std::to_string(powerOk) + " cpus=" + std::to_string(cpus_) +
                      " (stream s10-stall: while the capture card runs, system counters are read once a second and written around a capture "
                      "stall. PRIVATE: at each stall this log also gets [stall-witness] lines that NAME the programs that used the processor "
                      "most; the test kit leaves those lines out of what it collects unless asked. VEYRA_TEST_RP_LEGACY_STALL_WITNESS=1 "
                      "turns the witness off)");
        lastMono_ = monoNowMs();
        wallOffset_ = double(wallNowMs()) - lastMono_;
        std::int64_t second = 0;
        while (WaitForSingleObject(stop, 1000) == WAIT_TIMEOUT) {
            second_ = second;
            sample(second++);
            core_.takeFrom(g_mailbox);
            if (core_.ready(monoNowMs())) write();
        }
#if defined(_MSC_VER)
        for (auto h : power)
            if (h) PowerSettingUnregisterNotification(h);
        if (device) CM_Unregister_Notification(device);
#endif
        pdh_.close();
    }
    std::atomic<bool> probeOk{false};

private:
    void sample(std::int64_t second) {
        const double t0 = monoNowMs();
        PushWitnessSample s;
        s.monoMs = t0;
        s.wallMs = wallNowMs();
        s.spanMs = t0 - lastMono_;
        s.cpus = cpus_;
        if (pdh_.query && PdhCollectQueryData(pdh_.query) == ERROR_SUCCESS) {
            pdh_.array(pdh_.dpcPct, s.dpcPctMax, s.dpcCpu, s.dpcPctTotal, false);
            pdh_.array(pdh_.isrPct, s.isrPctMax, s.isrCpu, s.isrPctTotal, false);
            s.dpcPerSec = Pdh::value(pdh_.dpcRate);
            s.isrPerSec = Pdh::value(pdh_.isrRate);
            s.ctxPerSec = Pdh::value(pdh_.ctx);
            s.hardFaultsPerSec = Pdh::value(pdh_.pageReads);
            s.pagesInPerSec = Pdh::value(pdh_.pagesIn);
            s.availMb = Pdh::value(pdh_.avail);
            const double commit = Pdh::value(pdh_.commit);
            s.commitMb = commit >= 0 ? commit / 1048576.0 : -1;
            const double a = Pdh::value(pdh_.standbyCore), b = Pdh::value(pdh_.standbyNormal), c = Pdh::value(pdh_.standbyReserve);
            s.standbyMb = (a >= 0 && b >= 0 && c >= 0) ? (a + b + c) / 1048576.0 : -1;
            const double m = Pdh::value(pdh_.modified);
            s.modifiedMb = m >= 0 ? m / 1048576.0 : -1;
            if (deep_) {
                const double d = Pdh::value(pdh_.disk);
                s.diskMbps = d >= 0 ? d / 1048576.0 : -1;
                double netMax = -1, netSum = -1;
                int netIndex = -1;
                if (pdh_.array(pdh_.net, netMax, netIndex, netSum, true)) s.netMbps = netSum / 1048576.0;
            }
        }
        // This process.
        FILETIME c{}, e{}, k{}, u{};
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
            const std::uint64_t cpu = fileTime100ns(k) + fileTime100ns(u);
            if (ownCpu_ && s.spanMs > 0) s.ownCpuPct = 100.0 * double(cpu - ownCpu_) / (s.spanMs * 10000.0);
            ownCpu_ = cpu;
        }
        PROCESS_MEMORY_COUNTERS pmc{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
            if (ownFaults_ && s.spanMs > 0) s.ownFaultsPerSec = double(pmc.PageFaultCount - ownFaults_) * 1000.0 / s.spanMs;
            ownFaults_ = pmc.PageFaultCount;
            s.ownWsMb = double(pmc.WorkingSetSize) / 1048576.0;
        }
        // Other programs, when the budget allows (normally every second).
        if (budget_.due(second)) {
            const double p0 = monoNowMs();
            processList(s);
            budget_.ran(second, (monoNowMs() - p0) * 1000.0);
        }
        // Hooks and notifications.
        s.captureCopyMaxMs = g_copyMax.takeMax(&s.captureCallbacks);
        s.enginePickupMaxMs = g_pickupMax.takeMax();
        s.normalWakeMaxMs = probeOk.load(std::memory_order_relaxed) ? g_probeLateMax.takeMax() : -1;
        s.events = g_events.exchange(0, std::memory_order_relaxed);
        const double offset = double(s.wallMs) - t0;
        if (std::abs(offset - wallOffset_) > 20.0) { s.timeJumpMs = offset - wallOffset_; s.events |= kWitnessTimeJump; }
        wallOffset_ = offset;
        lastMono_ = t0;
        s.samplerUs = (monoNowMs() - t0) * 1000.0;
        core_.addSample(s);
    }
    void processList(PushWitnessSample& s) {
        const auto query = ntQuery();
        if (!query) return;
        ULONG needed = 0;
        LONG status = query(5, procBuffer_.data(), static_cast<ULONG>(procBuffer_.size()), &needed);
        if (status == kStatusInfoLengthMismatch && needed > 0 && needed < (64u << 20)) {
            procBuffer_.resize(std::size_t(needed) + (256u << 10));   // grows rarely; not per sample once sized
            status = query(5, procBuffer_.data(), static_cast<ULONG>(procBuffer_.size()), &needed);
        }
        if (status < 0) return;
        const std::uint64_t now100ns = systemTime100ns();
        const double span100ns = lastPass100ns_ ? double(now100ns - lastPass100ns_) : 0.0;
        const auto own = static_cast<std::uint32_t>(GetCurrentProcessId());
        table_.begin();
        int count = 0, vms = 0;
        const std::uint64_t previous = lastPass100ns_;
        const std::size_t length = std::min<std::size_t>(procBuffer_.size(), needed ? std::size_t(needed) : procBuffer_.size());
        pushParseProcessInfo(procBuffer_.data(), length,
                             [&](std::uint32_t pid, std::uint64_t create, std::uint64_t cpu, std::uint32_t, const std::uint16_t* name, std::size_t chars, std::size_t index) {
                                 ++count;
                                 if (pid == 0) return;   // the idle process
                                 table_.setIndex(index);
                                 table_.add(pid, create, cpu, own, previous);
                                 std::array<char, 32> base{};
                                 pushNarrowImageName(name, chars, base);
                                 if (pushIsVmWorkerImage(base.data())) ++vms;
                             });
        lastPass100ns_ = now100ns;
        vmWorkers_ = vms;
        s.procsRead = count;
        s.procsUnreadable = 0;
        s.processListSkipped = false;
        if (span100ns <= 0) return;
        s.topCount = static_cast<int>(table_.topCount());
        for (std::size_t i = 0; i < table_.topCount(); ++i) {
            s.topPid[i] = table_.top(i).pid;
            s.topCreate[i] = table_.top(i).create;
            s.topCpuPct[i] = 100.0 * table_.top(i).cpu100ns / span100ns;   // percent of one processor
        }
        // The names of the few busiest, from the same buffer (a second walk, no system call).
        pushParseProcessInfo(procBuffer_.data(), length,
                             [&](std::uint32_t, std::uint64_t, std::uint64_t, std::uint32_t, const std::uint16_t* name, std::size_t chars, std::size_t index) {
                                 for (std::size_t i = 0; i < table_.topCount(); ++i)
                                     if (table_.top(i).index == index) pushNarrowImageName(name, chars, s.topName[i]);
                             });
        s.topOtherCpuPct = s.topCount > 0 ? s.topCpuPct[0] : 0.0;
    }
    void write() {
        PushWitnessContext context;
        context.hypervisor = hv_;
        context.vmWorkers = vmWorkers_;
        context.processListInterval = budget_.interval();
        context.processListOff = budget_.off(second_) ? 1 : 0;
        auto names = [](const PushWitnessSample& s, int j) -> const char* { return s.topName[std::size_t(j)][0] ? s.topName[std::size_t(j)].data() : "?"; };
        core_.emit(monoNowMs(), context, names, [](const char* family, const std::string& text) { log::info(family, text); });
    }

    Pdh pdh_;
    PushHypervisorInfo hv_;
    PushStallWitnessCore core_;
    PushWitnessBudget budget_;
    PushProcessCpuTable table_;
    std::vector<std::uint8_t> procBuffer_;
    std::uint64_t ownCpu_ = 0, lastPass100ns_ = 0;
    DWORD ownFaults_ = 0;
    double lastMono_ = 0, wallOffset_ = 0;
    std::int64_t second_ = 0;
    int cpus_ = -1, vmWorkers_ = -1;
    bool deep_ = false;
};

// The probe (only with VEYRA_EXP_RP_STALL_WITNESS_DEEP=1): a normal-priority thread that asks to wake every 10 ms on a
// high-resolution waitable timer and records how late it woke (the largest delay of each second). It stands for "could an
// ordinary thread such as the interface thread run". A wait that runs long is counted, never the end of the probe.
void probeLoop(HANDLE stop, std::atomic<bool>* ok) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) return;   // no high-resolution timer: the field stays unknown (-1)
    ok->store(true, std::memory_order_relaxed);
    const HANDLE handles[2] = {stop, timer};
    for (;;) {
        LARGE_INTEGER due{};
        due.QuadPart = -100000;   // 10 ms, relative
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) break;
        const double asked = monoNowMs();
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, 5000);
        if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
        const double late = monoNowMs() - asked - 10.0;
        g_probeLateMax.note(late > 0 ? late : 0.0);
    }
    ok->store(false, std::memory_order_relaxed);
    CancelWaitableTimer(timer);
    CloseHandle(timer);
}

// Never destroyed (review SHOULD 6): a source that is still running at process exit must not meet a joinable std::thread
// in a static destructor.
struct Runner {
    std::mutex mutex;   // acquire / release only (capture source start / close), never the frame path
    int users = 0;
    HANDLE stop = nullptr;
    std::thread sampler, probe;
    Sampler* state = nullptr;
    // Set by each thread as its last action (pushJoinWithin); heap, so a detached thread never writes freed memory.
    std::shared_ptr<std::atomic<bool>> samplerDone, probeDone;
};
// Release review REL16 SHOULD 1: a sampler that did not stop within kStopLimit (stuck inside PDH) switched the witness off
// for the rest of the process.
std::atomic<bool> g_stuck{false};
constexpr std::chrono::milliseconds kStopLimit{2000};
Runner& runner() {
    static Runner* r = new Runner();
    return *r;
}
}  // namespace

bool enabled() {
    static const bool on = !pushLegacyRestored(PushLegacyPart::StallWitness, pushEnvironment());
    return on && !g_stuck.load(std::memory_order_acquire);
}

void acquire() {
    if (!enabled()) return;
    auto& r = runner();
    std::lock_guard lock(r.mutex);
    if (r.users++ > 0) return;
    r.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!r.stop) { r.users = 0; return; }
    r.state = new Sampler();
    r.samplerDone = std::make_shared<std::atomic<bool>>(false);
    r.probeDone = std::make_shared<std::atomic<bool>>(false);
    try {
        if (deepOn()) r.probe = std::thread([stop = r.stop, ok = &r.state->probeOk, done = r.probeDone] { probeLoop(stop, ok); done->store(true, std::memory_order_release); });
        r.sampler = std::thread([stop = r.stop, state = r.state, done = r.samplerDone] { state->run(stop); done->store(true, std::memory_order_release); });
    } catch (...) {
        SetEvent(r.stop);
        if (r.probe.joinable()) r.probe.join();
        CloseHandle(r.stop);
        r.stop = nullptr;
        delete r.state;
        r.state = nullptr;
        r.users = 0;
        log::warn("capture-stall-cause", "witness=off reason=thread-start-failed");
        return;
    }
    g_active.store(true, std::memory_order_release);
}

void release() {
    if (!enabled()) return;
    auto& r = runner();
    std::lock_guard lock(r.mutex);
    if (r.users == 0 || --r.users > 0) return;
    g_active.store(false, std::memory_order_release);
    SetEvent(r.stop);
    // Bounded (REL16 SHOULD 1): the engine thread waits at most kStopLimit per thread. A thread that does not finish keeps
    // its Sampler and stop event for ever (leaked, never freed under it) and the witness is off for the process.
    const bool samplerStopped = pushJoinWithin(r.sampler, *r.samplerDone, kStopLimit);
    const bool probeStopped = pushJoinWithin(r.probe, *r.probeDone, kStopLimit);
    if (!samplerStopped || !probeStopped) {
        g_stuck.store(true, std::memory_order_release);
        log::warn("capture-stall-cause", std::format("witness=off reason=sampler-stuck (did not stop within {} ms: {}; its objects are kept, the witness is off for this run of PushFPS)",
                                                     kStopLimit.count(), !samplerStopped ? "sampler" : "probe"));
        r.stop = nullptr;
        r.state = nullptr;
        return;
    }
    CloseHandle(r.stop);
    r.stop = nullptr;
    delete r.state;
    r.state = nullptr;
}

std::uint64_t threadCycles() {
    if (!g_active.load(std::memory_order_relaxed)) return 0;
    ULONG64 cycles = 0;
    return QueryThreadCycleTime(GetCurrentThread(), &cycles) ? cycles : 0;
}

void onCallback(double arrivalDeltaMs, double ptsDeltaMs, double copyMs, std::uint64_t copyCycles, double intervalMs, std::uint64_t received, std::uint64_t bytes) {
    if (!g_active.load(std::memory_order_relaxed)) return;
    if (received <= 1) g_watch.reset();   // the source (re)started: forget the old copy baseline
    g_copyMax.note(copyMs);
    if (auto report = g_watch.observe(arrivalDeltaMs, ptsDeltaMs, copyMs, copyCycles, intervalMs, received, monoNowMs(), wallNowMs(), bytes)) g_mailbox.post(*report);
}

void onEnginePickup(double ms) {
    if (g_active.load(std::memory_order_relaxed)) g_pickupMax.note(ms);
}

}  // namespace veyra::source::stall_witness
