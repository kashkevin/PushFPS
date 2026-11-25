// SPDX-License-Identifier: GPL-3.0-only
// Stream "laghunt" (Package 13 felt laggier than 12A): statistics switches must never change which frame is shown or
// when, the always-on [slipstream-sizes] line must stay quiet when a value flaps, and a neural runtime file must never
// turn a saved neural layer on by itself. Included from CoreTests.cpp.
#include "veyra/remoteplay/FrameDelivery.h"
#include "veyra/engine/PushSizes.h"
#include "veyra/engine/PushNrRuntime.h"
#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>
namespace {
// One decoded frame as RemotePlaySessionSource hands it on: sequence and flags (bit 0: Drop, bit 1: a repeated picture
// of a 30 FPS game in a 60 FPS stream, bit 2: discontinuity).
struct HuntFrame { std::uint64_t seq = 0; unsigned flags = 0; HostTime decoded = 0; };
void huntMerge(HuntFrame& dropped, HuntFrame& kept) { kept.flags |= dropped.flags | 1u; }

// The one-slot mailbox as RemotePlaySessionSource::publishDecoded / read() do it (VEYRA_EXP_RP_STATS unset). This is a
// COPY of those few lines, not the production code: the production one-slot path lives inside the Windows session object
// (its mutex, AVFrame ownership, the frame event) and cannot be driven without it. The two-slot side below is the real
// DeliverySlots / DeliveryStats / CatchUpGuard code. Keep this copy equal to publishDecoded / read() when they change.
struct OneSlotPath {
    std::optional<HuntFrame> latest; std::uint64_t skipped = 0; bool event = false; unsigned wakes = 0;
    void publish(HuntFrame f, HostTime) { if (latest) { ++skipped; huntMerge(*latest, f); } latest = f; event = true; ++wakes; }
    std::optional<HuntFrame> read(HostTime) { auto out = latest; latest.reset(); return out; }
};
// The two-slot path as publishDelivery / readDelivery with the Newest policy (VEYRA_EXP_RP_STATS=1 selects it), with
// or without the statistics being recorded (they are recorded after the lock since stream "laghunt").
struct TwoSlotPath {
    explicit TwoSlotPath(bool statsOn) : stats(statsOn) {}
    DeliverySlots<HuntFrame> slots{DeliveryPolicy::Newest}; CatchUpGuard guard; DeliveryStats statistics;
    bool stats; std::uint64_t skipped = 0, lines = 0; bool event = false; unsigned wakes = 0;
    void publish(HuntFrame f, HostTime at) {
        const bool replaced = slots.publish(f, at, huntMerge);
        if (replaced) ++skipped;
        event = true; ++wakes;
        if (stats) statistics.published(at, f.decoded, replaced);
    }
    std::optional<HuntFrame> read(HostTime now) {
        DeliveryContext context; context.refreshHz = 180; context.sourceFps = 59.94; context.frameGeneration = true;
        if (guard.awaitingFollow()) guard.followed(now, context.refreshHz, context.sourceFps);
        auto taken = slots.take(now, context, guard.allows(now), huntMerge);
        if (!taken) return std::nullopt;
        if (taken->replaced) ++skipped;
        if (taken->caughtUp) guard.caughtUp(now);
        if (taken->morePending) { event = true; ++wakes; }
        if (stats) {
            if (taken->value.flags & 4u) statistics.consumerRestart();
            statistics.took(now, taken->value.decoded, taken->replaced, taken->caughtUp);
            if (statistics.due(now, DeliveryPolicy::Newest)) ++lines;
        }
        return taken->value;
    }
};
struct HuntPickup { HostTime at = 0; std::uint64_t seq = 0; unsigned flags = 0; bool operator==(const HuntPickup&) const = default; };
struct HuntResult { std::vector<HuntPickup> pickups; std::uint64_t skipped = 0; unsigned wakes = 0; };
// Decoder arrivals (time, repeat flag) against an engine that waits on the frame event (4 ms timeout), reads, works on
// the frame for work(seq) and reads again at once. Steps of 0.1 ms.
template<class Path, class Work>
HuntResult huntRun(Path& path, const std::vector<std::pair<HostTime, unsigned>>& arrivals, HostTime end, Work work) {
    HuntResult r; std::size_t next = 0; std::uint64_t seq = 0;
    HostTime busyUntil = 0, waitUntil = 0; bool waiting = false;
    for (HostTime t = 0; t <= end; t += 1000) {
        while (next < arrivals.size() && arrivals[next].first <= t) {
            HuntFrame f; f.seq = ++seq; f.flags = arrivals[next].second; f.decoded = arrivals[next].first;
            path.publish(f, t); ++next;
        }
        if (t < busyUntil) continue;
        if (waiting) {
            if (!path.event && t < waitUntil) continue;
            path.event = false; waiting = false;
        }
        if (auto f = path.read(t)) { r.pickups.push_back({t, f->seq, f->flags}); busyUntil = t + work(f->seq); }
        else { waiting = true; waitUntil = t + 40000; }
    }
    r.skipped = path.skipped; r.wakes = path.wakes;
    return r;
}
}

TEST(laghunt_stats_switch_delivers_the_same_frames_at_the_same_times){
    // Three ways the stream arrives: an even 59.94 FPS stream carrying a 30 FPS game (every second picture a repeat),
    // network bursts (a 50 ms hole, then the frames of it back to back), and random jitter; against a fast engine,
    // a slow one (40 ms per frame for a stretch: a GPU stall at 4K) and one that varies frame to frame.
    const HostTime ms = 10000;
    std::vector<std::vector<std::pair<HostTime, unsigned>>> streams(3);
    for (int i = 0; i < 600; ++i) streams[0].push_back({static_cast<HostTime>(i) * 166833, (i % 2) ? 2u : 0u});   // 59.94 FPS
    for (HostTime i = 0, t = 0; i < 600; ++i) {
        if (i % 40 == 39) t += 50 * 10000;                        // a hole
        else if (i % 40 >= 36) t += 1000;                          // and its frames back to back
        else t += 166833;
        streams[1].push_back({t, (i % 2) ? 2u : 0u});
    }
    std::mt19937 rng(7);
    for (int i = 0, t = 0; i < 600; ++i) { t += static_cast<int>(60000 + rng() % 220000); streams[2].push_back({static_cast<HostTime>(t), (i == 300) ? 4u : 0u}); }
    const auto fast = [&](std::uint64_t) { return HostTime(6 * ms); };
    const auto slow = [&](std::uint64_t s) { return (s > 100 && s < 160) ? HostTime(40 * ms) : HostTime(9 * ms); };
    std::mt19937 workRng(11); std::vector<HostTime> varied(1000);
    for (auto& w : varied) w = static_cast<HostTime>(2000 + workRng() % 300000);
    const auto vary = [&](std::uint64_t s) { return varied[static_cast<std::size_t>(s % varied.size())]; };
    for (const auto& stream : streams) {
        const HostTime end = stream.back().first + 200 * ms;
        for (int engine = 0; engine < 3; ++engine) {
            OneSlotPath one; TwoSlotPath plain(false), withStats(true);
            const auto a = engine == 0 ? huntRun(one, stream, end, fast) : engine == 1 ? huntRun(one, stream, end, slow) : huntRun(one, stream, end, vary);
            const auto b = engine == 0 ? huntRun(plain, stream, end, fast) : engine == 1 ? huntRun(plain, stream, end, slow) : huntRun(plain, stream, end, vary);
            const auto c = engine == 0 ? huntRun(withStats, stream, end, fast) : engine == 1 ? huntRun(withStats, stream, end, slow) : huntRun(withStats, stream, end, vary);
            CHECK(!a.pickups.empty());
            CHECK(a.pickups == b.pickups);          // which frame, when, and the flags it carries (Drop, repeat, restart)
            CHECK(a.pickups == c.pickups);          // the statistics change nothing
            CHECK(a.skipped == b.skipped && a.skipped == c.skipped);
            CHECK(a.wakes == b.wakes && a.wakes == c.wakes);   // the same frame events: Newest never holds a second frame
            if (engine == 1) CHECK(a.skipped > 0);   // the slow stretch really replaced frames
        }
    }
    // The statistics were really recorded in the runs above (a line per 5 s).
    TwoSlotPath probe(true);
    huntRun(probe, streams[0], streams[0].back().first, fast);
    CHECK(probe.lines >= 1);
}

TEST(laghunt_sizes_line_that_flaps_is_written_at_most_once_per_10_s){
    using veyra::engine::PushSizesGate;
    PushSizesGate g;
    unsigned written = 0;
    HostTime t = 0;
    // A value that alternates every second for ten minutes (a window being resized, HDR toggled, a size that flaps).
    for (int second = 0; second < 600; ++second, t += PushSizesGate::kPeriod100ns) {
        CHECK(g.due(t));
        if (g.changed(second % 2 ? "client=2560x1440" : "client=2559x1440")) ++written;
    }
    // First line + kBurst at once, then one per 10 s: about 63 lines, not 600.
    CHECK(written <= 1 + PushSizesGate::kBurst + 600 / 10);
    CHECK(written >= 600 / 10);
    CHECK(g.held() > 200);                    // changed lines held back (half of the flaps return to the line last written)
    // Steady again: the newest text is written as soon as a token is back, then nothing.
    unsigned later = 0;
    for (int second = 0; second < 60; ++second, t += PushSizesGate::kPeriod100ns) { CHECK(g.due(t)); if (g.changed("client=3840x2160")) ++later; }
    CHECK(later == 1);
    // A rare change (once a minute) is always written at once.
    PushSizesGate rare; HostTime u = 0; unsigned rareWritten = 0;
    for (int minute = 0; minute < 30; ++minute, u += 60 * PushSizesGate::kPeriod100ns) { CHECK(rare.due(u)); if (rare.changed(std::to_string(minute))) ++rareWritten; }
    CHECK(rareWritten == 30);
    // Three quick changes after a quiet time are all written (a session start: graph, window, full screen).
    PushSizesGate start; CHECK(start.due(5));
    CHECK(start.changed("a")); CHECK(start.due(5 + PushSizesGate::kPeriod100ns)); CHECK(start.changed("b"));
    CHECK(start.due(5 + 2 * PushSizesGate::kPeriod100ns)); CHECK(start.changed("c"));
    CHECK(start.due(5 + 3 * PushSizesGate::kPeriod100ns)); CHECK(start.changed("d"));
    CHECK(start.due(5 + 4 * PushSizesGate::kPeriod100ns)); CHECK(!start.changed("e"));   // the next quick change waits for a token
    CHECK(start.due(5 + 5 * PushSizesGate::kPeriod100ns)); CHECK(!start.changed("f"));
    // A new live graph is written at its first look whatever the budget, and says how many looks were held back.
    start.forceNext(); CHECK(start.due(5 + 6 * PushSizesGate::kPeriod100ns)); CHECK(start.changed("rebuilt"));
    CHECK(start.takeHeld() == 2); CHECK(start.takeHeld() == 0);
    start.forceNext(); CHECK(start.due(5 + 7 * PushSizesGate::kPeriod100ns)); CHECK(!start.changed("rebuilt"));   // same sizes: nothing
    CHECK(start.due(5 + 8 * PushSizesGate::kPeriod100ns)); CHECK(!start.changed("g"));   // and that force does not linger
    start.reset(); CHECK(start.due(9)); CHECK(start.changed("e"));                      // a new session writes again
}

TEST(laghunt_original_neural_layer_is_held_off_only_by_a_marker){
    using namespace veyra::engine;
    const auto decide = [](bool enabled, bool usable, PushNrBlocker blocker, bool marked, bool original = true, bool keep = false) {
        PushNrHoldInputs in; in.originalLayer = original; in.savedEnabled = enabled; in.usable = usable; in.blocker = blocker;
        in.marked = marked; in.keepSavedOn = keep; return pushNrStartupHold(in);
    };
    // A blocker that can pass by itself keeps his saved choice: off for this start only (upstream), no marker.
    for (const auto b : {PushNrBlocker::SignatureUnchecked, PushNrBlocker::DriverTooOld, PushNrBlocker::GpuNotSupported}) {
        const auto d = decide(true, false, b, false);
        CHECK(d.rule == PushNrHoldRule::Transient); CHECK(!d.marked); CHECK(!d.announce);
        // ...so the next start with the blocker gone runs the layer as he saved it.
        CHECK(decide(true, true, PushNrBlocker::None, d.marked).rule == PushNrHoldRule::Runs);
    }
    // The runtime file missing (or not a signed original) sets the marker.
    for (const auto b : {PushNrBlocker::RuntimeMissing, PushNrBlocker::NotSignedOriginal}) {
        const auto d = decide(true, false, b, false);
        CHECK(d.rule == PushNrHoldRule::MarkerSet); CHECK(d.marked && d.offThisStart);
        // Still missing at the next start: held, nothing announced.
        const auto waiting = decide(true, false, b, true);
        CHECK(waiting.rule == PushNrHoldRule::HeldWaiting); CHECK(waiting.offThisStart && waiting.marked && !waiting.announce);
        // The file appears: the layer stays off, "available again" is said, the marker stays until his switch.
        const auto back = decide(true, true, PushNrBlocker::None, true);
        CHECK(back.rule == PushNrHoldRule::AvailableAgain); CHECK(back.offThisStart && back.marked && back.announce);
    }
    // His own switch clears it (any NR switch that turns the held layer on); nothing else does.
    CHECK(pushNrUserClears(true, true)); CHECK(!pushNrUserClears(true, false)); CHECK(!pushNrUserClears(false, true));
    // A layer he saved off needs no marker (a stale one is dropped); a usable unmarked layer runs as saved.
    CHECK(decide(false, false, PushNrBlocker::RuntimeMissing, true).rule == PushNrHoldRule::SavedOff);
    CHECK(!decide(false, false, PushNrBlocker::RuntimeMissing, true).marked);
    CHECK(!decide(true, true, PushNrBlocker::None, false).offThisStart);
    // Other neural layers (nr2, nr4, upstream's) are never touched, whatever the marker says.
    const auto other = decide(true, false, PushNrBlocker::RuntimeMissing, false, false);
    CHECK(other.rule == PushNrHoldRule::NotOriginal); CHECK(!other.offThisStart && !other.marked);
    // VEYRA_TEST_NR_KEEP_SAVED_ON=1: the old behaviour (runs when usable, even with a marker; no new marker).
    const auto keep = decide(true, true, PushNrBlocker::None, true, true, true);
    CHECK(keep.rule == PushNrHoldRule::KeepSavedOn); CHECK(!keep.offThisStart && !keep.announce);
    CHECK(!decide(true, false, PushNrBlocker::RuntimeMissing, false, true, true).marked);
    // The marker preference: keys with their list's layout print, validation (Preferences.cpp), lookup.
    CHECK(pushNrHoldKey(0, false, 1, 0x0a1b2c3du) == "0c1-0a1b2c3d"); CHECK(pushNrHoldKey(1, true, 12, 0xffffffffu) == "1e12-ffffffff");
    CHECK(pushNrHoldValid("")); CHECK(pushNrHoldValid("0c1-0a1b2c3d")); CHECK(pushNrHoldValid("0c1-0a1b2c3d,1e12-00000000"));
    CHECK(!pushNrHoldValid("0c1")); CHECK(!pushNrHoldValid("2c1-0a1b2c3d")); CHECK(!pushNrHoldValid("0x1-0a1b2c3d"));
    CHECK(!pushNrHoldValid("0c1-0A1B2C3D")); CHECK(!pushNrHoldValid("0c1-0a1b2c3")); CHECK(!pushNrHoldValid("0c1-0a1b2c3d,"));
    CHECK(!pushNrHoldValid("0c123-0a1b2c3d"));
    CHECK(!pushNrHoldValid("0c1-00000000,0c1-00000000,0c1-00000000,0c1-00000000,0c1-00000000,0c1-00000000,0c1-00000000,0c1-00000000,0c1-00000000"));
    bool keyed = false;
    CHECK(pushNrHoldHas("0c1-0000000a,1e12-0000000b", 1, true, 12, 0xbu, &keyed) && keyed);
    CHECK(!pushNrHoldHas("0c1-0000000a", 0, false, 1, 0xcu, &keyed) && keyed);   // keyed, but its list changed: not held
    CHECK(!pushNrHoldHas("0c12-0000000a", 0, false, 1, 0xau, &keyed) && !keyed);
    CHECK(pushNrHoldJoin({"0c1-0000000a", "1e0-0000000b"}) == "0c1-0000000a,1e0-0000000b"); CHECK(pushNrHoldJoin({}).empty());
    const auto line = pushNrHoldLine({{"0c1-0000000a", decide(true, false, PushNrBlocker::RuntimeMissing, false)}}, "0c1-0000000a", PushNrBlocker::RuntimeMissing, false, "1e0-0000000b");
    CHECK(line == "startup rule=held-off-until-switched-on originalReason=runtime-missing marker=0c1-0000000a layers=0c1-0000000a:marker-set droppedLayoutChanged=1e0-0000000b");
    CHECK(pushNrHoldLine({}, "", PushNrBlocker::None, true).find("keep-saved-on") != std::string::npos);
}

TEST(laghunt_neural_marker_follows_its_layer_not_a_node_position){
    using namespace veyra::engine;
    enum class Kind : std::uint8_t { Sr, Nr, Color };
    struct Node { Kind type = Kind::Sr; bool enabled = false; int runtime = 3; };
    struct Chain { std::array<Node, 6> nodes{}; std::uint32_t nodeCount = 0; };
    const auto isNr = [](const Node& n) { return n.type == Kind::Nr; };
    const auto runtimeOf = [](const Node& n) { return n.runtime; };
    // List chain: SR, NR(original, held), colour. Held key set for node 1.
    Chain list; list.nodeCount = 3;
    list.nodes[0] = {Kind::Sr, true, 0}; list.nodes[1] = {Kind::Nr, false, 3}; list.nodes[2] = {Kind::Color, true, 0};
    const auto print = pushNrLayoutPrint(list, isNr, runtimeOf);
    const auto key = *pushNrHoldParse(pushNrHoldKey(0, false, 1, print));
    CHECK(pushNrHoldCheckKey(key, &list, isNr, runtimeOf) == PushNrHoldCheck::Keep);
    // Settings that are not the neural layout (another effect on or off) keep the key.
    Chain toggled = list; toggled.nodes[2].enabled = false;
    CHECK(pushNrHoldCheckKey(key, &toggled, isNr, runtimeOf) == PushNrHoldCheck::Keep);
    // Reorder: a usable layer he chose now sits at index 1, the held one moved: the key is dropped, never applied to it.
    Chain reordered = list; std::swap(reordered.nodes[0], reordered.nodes[1]);
    reordered.nodes[1] = {Kind::Nr, true, 2};   // and now index 1 is another NR layer (nr2), on
    CHECK(pushNrHoldCheckKey(key, &reordered, isNr, runtimeOf) == PushNrHoldCheck::Dropped);
    // Delete a node before it: index 1 is now the colour node.
    Chain deleted = list; deleted.nodes[0] = list.nodes[1]; deleted.nodes[1] = list.nodes[2]; deleted.nodeCount = 2;
    CHECK(pushNrHoldCheckKey(key, &deleted, isNr, runtimeOf) == PushNrHoldCheck::Dropped);
    // Insert a node before it: index 1 is the inserted one.
    Chain inserted = list; inserted.nodeCount = 4;
    inserted.nodes[1] = {Kind::Color, true, 0}; inserted.nodes[2] = list.nodes[1]; inserted.nodes[3] = list.nodes[2];
    CHECK(pushNrHoldCheckKey(key, &inserted, isNr, runtimeOf) == PushNrHoldCheck::Dropped);
    // The same layer moved to another runtime: dropped too (it is not the layer the marker was set for).
    Chain otherRuntime = list; otherRuntime.nodes[1].runtime = 2;
    CHECK(pushNrHoldCheckKey(key, &otherRuntime, isNr, runtimeOf) == PushNrHoldCheck::Dropped);
    // No list at all (the node editor document is gone): dropped.
    CHECK(pushNrHoldCheckKey(key, static_cast<const Chain*>(nullptr), isNr, runtimeOf) == PushNrHoldCheck::Dropped);
    // His switch on the held node clears.
    Chain on = list; on.nodes[1].enabled = true;
    CHECK(pushNrHoldCheckKey(key, &on, isNr, runtimeOf) == PushNrHoldCheck::ClearedByUser);
    // Node editor: an "e" key is checked against the editor's own list, never against the list chain by position.
    Chain editor; editor.nodeCount = 2; editor.nodes[0] = {Kind::Nr, false, 3}; editor.nodes[1] = {Kind::Sr, true, 0};
    const auto editorKey = *pushNrHoldParse(pushNrHoldKey(0, true, 0, pushNrLayoutPrint(editor, isNr, runtimeOf)));
    CHECK(editorKey.editor && editorKey.node == 0);
    CHECK(pushNrHoldCheckKey(editorKey, &editor, isNr, runtimeOf) == PushNrHoldCheck::Keep);
    Chain editorOn = editor; editorOn.nodes[0].enabled = true;   // switched on only in the node editor
    CHECK(pushNrHoldCheckKey(editorKey, &editorOn, isNr, runtimeOf) == PushNrHoldCheck::ClearedByUser);
    // The list chain at index 0 (an SR node, on) says nothing about the editor key: checked against the list chain it
    // would be dropped, not "cleared", and the list's own key is unaffected by the editor.
    CHECK(pushNrHoldCheckKey(editorKey, &list, isNr, runtimeOf) == PushNrHoldCheck::Dropped);
    CHECK(pushNrHoldCheckKey(key, &list, isNr, runtimeOf) == PushNrHoldCheck::Keep);
}

