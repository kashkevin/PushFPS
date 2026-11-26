// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// SmartLink decisions (experiment VEYRA_EXP_RP_SMARTLINK=1 observe / =apply).
// Portable, header-only, O(1) per sample, no allocation, no locking: one
// owner thread calls Engine::update() once per observation window (~200 ms).
// Unverified on Windows and on a PS5; evaluated only in the Linux simulator.
//
// The one call another stream needs:
//
//     smartlink::Engine engine(smartlink::parseModel(modelSwitch));   // or Engine(Config)
//     smartlink::Decision d = engine.update(sample);                   // every window
//     if(logDue) log(smartlink::toLogLine(d, mode).view());            // at most every 5 s
//
// CONTRACT for the caller (both matter in apply mode):
//   - 0 in Decision::reorderWindowMs / audioTargetMs means RESTORE THE
//     CONFIGURED VALUE (the session's own reorder wait, the sink's own audio
//     target), not "do nothing": after a widening the output returns to 0, and
//     a caller that ignores 0 would keep the widened value for the rest of the
//     session. false in networkLimited / linkHeadroom and 0 in
//     suggestedStartKbps mean "no claim".
//   - call reset() (or build a new Engine) at EVERY stream (session) start:
//     the engine keeps per-session state (configured wait, rate history,
//     change budgets). A reconnect inside one session may keep it (the
//     session's configuration is the same; SmartLinkRunner.h does so).
//   - Decision::advice: one plain-language advice id, None until there is
//     evidence, at most one change a minute (rules at its section below).
//   - fill Sample::reorderWaitMs with the wait the library actually used during
//     the window (it applies a new wait at its next look, ~250 ms later).
// Observe vs apply is the caller's business: this only computes.
//
// The rules and their limits (constants below, each with where it comes from):
//   reorder wait (0 = restore the configured wait, else 2/4/8/16 ms):
//     - WIDEN within the window that shows evidence: Sample::lateRescueMs > 0
//       (frames were given up that a longer wait would have completed), to the
//       smallest step >= (the wait IN EFFECT during that window, i.e.
//       Sample::reorderWaitMs when known) + lateRescueMs. Never wider than that,
//       never on a link without such evidence.
//     - TRIM (narrow below the configured wait) only when the wait buys nothing:
//       packets lost in 2+ windows within 10 s and no late packet or rescuable
//       frame in the last minute (a lost packet holds every later one for the
//       whole wait, so a long wait is pure added delay there).
//     - RELAX one step toward the configured wait after 30 s without evidence
//       and with the stall risk below 0.5; a relax undone (by a widening or a
//       trim) within a minute doubles the next dwell (up to 8x), so a link
//       between two steps settles on one; a minute without any evidence clears
//       the doubling. Back at the configured wait the output is 0 again: at most
//       kReturnBoundMs (150 s) after the last evidence.
//     - at most 4 changes per minute.
//   audio target (0 = restore the sink's own target, else the reserve in ms,
//   multiples of 10, at most kAudioMaxMs = 50):
//     - RAISE after an underrun, to the floor plus the recent large frame-gap
//       excess (gaps longer than any reserve, i.e. interruptions, add one 20 ms
//       step); in the periodic-spike regime also BEFORE the next expected spike,
//       but such a forecast-only raise lasts kAudioPreemptHoldMs (3 s) unless an
//       underrun confirms it;
//     - DECAY 10 ms every 5 s once there was no underrun for 10 s, down to the
//       floor (then 0): latency never accumulates;
//     - at most 12 changes per minute.
//   networkLimited: 2+ LINK stalls within 20 s (or an interruption), held 10 s
//     after the last one. A link stall is a frame gap >= 10 ms above the
//     observed cadence whose frames arrived late (or with loss); a frame the
//     console skipped (one frame short, nothing lost) is not one; never while
//     the local decoder queue was >= 3 in the last second.
//   linkHeadroom: 120 s without stall, loss or late packets while the link
//     DELIVERED >= 80 % of the requested rate (measuredKbps) and the console's
//     target, when known, stayed >= 95 % of it.
//   suggestedStartKbps (next session): below the request when the console held
//     its target under 85 % of it for a minute AND the link lost packets or
//     stalled in that minute, or loss stayed above 1 % for a minute; above it
//     only after 5 minutes of headroom, to at most 1.25 x the request and
//     never above 50 Mbit/s. A low rate alone gives no suggestion.
#include "SmartLinkPredictor.h"
#include "SmartLinkProfile.h"
#include "SmartLinkTypes.h"
#include <algorithm>
#include <array>
#include <cmath>
namespace veyra::remoteplay::smartlink {

struct Config {
    // The reorder wait the session was configured with (0 = take
    // Sample::reorderWaitMs, else the library's 16 ms). TransportTuning.h:
    // 16 ms library default, 2 ms with VEYRA_EXP_RP_LAN_PROFILE.
    double configuredReorderMs=0;
    double floorReorderMs=2;  // the lowest step (the LAN profile's value)
    // The audio reserve the sink keeps by itself (docs/AUDIO.md finding 6: about 20 ms).
    double audioFloorMs=20;
    ModelKind model=ModelKind::Statistics;
};

class Engine {
public:
    static constexpr std::array<std::uint32_t,4> kLadderMs{2,4,8,16};
    static constexpr double kRelaxDwellMs=30000;   // ReorderWindow.h kNarrowDwell100ns (30 s): the same patience
    static constexpr double kTrimEvidenceMs=10000; // ReorderWindow.h kEvidenceWindow100ns (10 s)
    static constexpr double kTrimQuietLateMs=60000;
    static constexpr double kTrimDwellMs=5000;
    static constexpr unsigned kMaxPenalty=3;       // relax dwell up to 8x
    static constexpr double kPenaltyClearMs=60000;
    static constexpr double kSwingMs=60000;
    static constexpr double kFrequentReorderShare=0.01; // 1 % of packets out of order: the "frequent reordering" regime        // a trim or widening this soon after a relax doubles the next relax dwell
    static constexpr double kReturnBoundMs=150000; // every output back to "leave" this long after the last evidence (relax path; audio decays faster)
    static constexpr std::size_t kReorderChangesPerMinute=4,kAudioChangesPerMinute=12;
    // The largest audio reserve SmartLink asks for: 50 ms in all (the sink's own
    // ~20 ms floor + at most 30 ms). Why 50: the reserve delays sound against the
    // picture, and EBU R37 allows sound at most 60 ms behind the picture end to
    // end (ITU-R BT.1359 puts detectability of late sound near 125 ms, but its
    // acceptable range for programme chains is tighter, and games add the
    // console's own audio path). 50 ms leaves ~10 ms for the output device period.
    // The earlier 100 ms cap averaged 93 ms for an hour under sustained stalls.
    static constexpr double kAudioMaxMs=50;
    static constexpr double kAudioPreemptHoldMs=3000; // a raise made only from the periodic forecast lasts this long without an underrun
    static constexpr double kAudioStepMs=10,kAudioDecayEveryMs=5000,kAudioQuietMs=10000;
    static constexpr double kRiskAct=0.5,kRiskQuiet=0.3;
    static constexpr double kLimitedWindowMs=20000,kLimitedHoldMs=10000;
    static constexpr double kHeadroomMs=120000,kRaiseAfterMs=300000,kLowRateMs=60000;
    // Headroom and a raise need DELIVERY: measured >= 80 % of the request (a console
    // target alone proves nothing; a calm menu sends 25-60 % of it).
    static constexpr double kDeliveredShare=0.8;
    static constexpr double kRaiseFactor=1.25;      // the raise ceiling: never above 1.25 x this session's request
    static constexpr double kRaiseCeilingKbps=50000; // and never above 50 Mbit/s whatever was requested
    // Link stalls for networkLimited (see the comment at that section).
    static constexpr double kLinkStallExcessMs=10;  // frame gap this far above the observed cadence (a 20 ms drought)
    static constexpr double kEventSeparationMs=600;
    static constexpr double kAdviceHoldMs=60000,kAdviceCleanMs=120000; // advice: one change a minute; "fine" after 2 clean minutes // windows closer than this are one stall
    static constexpr double kQueueLookbackMs=1000;  // a local decoder backlog this recent explains gaps

    explicit Engine(ModelKind model=ModelKind::Statistics)noexcept:Engine(Config{0,2,20,model}){}
    explicit Engine(const Config& c)noexcept:config_(c),predictor_(c.model){}
    void reset()noexcept{const Config c=config_;*this=Engine(c);}
    const Prediction& prediction()const noexcept{return prediction_;}
    const ProfileResult& profile()const noexcept{return profile_;}
    double currentReorderMs()const noexcept{return current_;}

    Decision update(const Sample& s)noexcept{
        prediction_=predictor_.update(s);
        Decision d=last_;
        d.changed=false;
        if(!prediction_.usable){d.reasons=last_.reasons|reason::kInputRejected;return d;}
        const double now=predictor_.nowMs();
        if(!started_){
            started_=true;
            configured_=config_.configuredReorderMs>0?config_.configuredReorderMs:(known(s.reorderWaitMs)&&s.reorderWaitMs>0?s.reorderWaitMs:16.0);
            configured_=std::clamp(configured_,0.5,16.0);
            current_=configured_;relaxFrom_=now;lastEvidence_=now-1e9;lastLate_=now-1e9;lastUnderrun_=now-1e9;lastAudioChange_=now;
            headroomSince_=now;adviceCleanSince_=now;lowRateSince_=-1;lossSince_=-1;audio_=config_.audioFloorMs;
        }
        std::uint32_t why=0;
        if(predictor_.model()==ModelKind::Learned)why|=reason::kLearnedModel;
        const double risk=prediction_.stallRisk;
        if(risk>=kRiskAct)why|=reason::kPredictedStall;
        if(prediction_.regime==Regime::PeriodicSpikes)why|=reason::kPeriodic;
        if(prediction_.regime==Regime::WeakSignal)why|=reason::kWeakSignal;
        const bool interrupted=prediction_.regime==Regime::Interrupted;
        if(interrupted)why|=reason::kInterrupted;
        const std::int64_t late=known(s.packetsLate)?s.packetsLate:0,lost=known(s.packetsLost)?s.packetsLost:0;
        if(lost>0)why|=reason::kLoss;

        // ---------------- reorder wait ----------------
        const double before=current_;
        if(late>0||(known(s.lateRescueMs)&&s.lateRescueMs>0))lastLate_=now;
        // Frequent reordering (>= kFrequentReorderShare of packets out of order or late,
        // ~10 s average): a longer wait holds MANY frames to rescue a few, and in the
        // model it was worse than the existing switches on both delay and stutter
        // (reordering: P95 8.3 vs 6.2 ms, 71 vs 38 stutters/min; with 0.2 % loss
        // 76 vs 2.3). There SmartLink does not widen and returns to the configured
        // wait ("leave"); the library's FEC completes most such frames by itself.
        if(known(s.packetsReceived)&&s.packetsReceived>0&&known(s.windowMs)){
            const double outOfOrder=static_cast<double>((known(s.packetsReordered)?s.packetsReordered:0)+late);
            const double share=outOfOrder/static_cast<double>(s.packetsReceived+lost);
            reorderShare_+=(share-reorderShare_)*(std::min)(1.0,s.windowMs/10000.0);
        }
        const bool frequentReorder=reorderShare_>=kFrequentReorderShare;
        if(frequentReorder){
            why|=reason::kFrequentReordering;
            if(current_>configured_&&budget(reorderChanges_,now,kReorderChangesPerMinute)){current_=configured_;relaxFrom_=now;lastWasRelax_=false;}
        }
        if(!frequentReorder&&known(s.lateRescueMs)&&s.lateRescueMs>0&&s.lateRescueMs<1000){
            lastEvidence_=now;
            why|=reason::kLateFramesRescuable;
            // lateRescueMs is measured against the wait that was IN EFFECT during
            // the window. The library applies a new wait only at its next look
            // (~250 ms), so right after a change the sample still reports the old
            // wait: add the rescue to that one, never to the newer current_
            // (adding it to current_ overshot: 3 ms late -> 8 ms, 5 ms -> 16 ms).
            const double inEffect=known(s.reorderWaitMs)&&s.reorderWaitMs>0?s.reorderWaitMs:current_;
            const double need=snapUp(inEffect+s.lateRescueMs);
            if(need>current_){
                if(budget(reorderChanges_,now,kReorderChangesPerMinute)){
                    if(lastWasRelax_&&now-relaxFrom_<60000&&penalty_<kMaxPenalty)++penalty_;
                    current_=need;lastWasRelax_=false;relaxFrom_=now;
                }else why|=reason::kChangeBudget;
            }
        }
        if(lost>0){
            if(now-lossWindowAt_[0]>kTrimEvidenceMs)lossWindows_=0;
            ++lossWindows_;lossWindowAt_[1]=lossWindowAt_[0];lossWindowAt_[0]=now;
            if(lossWindows_==1)lossWindowAt_[1]=now;
            lastEvidence_=now;
        }
        // Trim: lost (not late) packets, the wait buys nothing.
        const bool lossRecent=lossWindows_>=2&&now-lossWindowAt_[0]<=kTrimEvidenceMs;
        if(current_==before&&lossRecent&&now-lastLate_>=kTrimQuietLateMs&&current_>config_.floorReorderMs&&now-relaxFrom_>=kTrimDwellMs){
            const double lower=snapDown(current_);
            if(lower<current_){
                if(budget(reorderChanges_,now,kReorderChangesPerMinute)){
                    // A trim that undoes a relax within kSwingMs is a swing between two
                    // steps (loss pairs every ~35 s did 203 changes per hour): double
                    // the next relax dwell, as a widening after a relax does.
                    if(lastWasRelax_&&now-relaxFrom_<kSwingMs&&penalty_<kMaxPenalty)++penalty_;
                    current_=lower;relaxFrom_=now;lastWasRelax_=false;lossWindows_=0;
                }
                else why|=reason::kChangeBudget;
            }
        }
        // A minute without any evidence clears the doubling: the link that
        // oscillated has calmed down (this bounds the return to "leave" after a
        // disturbance: 60 s + 3 steps x 30 s = 150 s at most, kReturnBoundMs).
        if(penalty_>0&&now-lastEvidence_>=kPenaltyClearMs)penalty_=0;
        // Relax toward the configured wait after a long clean period.
        const double dwell=kRelaxDwellMs*static_cast<double>(1u<<penalty_);
        if(current_==before&&current_!=configured_&&now-lastEvidence_>=dwell&&now-relaxFrom_>=dwell&&risk<kRiskAct){
            const double next=current_>configured_?(std::max)(snapDown(current_),configured_):(std::min)(snapUpStrict(current_),configured_);
            if(budget(reorderChanges_,now,kReorderChangesPerMinute)){
                current_=next;relaxFrom_=now;lastWasRelax_=true;why|=reason::kLongClean;
                if(penalty_>0&&current_==configured_)--penalty_;
            }else why|=reason::kChangeBudget;
        }
        d.reorderWindowMs=current_==configured_?0u:static_cast<std::uint32_t>(std::lround(current_));

        // ---------------- network limited ----------------
        // A frame gap is a LINK stall only when the frames came late, not when the
        // console never sent one. The app does not know packetGapMaxMs, so this
        // judges each gap window together with the next one:
        //   - excess over the observed cadence (a 30 fps game in a 60 fps stream
        //     has 33 ms gaps that are no stall) >= kLinkStallExcessMs: a candidate;
        //   - packets lost or frames given up in the two windows: the link;
        //   - otherwise the frames of the two windows reach the cadence's count
        //     (the late frames arrived in a burst): the link; a frame short and
        //     nothing lost: the console skipped a frame (not the link);
        //   - a decoder backlog (videoQueueDepth >= 3) within kQueueLookbackMs:
        //     local, never the link.
        // Without frame counts, gaps above Predictor::kStallExcessMs count as before.
        const bool localQueue=known(s.videoQueueDepth)&&s.videoQueueDepth>=3;
        if(localQueue)lastQueueHigh_=now;
        const bool queueRecent=now-lastQueueHigh_<kQueueLookbackMs;
        const bool framesKnown=known(s.framesArrived)&&known(s.windowMs)&&s.windowMs>0;
        const double lossyNow=(lost>0||(known(s.framesIncomplete)&&s.framesIncomplete>0))?1.0:0.0;
        double cadenceMs=-1;
        if(frameRate_>0)cadenceMs=1.0/frameRate_;
        const double gapMs=known(s.frameGapMaxMs)?s.frameGapMaxMs:(known(s.frameGapExcessMs)?s.frameGapExcessMs+16.683:-1);
        const double cadenceExcess=cadenceMs>0&&gapMs>=0?gapMs-cadenceMs:-1;
        bool linkStall=false;
        if(pendingAt_>=0){
            if(framesKnown){
                const double frames=static_cast<double>(pendingFrames_+s.framesArrived);
                const double expected=frameRate_*(pendingWindowMs_+s.windowMs);
                linkStall=pendingLossy_>0||lossyNow>0||frames>=expected-0.5;
            }
            if(pendingQueue_||queueRecent)linkStall=false;
            pendingAt_=-1;
        }
        const bool candidate=framesKnown&&frameRate_>0&&cadenceExcess>=kLinkStallExcessMs;
        if(candidate&&now-lastStallEvent_>=kEventSeparationMs){
            pendingAt_=now;pendingFrames_=s.framesArrived;pendingWindowMs_=s.windowMs;pendingLossy_=lossyNow;pendingQueue_=queueRecent;
        }else if(!framesKnown&&!queueRecent&&prediction_.features[0]*100.0>Predictor::kStallExcessMs&&now-lastStallEvent_>=kEventSeparationMs)linkStall=true;
        if(linkStall){stallAt_[stallNext_]=now;stallNext_=(stallNext_+1)%stallAt_.size();lastStallEvent_=now;}
        // The cadence: the second highest frame rate of the last 25 windows (~5 s):
        // a frame the console skipped shows as a frame short (57 of 60 fps: 11
        // instead of 12), while one burst after a stall (13) does not raise it.
        if(framesKnown){
            rates_[rateNext_]=static_cast<double>(s.framesArrived)/s.windowMs;rateNext_=(rateNext_+1)%rates_.size();
            if(rateCount_<rates_.size())++rateCount_;
            std::array<double,25> sorted=rates_;
            const auto k=static_cast<std::ptrdiff_t>(rateCount_>=2?rateCount_-2:0);
            std::nth_element(sorted.begin(),sorted.begin()+(std::min)(k,static_cast<std::ptrdiff_t>(rateCount_)-1),sorted.begin()+static_cast<std::ptrdiff_t>(rateCount_));
            frameRate_=sorted[static_cast<std::size_t>((std::min)(k,static_cast<std::ptrdiff_t>(rateCount_)-1))];
        }
        unsigned recentStalls=0;for(double t:stallAt_)if(t>=0&&now-t<kLimitedWindowMs)++recentStalls;
        if(!queueRecent&&(recentStalls>=2||interrupted))limitedUntil_=now+kLimitedHoldMs;
        if(queueRecent)limitedUntil_=-1;
        d.networkLimited=now<limitedUntil_;
        if(d.networkLimited)why|=reason::kLinkStalls;

        // ---------------- audio target ----------------
        const double floor=config_.audioFloorMs;
        const bool underrun=known(s.audioUnderruns)&&s.audioUnderruns>0;
        if(underrun){lastUnderrun_=now;why|=reason::kAudioUnderrun;}
        double want=audio_;
        const double large=(std::max)(prediction_.gapExcessP95Ms,prediction_.gapExcessMaxMs);
        // A gap longer than the largest reserve (an interruption) cannot be bridged by
        // any reserve: then only one step is added after an underrun, and the risk adds nothing.
        const bool coverable=large<=kAudioMaxMs-floor;
        // Real underruns raise; the forecast raises only in the periodic regime
        // (the next spike is expected) with a link stall in the last 20 s (a frame the
        // console skipped says nothing about audio), and only for kAudioPreemptHoldMs unless an
        // underrun confirms it. Both stop at kAudioMaxMs.
        const bool periodic=prediction_.regime==Regime::PeriodicSpikes;
        if(underrun)want=(std::max)(want,floor+roundUp(coverable?(std::max)(large,20.0):20.0));
        if(periodic&&recentStalls>=1&&risk>=kRiskAct&&large>0&&coverable&&floor+roundUp(large)>audio_){want=(std::max)(want,floor+roundUp(large));preemptUntil_=now+kAudioPreemptHoldMs;}
        want=(std::min)(want,kAudioMaxMs);
        const bool riskQuiet=risk<kRiskQuiet||!periodic||(now>=preemptUntil_&&now-lastUnderrun_>=kAudioQuietMs);
        if(want>audio_){
            if(budget(audioChanges_,now,kAudioChangesPerMinute)){audio_=want;lastAudioChange_=now;}
            else why|=reason::kChangeBudget;
        }else if(audio_>floor&&now-lastUnderrun_>=kAudioQuietMs&&riskQuiet&&now-lastAudioChange_>=kAudioDecayEveryMs){
            if(budget(audioChanges_,now,kAudioChangesPerMinute)){audio_=(std::max)(floor,audio_-kAudioStepMs);lastAudioChange_=now;why|=reason::kAudioDecay;}
        }
        if(audio_>kAudioMaxMs)audio_=kAudioMaxMs;
        if(audio_<floor)audio_=floor;
        d.audioTargetMs=audio_>floor?static_cast<std::uint32_t>(std::lround(audio_)):0u;
        if(d.reorderWindowMs!=0)why|=reason::kReorderHeld;
        if(d.audioTargetMs!=0)why|=reason::kAudioHeld;

        // ---------------- headroom and the next session's rate ----------------
        const bool targetKnown=known(s.consoleTargetKbps)&&s.consoleTargetKbps>0,requestKnown=known(s.requestedKbps)&&s.requestedKbps>0;
        if(requestKnown)requested_=s.requestedKbps;
        if(targetKnown){targetEw_=targetHave_?targetEw_+(1-std::exp(-(std::min)(now-lastTargetAt_,5000.0)/60000.0))*(s.consoleTargetKbps-targetEw_):s.consoleTargetKbps;targetHave_=true;lastTargetAt_=now;}
        // Headroom needs delivery: measured >= 80 % of the request (and, when the
        // console's target is known, a target near the request). A calm picture
        // that needs only part of the rate shows nothing about the link.
        bool rateOk=false;
        if(requestKnown&&known(s.measuredKbps)){
            rateOk=s.measuredKbps>=kDeliveredShare*s.requestedKbps;
            if(targetKnown&&s.consoleTargetKbps<0.95*s.requestedKbps)rateOk=false;
        }
        const bool spikeNow=prediction_.features[0]*100.0>Predictor::kStallExcessMs;
        if(!rateOk||spikeNow||lost>0||late>0||interrupted)headroomSince_=now;
        d.linkHeadroom=now-headroomSince_>=kHeadroomMs;
        if(d.linkHeadroom)why|=reason::kHeadroom;
        const bool lowRate=targetKnown&&requestKnown&&s.consoleTargetKbps<0.85*s.requestedKbps;
        if(lowRate){if(lowRateSince_<0)lowRateSince_=now;}else if(targetKnown)lowRateSince_=-1;
        if(predictor_.lossRate()>0.01){if(lossSince_<0)lossSince_=now;}else lossSince_=-1;
        // "Start lower next time" only when the link is to blame: the console held its
        // target low for a minute AND the link lost packets or stalled within it.
        // A low rate alone (a calm menu) is no reason.
        if(lost>0||d.networkLimited||interrupted)lastLinkTrouble_=now;
        const bool rateBelow=lowRateSince_>=0&&now-lowRateSince_>=kLowRateMs&&now-lastLinkTrouble_<=kLowRateMs;
        if(rateBelow)why|=reason::kRateBelowRequest;
        d.suggestedStartKbps=0;
        if(requested_>0){
            if(rateBelow&&targetHave_)d.suggestedStartKbps=round500((std::max)(2000.0,(std::min)(targetEw_,requested_*0.95)));
            else if(lossSince_>=0&&now-lossSince_>=kLowRateMs)d.suggestedStartKbps=round500((std::max)(2000.0,requested_*0.75));
            else if(now-headroomSince_>=kRaiseAfterMs){
                const double ceiling=(std::min)(requested_*kRaiseFactor,kRaiseCeilingKbps);
                if(ceiling>requested_)d.suggestedStartKbps=round500(ceiling);
            }
        }

        // ---------------- profile, summary ----------------
        ProfileInput pin;
        pin.route=s.route;pin.nowMs=now;pin.spikesLastMinute=predictor_.spikesSince(now-60000);pin.lossRate=predictor_.lossRate();
        pin.signalPercent=predictor_.signal();pin.signalTrendPerMin=prediction_.signalTrendPerMin;pin.regime=prediction_.regime;
        pin.rateBelowRequest=rateBelow;pin.raiseSuggested=d.suggestedStartKbps>0&&requested_>0&&d.suggestedStartKbps>requested_;
        profile_=profiles_.update(pin);
        // ---------------- advice (Decision::advice) ----------------
        // The profile's first advice, held to the rate rules above and slowed down:
        //   - LowerPictureRateNextTime only under the "suggest lower" rules (rate held
        //     low with link trouble, or loss above 1 % for a minute); RaiseRateNextTime
        //     only with a raise suggested; otherwise neither;
        //   - LinkIsFine only after kAdviceCleanMs without loss, late packets or link stalls;
        //   - None for the first kAdviceHoldMs, and at most one change per kAdviceHoldMs.
        if(lost>0||late>0||d.networkLimited||interrupted||spikeNow)adviceCleanSince_=now;
        // On unstable Wi-Fi a specific cause (weak signal, periodic pauses) beats the general "use a cable".
        Advice wantAdvice=profile_.advice[0];
        if(profile_.advice[1]==Advice::MoveCloserOrFasterBand||profile_.advice[1]==Advice::BackgroundActivity)wantAdvice=profile_.advice[1];
        const bool lowerOk=rateBelow||(lossSince_>=0&&now-lossSince_>=kLowRateMs);
        if(wantAdvice==Advice::LowerPictureRateNextTime&&!lowerOk)wantAdvice=profile_.profile==Profile::UnstableWifi?Advice::UseCable:Advice::None;
        if(wantAdvice==Advice::RaiseRateNextTime&&!pin.raiseSuggested)wantAdvice=Advice::LinkIsFine;
        if(wantAdvice==Advice::LinkIsFine&&now-adviceCleanSince_<kAdviceCleanMs)wantAdvice=Advice::None;
        if(adviceSince_<0)adviceSince_=now;
        if(wantAdvice!=advice_&&now-adviceSince_>=kAdviceHoldMs){advice_=wantAdvice;adviceSince_=now;}
        d.advice=advice_;
        d.profile=profile_.profile;d.regime=prediction_.regime;d.stallRisk=risk;
        if(s.route==Route::Wired&&d.reorderWindowMs==0&&d.audioTargetMs==0&&!d.networkLimited&&!(why&(reason::kLoss|reason::kLateFramesRescuable)))why|=reason::kWiredClean;
        d.rttMs=predictor_.lastRtt();d.rttMaxMs=prediction_.rttMaxMs;
        d.targetKbps=targetKnown?s.consoleTargetKbps:(targetHave_?targetEw_:-1);
        d.measuredKbps=known(s.measuredKbps)?s.measuredKbps:-1;
        d.gapP95Ms=prediction_.gapExcessP95Ms;d.gapMaxMs=prediction_.gapExcessMaxMs;d.signalPercent=predictor_.signal();
        d.late=predictor_.lateSince(now-Predictor::kSummaryMs);d.lost=predictor_.lostSince(now-Predictor::kSummaryMs);
        d.reasons=why;
        d.changed=d.reorderWindowMs!=last_.reorderWindowMs||d.audioTargetMs!=last_.audioTargetMs;
        if(d.changed)++changes_;
        last_=d;
        return d;
    }
    std::uint64_t changes()const noexcept{return changes_;}
private:
    static double snapUp(double ms)noexcept{for(auto step:kLadderMs)if(static_cast<double>(step)>=ms-1e-9)return step;return kLadderMs.back();}
    static double snapUpStrict(double ms)noexcept{for(auto step:kLadderMs)if(static_cast<double>(step)>ms+1e-9)return step;return kLadderMs.back();}
    static double snapDown(double ms)noexcept{double r=kLadderMs.front();for(auto step:kLadderMs)if(static_cast<double>(step)<ms-1e-9)r=step;return r;}
    static double roundUp(double ms)noexcept{return std::ceil((std::min)(ms,1000.0)/kAudioStepMs)*kAudioStepMs;}
    static std::uint32_t round500(double kbps)noexcept{return static_cast<std::uint32_t>(std::floor((std::min)(kbps,1e6)/500.0))*500u;}
    template<std::size_t N> struct Changes {std::array<double,N> at{};std::size_t next=0,count=0;};
    template<std::size_t N> static bool budget(Changes<N>& c,double now,std::size_t)noexcept{
        std::size_t inMinute=0;for(std::size_t i=0;i<c.count;++i)if(now-c.at[i]<60000)++inMinute;
        if(inMinute>=N)return false;
        c.at[c.next]=now;c.next=(c.next+1)%N;if(c.count<N)++c.count;
        return true;
    }
    Config config_;
    Predictor predictor_;
    ProfileTracker profiles_;
    Prediction prediction_{};
    ProfileResult profile_{};
    Decision last_{};
    bool started_=false,lastWasRelax_=false,targetHave_=false;
    double configured_=16,current_=16,relaxFrom_=0,lastEvidence_=0,lastLate_=0;
    unsigned penalty_=0,lossWindows_=0;
    std::array<double,2> lossWindowAt_{-1e18,-1e18};
    double audio_=0,lastUnderrun_=0,lastAudioChange_=0;
    double limitedUntil_=-1,lastQueueHigh_=-1e18,frameRate_=-1,lastStallEvent_=-1e18,pendingAt_=-1,pendingWindowMs_=0,pendingLossy_=0;
    std::int64_t pendingFrames_=0;bool pendingQueue_=false;
    std::array<double,8> stallAt_{-1,-1,-1,-1,-1,-1,-1,-1};std::size_t stallNext_=0;
    std::array<double,25> rates_{};std::size_t rateNext_=0,rateCount_=0;
    Advice advice_=Advice::None;double adviceSince_=-1,adviceCleanSince_=0;
    double preemptUntil_=-1,lastLinkTrouble_=-1e18,reorderShare_=0;
    double headroomSince_=0,lowRateSince_=-1,lossSince_=-1,requested_=0,targetEw_=0,lastTargetAt_=0;
    Changes<kReorderChangesPerMinute> reorderChanges_{};
    Changes<kAudioChangesPerMinute> audioChanges_{};
    std::uint64_t changes_=0;
};
} // namespace veyra::remoteplay::smartlink
