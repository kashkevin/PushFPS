// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Adaptive AV reorder wait (experiment VEYRA_EXP_RP_REORDER_ADAPTIVE, off by
// default). Portable and header-only; unverified on Windows and on a PS5.
//
// The library holds every later video packet while one is missing, for a fixed
// time: 16 ms upstream, 2 ms with VEYRA_EXP_RP_LAN_PROFILE. Too short gives up
// on packets that were only late; too long delays the picture for packets that
// never come. This picks the wait from what the link shows, among
// 2/3/4/6/8/12/16 ms, from what the patched library counts (chiaki patch 0004):
//   - how long after its wait began each out-of-order packet arrived, in time
//     or too late (a histogram; its 99th percentile, with the newest minute
//     weighing most, is the wait the link needs);
//   - packets that arrived after they were given up on (the wait was too short);
//   - waits that ran out with nothing arriving (a longer wait buys nothing);
//   - frames FEC could not complete (what a wait that is too short costs).
// Rules:
//   - Widen quickly: two packets later than the window within 10 s AND frames
//     being lost at the current wait (one within those 10 s and since the
//     last change, and recently at 0.1 a second or more), to the step that
//     covers the 99th percentile, at most three steps per change, 1 s between
//     changes. And at least two of the frames lost since the last change
//     were lost to one of their own packets that was given up and arrived
//     afterwards (ReorderEvidence::lateFramesFailed; not counted by the
//     library yet, so in the app the wait only narrows). A period without
//     such losses narrows one step, unless a widening that paid reached it. (Sprint 3,
//     Linux model reorder_study.cpp, reorder_study_results.json, 5 seeds: at
//     0.5 a second a link with 5 % of packets 1-12 ms late lost 127 frames
//     where the fixed 4 ms lost 9 and 8 ms 5; the refined rule's count is in
//     the doc. Requiring a loss since the last change keeps one old loss from
//     carrying the wait up step after step; requiring it beside late arrivals
//     keeps link losses next to harmless late packets from widening.) Late packets alone do not widen, nor
//     does an occasional lost frame: while FEC completes the frames, giving a
//     late packet up early is the faster hand-over (Linux model,
//     reorder_sim.cpp).
//   - A widening must pay: if frames are not lost at less than half the rate
//     after it, it is rolled back and widening is blocked for 1 minute, then
//     2, 4 ... 16; the block ends at once when frames are then lost clearly
//     faster than they were with the longer wait (the rollback was wrong).
//   - Narrow slowly: one step, after 30 s in which at most 1 % of out-of-order
//     packets needed more than the lower step (10 s when waits keep ending in
//     true losses); a narrowing that had to be undone doubles the next dwell
//     (up to 16x), so a link between two steps settles on the wider one.
//   - A link without reordering shows no evidence and is never touched.
//   - Counters that contradict each other, or a library that reports another
//     wait than the one set, send the wait to the library's own 16 ms for a
//     minute; the third time for good.
//
// It only decides. The session owner reads the counters, calls update() about
// four times a second and passes a changed value to the library. Nothing sent
// to the console changes directly; when a late packet is given up changes.
#include "TransportTuning.h"
#include <algorithm>
#include <cmath>
#include <optional>
namespace veyra::remoteplay {
// Cumulative since the transport was configured; see TransportCounters.
struct ReorderEvidence {
    std::uint64_t videoPackets=0;
    std::uint64_t gaps=0,timeouts=0,skipped=0,lateArrivals=0;
    std::uint64_t fecFailed=0; // frames FEC could not complete
    // Of those, frames one of whose given-up packets arrived afterwards: losses
    // a longer wait would have prevented. The only evidence that widens.
    // Not counted by the library yet (patch 0004 has no per-frame link
    // between a late arrival and a failed frame): 0 in the app, so there the
    // adaptive wait only narrows. The Linux model supplies it.
    std::uint64_t lateFramesFailed=0;
    std::array<std::uint64_t,kReorderLatenessBuckets> lateness{};
    std::uint32_t reorderTimeoutUs=0; // the wait the library says it uses; 0 = no connection yet
};
inline ReorderEvidence reorderEvidence(const TransportCounters& c)noexcept{
    ReorderEvidence e;
    e.videoPackets=c.videoPackets;e.gaps=c.reorderGaps;e.timeouts=c.reorderTimeouts;e.skipped=c.reorderSkipped;
    e.lateArrivals=c.reorderLateArrivals;e.fecFailed=c.fecFailed;e.lateness=c.reorderLateness;e.reorderTimeoutUs=c.reorderTimeoutUs;
    return e;
}
class ReorderWindowController {
public:
    static constexpr std::array<std::uint32_t,7> kStepsUs{2000,3000,4000,6000,8000,12000,16000};
    static constexpr std::uint32_t kFloorUs=2000,kCeilingUs=16000;
    static constexpr HostTime kWidenDwell100ns=10000000;        // 1 s between any change and a widening
    static constexpr HostTime kNarrowDwell100ns=300000000;      // 30 s of evidence at a step before narrowing
    static constexpr HostTime kNarrowDwellLossy100ns=100000000; // 10 s when waits keep ending in true losses
    static constexpr HostTime kEvidenceWindow100ns=100000000;   // late packets and a lost frame within 10 s
    static constexpr HostTime kWidenBlock100ns=600000000;       // 60 s without widening after a rollback, doubling
    static constexpr HostTime kFaultHold100ns=600000000;        // 60 s at the ceiling after inconsistent input
    static constexpr double kHalfLifeSeconds=15.0,kPercentile=0.99,kMinWeight=0.5;
    // A decayed count is this many seconds' worth of a steady rate (half-life / ln 2).
    static constexpr double kRecentSeconds=kHalfLifeSeconds/0.6931471805599453;
    static constexpr double kSurgePerSecond=0.2; // lost frames per second above twice the rate seen with the longer wait
    static constexpr double kWidenLossPerSecond=0.1; // frames must be lost at this rate before a longer wait is tried (0.5 before 19 Nov sprint 3)
    static constexpr HostTime kExplainedWindow100ns=600000000; // the two losses that widen, within 60 s
    static constexpr unsigned kPaidQuietPeriods=3; // then a paid widening no longer blocks narrowing
    static constexpr std::uint64_t kExplainedLosses=2; // lost frames with late arrivals beside them, since the last change
    static constexpr unsigned kWidenEvents=2,kMaxWidenSteps=3,kLossyTimeouts=8,kMaxPenalty=4,kMaxFaults=3;
    enum class Reason : std::uint8_t {None,Widen,Narrow,Rollback,Fault};
    struct Decision {
        std::uint32_t windowUs=kCeilingUs;
        bool changed=false;        // pass windowUs to the library
        Reason reason=Reason::None; // Fault is reported even when the wait was already 16 ms
    };
    // startUs: the wait the session starts with (0 = the library's 16 ms).
    // Snapped up to a step, so never shorter than what was configured.
    // widenOnlyForLostFrames=false is the rule without the lost-frame condition
    // and without the rollback; the simulator compares the two, the app uses true.
    explicit ReorderWindowController(std::uint32_t startUs=kCeilingUs,bool widenOnlyForLostFrames=true)noexcept:gate_(widenOnlyForLostFrames){
        const std::uint32_t wanted=startUs==0?kCeilingUs:startUs;
        index_=kStepsUs.size()-1;
        for(std::size_t i=0;i<kStepsUs.size();++i)if(kStepsUs[i]>=wanted){index_=i;break;}
    }
    std::uint32_t windowUs()const noexcept{return kStepsUs[index_];}
    // The library's counters restart with each connection. The window, the
    // penalties and the fault count are kept: a reconnect is not evidence that
    // the link got better.
    void newConnection()noexcept{based_=false;}
    Decision update(HostTime now,const ReorderEvidence& e)noexcept{
        if(!based_){
            based_=true;previous_=e;mismatch_=0;
            if(!started_){started_=true;lastChange_=now;epochStart_=now;}
            last_=now;
            return {windowUs(),false,Reason::None};
        }
        // Three faults: nothing is read any more, the library's own wait stays.
        if(latched_){previous_=e;last_=now;return {windowUs(),false,Reason::None};}
        bool bad=now<last_||e.videoPackets<previous_.videoPackets||e.gaps<previous_.gaps||e.timeouts<previous_.timeouts||
            e.skipped<previous_.skipped||e.lateArrivals<previous_.lateArrivals||e.fecFailed<previous_.fecFailed||e.lateFramesFailed<previous_.lateFramesFailed||e.lateFramesFailed>e.fecFailed||
            e.timeouts>e.gaps||e.lateArrivals>e.skipped;
        for(std::size_t b=0;b<kReorderLatenessBuckets;++b)bad=bad||e.lateness[b]<previous_.lateness[b];
        // The library applies a new wait when its thread next looks at the video
        // queue, so one differing read can fall between our call and its look.
        // Twice in a row with video flowing means it is not using our value.
        if(!bad&&e.reorderTimeoutUs!=0&&e.videoPackets>previous_.videoPackets){
            if(e.reorderTimeoutUs!=windowUs()){if(++mismatch_>=2)bad=true;}
            else mismatch_=0;
        }
        if(bad)return fault(now,e);
        const double seconds=static_cast<double>(now-last_)/10000000.0;
        const double decay=std::exp2(-seconds/kHalfLifeSeconds);
        std::array<std::uint64_t,kReorderLatenessBuckets> delta{};
        for(std::size_t b=0;b<kReorderLatenessBuckets;++b)delta[b]=e.lateness[b]-previous_.lateness[b];
        const std::uint64_t timeouts=e.timeouts-previous_.timeouts,late=e.lateArrivals-previous_.lateArrivals,failed=e.fecFailed-previous_.fecFailed;
        const std::uint64_t explainedNow=e.lateFramesFailed-previous_.lateFramesFailed;
        previous_=e;last_=now;
        if(now<holdUntil_)return {windowUs(),false,Reason::None};
        // Bucket b ends at kReorderLatenessEdgesUs[b]; step i is that edge for b = i + 1.
        std::uint64_t beyond=0;
        for(std::size_t b=0;b+1<kReorderLatenessBuckets;++b){
            recent_[b]=recent_[b]*decay+static_cast<double>(delta[b]);
            epochTotal_+=delta[b];
            if(b>index_)epochAboveLower_+=delta[b]; // needed more than the next lower step
            if(b>index_+1)beyond+=delta[b];         // later than this window, within the ceiling
        }
        failRecent_=failRecent_*decay+static_cast<double>(failed);epochFailed_+=failed;
        if(failed){lastFailed_=now;anyFailed_=true;}
        // Frames lost in a look in which packets also arrived after being given
        // up: losses a longer wait could have prevented. Counted since the last change.
        failedSinceChange_+=failed;explainedSinceChange_+=explainedNow;epochExplained_+=explainedNow;
        // The two newest such losses (both at `now` when two came together).
        if(explainedNow){explainedAt_[1]=explainedNow>=2?now:explainedAt_[0];explainedAt_[0]=now;}
        if(pendingCount_!=0&&now-pendingLast_>kEvidenceWindow100ns)pendingCount_=0;
        if(beyond){pendingCount_+=beyond;pendingLast_=now;}
        if(timeouts>late)epochTrueLosses_+=timeouts-late;
        const auto needed=neededIndex();
        // Frames lost at the current wait: one since the last change (a loss
        // from before a widening is not evidence that it was too small).
        // And two or more of the frames lost since the last change were lost to
        // one of their own packets that was given up and arrived afterwards
        // (lateFramesFailed): a loss elsewhere beside unrelated late packets is
        // no reason to wait longer (review of sprint 3: such links sat at 6 ms).
        // Within a minute of each other: two over many minutes are chance
        // coincidences on a link whose late packets cost nothing (final review).
        const bool explained=explainedSinceChange_>=kExplainedLosses&&explainedAt_[1]>lastChange_&&now-explainedAt_[1]<=kExplainedWindow100ns;
        const bool costly=!gate_||(anyFailed_&&lastFailed_>lastChange_&&now-lastFailed_<=kEvidenceWindow100ns&&failRecent_/kRecentSeconds>=kWidenLossPerSecond&&explained);
        // After a rollback: frames now lost clearly faster than with the longer wait.
        const bool surge=failRecent_/kRecentSeconds>2.0*wideFailRate_+kSurgePerSecond;
        if(needed&&*needed>index_&&pendingCount_>=kWidenEvents&&costly&&now-lastChange_>=kWidenDwell100ns&&(now>=widenBlockedUntil_||surge)){
            if(now<widenBlockedUntil_){widenBlockedUntil_=0;blockPenalty_=0;} // the rollback was wrong
            if(lastWasNarrow_&&penalty_<kMaxPenalty)++penalty_;
            if(gate_&&!payCheck_){payCheck_=true;rollbackIndex_=index_;failedBefore_=failRecent_;}
            index_=(std::min)(*needed,index_+kMaxWidenSteps);
            if(*needed<=index_)pendingCount_=0; // else the rest follows after the dwell
            changed(now);lastWasNarrow_=false;++widenings_;
            return {windowUs(),true,Reason::Widen};
        }
        const HostTime dwell=(epochTrueLosses_>=kLossyTimeouts?kNarrowDwellLossy100ns:kNarrowDwell100ns)<<penalty_;
        if(now-epochStart_<dwell)return {windowUs(),false,Reason::None};
        // One period of evidence at this step is complete.
        if(payCheck_){
            // Did the widening pay? Frames lost per second in this period against
            // the (decayed) count before it; kHalfLifeSeconds/ln 2 seconds of weight.
            payCheck_=false;
            const double after=static_cast<double>(epochFailed_)/(static_cast<double>(now-epochStart_)/10000000.0);
            const double before=failedBefore_/kRecentSeconds;
            if(after*2.0>before){
                wideFailRate_=after;
                index_=rollbackIndex_;
                widenBlockedUntil_=now+(kWidenBlock100ns<<blockPenalty_);
                if(blockPenalty_<kMaxPenalty)++blockPenalty_;
                pendingCount_=0;
                changed(now);lastWasNarrow_=false;++rollbacks_;
                return {windowUs(),true,Reason::Rollback};
            }
            blockPenalty_=0;paid_=true;
        }
        // At most 1 % of what arrived out of order needed more than the lower step.
        // Or: no frame in the period was lost to a packet given up too early,
        // whatever the lateness, unless this step was reached by a widening
        // that paid (review of sprint 3: packets 3-5 ms late that cost nothing
        // kept a 6 ms wait for good).
        // A paid widening protects its step only while it keeps being needed:
        // after kPaidQuietPeriods periods in a row without a frame lost to a late
        // packet it no longer does (final review of sprint 3: a pay check passed
        // by chance on rare losses held 6 ms for good).
        if(paid_){quietPeriods_=epochExplained_==0?quietPeriods_+1:0;if(quietPeriods_>=kPaidQuietPeriods)paid_=false;}
        const bool lowerIsEnough=epochAboveLower_*100u<=epochTotal_;
        if(index_>0&&((lowerIsEnough&&pendingCount_==0)||(gate_&&epochExplained_==0&&!paid_))){
            if(lastWasNarrow_&&penalty_>0)--penalty_; // two in a row: the trend is real
            --index_;
            changed(now);lastWasNarrow_=true;++narrowings_;
            return {windowUs(),true,Reason::Narrow};
        }
        newEpoch(now);
        return {windowUs(),false,Reason::None};
    }
    // The step that covers the 99th percentile of recent out-of-order arrivals
    // (those within the ceiling); nothing when there is no recent evidence.
    std::optional<std::uint32_t> neededUs()const noexcept{
        const auto index=neededIndex();
        if(!index)return {};
        return kStepsUs[*index];
    }
    unsigned widenings()const noexcept{return widenings_;}
    unsigned narrowings()const noexcept{return narrowings_;}
    unsigned rollbacks()const noexcept{return rollbacks_;}
    unsigned faults()const noexcept{return faults_;}
    unsigned penalty()const noexcept{return penalty_;}
    bool latched()const noexcept{return latched_;} // three faults: the library's wait for the rest of the session
private:
    void newEpoch(HostTime now)noexcept{epochExplained_=0;epochStart_=now;epochTotal_=0;epochAboveLower_=0;epochTrueLosses_=0;epochFailed_=0;}
    void changed(HostTime now)noexcept{paid_=false;quietPeriods_=0;lastChange_=now;newEpoch(now);failedSinceChange_=0;explainedSinceChange_=0;}
    std::optional<std::size_t> neededIndex()const noexcept{
        double total=0;
        for(std::size_t b=0;b+1<kReorderLatenessBuckets;++b)total+=recent_[b];
        if(total<kMinWeight)return {};
        double sum=0;
        for(std::size_t b=0;b+1<kReorderLatenessBuckets;++b){
            sum+=recent_[b];
            if(sum>=kPercentile*total)return b==0?std::size_t{0}:b-1;
        }
        return kStepsUs.size()-1;
    }
    Decision fault(HostTime now,const ReorderEvidence& e)noexcept{
        ++faults_;
        if(faults_>=kMaxFaults)latched_=true;
        previous_=e;last_=now;mismatch_=0;recent_={};pendingCount_=0;failRecent_=0;anyFailed_=false;payCheck_=false;widenBlockedUntil_=0;
        holdUntil_=now+kFaultHold100ns;
        const bool moved=index_!=kStepsUs.size()-1;
        index_=kStepsUs.size()-1;
        // Narrowing starts over after the hold, one dwell later.
        lastChange_=holdUntil_;newEpoch(holdUntil_);lastWasNarrow_=false;
        return {windowUs(),moved,Reason::Fault};
    }
    std::size_t index_=0,rollbackIndex_=0;
    ReorderEvidence previous_{};
    std::array<double,kReorderLatenessBuckets> recent_{};
    double failRecent_=0,failedBefore_=0,wideFailRate_=0;
    HostTime last_=0,lastChange_=0,epochStart_=0,pendingLast_=0,lastFailed_=0,holdUntil_=0,widenBlockedUntil_=0;
    std::uint64_t failedSinceChange_=0,explainedSinceChange_=0,epochExplained_=0;
    std::uint64_t pendingCount_=0,epochTotal_=0,epochAboveLower_=0,epochTrueLosses_=0,epochFailed_=0;
    unsigned mismatch_=0,penalty_=0,blockPenalty_=0,widenings_=0,narrowings_=0,rollbacks_=0,faults_=0;
    bool paid_=false; // the current step was reached by a widening whose pay check passed
    unsigned quietPeriods_=0;
    std::array<HostTime,2> explainedAt_{}; // when the two newest frames lost to late packets were seen // periods in a row, while paid_, without a frame lost to a late packet
    bool gate_=true,based_=false,started_=false,lastWasNarrow_=false,latched_=false,anyFailed_=false,payCheck_=false;
};
} // namespace veyra::remoteplay
