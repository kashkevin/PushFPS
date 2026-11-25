// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// [slipstream-cadence]: how evenly the presents of one 5 s window can reach the screen (sprint 5, stream
// "present"; the owner's report of 21 Nov: "about 120 FPS, sometimes extremely smooth, sometimes noticeably
// less fluid, while the counter still says about 120").
//
// The FPS counter is the rate of Present CALLS (FrameFlowWindow presentSubmitFps, "not scanout FPS"). Every
// present goes out with sync interval 1 (PresentSink::present), so on a fixed refresh each one is shown at a
// vblank of its own: 120 presents a second on a 179.95 Hz panel WITHOUT variable refresh can only be shown
// for 1 and 2 refreshes in turn (5.6 / 11.1 ms), a visible judder, while the counter says 120. With variable
// refresh active the panel follows the presents and their own spacing is what is seen.
//
// This window records the Present call times (real and generated) and reports:
// - rates: real, generated and all presents per second (submitted, the counter's quantity);
// - the spacing of the calls (P50 / P95 / P99 / max) and the share of intervals off the window's mean
//   interval by more than half a refresh (or a quarter of the mean when the refresh is unknown);
// - the FIXED-REFRESH MODEL: each present shown at the first vblank at or after its call and never before
//   the vblank after the previous one (what sync interval 1 does on a fixed refresh, ignoring GPU time and
//   composition); its hold counts (1, 2, 3+ refreshes), the share of holds unlike the most common one, and
//   the shown rate. Under variable refresh this model does NOT apply (`vrr=1`): there the call spacing is
//   the answer. It is a model of DXGI's rule, not a measurement of the panel (PresentMon measures that);
// - the even cadence: the highest rate at or below the present rate that a fixed refresh shows with equal
//   holds (refresh / k), e.g. 89.98 at 179.95 Hz for a 120 present rate, 59.98 for 60.
//
// Portable, fixed memory, one owner thread (the presenter's).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>

namespace veyra::engine {

struct PushCadenceSummary {
    double seconds=0;
    std::uint64_t real=0,generated=0;          // presents submitted in the window
    double submittedFps=0,realFps=0,generatedFps=0;
    std::uint64_t intervals=0;                 // call intervals measured (gaps over 250 ms excluded: a pause)
    double intervalP50Ms=0,intervalP95Ms=0,intervalP99Ms=0,intervalMaxMs=0,intervalMeanMs=0;
    double offPct=0;                           // % of intervals off the mean by more than half a refresh
    double refreshHz=0;                        // 0 = unknown
    int vrr=-1;                                // driver's variable refresh active: 1, 0, -1 unknown
    // Fixed-refresh model (meaningful when vrr != 1 and refreshHz is known).
    std::uint64_t hold1=0,hold2=0,hold3=0;     // shown intervals of 1, 2, 3+ refreshes
    double unevenPct=0;                        // % of shown intervals unlike the most common hold
    double shownFps=0;                         // presents shown per second in the model
    double evenFps=0;std::uint32_t evenDivisor=0;  // refresh / evenDivisor: even holds at or below the present rate
    std::uint64_t overflow=0;                  // presents not recorded (window full)
    std::uint64_t pauses=0;                    // gaps over 250 ms between presents (the window then is no rate)
};

// The even rate a fixed refresh can show for a present rate `fps`: refresh / k for the smallest k with
// refresh / k <= fps * 1.02 (2 % for a slightly fast source). 0 when either is unknown.
inline double pushEvenCadenceFps(double refreshHz,double fps,std::uint32_t* divisor=nullptr){
    if(divisor)*divisor=0;
    if(!(refreshHz>=1)||!(fps>=1))return 0;
    std::uint32_t k=1;
    while(k<64&&refreshHz/k>fps*1.02)++k;
    if(divisor)*divisor=k;
    return refreshHz/k;
}

// True when `fps` presents a second sit on a fixed refresh with equal holds: refresh / fps within 2 % of a
// whole number of refreshes (1, 2, 3, ...).
inline bool pushEvenOnRefresh(double refreshHz,double fps){
    if(!(refreshHz>=1)||!(fps>=1)||fps>refreshHz*1.02)return false;
    const double ratio=refreshHz/fps,k=std::round(ratio);
    return k>=1&&std::abs(ratio-k)<=0.02*k;
}

// Experiment VEYRA_EXP_PRESENT_EVEN_CADENCE (adaptive frame generation only, while the driver reports variable
// refresh NOT active): among the multipliers 2..ceiling that `selectable(m)` allows, those whose output
// content x m has equal holds on the refresh and does not exceed `limitFps` (the output cap, or the
// refresh), the one closest to the target on a log scale (lower on a tie, as pushAdaptiveFgMultiplier).
// 0 = none is even: the caller keeps its usual choice. 180 Hz, 60 content: 3X (180). 144 Hz, 60: none.
template<class Selectable>
unsigned pushEvenFgMultiplier(unsigned ceiling,double contentFps,double targetFps,double refreshHz,double limitFps,Selectable selectable){
    if(ceiling<2||!(contentFps>0)||!(targetFps>0)||!(refreshHz>=1)||!(limitFps>0))return 0;
    unsigned best=0;double bestError=0;
    for(unsigned m=2;m<=ceiling;++m){
        const double out=contentFps*m;
        if(!selectable(m)||out>limitFps*1.02||!pushEvenOnRefresh(refreshHz,out))continue;
        const double error=std::abs(std::log(out/targetFps));
        if(best==0||error<bestError-1e-9){best=m;bestError=error;}
    }
    return best;
}

// The VRR state the cadence figures use (review 03:44 UTC 21 Nov). Only NVAPI's bIsVRREnabled says variable
// timing applies to our flips; bIsDisplayInVRRMode describes the monitor's mode (G-SYNC Compatible switched on
// for the display) and was 1 in all 144 readings of the owner's panel, including BENCH1's whole runs on a
// fixed refresh grid, so it is ignored here.
//   1  = the driver reports VRR enabled;
//   0  = the driver answered, VRR not enabled, in a reading taken AFTER this chain's first present, with no
//        confirmation pending and no resize / full-screen change in the last 3 s;
//  -1  = anything else: no driver answer (AMD, Intel, NVAPI not ready), only the start-up reading, a pending
//        confirmation, or a recent resize. With -1 nothing is claimed about evenness either way.
inline int pushVrrCadenceState(bool known,bool enabled,bool readAfterFirstPresent,bool confirmationPending,bool recentResize){
    if(!known)return -1;
    if(enabled)return 1;
    if(!readAfterFirstPresent||confirmationPending||recentResize)return -1;
    return 0;
}

// When the presenter reads the driver's VRR state for the cadence figures (phase-3 review, 21 Nov). One forced read
// 5 s after a chain's first present (the owner's evidence: the first post-present read at about 5 s said active in
// every run; BENCH2 puts engagement at 1-3 s); while that read says "not enabled", one more at about 15 s. A resize /
// full-screen change forgets the post-present reading (VRR may start or stop for the new size) and reads again 5 s
// later. Other forced reads issued after the first present (vrr-confirm, slipstream-stats) count too. Times: ms on
// one clock. Pure; the presenter owns it (one thread).
class PushVrrConfirmSchedule {
public:
    static constexpr double kFirstMs=5000,kSecondMs=15000,kResizeMs=5000,kResizeUnknownMs=3000;
    void reset(){*this=PushVrrConfirmSchedule{};}
    void firstPresent(double nowMs){if(presented_)return;presented_=true;firstMs_=nowMs;dueMs_=nowMs+kFirstMs;second_=false;}
    void resized(double nowMs){resizeMs_=nowMs;haveResize_=true;readAfterPresent_=false;pending_=false;if(presented_){dueMs_=nowMs+kResizeMs;second_=false;}}
    void monitorChanged(){readAfterPresent_=false;pending_=false;}
    // The presenter asks every loop; true = issue the forced read now (once per due time).
    bool due(double nowMs){
        if(dueMs_<0||nowMs<dueMs_)return false;
        dueMs_=-1;pending_=true;return true;
    }
    // Another forced read was issued (vrr-confirm, slipstream-stats): it counts once the chain presented.
    void otherReadIssued(){if(presented_)pending_=true;}
    // A new driver state was applied; `enabled` = bIsVRREnabled in it.
    void applied(double nowMs,bool enabled){
        if(!pending_)return;
        pending_=false;readAfterPresent_=true;
        if(!enabled&&!second_&&presented_&&dueMs_<0){second_=true;dueMs_=std::max(nowMs,firstMs_+kSecondMs);}
    }
    bool readAfterPresent()const{return readAfterPresent_;}
    bool recentResize(double nowMs)const{return haveResize_&&nowMs>=resizeMs_&&nowMs-resizeMs_<kResizeUnknownMs;}
    double dueMs()const{return dueMs_;}
private:
    bool presented_=false,pending_=false,readAfterPresent_=false,second_=false,haveResize_=false;
    double firstMs_=0,dueMs_=-1,resizeMs_=0;
};

class PushCadenceWindow {
public:
    static constexpr std::size_t kCapacity=4096;   // 5 s at 800 presents a second

    void reset(){count_=0;real_=generated_=overflow_=0;startMs_=-1;haveLast_=false;}

    // One present was submitted at host time `callMs` (the Present call).
    void presented(double callMs,bool generated){
        if(startMs_<0)startMs_=callMs;
        if(generated)++generated_;else ++real_;
        if(count_<times_.size())times_[count_++]=callMs;else ++overflow_;
        lastMs_=callMs;lastGenerated_=generated;haveLast_=true;
    }

    // Close the window at the call time of the most recent present and carry that present into the next
    // window, so N presents span exactly N intervals (closing at a later time, with the newest present
    // inside, counted one present too many per window: about +2 FPS on a 1 s window).
    PushCadenceSummary takeAtLastPresent(double refreshHz,int vrr,double vblankMs=-1){
        if(!haveLast_)return take(0,refreshHz,vrr,vblankMs);
        const double last=lastMs_;const bool generated=lastGenerated_;
        if(generated){if(generated_)--generated_;}else if(real_)--real_;
        if(overflow_)--overflow_;else if(count_)--count_;
        const auto s=take(last,refreshHz,vrr,vblankMs);
        presented(last,generated);
        return s;
    }

    // Close the window at `nowMs`. refreshHz 0 = unknown; vrr 1 / 0 / -1; vblankMs = one known vblank time
    // on the same clock (any past vblank; < 0 = unknown phase, the model then uses phase 0).
    PushCadenceSummary take(double nowMs,double refreshHz,int vrr,double vblankMs=-1){
        PushCadenceSummary s;
        s.refreshHz=refreshHz>=1?refreshHz:0;s.vrr=vrr;s.overflow=overflow_;
        s.seconds=startMs_>=0?std::max(0.0,nowMs-startMs_)/1000.0:0;
        s.real=real_;s.generated=generated_;
        if(s.seconds>0){s.submittedFps=double(real_+generated_)/s.seconds;s.realFps=double(real_)/s.seconds;s.generatedFps=double(generated_)/s.seconds;}
        // Call intervals.
        std::size_t n=0;double sum=0;
        for(std::size_t i=1;i<count_;++i){
            const double d=times_[i]-times_[i-1];
            if(!(d>=0)||d>250){if(d>250)++s.pauses;continue;}
            intervals_[n++]=d;sum+=d;
        }
        if(count_&&nowMs-times_[count_-1]>250)++s.pauses;   // the window ends after a pause
        s.intervals=n;
        if(n){
            s.intervalMeanMs=sum/double(n);
            const double refreshMs=s.refreshHz>0?1000.0/s.refreshHz:0;
            const double tolerance=refreshMs>0?refreshMs/2:s.intervalMeanMs/4;
            std::size_t off=0;for(std::size_t i=0;i<n;++i)if(std::abs(intervals_[i]-s.intervalMeanMs)>tolerance)++off;
            s.offPct=100.0*double(off)/double(n);
            std::sort(intervals_.begin(),intervals_.begin()+static_cast<std::ptrdiff_t>(n));
            const auto at=[&](double q){return intervals_[std::min(n-1,static_cast<std::size_t>(std::ceil(q*double(n))-1))];};
            s.intervalP50Ms=at(0.50);s.intervalP95Ms=at(0.95);s.intervalP99Ms=at(0.99);s.intervalMaxMs=intervals_[n-1];
        }
        // Fixed-refresh model.
        if(s.refreshHz>0&&count_>1){
            const double period=1000.0/s.refreshHz,phase=vblankMs>=0?vblankMs:0;
            double previous=0;bool first=true;std::size_t shown=0;
            for(std::size_t i=0;i<count_;++i){
                double slot=std::ceil((times_[i]-phase)/period-1e-9);
                if(!first&&times_[i]-times_[i-1]>250)first=true;   // after a pause: a new run
                if(!first&&slot<previous+1)slot=previous+1;
                if(!first){
                    const double hold=slot-previous;
                    if(hold<1.5)++s.hold1;else if(hold<2.5)++s.hold2;else ++s.hold3;
                }
                previous=slot;first=false;++shown;
            }
            const std::uint64_t holds=s.hold1+s.hold2+s.hold3;
            if(holds){
                const std::uint64_t common=std::max({s.hold1,s.hold2,s.hold3});
                s.unevenPct=100.0*double(holds-common)/double(holds);
            }
            if(s.seconds>0)s.shownFps=double(shown)/s.seconds;
        }
        s.evenFps=pushEvenCadenceFps(s.refreshHz,s.submittedFps,&s.evenDivisor);
        reset();
        return s;
    }

private:
    std::array<double,kCapacity> times_{};
    std::array<double,kCapacity> intervals_{};
    std::size_t count_=0;
    std::uint64_t real_=0,generated_=0,overflow_=0;
    double startMs_=-1;
    double lastMs_=0;bool lastGenerated_=false,haveLast_=false;
};

inline std::string formatSlipstreamCadence(const PushCadenceSummary& s){
    return std::format("window={:.1f}s submittedFps={:.2f} realFps={:.2f} generatedFps={:.2f} intervals={} intervalP50Ms={:.2f} intervalP95Ms={:.2f} intervalP99Ms={:.2f} intervalMaxMs={:.2f} offPct={:.1f} refreshHz={:.3f} vrr={} fixedHold1={} fixedHold2={} fixedHold3={} fixedUnevenPct={:.1f} fixedShownFps={:.2f} evenFps={:.2f} evenDivisor={} overflow={} pauses={} (Present call spacing; fixed* = what sync interval 1 shows on a fixed refresh, a model that does not apply while vrr=1; not a photon measurement)",
        s.seconds,s.submittedFps,s.realFps,s.generatedFps,s.intervals,s.intervalP50Ms,s.intervalP95Ms,s.intervalP99Ms,s.intervalMaxMs,s.offPct,s.refreshHz,s.vrr,s.hold1,s.hold2,s.hold3,s.unevenPct,s.shownFps,s.evenFps,s.evenDivisor,s.overflow,s.pauses);
}

// The live readout (the owner: never a misleading FPS number). From a 1 s window of the same kind, kept by
// the presenter whatever the statistics switch says, read by the interface's live statistics.
// shownFps (INFERRED: no presentation statistics are read): presents that reach the screen per second. With sync interval 1 or more DXGI shows every present
// for at least one refresh and replaces none, so it is the present rate, at most the refresh rate. With sync
// interval 0 (display sync Automatic) presents can be replaced unseen: not known. uneven: the share of holds
// unlike the most common one on a fixed refresh (vrr=0, the model), or of present intervals off their mean by
// more than half a refresh under variable refresh (vrr=1); not known while the VRR state is unknown.
struct PushLiveCadence {
    bool valid=false;
    double hostMs=0;              // when it was published (steady clock, ms)
    double submittedFps=0,refreshHz=0;
    bool shownKnown=false;double shownFps=0;
    int vrr=-1;
    bool unevenKnown=false;double unevenPct=0;
    static constexpr double kUnevenPct=20; // above this the readout says "uneven"
    bool uneven()const{return unevenKnown&&unevenPct>kUnevenPct;}
    bool fresh(double nowMs)const{return valid&&nowMs>=hostMs&&nowMs-hostMs<3000;}
};
inline PushLiveCadence pushLiveCadenceFrom(const PushCadenceSummary& s,int syncInterval,double hostMs){
    PushLiveCadence c;c.valid=s.seconds>0.2&&s.pauses==0;c.hostMs=hostMs;c.submittedFps=s.submittedFps;c.refreshHz=s.refreshHz;c.vrr=s.vrr;
    c.shownKnown=c.valid&&syncInterval>=1&&s.refreshHz>0&&s.submittedFps>0;
    c.shownFps=c.shownKnown?std::min(s.submittedFps,s.refreshHz):0;
    c.unevenKnown=c.valid&&s.intervals>=10&&s.refreshHz>0&&s.vrr>=0;
    c.unevenPct=!c.unevenKnown?0:s.vrr==1?s.offPct:s.unevenPct;
    return c;
}
class PushLiveCadenceBoard {
public:
    void publish(const PushLiveCadence& c){std::lock_guard lock(mutex_);value_=c;}
    void clear(){std::lock_guard lock(mutex_);value_={};}
    PushLiveCadence read()const{std::lock_guard lock(mutex_);return value_;}
private:
    mutable std::mutex mutex_;
    PushLiveCadence value_{};
};
inline PushLiveCadenceBoard& pushLiveCadenceBoard(){static PushLiveCadenceBoard board;return board;}

} // namespace veyra::engine
