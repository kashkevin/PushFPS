// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "s10-stall": the capture-stall witness (engine/PushStallWitness.h): ring, selection, capture-side trigger, mailbox,
// budget, per-process CPU table, hypervisor decoding, line formats and the redaction rule (no names in the summary), and a
// headless run that injects a stall the way the owner's logs show it and checks the lines.
#include "veyra/engine/PushStallWitness.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace stall_witness_tests {
using veyra::engine::PushWitnessSample;
inline std::string field(const std::string& line, const std::string& key) {
    const auto at = line.find(" " + key + "=");
    const auto at0 = line.rfind(key + "=", 0);
    std::size_t start;
    if (at != std::string::npos) start = at + key.size() + 2;
    else if (at0 == 0) start = key.size() + 1;
    else return "<missing>";
    return line.substr(start, line.find(' ', start) - start);
}
inline PushWitnessSample quiet(double monoMs) {
    PushWitnessSample s;
    s.monoMs = monoMs;
    s.wallMs = 1764032105000 + static_cast<std::int64_t>(monoMs);
    s.spanMs = 1000;
    s.dpcPctMax = 0.4; s.dpcCpu = 2; s.isrPctMax = 0.2; s.isrCpu = 0; s.dpcPctTotal = 0.1; s.isrPctTotal = 0.05; s.cpus = 16;
    s.dpcPerSec = 900; s.isrPerSec = 4000; s.ctxPerSec = 30000; s.syscallsPerSec = 200000;
    s.hardFaultsPerSec = 0; s.pagesInPerSec = 0; s.availMb = 20000; s.commitMb = 18000; s.standbyMb = 9000; s.modifiedMb = 200;
    s.diskMbps = 0.5; s.netMbps = 0.1; s.ownCpuPct = 9; s.ownFaultsPerSec = 10; s.ownWsMb = 900;
    s.captureCallbacks = 30; s.captureCopyMaxMs = 1.2; s.enginePickupMaxMs = 1.3; s.normalWakeMaxMs = 0.6;
    s.topOtherCpuPct = 1.0; s.topCount = 2; s.topPid = {100, 200, 0, 0, 0}; s.topCreate = {11, 22, 0, 0, 0}; s.topCpuPct = {1.0, 0.5, 0, 0, 0};
    s.procsRead = 280; s.procsUnreadable = 0; s.processListSkipped = false; s.samplerUs = 400;
    return s;
}
}  // namespace stall_witness_tests

TEST(stall_witness_ring_keeps_the_newest_and_selects_by_time) {
    veyra::engine::PushWitnessRing<4> r;
    for (int i = 0; i < 6; ++i) r.push(stall_witness_tests::quiet(1000.0 * i));
    CHECK(r.size() == 4);
    CHECK(r.at(0).monoMs == 2000.0 && r.at(3).monoMs == 5000.0);
    std::array<std::size_t, 4> idx{};
    CHECK(r.select(2500.0, 4000.0, idx) == 2);
    CHECK(r.at(idx[0]).monoMs == 3000.0 && r.at(idx[1]).monoMs == 4000.0);
    std::array<std::size_t, 1> one{};
    CHECK(r.select(0.0, 1e9, one) == 1);   // bounded by the output
    r.clear();
    CHECK(r.size() == 0 && r.select(0.0, 1e9, idx) == 0);
}

TEST(stall_witness_copy_watch_reports_gaps_and_slow_copies_like_the_owners_log) {
    veyra::engine::PushCopyWatch w;
    double t = 0;
    for (int i = 0; i < 200; ++i, t += 33.34) CHECK(!w.observe(33.34, 33.338, 1.11, 3000000, 33.334, std::uint64_t(i), t, 0));
    CHECK(std::abs(w.normalMs() - 1.11) < 1e-6 && std::abs(w.normalCycles() - 3000000.0) < 1e-3);
    // 23:15:27.706: normal arrival, copy 9.01 ms, the thread's own cycles normal (preempted): a "copy" report.
    auto a = w.observe(35.5, 33.338, 9.01, 3100000, 33.334, 200, t, 1764032127706);
    CHECK(a && a->kind == 2 && a->gapMs == 0);
    CHECK(std::abs(a->copyWallRatio - 9.01 / 1.11) < 1e-6 && std::abs(a->copyCycleRatio - 3100000.0 / 3000000.0) < 1e-6);
    // The gap 134 ms later is reported although a copy report came just before it (review SHOULD 2).
    auto gapAfterCopy = w.observe(137.2, 33.338, 6.68, 3000000, 33.334, 201, t + 134, 0);
    CHECK(gapAfterCopy && gapAfterCopy->kind == 1);
    // A second gap within 1 s is not.
    CHECK(!w.observe(140.0, 33.338, 1.1, 3000000, 33.334, 202, t + 600, 0));
    // The slow copies did not raise the baseline.
    CHECK(std::abs(w.normalMs() - 1.11) < 1e-6);
    // A later gap with a normal copy (02:45:22.8, 88 ms) is reported as a gap.
    auto g = w.observe(87.6, 33.338, 1.12, 3000000, 33.334, 300, t + 5000, 0);   // 2.6 intervals
    CHECK(g && g->kind == 1 && std::abs(g->gapMs - 87.6) < 1e-9);
    // Without cycle counts the ratio is unknown, not zero.
    veyra::engine::PushCopyWatch n;
    for (int i = 0; i < 10; ++i) (void)n.observe(33.3, 33.3, 1.1, 0, 33.3, std::uint64_t(i), 33.3 * i, 0);
    auto u = n.observe(150, 33.3, 7.0, 0, 33.3, 10, 2000, 0);
    CHECK(u && u->copyCycleRatio < 0);
    // Before anything is learned only a gap counts.
    veyra::engine::PushCopyWatch f;
    CHECK(!f.observe(33.3, 33.3, 9.0, 1, 33.3, 1, 0, 0));
}

// Review MUST 1: the reviewer's scratch case (1.1 ms copies, then 2.4-3.5 ms for good) made 133 false stalls in 10 minutes.
TEST(stall_witness_copy_baseline_follows_a_lasting_change_and_resets) {
    veyra::engine::PushCopyWatch w;
    double t = 0;
    int reports = 0;
    std::uint32_t x = 12345;
    for (int i = 0; i < 300; ++i, t += 33.34) if (w.observe(33.34, 33.338, 1.1, 3000, 33.334, std::uint64_t(i), t, 0, 12441600)) ++reports;
    CHECK(reports == 0);
    for (int i = 0; i < 18000; ++i, t += 33.34) {   // 10 minutes
        x = x * 1103515245u + 12345u;
        const double copy = 2.4 + 1.1 * double((x >> 8) % 1000) / 1000.0;
        if (w.observe(33.34, 33.338, copy, 7000, 33.334, std::uint64_t(300 + i), t, 0, 12441600)) ++reports;
    }
    CHECK(reports <= 2);
    CHECK(w.normalMs() > 2.4 && w.normalMs() < 3.5);
    // A real stall on the new level is still seen.
    auto r = w.observe(130.0, 33.338, 9.5, 7000, 33.334, 99999, t + 2000, 0, 12441600);
    CHECK(r && r->kind == 1);
    // Another frame size forgets the baseline; a restart (reset) too.
    (void)w.observe(33.34, 33.338, 0.6, 1000, 33.334, 1, t + 4000, 0, 6220800);
    CHECK(w.normalMs() == 0);
    for (int i = 0; i < 20; ++i) (void)w.observe(33.34, 33.338, 0.6, 1000, 33.334, std::uint64_t(i), t + 4100 + 33.34 * i, 0, 6220800);
    CHECK(std::abs(w.normalMs() - 0.6) < 1e-9);
    w.reset();
    CHECK(w.normalMs() == 0 && w.normalCycles() == 0);
}

TEST(stall_witness_core_gap_replaces_copy_and_quotas_are_separate) {
    veyra::engine::PushStallWitnessCore core;
    veyra::engine::PushStallMailbox box;
    std::vector<std::string> causes;
    auto sink = [&causes](const char* family, const std::string& text) { if (std::string(family) == "capture-stall-cause") causes.push_back(text); };
    auto names = [](const veyra::engine::PushWitnessSample&, int) { return "x"; };
    veyra::engine::PushStallReport copy;
    copy.monoMs = 1000; copy.kind = 2; copy.copyMs = 9.0;
    box.post(copy);
    core.takeFrom(box);
    veyra::engine::PushStallReport gap = copy;
    gap.monoMs = 1150; gap.kind = 1; gap.gapMs = 137.2;
    box.post(gap);
    core.takeFrom(box);
    core.addSample(stall_witness_tests::quiet(4200));
    CHECK(core.emit(4200, veyra::engine::PushWitnessContext{}, names, sink));
    CHECK(causes.size() == 1 && causes[0].find("kind=gap") != std::string::npos && causes[0].find("merged=1") != std::string::npos);
    // Copy-only reports: at most kMaxCopyPerHour, and they never use up the gap quota.
    double t = 10000;
    for (int i = 0; i < 30; ++i, t += 10000) {
        veyra::engine::PushStallReport c;
        c.monoMs = t; c.kind = 2;
        box.post(c);
        core.takeFrom(box);
        core.emit(t + 3000, veyra::engine::PushWitnessContext{}, names, sink);
    }
    CHECK(causes.size() == 1 + std::size_t(veyra::engine::PushStallWitnessCore::kMaxCopyPerHour));
    veyra::engine::PushStallReport late = gap;
    late.monoMs = t + 5000;
    box.post(late);
    core.takeFrom(box);
    CHECK(core.emit(t + 8000, veyra::engine::PushWitnessContext{}, names, sink));
}

TEST(stall_witness_process_list_without_handles) {
    // A synthetic SystemProcessInformation buffer: three entries, x64 layout.
    using L = veyra::engine::PushProcessInfoLayout;
    std::vector<std::uint8_t> buf(3 * 160, 0);
    const std::u16string names[3] = {u"", u"System", u"C:\\Tools\\job tool.exe"};
    auto put32 = [&buf](std::size_t at, std::uint32_t v) { std::memcpy(buf.data() + at, &v, 4); };
    auto put64 = [&buf](std::size_t at, std::uint64_t v) { std::memcpy(buf.data() + at, &v, 8); };
    for (std::size_t k = 0; k < 3; ++k) {
        const std::size_t e = k * 160;
        put32(e + L::kNext, k < 2 ? 160u : 0u);
        put32(e + L::kHardFaults, std::uint32_t(10 * k));
        put64(e + L::kCreate, 1000 + k);
        put64(e + L::kUser, 100 * k);
        put64(e + L::kKernel, 50 * k);
        put64(e + L::kPid, k == 0 ? 0 : 4 * k);
        const std::size_t nameAt = e + 96;
        std::memcpy(buf.data() + nameAt, names[k].data(), names[k].size() * 2);
        const std::uint16_t bytes = std::uint16_t(names[k].size() * 2);
        std::memcpy(buf.data() + e + L::kNameLength, &bytes, 2);
        put64(e + L::kNameBuffer, std::uint64_t(reinterpret_cast<std::uintptr_t>(buf.data() + nameAt)));
    }
    std::vector<std::string> seen;
    std::uint64_t cpuSum = 0;
    const std::size_t n = veyra::engine::pushParseProcessInfo(buf.data(), buf.size(), [&](std::uint32_t pid, std::uint64_t create, std::uint64_t cpu, std::uint32_t faults,
                                                                                           const std::uint16_t* name, std::size_t chars, std::size_t index) {
        std::array<char, 32> base{};
        veyra::engine::pushNarrowImageName(name, chars, base);
        seen.push_back(std::string(base.data()) + "/" + std::to_string(pid) + "/" + std::to_string(create) + "/" + std::to_string(faults) + "/" + std::to_string(index));
        cpuSum += cpu;
    });
    CHECK(n == 3);
    CHECK(seen[0] == "Idle/0/1000/0/0" && seen[1] == "System/4/1001/10/1" && seen[2] == "job_tool.exe/8/1002/20/2");
    CHECK(cpuSum == 450);
    // A broken offset or a name pointer outside the buffer never reads outside it.
    std::memcpy(buf.data() + 160 + L::kNext, "\xff\xff\x00\x00", 4);
    std::size_t names2 = 0;
    const std::size_t m = veyra::engine::pushParseProcessInfo(buf.data(), buf.size(), [&](std::uint32_t, std::uint64_t, std::uint64_t, std::uint32_t, const std::uint16_t* name, std::size_t, std::size_t) {
        if (name) ++names2;
    });
    CHECK(m == 2 && names2 == 2);
    CHECK(veyra::engine::pushParseProcessInfo(nullptr, 100, [](std::uint32_t, std::uint64_t, std::uint64_t, std::uint32_t, const std::uint16_t*, std::size_t, std::size_t) {}) == 0);
    // A process started since the previous pass counts with all of its CPU time (review SHOULD 1).
    veyra::engine::PushProcessCpuTable t;
    t.begin();
    t.add(10, 1, 1000, 99, 0);
    t.begin();
    t.setIndex(7);
    CHECK(t.add(50, 5000, 3000, 99, 4000) == 3000);
    CHECK(t.add(60, 3000, 3000, 99, 4000) < 0);   // older than the previous pass and not seen there
    CHECK(t.topCount() == 1 && t.top(0).pid == 50 && t.top(0).index == 7);
}

TEST(stall_witness_mailbox_hands_over_the_newest_report_once) {
    veyra::engine::PushStallMailbox box;
    CHECK(!box.take());
    veyra::engine::PushStallReport r;
    r.monoMs = 5; r.kind = 1; r.gapMs = 120; r.received = 7;
    box.post(r);
    r.gapMs = 140; r.received = 8;
    box.post(r);
    auto got = box.take();
    CHECK(got && got->gapMs == 140 && got->received == 8);
    CHECK(!box.take());
    CHECK(box.posted() == 2);
    // A writer thread and a reader thread: every report read is one that was written whole (gap = 10 x received).
    veyra::engine::PushStallMailbox shared;
    std::thread writer([&shared] {
        for (std::uint64_t i = 1; i <= 20000; ++i) {
            veyra::engine::PushStallReport x;
            x.received = i; x.gapMs = 10.0 * double(i); x.monoMs = double(i);
            shared.post(x);
        }
    });
    std::uint64_t last = 0;
    bool consistent = true;
    for (int k = 0; k < 200000 && last < 20000; ++k)
        if (auto x = shared.take()) {
            consistent = consistent && x->gapMs == 10.0 * double(x->received) && x->monoMs == double(x->received) && x->received > last;
            last = x->received;
        }
    writer.join();
    if (auto x = shared.take()) { consistent = consistent && x->gapMs == 10.0 * double(x->received); last = x->received; }
    CHECK(consistent);
    CHECK(last == 20000);
}

TEST(stall_witness_atomic_max_reads_and_clears) {
    veyra::engine::PushAtomicMax m;
    double n = 0;
    CHECK(m.takeMax(&n) < 0 && n == 0);
    m.note(1.2); m.note(7.5); m.note(3.0);
    CHECK(m.takeMax(&n) == 7.5 && n == 3);
    CHECK(m.takeMax() < 0);
}

TEST(stall_witness_budget_spaces_out_an_expensive_process_list) {
    veyra::engine::PushWitnessBudget b;
    CHECK(b.due(0));
    b.ran(0, 400);                 // cheap: every second
    CHECK(b.interval() == 1 && b.due(1));
    b.ran(1, 2000);                // average 800 us -> every 2 s
    CHECK(b.interval() == 2 && !b.due(2) && b.due(3));
    for (int s = 3, k = 0; k < 30; ++k, s += b.interval()) b.ran(s, 5000);   // 5 ms a pass: spaced to <= 700 us a second
    CHECK(b.interval() == 8);
    b.ran(50, 25000);              // over the hard cap: off for ten minutes
    CHECK(b.off(51) && !b.due(100) && b.timesDisabled() == 1);
    CHECK(!b.off(50 + 600) && b.due(50 + 600));
}

TEST(stall_witness_process_table_gives_deltas_and_the_busiest_others) {
    veyra::engine::PushProcessCpuTable t;
    t.begin();
    CHECK(t.add(10, 1, 1000, 99) < 0);   // first pass: no deltas
    t.add(20, 2, 5000, 99);
    t.add(99, 3, 7000, 99);              // our own process
    t.add(30, 4, 100, 99);
    CHECK(t.topCount() == 0);
    t.begin();
    CHECK(t.add(10, 1, 3000, 99) == 2000);
    CHECK(t.add(20, 2, 5100, 99) == 100);
    CHECK(t.add(99, 3, 107000, 99) == 100000);   // counted in the total, never in the top list
    CHECK(t.add(30, 5, 900, 99) < 0);            // pid reused by a new process (other creation time): no delta
    t.add(40, 6, 50, 99);                        // new process
    CHECK(t.topCount() == 2 && t.top(0).pid == 10 && t.top(1).pid == 20);
    CHECK(t.total100ns() == 102100);
    // More than five: only the five largest stay, in order.
    t.begin();
    for (std::uint32_t p = 1; p <= 9; ++p) t.add(p * 1000, 1, 0, 99);
    t.begin();
    for (std::uint32_t p = 1; p <= 9; ++p) t.add(p * 1000, 1, p * 10, 99);
    CHECK(t.topCount() == 5 && t.top(0).pid == 9000 && t.top(4).pid == 5000);
}

TEST(stall_witness_hypervisor_and_vm_worker_names) {
    using veyra::engine::pushDecodeHypervisor;
    auto none = pushDecodeHypervisor(0x7FFFFFFFu, 0x40000006u, 0x31237648u, 1);
    CHECK(!none.present && !none.microsoftInterface && !none.createPartitions);
    auto guest = pushDecodeHypervisor(0x80000000u, 0x4000000Bu, 0x31237648u, 0);
    CHECK(guest.present && guest.microsoftInterface && !guest.createPartitions);
    auto host = pushDecodeHypervisor(0x80000000u, 0x4000000Bu, 0x31237648u, 0x1u);
    CHECK(host.present && host.microsoftInterface && host.createPartitions);
    auto other = pushDecodeHypervisor(0x80000000u, 0x40000010u, 0x12345678u, 1);
    CHECK(other.present && !other.microsoftInterface && !other.createPartitions);
    CHECK(veyra::engine::pushIsVmWorkerImage("vmmem"));
    CHECK(veyra::engine::pushIsVmWorkerImage("VmWp.EXE"));
    CHECK(veyra::engine::pushIsVmWorkerImage("vmmemWSL"));
    CHECK(!veyra::engine::pushIsVmWorkerImage("PushFPS.exe"));
    CHECK(!veyra::engine::pushIsVmWorkerImage(nullptr));
    char out[32];
    veyra::engine::pushImageBaseName("C:\\Program Files\\Some Tool\\tool name=x,y:z.exe", out);
    CHECK(std::string(out) == "tool_name_x_y_z.exe");
    veyra::engine::pushImageBaseName("averyveryveryveryveryverylongname.exe", out);
    CHECK(std::string(out).size() == 31);
}

// The headless "injected stall": a quiet machine, then a second with a DPC storm on one processor, slow copies and a
// late Present-style wake, reported by the capture side as the owner's 23:15:27 pair; the lines must name the storm,
// put names only in the private family, and keep the summary free of names.
TEST(stall_witness_injected_stall_writes_the_private_ring_and_a_nameless_summary) {
    using namespace stall_witness_tests;
    veyra::engine::PushStallWitnessCore core;
    veyra::engine::PushStallMailbox box;
    veyra::engine::PushCopyWatch watch;
    std::vector<std::pair<std::string, std::string>> lines;
    auto sink = [&lines](const char* family, const std::string& text) { lines.emplace_back(family, text); };
    const char* privateName = "PlantedProcessName.exe";
    auto names = [privateName](const PushWitnessSample& s, int i) { return s.topPid[std::size_t(i)] == 777 ? privateName : "other.exe"; };
    veyra::engine::PushWitnessContext ctx;
    ctx.hypervisor = veyra::engine::pushDecodeHypervisor(0x80000000u, 0x4000000Bu, 0x31237648u, 1);
    ctx.vmWorkers = 2;
    // Capture side learns its normal copy over 20 s while the sampler fills the ring.
    double t = 0;
    for (int i = 0; i < 600; ++i, t += 33.34) {
        if (auto r = watch.observe(33.34, 33.338, 1.11, 3000000, 33.334, std::uint64_t(i), t, 0)) box.post(*r);
        if (i % 30 == 29) { core.addSample(quiet(t)); core.takeFrom(box); }
    }
    CHECK(!core.pending());
    // The stall second: the sample that covers it shows the storm on processor 5 and a busy other process.
    const double stallAt = t;
    auto r = watch.observe(137.2, 33.338, 6.68, 3050000, 33.334, 600, stallAt, 1764032127840);
    CHECK(r.has_value());
    box.post(*r);
    for (int k = 1; k <= 4; ++k) {
        auto s = quiet(stallAt + 1000.0 * k - 500.0);
        if (k == 1) {
            s.dpcPctMax = 38.0; s.dpcCpu = 5; s.dpcPerSec = 9000; s.ctxPerSec = 52000; s.normalWakeMaxMs = 9.5; s.enginePickupMaxMs = 8.4;
            s.topOtherCpuPct = 61.0; s.topCount = 1; s.topPid = {777, 0, 0, 0, 0}; s.topCpuPct = {61.0, 0, 0, 0, 0}; s.events = veyra::engine::kWitnessDeviceArrival;
        }
        core.addSample(s);
        core.takeFrom(box);
        CHECK(core.pending());
        // A second report during the wait is merged, not written twice.
        if (k == 2) { veyra::engine::PushStallReport extra = *r; extra.monoMs += 1500; box.post(extra); }
        const bool wrote = core.emit(s.monoMs, ctx, names, sink);
        CHECK(wrote == (k == 4));
    }
    CHECK(!core.pending() && core.stalls() == 1);
    std::size_t witness = 0;
    std::string cause;
    for (const auto& [family, text] : lines) {
        if (family == std::string("stall-witness")) ++witness;
        else if (family == std::string("capture-stall-cause")) cause = text;
    }
    CHECK(witness >= 7 && witness <= 9);       // 5 s before + 3 s after, one a second
    CHECK(!cause.empty());
    CHECK(field(cause, "stall") == "1" && field(cause, "kind") == "gap" && field(cause, "gapMs") == "137.2");
    CHECK(field(cause, "dpcPct") == "38.0" && field(cause, "dpcCpu") == "5" && field(cause, "dpcPctBase") == "0.4");
    CHECK(field(cause, "uiWakeMs") == "9.50" && field(cause, "engineWakeMs") == "8.40" && field(cause, "topOtherCpuPct") == "61.0");
    CHECK(field(cause, "events") == "0x1" && field(cause, "hvPresent") == "1" && field(cause, "hvCreatePartitions") == "1");
    CHECK(cause.find("clockMs") == std::string::npos && cause.find("uncalibrated") != std::string::npos);
    CHECK(field(cause, "vmWorkers") == "2" && field(cause, "merged") == "1");
    // captureThreadRanPct: cycles ~normal over a 6x longer copy = the thread ran ~17 % of that time (it was preempted).
    CHECK(field(cause, "captureThreadRanPct") == "17");
    // Redaction: the summary carries no image name; the private family does.
    CHECK(cause.find(privateName) == std::string::npos && cause.find("other.exe") == std::string::npos && cause.find(".exe") == std::string::npos);
    bool named = false;
    for (const auto& [family, text] : lines)
        if (family == std::string("stall-witness") && text.find(std::string(privateName) + ":61.0") != std::string::npos) named = true;
    CHECK(named);
    CHECK(lines.front().second.rfind("stall=1 dt=-", 0) == 0);
}

TEST(stall_witness_core_limits_lines_per_hour) {
    veyra::engine::PushStallWitnessCore core;
    veyra::engine::PushStallMailbox box;
    std::size_t written = 0;
    auto sink = [&written](const char* family, const std::string&) { if (std::string(family) == "capture-stall-cause") ++written; };
    auto names = [](const veyra::engine::PushWitnessSample&, int) { return "x"; };
    for (int i = 0; i < 60; ++i) {
        veyra::engine::PushStallReport r;
        r.monoMs = 10000.0 * i; r.kind = 1;
        box.post(r);
        core.takeFrom(box);
        core.addSample(stall_witness_tests::quiet(r.monoMs + 3000));
        CHECK(core.ready(r.monoMs + 3000));
        core.emit(r.monoMs + 3000, veyra::engine::PushWitnessContext{}, names, sink);
    }
    CHECK(written == std::size_t(veyra::engine::PushStallWitnessCore::kMaxPerHour));
    CHECK(core.suppressed() == 20);
}
