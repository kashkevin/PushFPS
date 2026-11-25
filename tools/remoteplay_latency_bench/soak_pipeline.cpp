// SPDX-License-Identifier: GPL-3.0-only
// Long-run soak of the portable PS5 Remote Play pieces working together, at accelerated time.
//
// A Linux MODEL, not the app: no Chiaki, no decoder, no GPU, no Windows. It drives the real
// portable code on real threads and checks invariants all the time:
//
//   network thread  59.94 fps access units and 10 ms PCM blocks through SessionInbox::Token
//                   (jitter, bursts, lost frames, late callbacks of an ended session, keyframes on
//                   request), as the Chiaki callbacks do
//   input thread    random keyboard / mouse events, focus loss, device removal, the release
//                   hotkey, overlay toggles, profile switches, calibration start / abort
//   pad thread      every 4 ms: KbmCaptureState + KeyboardMousePad::apply -> ControllerQueue,
//                   then the owner's WakeSignal
//   owner thread    WakeSignal wait, tryVideo / tryAudio / ControllerQueue::take, decode
//                   bookkeeping, IDR requests, StreamCadence, FrameJitter, ConnectionHealth, the
//                   adaptive reorder window, AudioLatencyController, the governor feed + latency
//                   governor + GPU budget driver; reconnects (new session generation) at random
//
// Invariants (any violation prints "FAIL soak: ..." and the exit code is 1):
//   - a frame handed to the owner belongs to the current generation and is not older than the
//     queue's age limit; queue depth and bytes stay within QueueLimits; audio buffered <= its cap
//   - a late callback of an ended session is refused
//   - ControllerQueue depth <= its capacity; the pad state after every "release all" is the
//     physical pad alone; at the end, with every key let go, nothing is held
//   - every number the controllers return is finite; the reorder window stays inside its steps;
//     the GPU budget never asks above the request; picture numbers never go back
//   - resident memory is flat after warm-up (plain builds; sanitizer builds report it only)
//
//   soak_pipeline --seed 7 --sim-hours 2 --wall-minutes 10 --speed 20
// The run stops at whichever limit comes first and prints one JSON line.
// Build: tools/remoteplay_latency_bench/run_soak.sh (plain, tsan, asan).
#include "veyra/remoteplay/SessionInbox.h"
#include "veyra/remoteplay/WakeSignal.h"
#include "veyra/remoteplay/ControllerQueue.h"
#include "veyra/remoteplay/KeyboardMousePad.h"
#include "veyra/remoteplay/KbmCapture.h"
#include "veyra/remoteplay/KbmCalibration.h"
#include "veyra/remoteplay/StreamCadence.h"
#include "veyra/remoteplay/ConnectionHealth.h"
#include "veyra/remoteplay/ReorderWindow.h"
#include "veyra/sink/AudioLatencyController.h"
#include "veyra/engine/PushGovernorFeed.h"
#include "veyra/engine/PushLatencyGovernor.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace veyra::remoteplay;
namespace ve = veyra::engine;

namespace {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

constexpr HostTime kMs = 10000, kSec = 10000000;
std::atomic<unsigned> violations{0};
std::mutex printMutex;
void fail(const std::string& what) {
    const unsigned n = ++violations;
    if (n <= 40) { std::lock_guard lock(printMutex); std::printf("FAIL soak: %s\n", what.c_str()); std::fflush(stdout); }
}
#define SOAK_CHECK(cond, what) do { if (!(cond)) fail(std::string(what) + " [" #cond "]"); } while (false)
bool finite(double v) { return std::isfinite(v); }

// Simulated time: real elapsed time times `speed`, in 100 ns units. Every thread reads the same clock.
// The inbox itself stamps and ages frames with the real monotonic clock (SessionInbox::Token::video
// calls monotonic100ns()), so frames carry real arrival times and are aged in real time: the
// network thread sends them `speed` times faster than 59.94 fps instead.
struct SimClock {
    HostTime realStart = monotonic100ns();
    double speed = 10;
    HostTime toSim(HostTime real) const { return HostTime(double(real - realStart) * speed) + kSec; }   // never 0
    HostTime now() const { return toSim(monotonic100ns()); }
    // Sleeps for `sim` simulated time.
    void sleep(HostTime sim) const { std::this_thread::sleep_for(std::chrono::nanoseconds(int64_t(double(sim) * 100.0 / speed))); }
};

long residentKb() {
    std::ifstream f("/proc/self/statm");
    long size = 0, resident = 0;
    f >> size >> resident;
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

VideoSample configSample(Generation g, HostTime t) {
    VideoSample s; s.generation = g; s.kind = SampleKind::CodecConfig; s.width = 1920; s.height = 1080;
    const std::array<std::uint8_t, 14> bytes{0, 0, 0, 1, 0x67, 0x42, 0, 0x1f, 0, 0, 1, 0x68, 0xee, 0x3c};
    s.payload = PaddedBytes::copy(bytes); s.arrival100ns = t; return s;
}
VideoSample frameSample(Generation g, std::uint16_t index, bool idr, std::size_t bytes, HostTime t) {
    VideoSample s; s.generation = g; s.wireFrameIndex = index; s.width = 1920; s.height = 1080; s.arrival100ns = t;
    std::vector<std::uint8_t> body(std::max<std::size_t>(bytes, 8), 0x77);
    body[0] = 0; body[1] = 0; body[2] = 1; body[3] = idr ? 0x65 : 0x41;
    s.payload = PaddedBytes::copy(body); return s;
}
PcmBlock pcmBlock(Generation g, std::uint64_t first, HostTime t) {
    PcmBlock b; b.generation = g; b.firstSample = first; b.channels = 2; b.rate = 48000; b.arrival100ns = t;
    b.samples.resize(480 * 2, 0); return b;
}

// The KBM layer: walking, aiming while L2 is held, driving on G with the mouse as a self-centring wheel.
KbmProfileSet soakProfiles() {
    KbmProfileSet set;
    for (auto& p : set.profiles) {
        p.bindings.bind('G', PadAction::Drive);
        p.triggerRiseMs = 80; p.triggerFallMs = 60; p.moveRiseMs = 40; p.moveFallMs = 40;
    }
    set.aimWhile = PadAction::L2;
    auto& drive = set[KbmProfileId::Drive];
    drive.mouse = KbmProfile::Mouse::Steer; drive.steerReturnMs = 400;
    drive.bindings.bind('W', PadAction::R2); drive.bindings.bind('S', PadAction::L2);
    return set;
}
bool neutral(const ControllerState& s) {
    return s.buttons == 0 && s.l2 == 0 && s.r2 == 0 && s.leftX == 0 && s.leftY == 0 && s.rightX == 0 && s.rightY == 0;
}
std::string describe(const ControllerState& s) {
    char text[160];
    std::snprintf(text, sizeof(text), "buttons=0x%x l2=%u r2=%u left=(%d,%d) right=(%d,%d)", unsigned(s.buttons), unsigned(s.l2), unsigned(s.r2),
                  int(s.leftX), int(s.leftY), int(s.rightX), int(s.rightY));
    return text;
}

struct Options {
    unsigned seed = 1;
    double simHours = 1, wallMinutes = 5, speed = 10;
};

class Soak {
public:
    explicit Soak(const Options& o) : options_(o), inbox_(limits_), rng_(o.seed) { clock_.speed = o.speed; }

    int run() {
        wake_ = std::make_shared<WakeSignal>();
        inbox_.wakeOn(wake_);
        reconnect(clock_.now());
        std::thread network([this] { networkLoop(); });
        std::thread input([this] { inputLoop(); });
        std::thread pad([this] { padLoop(); });
        ownerLoop();
        stop_ = true;
        wake_->notify();
        network.join(); input.join(); pad.join();
        finalChecks();
        return report();
    }

private:
    // --- shared ---------------------------------------------------------------------------------
    Options options_;
    SimClock clock_;
    QueueLimits limits_{};
    SessionInbox inbox_;
    std::shared_ptr<WakeSignal> wake_;
    std::atomic<bool> stop_{false};
    std::mt19937 rng_;   // owner thread only (the others seed their own from options_.seed)
    // The session the network thread delivers to. Replaced on reconnect; the old one is kept by
    // the network thread for a while to send late callbacks.
    std::mutex tokenMutex_;
    std::optional<SessionInbox::Token> token_;
    std::uint64_t tokenEpoch_ = 0;
    std::atomic<Generation> generation_{0};
    std::atomic<bool> idrWanted_{false};
    std::atomic<std::uint64_t> staleAccepted_{0}, staleSent_{0}, framesSent_{0}, framesLostOnWire_{0}, audioSent_{0};
    // KBM: the window procedure / pad poll share these under one lock, as the glue does.
    std::mutex kbmMutex_;
    KeyboardMousePad pad_{soakProfiles()};
    KbmCaptureState capture_;
    KbmCalibration calibration_;
    bool front_ = true;
    std::vector<std::uint32_t> keysDown_;
    std::array<bool, 6> buttonsDown_{};
    std::uint64_t releases_ = 0, neutralChecks_ = 0, calibrationsStarted_ = 0, calibrationsAborted_ = 0, profileSwitches_ = 0;
    // Controller queue between the pad thread and the owner.
    std::mutex controllerMutex_;
    ControllerQueue controller_;
    std::atomic<std::uint64_t> padPolls_{0};
    std::size_t controllerDepthMax_ = 0;
    // Owner statistics.
    std::uint64_t framesTaken_ = 0, audioTaken_ = 0, reconnects_ = 0, idrRequests_ = 0, stalls_ = 0, wakeTimeouts_ = 0;
    std::uint64_t cadenceRepeats_ = 0, healthSnapshots_ = 0, reorderChanges_ = 0, governorRecs_ = 0, budgetChanges_ = 0, audioDrops_ = 0;
    double maxAgeMs_ = 0, maxJitterMs_ = 0;
    std::size_t maxDepth_ = 0, maxBytes_ = 0;
    double maxAudioMs_ = 0;
    HostTime simEnd_ = 0;
    double wallSeconds_ = 0;
    std::vector<long> rss_;          // one sample per simulated minute after warm-up

    // --- owner -----------------------------------------------------------------------------------
    void reconnect(HostTime now) {
        std::lock_guard lock(tokenMutex_);
        inbox_.invalidate();
        inbox_.finishStop();
        auto token = inbox_.begin(now);
        token.connected();
        generation_ = token.generation();
        token_ = token;
        ++tokenEpoch_;
        ++reconnects_;
    }

    void ownerLoop() {
        const auto wallStart = std::chrono::steady_clock::now();
        const HostTime simStart = clock_.now();
        const HostTime simLimit = simStart + HostTime(options_.simHours * 3600.0 * double(kSec));
        const double wallLimit = options_.wallMinutes * 60.0;
        StreamCadence cadence;
        FrameJitter jitter;
        ConnectionHealth health;
        health.begin(simStart);
        ReorderWindowController reorder(4000, true);
        ReorderEvidence evidence; evidence.reorderTimeoutUs = reorder.windowUs();
        veyra::sink::AudioLatencyController audio;
        audio.reset(double(simStart) / double(kMs));
        ve::PushGovernorFeed feed;
        ve::PushLatencyGovernor governor;
        ve::PushGpuBudgetDriver budget;
        ve::GovernorKnobs knobs = ve::pushGovernorKnobs(ve::PushGovernorSwitches{}, 2, 1);
        const ve::PushFgRequest userRequest{ve::PushFgMethod::PushFlow, 3, 1};
        ve::PushFgRequest fgNow = userRequest;
        std::uniform_real_distribution<double> unit(0, 1);
        std::uint64_t lastPicture = 0, cumulativeLost = 0, presented = 0, pairs = 0, skipped = 0;
        HostTime nextReorder = simStart + kSec / 4, nextSecond = simStart + kSec, nextMinute = simStart + 60 * kSec;
        HostTime nextReconnect = simStart + HostTime((60 + 600 * unit(rng_)) * double(kSec));
        HostTime lastDecodeEnd = 0;
        bool boundary = true;
        for (;;) {
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
            HostTime now = clock_.now();
            if (now >= simLimit || wall >= wallLimit) { simEnd_ = now - simStart; wallSeconds_ = wall; break; }
            if (!wake_->waitFor(std::chrono::milliseconds(20))) ++wakeTimeouts_;
            // A stalled owner now and then (a long present, the OS taking the core): up to 150 ms of
            // real time, so the queue's real-time age limit is exercised.
            if (unit(rng_) < 0.0002) { ++stalls_; std::this_thread::sleep_for(std::chrono::microseconds(int(unit(rng_) * 150000.0))); }
            now = clock_.now();
            // Video: the clock is read just before the take, as RemotePlaySource::read does.
            for (;;) {
                const HostTime realNow = monotonic100ns();
                auto q = inbox_.tryVideo(realNow);
                if (!q) break;
                ++framesTaken_;
                const double ageMs = double(realNow - q->sample.arrival100ns) / double(kMs);
                maxAgeMs_ = std::max(maxAgeMs_, ageMs);
                SOAK_CHECK(q->sample.generation == generation_.load(), "a frame of another session reached the owner");
                SOAK_CHECK(realNow - q->sample.arrival100ns <= limits_.maxAge100ns, "a frame older than the age limit reached the owner");
                // Decode: 2-6 ms, sometimes 20 ms (a slow frame).
                // (real time, like the arrival stamps: a simulated decode time divided by the speed)
                const HostTime decode = HostTime((unit(rng_) < 0.01 ? 20.0 : 2.0 + 4.0 * unit(rng_)) * double(kMs) / options_.speed);
                const HostTime begin = std::max(realNow, lastDecodeEnd);
                inbox_.decodeStarted(begin, q->sample.arrival100ns);
                inbox_.decodeFinished(begin + decode);
                inbox_.frameDecoded(begin + decode);
                lastDecodeEnd = begin + decode;
                if (q->resetDecoder) boundary = true;
                if (q->sample.wireFrameIndex) jitter.observeWire(*q->sample.wireFrameIndex, clock_.toSim(q->sample.arrival100ns), 60);
                // Cadence: a 30 fps game in a 60 fps stream with scene cuts and still menus.
                const bool second = (q->sample.wireFrameIndex.value_or(0) & 1u) != 0;
                const double r = unit(rng_);
                float change = second ? float(1.0 + 3.0 * r) : float(30.0 + 40.0 * r);
                if (r < 0.002) change = 255.0f;
                const auto c = cadence.observe(change, boundary);
                boundary = false;
                SOAK_CHECK(c.picture >= lastPicture && c.picture > 0, "picture numbers went back");
                SOAK_CHECK(finite(c.confidence) && finite(c.sourceFps), "cadence returned a non-finite number");
                if (c.repeat) ++cadenceRepeats_;
                lastPicture = c.picture;
                ++presented; ++pairs;
            }
            while (auto block = inbox_.tryAudio()) { ++audioTaken_; (void)block; }
            if (inbox_.takeIdrRequest(monotonic100ns())) { ++idrRequests_; idrWanted_ = true; boundary = true; }
            const auto snap = inbox_.snapshot();
            maxDepth_ = std::max(maxDepth_, snap.video.highWaterDepth);
            maxBytes_ = std::max(maxBytes_, snap.video.highWaterBytes);
            maxAudioMs_ = std::max(maxAudioMs_, snap.audio.bufferedMs);
            SOAK_CHECK(snap.video.depth <= limits_.maxFrames, "video queue deeper than its limit");
            SOAK_CHECK(snap.video.highWaterDepth <= limits_.maxFrames, "video queue high water above its limit");
            SOAK_CHECK(snap.video.bytes <= limits_.maxBytes, "video queue bytes above the limit");
            SOAK_CHECK(snap.audio.bufferedMs <= 100.0 + 10.0 + 1e-6, "audio buffered above its cap");
            SOAK_CHECK(finite(snap.frameJitterMs) && finite(snap.frameJitterPeakMs), "inbox jitter not finite");
            health.observeIngressDepth(snap.video.depth);
            // Controller: what the owner submits now.
            {
                std::lock_guard lock(controllerMutex_);
                controllerDepthMax_ = std::max(controllerDepthMax_, controller_.depth());
                SOAK_CHECK(controller_.depth() <= ControllerQueue::kCapacity, "controller queue above its capacity");
                const auto state = controller_.take(now);
                SOAK_CHECK(finite(state.gyro[0]) && finite(state.gyro[1]), "controller gyro not finite");
            }
            // Adaptive reorder window every 250 ms of evidence.
            if (now >= nextReorder) {
                nextReorder += kSec / 4;
                evidence.videoPackets += 350;
                const unsigned events = unit(rng_) < 0.05 ? unsigned(1 + 12 * unit(rng_)) : 0u;
                for (unsigned i = 0; i < events; ++i) {
                    const auto us = std::uint32_t(500 + 20000 * unit(rng_));
                    std::size_t bucket = 0;
                    while (bucket < kReorderLatenessEdgesUs.size() && us > kReorderLatenessEdgesUs[bucket]) ++bucket;
                    ++evidence.lateness[bucket]; ++evidence.gaps;
                    if (us > reorder.windowUs()) { ++evidence.timeouts; ++evidence.skipped; ++evidence.lateArrivals; if (unit(rng_) < 0.5) ++evidence.fecFailed; }
                }
                const auto d = reorder.update(now, evidence);
                if (d.changed) { ++reorderChanges_; evidence.reorderTimeoutUs = d.windowUs; }
                SOAK_CHECK(d.windowUs >= ReorderWindowController::kFloorUs && d.windowUs <= ReorderWindowController::kCeilingUs, "reorder window outside its steps");
            }
            // Once a simulated second: health, audio, governor feed, latency governor, GPU budget.
            if (now >= nextSecond) {
                nextSecond += kSec;
                const double nowMs = double(now) / double(kMs);
                maxJitterMs_ = std::max(maxJitterMs_, jitter.jitterMs());
                SOAK_CHECK(finite(jitter.jitterMs()) && finite(jitter.peakMs()) && jitter.jitterMs() >= 0, "frame jitter not finite");
                HealthCounters hc;
                hc.packetsReceived = evidence.videoPackets; hc.packetsLost = framesLostOnWire_.load();
                hc.reorderGaps = evidence.gaps; hc.reorderTimeouts = evidence.timeouts; hc.reorderSkipped = evidence.skipped;
                hc.fecFailed = evidence.fecFailed; hc.keyframeRequests = snap.video.idrRequests; hc.decoderResets = snap.video.recoveries;
                hc.framesReceived = snap.video.accessUnits; hc.ingressDepth = snap.video.depth; hc.decodeP95Ms = snap.decodeP95Ms;
                hc.frameJitterMs = snap.frameJitterMs; hc.frameJitterPeakMs = snap.frameJitterPeakMs; hc.framesLost = framesLostOnWire_.load();
                if (auto h = health.sample(now, hc)) {
                    ++healthSnapshots_;
                    SOAK_CHECK(finite(h->lossPercent) && h->lossPercent >= 0 && finite(h->receivedFps) && finite(h->reorderOvershootMs), "health snapshot not finite");
                }
                // Audio: the measured A/V error wanders, with gaps and the odd missing measurement.
                veyra::sink::AudioLatencyController::Input ai;
                ai.nowMs = nowMs; ai.queuedMs = snap.audio.bufferedMs + 10; ai.endpointMs = 10; ai.gap = unit(rng_) < 0.01;
                if (unit(rng_) > 0.05) ai.errorMs = 40.0 * (unit(rng_) - 0.3) + (unit(rng_) < 0.01 ? 500.0 : 0.0);
                const auto ad = audio.update(ai);
                SOAK_CHECK(finite(ad.ratioPpm) && std::abs(ad.ratioPpm) <= audio.config().maxRatioPpm + 1e-6, "audio ratio out of bounds");
                SOAK_CHECK(finite(ad.dropMs) && ad.dropMs >= 0 && finite(ad.loudDropMs) && ad.loudDropMs >= 0, "audio drop not finite");
                SOAK_CHECK(finite(audio.pendingMs()) && audio.pendingMs() < 10000, "audio pending cuts grow");
                if (ad.dropMs > 0 || ad.loudDropMs > 0) ++audioDrops_;
                // Governor feed and GPU budget, from counters that are made up but consistent.
                cumulativeLost = framesLostOnWire_.load();
                if (unit(rng_) < 0.1) skipped += unsigned(10 * unit(rng_));
                ve::PushFeedInput in;
                in.now100ns = now; in.sessionId = tokenEpoch_; in.sourceFps = 30; in.outputFps = 30.0 * fgNow.multiplier;
                in.enhancementP95Ms = 4 + 20 * unit(rng_); in.fgBatchP95Ms = fgNow.multiplier > 1 ? 1 + 8 * unit(rng_) : 0;
                in.decodeQueueP95Ms = 1 + 3 * unit(rng_); in.decodeP95Ms = snap.decodeP95Ms.value_or(3.0);
                in.jitterMs = snap.frameJitterMs; in.lossPercent = unit(rng_) < 0.02 ? unit(rng_) : 0; in.queueDepth = double(snap.video.depth);
                in.gpuUtilizationPercent = 100 * unit(rng_); in.countersKnown = true; in.presented = presented; in.pairsCandidate = pairs;
                in.pairsSkipped = std::min(skipped, pairs); in.framesLostKnown = true; in.framesLost = cumulativeLost; in.multiplier = fgNow.multiplier;
                in.power = ve::PushPowerSource::Mains;
                const auto out = feed.update(in, knobs);
                SOAK_CHECK(finite(out.budget.gpuFrameMs) && finite(out.budget.fgMs) && finite(out.budget.sourceIntervalMs), "budget sample not finite");
                const auto step = budget.update(fgNow, out.budget, true);
                if (step.apply) {
                    ++budgetChanges_;
                    SOAK_CHECK(step.target.multiplier <= userRequest.multiplier && step.target.tier <= userRequest.tier, "GPU budget asked above the request");
                    fgNow = step.target;
                }
                if (out.governorValid) {
                    const auto rec = governor.update(out.nowMs, out.governor, knobs);
                    if (rec) { ++governorRecs_; ve::applyRecommendation(knobs, *rec); }
                }
            }
            // Once a simulated minute: memory.
            if (now >= nextMinute) {
                nextMinute += 60 * kSec;
                const double simMinutes = double(now - simStart) / double(60 * kSec);
                if (wall > 20.0 && simMinutes > 5.0) rss_.push_back(residentKb());
            }
            // Reconnect: the session ends and a new one starts (new generation).
            if (now >= nextReconnect) {
                nextReconnect = now + HostTime((120 + 1200 * unit(rng_)) * double(kSec));
                reconnect(now);
                health.begin(now);
                jitter.reset();
                boundary = true;
                {
                    std::lock_guard lock(kbmMutex_);   // the session ended: everything released, back on foot
                    const auto a = capture_.on(KbmCaptureState::Event::SessionEnded);
                    applyActions(a, "session ended");
                }
                {
                    std::lock_guard lock(controllerMutex_);
                    controller_.reset();
                }
            }
        }
    }

    // --- network (Chiaki callbacks) --------------------------------------------------------------
    void networkLoop() {
        std::mt19937 rng(options_.seed * 7919u + 1u);
        std::uniform_real_distribution<double> unit(0, 1);
        std::optional<SessionInbox::Token> current, stale;
        std::uint64_t epoch = 0;
        int staleLeft = 0;
        std::uint16_t wire = 0;
        std::uint64_t audioSample = 0;
        bool needIdr = true;
        HostTime next = clock_.now(), nextAudio = next;   // simulated
        while (!stop_) {
            {
                std::lock_guard lock(tokenMutex_);
                if (tokenEpoch_ != epoch) {
                    if (current) { stale = current; staleLeft = 1 + int(10 * unit(rng)); }
                    current = token_; epoch = tokenEpoch_;
                    needIdr = true; wire = 0; audioSample = 0;
                }
            }
            if (!current) { clock_.sleep(kMs); continue; }
            const Generation g = current->generation();
            // 59.94 fps with +-1.5 ms jitter; now and then a burst of 3 frames at once (a late Wi-Fi
            // aggregate on a wired LAN is rare, but the queue must take it) or a long hole.
            next += HostTime(166833);
            HostTime due = next + HostTime((unit(rng) - 0.5) * 3.0 * double(kMs));
            if (unit(rng) < 0.0003) due += HostTime(unit(rng) * 300.0 * double(kMs));   // an outage of up to 300 ms
            const HostTime now = clock_.now();
            if (due > now) clock_.sleep(due - now);
            if (stop_) break;
            const int burst = unit(rng) < 0.002 ? 3 : 1;
            for (int b = 0; b < burst; ++b) {
                const HostTime t = monotonic100ns();   // as ChiakiBackend stamps arrival
                if (needIdr) { current->video(configSample(g, t)); }
                if (idrWanted_.exchange(false)) needIdr = true;
                // A frame the library could not complete: its index is skipped.
                if (unit(rng) < 0.001) { ++wire; ++framesLostOnWire_; }
                const bool idr = needIdr;
                current->video(frameSample(g, wire++, idr, idr ? 120000 : std::size_t(20000 + 30000 * unit(rng)), t));
                needIdr = false;
                ++framesSent_;
                if (b + 1 < burst) next += HostTime(166833);
            }
            // Audio: 10 ms blocks.
            for (const HostTime t = clock_.now(); nextAudio <= t; nextAudio += 10 * kMs) {
                current->audio(pcmBlock(g, audioSample, monotonic100ns()));
                audioSample += 480;
                ++audioSent_;
            }
            // Late callbacks of the session that ended: must be refused.
            if (stale && staleLeft > 0) {
                --staleLeft; ++staleSent_;
                if (stale->video(frameSample(stale->generation(), 9999, true, 100, monotonic100ns()))) ++staleAccepted_;
                if (staleLeft == 0) stale.reset();
            }
        }
    }

    // --- input (window procedure) ----------------------------------------------------------------
    // Carries out what the capture state machine asks for, as RemotePlayKeyboardMouse::handle
    // does, and checks at once (same lock: nothing can be pressed in between) that a poll now
    // sends the physical pad alone.
    void applyActions(const KbmCaptureState::Actions& a, const char* why) {
        if (a.releaseInputs) {
            if (calibration_.active()) { calibration_.stop(why); ++calibrationsAborted_; }
            pad_.releaseAll();
            ++releases_;
            checkNeutral(why);
        }
        if (a.restart) { pad_.restart(); ++releases_; checkNeutral(why); }
        // Keys still physically down stay in keysDown_: their key-up messages arrive later and
        // must do nothing harmful; an auto-repeat press of one is a new press (as on Windows).
    }
    void checkNeutral(const char* why) {
        ControllerState physical; physical.inputActive = true;
        const auto out = pad_.apply(physical, std::uint64_t(clock_.now() / 10));
        ++neutralChecks_;
        if (!neutral(out)) fail(std::string("after a release (") + why + ") the pad is not the physical pad alone: " + describe(out));
    }
    void inputLoop() {
        std::mt19937 rng(options_.seed * 104729u + 3u);
        std::uniform_real_distribution<double> unit(0, 1);
        const std::uint32_t keys[] = {'W', 'A', 'S', 'D', 'G', 'R', 'F', 'Q', 'E', 'C', 'V', 0x20, 0x10, 0x12, 0x11, 0x26, 0x09, 'Z', 'X'};
        while (!stop_) {
            clock_.sleep(HostTime((1.0 + 15.0 * unit(rng)) * double(kMs)));
            std::lock_guard lock(kbmMutex_);
            const double r = unit(rng);
            const auto now = clock_.now();
            // Keys and mouse reach the pad only while captured (the native event filter's first test).
            const bool captured = capture_.captured();
            if (r < 0.85 && !captured) {
                // Physically the keys still go up and down; nothing reaches the pad.
                if (!keysDown_.empty() && unit(rng) < 0.5) keysDown_.pop_back();
            } else if (r < 0.45) {
                const std::uint32_t k = keys[std::size_t(unit(rng) * double(std::size(keys))) % std::size(keys)];
                const bool isDown = std::find(keysDown_.begin(), keysDown_.end(), k) != keysDown_.end();
                const bool down = unit(rng) < 0.3 ? true : !isDown;   // auto-repeat sends the press again
                pad_.key(k, down);
                if (down && !isDown) keysDown_.push_back(k);
                if (!down) std::erase(keysDown_, k);
                if (k == 'G' && down && !isDown) ++profileSwitches_;
            } else if (r < 0.75) {
                pad_.mouseMove(int(60 * (unit(rng) - 0.5)), int(30 * (unit(rng) - 0.5)));
            } else if (r < 0.85) {
                const int button = 1 + int(unit(rng) * 5) % 5;
                const bool down = !buttonsDown_[std::size_t(button)];
                pad_.mouseButton(button, down);
                buttonsDown_[std::size_t(button)] = down;
            } else if (r < 0.88 && captured) {
                pad_.scroll(unit(rng) < 0.5 ? 120 : -120);
            } else if (r < 0.90) {
                applyActions(capture_.on(KbmCaptureState::Event::FocusLost), "focus lost");
            } else if (r < 0.91) {
                applyActions(capture_.on(KbmCaptureState::Event::DeviceRemoved), "device removed");
            } else if (r < 0.915) {
                applyActions(capture_.on(KbmCaptureState::Event::ReleaseHotkey), "release hotkey");
            } else if (r < 0.93) {
                front_ = !front_;   // the overlay / another window comes to the front and goes again
            } else if (r < 0.935) {
                if (!calibration_.active() && calibration_.start(pad_.profiles())) ++calibrationsStarted_;
                else if (calibration_.active()) calibration_.tap(std::uint64_t(now / 10));
            } else if (r < 0.937) {
                if (calibration_.active()) { calibration_.stop("aborted"); ++calibrationsAborted_; }
            } else if (r < 0.938) {
                pad_.setProfiles(soakProfiles());   // a calibration finished: new numbers, everything released
                ++releases_;
                checkNeutral("new profiles");
            }
            (void)calibration_.takeMessages();
        }
    }

    // --- pad poll ----------------------------------------------------------------------------------
    void padLoop() {
        while (!stop_) {
            clock_.sleep(4 * kMs);
            ControllerState physical;   // the physical controller at rest
            physical.inputActive = true;
            ControllerState out = physical;
            const HostTime now = clock_.now();
            {
                std::lock_guard lock(kbmMutex_);
                applyActions(capture_.on(front_ ? KbmCaptureState::Event::PollFront : KbmCaptureState::Event::PollAway), "poll");
                if (capture_.captured()) {
                    // The physical pad at rest; anything that is not neutral comes from keyboard and mouse.
                    out = pad_.apply(physical, std::uint64_t(now / 10));
                    if (calibration_.active()) {
                        const auto c = calibration_.poll(std::uint64_t(now / 10));
                        SOAK_CHECK(finite(c.x) && finite(c.y) && std::abs(c.x) <= 1.0 && std::abs(c.y) <= 1.0, "calibration deflection out of range");
                    } else {
                        const auto c = calibration_.poll(std::uint64_t(now / 10));
                        SOAK_CHECK(!c.deflect && !c.holdAim, "a stopped calibration still moves the stick or holds aim");
                    }
                }
            }
            bool woke;
            {
                std::lock_guard lock(controllerMutex_);
                woke = controller_.push(now, out);
            }
            ++padPolls_;
            if (woke) wake_->notify();   // never under a lock
        }
    }

    // --- end ---------------------------------------------------------------------------------------
    void finalChecks() {
        SOAK_CHECK(staleAccepted_.load() == 0, "a late callback of an ended session was accepted");
        // Every key and button let go by its own release message (not by releaseAll): after the
        // ramps, the wheel pulse and the mouse hold, nothing is held.
        std::lock_guard lock(kbmMutex_);
        for (const auto k : keysDown_) pad_.key(k, false);
        for (int b = 1; b <= 5; ++b) if (buttonsDown_[std::size_t(b)]) pad_.mouseButton(b, false);
        keysDown_.clear();
        ControllerState physical; physical.inputActive = true;
        ControllerState out;
        std::uint64_t us = std::uint64_t(clock_.now() / 10);
        for (int i = 0; i < 1000; ++i) { us += 4000; out = pad_.apply(physical, us); }
        if (!neutral(out)) fail("with every key let go, after 4 s the pad still shows input: " + describe(out));
        pad_.releaseAll();
        out = pad_.apply(physical, us + 4000);
        if (!neutral(out)) fail("after the final release the pad still shows input: " + describe(out));
        // Memory: flat after warm-up (sanitizer builds keep quarantines: reported only).
        if (rss_.size() >= 6) {
            const std::size_t third = rss_.size() / 3;
            double first = 0, last = 0;
            for (std::size_t i = 0; i < third; ++i) { first += double(rss_[i]); last += double(rss_[rss_.size() - 1 - i]); }
            first /= double(third); last /= double(third);
            growthKb_ = last - first;
            if (!kSanitized && growthKb_ > 8192) fail("resident memory grew by " + std::to_string(long(growthKb_)) + " kB after warm-up");
        }
    }
    double growthKb_ = 0;

    int report() {
        const auto stats = inbox_.snapshot();
        std::printf("{\"bench\":\"soak_pipeline\",\"seed\":%u,\"speed\":%.1f,\"sanitized\":%s,\"simHours\":%.3f,\"wallSeconds\":%.1f,"
                    "\"framesSent\":%llu,\"framesTaken\":%llu,\"sessionDropped\":%llu,\"sessionRecoveries\":%llu,\"idrRequests\":%llu,\"reconnects\":%llu,"
                    "\"staleSent\":%llu,\"staleAccepted\":%llu,\"audioSent\":%llu,\"audioTaken\":%llu,\"maxAgeMs\":%.2f,\"maxDepth\":%zu,"
                    "\"maxBytes\":%zu,\"maxAudioMs\":%.1f,\"maxJitterMs\":%.3f,\"padPolls\":%llu,\"controllerDepthMax\":%zu,\"releases\":%llu,"
                    "\"neutralChecks\":%llu,\"profileSwitches\":%llu,\"calibrationsStarted\":%llu,\"calibrationsAborted\":%llu,"
                    "\"cadenceRepeats\":%llu,\"healthSnapshots\":%llu,\"reorderChanges\":%llu,\"governorRecommendations\":%llu,"
                    "\"budgetChanges\":%llu,\"audioDrops\":%llu,\"ownerStalls\":%llu,\"wakeTimeouts\":%llu,\"rssSamples\":%zu,"
                    "\"rssFirstKb\":%ld,\"rssLastKb\":%ld,\"rssGrowthKb\":%.0f,\"violations\":%u}\n",
                    options_.seed, options_.speed, kSanitized ? "true" : "false", double(simEnd_) / double(3600 * kSec), wallSeconds_,
                    ull(framesSent_), ull(framesTaken_), ull(stats.video.dropped), ull(stats.video.recoveries), ull(idrRequests_), ull(reconnects_),
                    ull(staleSent_), ull(staleAccepted_), ull(audioSent_), ull(audioTaken_), maxAgeMs_, maxDepth_, maxBytes_, maxAudioMs_, maxJitterMs_,
                    ull(padPolls_), controllerDepthMax_, ull(releases_), ull(neutralChecks_), ull(profileSwitches_), ull(calibrationsStarted_),
                    ull(calibrationsAborted_), ull(cadenceRepeats_), ull(healthSnapshots_), ull(reorderChanges_), ull(governorRecs_),
                    ull(budgetChanges_), ull(audioDrops_), ull(stalls_), ull(wakeTimeouts_), rss_.size(), rss_.empty() ? 0L : rss_.front(),
                    rss_.empty() ? 0L : rss_.back(), growthKb_, violations.load());
        // Liveness: the run did something.
        if (framesTaken_ < 100 || neutralChecks_ == 0 || padPolls_ < 100) { std::printf("FAIL soak: too little traffic to mean anything\n"); return 1; }
        return violations.load() ? 1 : 0;
    }
    static unsigned long long ull(std::uint64_t v) { return static_cast<unsigned long long>(v); }
    static unsigned long long ull(const std::atomic<std::uint64_t>& v) { return static_cast<unsigned long long>(v.load()); }
};
} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--seed" && next) { o.seed = unsigned(std::strtoul(next, nullptr, 10)); ++i; }
        else if (a == "--sim-hours" && next) { o.simHours = std::atof(next); ++i; }
        else if (a == "--wall-minutes" && next) { o.wallMinutes = std::atof(next); ++i; }
        else if (a == "--speed" && next) { o.speed = std::clamp(std::atof(next), 0.1, 200.0); ++i; }
        else { std::fprintf(stderr, "usage: soak_pipeline [--seed N] [--sim-hours H] [--wall-minutes M] [--speed K]\n"); return 2; }
    }
    Soak soak(o);
    return soak.run();
}
