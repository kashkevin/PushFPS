// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental (sprint 5): where keyboard and mouse input spends its time inside the PC, measured
// stage by stage, and the self-check that falls back to the simpler path when the adaptive one
// measures worse. Portable and unit-tested (tests/remoteplay/KbmInputStatsTests.inc); the Win32
// capture (RemotePlayKeyboardMouse.h) and the session owner (RemotePlaySessionSource.cpp) feed it
// with one QueryPerformanceCounter read per stage, only when VEYRA_EXP_RP_STATS=1.
//
// Stages (all software times on the PC, never input-to-photon):
//   rawToTranslate  the oldest input event not yet translated (WM_INPUT, key or button message)
//                   -> the pad poll that translated it (waiting for the poll)
//   translate       the cost of that poll's translation (KeyboardMousePad::apply)
//   stateToSend     the pad state handed to the engine -> the session owner gave it to Chiaki
//   eventToSend     the oldest event in that state -> given to Chiaki
//   queueAge        age of the oldest pad state still queued when the owner took one
//   sendGap         time between two CHANGED states given to Chiaki that follow each other within
//                   100 ms (one movement, one press): the 4 ms repeats of an unchanged state make no
//                   packet and a pause of the hands is not a gap. A late pad poll shows here.
//   pollGap         time between two pad polls on the interface thread (its own line,
//                   [slipstream-input-poll]): a busy interface shows as late polls.
// Chiaki's feedback sender thread and the network come after the last stage and are not seen here.
// Two states handed before the owner woke are counted as one hand-off (the newer one): the older
// stick position was replaced, which is what the queue does with it.
//
// Threads: the interface thread owns KbmInputMeter and calls KbmInputProbe::handed / drain; the
// session owner thread calls KbmInputProbe::sent only. The probe is lock-free and does not
// allocate: a seqlock for the last hand-off and a single-producer ring of packed records.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include "KbmInputProfile.h"
#include "Types.h"
namespace veyra::remoteplay {

// The newest kCapacity samples of one stage; percentiles over them.
class KbmStageSamples {
public:
    static constexpr std::size_t kCapacity=4096;
    void add(double v)noexcept{
        if(!std::isfinite(v)||v<0)return;
        values_[head_]=static_cast<float>(v);head_=(head_+1)%kCapacity;count_=std::min(kCapacity,count_+1);
        max_=std::max(max_,v);
    }
    std::size_t count()const noexcept{return count_;}
    double max()const noexcept{return count_?max_:-1.0;}
    // Nearest rank; -1 without samples. Sorts a copy (called at most once per window per stage).
    double percentile(double p)const noexcept{
        if(!count_)return -1;
        std::array<float,kCapacity>& scratch=scratch_;
        std::copy(values_.begin(),values_.begin()+std::ptrdiff_t(count_),scratch.begin());
        const std::size_t rank=std::min(count_-1,static_cast<std::size_t>(std::ceil(std::clamp(p,0.0,1.0)*double(count_)))-(p>0?std::size_t{1}:std::size_t{0}));
        std::nth_element(scratch.begin(),scratch.begin()+std::ptrdiff_t(rank),scratch.begin()+std::ptrdiff_t(count_));
        return double(scratch[rank]);
    }
    void clear()noexcept{count_=0;head_=0;max_=0;}
private:
    std::array<float,kCapacity> values_{};
    mutable std::array<float,kCapacity> scratch_{};
    std::size_t count_=0,head_=0;
    double max_=0;
};

// Cross-thread part. Times are ticks of one clock (QueryPerformanceCounter on Windows) with
// `ticksPerSecond` given by the reader.
class KbmInputProbe {
public:
    enum class Kind : std::uint32_t {StateToSend=1,EventToSend=2,QueueAge=3,SendGap=4,QueueDepth=5,Report=6};
    static constexpr std::size_t kRing=8192;
    // Interface thread: a pad state was handed to the engine at `handTicks`; `eventTicks` is the
    // oldest input event it contains (0: none, a poll without new keyboard / mouse input).
    void handed(std::int64_t handTicks,std::int64_t eventTicks)noexcept{
        const std::uint64_t s=seq_.load(std::memory_order_relaxed);
        seq_.store(s+1,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        hand_.store(handTicks,std::memory_order_relaxed);
        event_.store(eventTicks,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s+2,std::memory_order_release);
    }
    // Session owner thread: a pad state went to Chiaki at `nowTicks`. `changed`: it differs from
    // the previous one (an unchanged repeat makes no packet and only reports the queue).
    // `depth` / `oldestTicks`: the queue before the take (0 / 0 when empty).
    // Microsecond values are packed with the stage into one 32-bit record (values above 2^28 us,
    // about 268 s, are clamped).
    static constexpr double kIdleGapUs=100000;
    void sent(std::int64_t nowTicks,std::int64_t ticksPerSecond,bool changed,std::uint32_t depth,std::int64_t oldestTicks)noexcept{
        if(ticksPerSecond<=0)return;
        const auto us=[&](std::int64_t ticks){return ticks<=0?0.0:double(ticks)*1e6/double(ticksPerSecond);};
        if(depth>0)push(Kind::QueueDepth,double(depth));
        if(oldestTicks>0&&nowTicks>oldestTicks)push(Kind::QueueAge,us(nowTicks-oldestTicks));
        if(!changed)return;
        if(lastSent_>0){const double gap=us(nowTicks-lastSent_);if(gap<kIdleGapUs)push(Kind::SendGap,gap);}
        lastSent_=nowTicks;
        push(Kind::Report,0);
        // The last hand-off, read consistently or not at all.
        for(int attempt=0;attempt<2;++attempt){
            const std::uint64_t before=seq_.load(std::memory_order_acquire);
            if(before&1u)continue;
            const std::int64_t hand=hand_.load(std::memory_order_relaxed),event=event_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if(seq_.load(std::memory_order_relaxed)!=before)continue;
            if(before==consumed_)return;   // nothing new handed since the last send
            consumed_=before;
            if(hand>0&&nowTicks>=hand)push(Kind::StateToSend,us(nowTicks-hand));
            if(event>0&&nowTicks>=event)push(Kind::EventToSend,us(nowTicks-event));
            return;
        }
    }
    // Interface thread: every record written since the last drain, oldest first. Records the owner
    // overwrote before they were read are lost and counted.
    template<class F> void drain(F&& record)noexcept{
        const std::uint64_t head=head_.load(std::memory_order_acquire);
        if(head-tail_>kRing){lost_+=head-tail_-kRing;tail_=head-kRing;}
        for(;tail_<head;++tail_){
            const std::uint32_t r=ring_[tail_%kRing].load(std::memory_order_relaxed);
            record(Kind(r>>28),double(r&0x0FFFFFFFu));
        }
    }
    std::uint64_t lost()const noexcept{return lost_;}
    // A new session: the owner side starts over (called by the owner before its first send).
    void ownerRestart()noexcept{lastSent_=0;}
private:
    void push(Kind kind,double value)noexcept{
        const double clamped=std::clamp(value,0.0,double(0x0FFFFFFF));
        const std::uint32_t r=(std::uint32_t(kind)<<28)|static_cast<std::uint32_t>(std::lround(clamped));
        const std::uint64_t h=head_.load(std::memory_order_relaxed);
        ring_[h%kRing].store(r,std::memory_order_relaxed);
        head_.store(h+1,std::memory_order_release);
    }
    std::atomic<std::uint64_t> seq_{0};
    std::atomic<std::int64_t> hand_{0},event_{0};
    std::array<std::atomic<std::uint32_t>,kRing> ring_{};
    std::atomic<std::uint64_t> head_{0};
    // owner thread only
    std::int64_t lastSent_=0;
    std::uint64_t consumed_=0;
    // interface thread only
    std::uint64_t tail_=0,lost_=0;
};
// The one probe of the process (one PS5 session at a time).
inline KbmInputProbe& kbmInputProbe()noexcept{static KbmInputProbe probe;return probe;}

// The session owner's side, as one object in its loop: what the queue held before a take, then the
// state that went to Chiaki. Off (statistics not asked for): every call returns at once and no
// clock is read for it. Times are remoteplay::monotonic100ns.
class KbmOwnerTap {
public:
    explicit KbmOwnerTap(bool on)noexcept:on_(on){if(on_)kbmInputProbe().ownerRestart();}
    bool on()const noexcept{return on_;}
    void queued(std::size_t depth,HostTime oldest)noexcept{if(on_){depth_=static_cast<std::uint32_t>(depth);oldest_=oldest;}}
    void sent(const ControllerState& state,HostTime now)noexcept{
        if(!on_)return;
        const bool changed=!(state==last_);
        if(changed)last_=state;
        kbmInputProbe().sent(now,10000000,changed,depth_,oldest_);
        depth_=0;oldest_=0;
    }
private:
    bool on_;
    std::uint32_t depth_=0;
    HostTime oldest_=0;
    ControllerState last_{};
};

// What a release of the capture had to let go of. `stuckReleases`: releases with something held
// (without the release it would have stayed pressed on the console). `heldAtLoss`: how many inputs
// were held when the session itself went away (connection loss, session end).
struct KbmReleaseCounters {
    std::uint64_t stuckReleases=0,heldAtLoss=0;
    void released(std::size_t held,bool sessionEnded)noexcept{
        if(!held)return;
        ++stuckReleases;
        if(sessionEnded)heldAtLoss+=held;
    }
};

// What the [slipstream-input] line says besides the stages.
struct KbmInputLineInfo {
    const char* source="none";          // kbm | pad | mixed | none
    const char* inputProfile="legacy";
    const char* context="none";
    std::string profile="builtin";      // file stem or builtin id, never a path
    double mouseHz=-1;
    double smoothingMs=-1;
    double saturatedPct=-1;
    const char* curve="legacy";
    std::uint64_t switches=0,stuckReleases=0,heldAtLoss=0;
    const char* fallback="none";
};

// What the Mouse & Keyboard page shows: the last finished 5 s window. Software times inside the PC
// ("processing"), never input-to-photon. -1: not known (not measured, or nothing happened).
struct KbmInputReadout {
    const char* source="none";          // kbm | pad | mixed | none
    KbmInputProfileKind profile=KbmInputProfileKind::Legacy;
    KbmContext context=KbmContext::Walk;
    int mouseHz=0;                      // 0: not measured yet
    double translateUsP95=-1,rawToTranslateUsP95=-1,stateToSendMsP95=-1,eventToSendMsP95=-1,sendGapMsP95=-1;
    double smoothingMs=0,saturatedPct=-1;
    std::uint64_t stuckReleases=0,heldAtLoss=0;
    const char* fallback="none";        // none | translate-slower | queue-older
};

// Interface thread: one window of samples (5 s), the line, and the readout for the page.
class KbmInputMeter {
public:
    KbmStageSamples rawToTranslate,translate,stateToSend,eventToSend,queueAge,sendGap,pollGap;
    KbmStageSamples shadowAdaptive,shadowSimple;      // the self-check's two timings of the same counts (us)
    // Stream "kbm" (docs/drafts/KBM_AUDIT.md D1 / D4): how steady the right stick is while it moves. stickChange:
    // the change of its tilt from one poll to the next, in % of their mean, for two moving polls in a row (a steady
    // hand should give a few %; report quantisation gives tens). stickDips: a poll at rest between two moving ones.
    KbmStageSamples stickChange;
    std::uint64_t stickDips=0;
    std::uint64_t mouseEvents=0,reports=0,polls=0,latePolls=0,kbmPolls=0,padPolls=0;
    std::uint32_t queueDepthMax=0,rawBacklogMax=0;
    double windowSeconds=0;
    // One pad poll `gapUs` after the previous one, with a nominal interval of `pollMs`. Late: more
    // than twice the interval (a timer that fired one whole interval late, or a busy thread).
    void poll(double gapUs,int pollMs)noexcept{
        ++polls;
        if(gapUs<=0)return;
        pollGap.add(gapUs/1000);
        if(pollMs>0&&gapUs>2000.0*pollMs)++latePolls;
    }
    // The right stick's tilt (0..1) of a poll's state, in order. Across windows on purpose: a dip at a window's edge counts.
    void stick(double tilt)noexcept{
        if(!std::isfinite(tilt)||tilt<0)tilt=0;
        if(tilt>0&&lastTilt_>0)stickChange.add(200.0*std::abs(tilt-lastTilt_)/(tilt+lastTilt_));
        if(tilt>0&&lastTilt_==0&&beforeTilt_>0)++stickDips;
        beforeTilt_=lastTilt_;lastTilt_=tilt;
    }
    // The second line, without its "[slipstream-input-poll] " tag: the interface thread's own timing.
    std::string pollLine(int pollMs)const{
        char buffer[256];
        std::snprintf(buffer,sizeof(buffer),"polls=%llu pollMs=%d pollGapMsP50=%.3f pollGapMsP95=%.3f pollGapMsP99=%.3f pollGapMsMax=%.3f latePolls=%llu rawBacklogMax=%u",
            static_cast<unsigned long long>(polls),pollMs,pollGap.percentile(.5),pollGap.percentile(.95),pollGap.percentile(.99),pollGap.max(),
            static_cast<unsigned long long>(latePolls),rawBacklogMax);
        return buffer;
    }
    void takeProbe(KbmInputProbe& probe)noexcept{
        probe.drain([this](KbmInputProbe::Kind kind,double us){
            switch(kind){
            case KbmInputProbe::Kind::StateToSend:stateToSend.add(us/1000);break;
            case KbmInputProbe::Kind::EventToSend:eventToSend.add(us/1000);break;
            case KbmInputProbe::Kind::QueueAge:queueAge.add(us/1000);break;
            case KbmInputProbe::Kind::SendGap:sendGap.add(us/1000);break;
            case KbmInputProbe::Kind::QueueDepth:queueDepthMax=std::max(queueDepthMax,static_cast<std::uint32_t>(us));break;
            case KbmInputProbe::Kind::Report:++reports;break;
            }
        });
    }
    void clear()noexcept{
        rawToTranslate.clear();translate.clear();stateToSend.clear();eventToSend.clear();queueAge.clear();sendGap.clear();pollGap.clear();
        shadowAdaptive.clear();shadowSimple.clear();stickChange.clear();stickDips=0;
        mouseEvents=reports=polls=latePolls=kbmPolls=padPolls=0;queueDepthMax=rawBacklogMax=0;windowSeconds=0;
    }
    // The contract's line (S5_CONTRACT.md item 5), without the "[slipstream-input] " tag.
    std::string line(const KbmInputLineInfo& i)const{
        std::string out;
        const auto n=[&](const char* key,double v,int decimals){
            char buffer[64];
            if(v<0)std::snprintf(buffer,sizeof(buffer)," %s=-1",key);
            else std::snprintf(buffer,sizeof(buffer)," %s=%.*f",key,decimals,v);
            out+=buffer;
        };
        const auto s=[&](const char* key,const std::string& v){out+=' ';out+=key;out+='=';out+=v;};
        const auto u=[&](const char* key,std::uint64_t v){out+=' ';out+=key;out+='=';out+=std::to_string(v);};
        s("source",i.source);s("inputProfile",i.inputProfile);s("context",i.context);s("profile",i.profile);
        u("mouseEvents",mouseEvents);n("mouseHz",i.mouseHz,0);
        n("rawToTranslateUsP50",rawToTranslate.percentile(.5),0);n("rawToTranslateUsP95",rawToTranslate.percentile(.95),0);n("rawToTranslateUsP99",rawToTranslate.percentile(.99),0);
        n("translateUsP50",translate.percentile(.5),1);n("translateUsP95",translate.percentile(.95),1);n("translateUsP99",translate.percentile(.99),1);
        n("stateToSendMsP50",stateToSend.percentile(.5),3);n("stateToSendMsP95",stateToSend.percentile(.95),3);n("stateToSendMsP99",stateToSend.percentile(.99),3);
        n("eventToSendMsP50",eventToSend.percentile(.5),3);n("eventToSendMsP95",eventToSend.percentile(.95),3);n("eventToSendMsP99",eventToSend.percentile(.99),3);
        n("eventToSendMsMax",eventToSend.max(),3);
        n("queueAgeMsP95",queueAge.percentile(.95),3);
        out+=" queueDepthMax="+std::to_string(queueDepthMax);
        u("reports",reports);n("sendHz",windowSeconds>0?double(reports)/windowSeconds:-1,1);
        n("sendGapMsP50",sendGap.percentile(.5),3);n("sendGapMsP95",sendGap.percentile(.95),3);n("sendGapMsMax",sendGap.max(),3);
        n("smoothingMs",i.smoothingMs,1);n("saturatedPct",i.saturatedPct,1);s("curve",i.curve);
        u("switches",i.switches);u("stuckReleases",i.stuckReleases);u("heldAtLoss",i.heldAtLoss);s("fallback",i.fallback);
        // Stream "kbm": appended after the contract's keys (their order is unchanged).
        n("stickChangePctP50",stickChange.percentile(.5),1);n("stickChangePctP95",stickChange.percentile(.95),1);u("stickDips",stickDips);
        return out.substr(1);
    }
private:
    double lastTilt_=0,beforeTilt_=0;
};

// The self-check. Inputs per 5 s window; the decision is pure.
//   translate-slower: the adaptive transform's P95 cost is above the simple one's by more than
//                     max(250 us, the simple cost) - i.e. more than twice it and at least 250 us more -
//                     in kWindows windows in a row (both timed on copies of the same state, same poll).
//   queue-older:      the state-to-send P95 is more than 2 ms above the baseline measured with the
//                     legacy path in this session, in kWindows windows in a row. Without a baseline
//                     this rule does not act (it cannot know what the simple path would have done).
// Once fallen back it stays until the player picks a profile again (reset()).
struct KbmFallbackWindow {
    double adaptiveTranslateUsP95=-1,simpleTranslateUsP95=-1;
    double stateToSendMsP95=-1,baselineStateToSendMsP95=-1;
};
class KbmFallbackCheck {
public:
    static constexpr int kWindows=3;
    static constexpr double kTranslateMarginUs=250,kQueueMarginMs=2;
    // Returns the reason when this window makes it fall back, else nullptr.
    const char* update(const KbmFallbackWindow& w)noexcept{
        if(reason_)return nullptr;
        const bool slower=w.adaptiveTranslateUsP95>=0&&w.simpleTranslateUsP95>=0&&
            w.adaptiveTranslateUsP95>w.simpleTranslateUsP95+std::max(kTranslateMarginUs,w.simpleTranslateUsP95);
        const bool older=w.stateToSendMsP95>=0&&w.baselineStateToSendMsP95>=0&&
            w.stateToSendMsP95>w.baselineStateToSendMsP95+kQueueMarginMs;
        slower_=slower?slower_+1:0;older_=older?older_+1:0;
        if(slower_>=kWindows)reason_="translate-slower";
        else if(older_>=kWindows)reason_="queue-older";
        return reason_;
    }
    const char* reason()const noexcept{return reason_?reason_:"none";}
    bool fellBack()const noexcept{return reason_!=nullptr;}
    void reset()noexcept{reason_=nullptr;slower_=older_=0;}
private:
    const char* reason_=nullptr;
    int slower_=0,older_=0;
};

// Which device moved the pad in a window: keyboard / mouse, the physical controller, both, none.
inline const char* kbmInputSource(std::uint64_t kbmPolls,std::uint64_t padPolls)noexcept{
    if(kbmPolls&&padPolls)return "mixed";
    if(kbmPolls)return "kbm";
    if(padPolls)return "pad";
    return "none";
}
} // namespace veyra::remoteplay
