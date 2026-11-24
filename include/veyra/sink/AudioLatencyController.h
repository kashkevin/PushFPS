// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Live audio playout control for PS5 Remote Play (experiment VEYRA_EXP_RP_AUDIO_FAST_RECOVER).
// Portable: no Windows, no FFmpeg. Used on the capture audio owner thread only; no locking inside.
//
// Conventions: times in milliseconds on one host clock. "error" = audio media time now playing minus
// the media time that should be playing; positive = audio early, negative = audio late (excess delay).
// "ratio ppm" follows libswresample compensation: positive = more output samples = slower playback.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace veyra::sink {

// Median of the last presentations' (host time - video pts). Every presented frame counts, generated
// ones included, so frame generation's pair delay is part of the value. Old samples expire.
class AudioVideoDelayFilter {
public:
    void reset(){count_=cursor_=0;}
    void observe(double nowMs,double offsetMs){
        if(!std::isfinite(nowMs)||!std::isfinite(offsetMs))return;
        samples_[cursor_]={nowMs,offsetMs};cursor_=(cursor_+1)%samples_.size();
        count_=std::min(count_+1,samples_.size());
    }
    std::optional<double> value(double nowMs)const{
        std::array<double,9> values{};std::size_t n=0;
        for(std::size_t i=0;i<count_;++i)if(nowMs-samples_[i].at<=kMaxAgeMs&&samples_[i].at<=nowMs)values[n++]=samples_[i].offset;
        if(!n)return std::nullopt;
        std::sort(values.begin(),values.begin()+static_cast<std::ptrdiff_t>(n));
        return values[n/2];
    }
    static constexpr double kMaxAgeMs=500;
private:
    struct Sample {double at=0,offset=0;};
    std::array<Sample,9> samples_{};
    std::size_t count_=0,cursor_=0;
};

// Remote Play audio PTS are counted in samples from the arrival of the first block after a
// discontinuity. Two things move them away from arrival time: a console/PC clock difference, and a
// re-anchor on a burst (Chiaki conceals lost frames and delivers them at once; AudioIngress keeps the
// newest 100 ms and the oldest kept block gets the burst's arrival time, so everything after it is
// stamped up to 100 ms late). The session's ingress mapping (2 s lower envelope of arrival minus PTS)
// shows both. This keeps its change against a reference learned once per session: slewed in steady
// state, so a jump of the 2 s minimum never becomes a jump in the schedule, and taken at once for a
// short window after a discontinuity, when the session restarts playback anyway.
class AudioArrivalDrift {
public:
    void reset(){*this=AudioArrivalDrift{};}
    // A discontinuity: keep the reference, follow the new anchor without slew for kSnapMs. One inside
    // the first kSettleMs ends the learning there: the reference keeps the value from before it (learning
    // on would take a late re-anchor as the reference and keep audio late for the whole session).
    void rebase(double nowMs){
        if(started_&&nowMs-since_<kSettleMs)since_=nowMs-kSettleMs;
        snapUntil_=nowMs+kSnapMs;slopeAt_=nowMs+kSnapMs;haveSlope_=false;
    }
    // Returns the offset (ms) to add to the video-derived mapping.
    double observe(double nowMs,double ingressMs){
        if(!std::isfinite(nowMs)||!std::isfinite(ingressMs))return offset_;
        if(!started_){started_=true;since_=nowMs;last_=nowMs;}
        if(nowMs-since_<kSettleMs){reference_=ingressMs;offset_=0;last_=nowMs;slopeAt_=nowMs;slopeOffset_=0;haveSlope_=true;return 0;}
        const double dt=std::clamp(nowMs-last_,0.0,1000.0);last_=nowMs;
        const double wanted=ingressMs-reference_;
        if(nowMs<snapUntil_){offset_=wanted;return offset_;}
        if(!haveSlope_){haveSlope_=true;slopeAt_=nowMs;slopeOffset_=offset_;}
        const double step=kMaxSlewMsPerSecond*dt/1000;
        offset_+=std::clamp(wanted-offset_,-step,step);
        if(nowMs-slopeAt_>=kSlopeWindowMs){
            const double ppm=-1e6*(offset_-slopeOffset_)/(nowMs-slopeAt_);
            driftPpm_=haveDrift_?driftPpm_*0.5+ppm*0.5:ppm;haveDrift_=true;
            slopeAt_=nowMs;slopeOffset_=offset_;
        }
        return offset_;
    }
    double offsetMs()const{return offset_;}
    // Positive: the console's audio clock runs fast against this PC. Valid after one window.
    std::optional<double> driftPpm()const{return haveDrift_?std::optional<double>(driftPpm_):std::nullopt;}
    static constexpr double kSettleMs=2000,kMaxSlewMsPerSecond=2,kSlopeWindowMs=10000,kSnapMs=250;
private:
    bool started_=false,haveDrift_=false,haveSlope_=false;
    double snapUntil_=-1e300,since_=0,last_=0,reference_=0,offset_=0,slopeAt_=0,slopeOffset_=0,driftPpm_=0;
};

struct AudioLatencyConfig {
    double deadbandMs=2;              // no drops inside this
    double gainPpmPerMs=500;          // ratio controller (the original uses 250 per 250 ms step)
    double maxRatioPpm=5000;          // same bound as the original: no new pitch change
    double ratioSlewPpmPerSecond=20000;
    double quietRms=0.0035;           // about -49 dBFS: removing such a window is not heard
    double maxQuietDropMs=10;
    double loudDropMinExcessMs=15;    // below this only quiet windows and the ratio remove delay
    double loudDropPersistMs=150;     // excess must last this long before a crossfaded cut in sound
    double loudDropMs=5,loudDropIntervalMs=100;  // at most 50 ms/s cut into sound
    double afterGapMs=150;            // right after an endpoint gap the whole excess goes at once
    double crossfadeMs=2.5;
    double lateResetMs=400;           // only beyond this a fade and restart (original: 60 either way)
    double earlyResetMs=60;           // audio early needs silence inserted: keep the original reset
    double resetSpacingMs=1000;
};

// A change of the playout reserve (SmartLink's ask, ADAPTIVE) moves the schedule at once:
// the error (audio PTS minus the schedule) jumps by the change, while the queue itself
// follows only through the rate trim (at most about 5 ms per second). That jump is planned,
// not trouble: it is left out of the hard-reset test (|error| > 60 ms on the default rule,
// the controller's early/late limits with FAST_RECOVER) until the trim has worked it off.
// planned: the change since the last anchor, shrunk to the error as the error comes back
// (never grown by it), so real trouble on top still resets. With the reserve never changed
// (every switch unset) it stays 0 and the test is exactly the original one.
struct ReserveChangeAllowance {
    double plannedMs=0,lastExtraMs=0;
    // At every (re-)anchor: the schedule then includes the extra as it is.
    void anchor(double extraMs)noexcept{plannedMs=0;lastExtraMs=extraMs;}
    // extraMs: the reserve above the base in the schedule now; errorMs: the measured error.
    // Returns the planned part to leave out of the reset test.
    double update(double extraMs,double errorMs)noexcept{
        if(!std::isfinite(extraMs)||!std::isfinite(errorMs)){plannedMs=0;return 0;}
        plannedMs+=extraMs-lastExtraMs;lastExtraMs=extraMs;
        if(plannedMs>0)plannedMs=std::min(plannedMs,std::max(0.0,errorMs));
        else if(plannedMs<0)plannedMs=std::max(plannedMs,std::min(0.0,errorMs));
        return plannedMs;
    }
};
class AudioLatencyController {
public:
    struct Input {
        double nowMs=0;
        std::optional<double> errorMs;  // none: no valid measurement (no fresh video, clock invalid)
        double queuedMs=0;              // PCM queue + endpoint padding
        double endpointMs=0;            // endpoint padding: a cut at the queue head is heard after this
        double reserveMs=20;            // never remove audio below this (scheduling reserve)
        bool gap=false;                 // the endpoint ran dry since the last update
        double plannedMs=0;             // part of errorMs a reserve change put there on purpose
                                        // (ReserveChangeAllowance): left out of the reset test only
    };
    struct Decision {
        double ratioPpm=0;
        double dropMs=0;                // remove this much from the head if that window is quiet
        double loudDropMs=0;            // remove this much even if it is not quiet (0: not allowed)
        bool reset=false;               // fade and restart (the original recovery)
    };
    struct Stats {std::uint64_t quietDrops=0,loudDrops=0,gapDrops=0,resets=0,clockStalls=0,frozenUpdates=0;double droppedMs=0;};

    explicit AudioLatencyController(AudioLatencyConfig config={}):config_(config){}
    void reset(double nowMs){filtered_.reset();ratio_=0;lastUpdate_=nowMs;lateSince_.reset();lastGap_.reset();lastReset_=nowMs;pendingCount_=0;clockAnchor_.reset();recentCheck_.reset();frozenMs_=0;}
    const AudioLatencyConfig& config()const{return config_;}
    double filteredErrorMs()const{return filtered_.value_or(0);}
    double ratioPpm()const{return ratio_;}
    // Cuts made but not yet heard: their sum is added to every measurement until they are played.
    double pendingMs()const{double sum=0;for(std::size_t i=0;i<pendingCount_;++i)sum+=pending_[i].ms;return sum;}
    const Stats& stats()const{return stats_;}

    Decision update(const Input& in){
        Decision out;
        const double dt=lastUpdate_?std::clamp(in.nowMs-*lastUpdate_,0.0,250.0):0;lastUpdate_=in.nowMs;
        if(in.gap)lastGap_=in.nowMs;
        if(!in.errorMs||!std::isfinite(*in.errorMs)){
            // No measurement: hold the ratio, never cut blindly.
            expirePending(in.nowMs);
            lateSince_.reset();clockAnchor_.reset();recentCheck_.reset();frozenMs_=0;out.ratioPpm=ratio_;return out;
        }
        // A device clock that stands still while the endpoint holds audio (driver or USB/Bluetooth link
        // stall: the measured error falls by the elapsed time, update after update) is a dropout the
        // listener hears, without an endpoint underrun. Hold everything while it lasts (a cut would not
        // be played), then treat the resume like an endpoint gap: the delay the stall added goes at once,
        // masked by the dropout. Judged on the raw error (pending cuts expiring are not a fall), against
        // an anchor with a slack for the video delay's jitter, so one update where jitter hid the fall
        // does not end the hold; a fall faster than time itself is a step of the target (video delay),
        // not a stopped clock. While the clock clearly stands still, pending cuts are not being played:
        // their deadlines move with it (otherwise they expire unheard and the excess is cut twice).
        bool resumed=false;
        const double raw=*in.errorMs;
        if(!clockAnchor_)clockAnchor_=Anchor{in.nowMs,raw};
        else{
            const double elapsed=in.nowMs-clockAnchor_->at,fall=clockAnchor_->error-raw;
            // Once a stall is established, the clock running again shows within about 10 ms: less than half
            // of the time elapsed since the last check came off the error.
            bool running=false;
            if(frozenMs_>=kFrozenMinMs&&recentCheck_&&in.nowMs-recentCheck_->at>=kResumeWindowMs){
                running=recentCheck_->error-raw<0.5*(in.nowMs-recentCheck_->at);
                recentCheck_=Anchor{in.nowMs,raw};
            }
            if(!running&&elapsed>0&&fall>=kFrozenFraction*elapsed-kFrozenSlackMs&&fall<=kFrozenMaxFraction*elapsed+kFrozenSlackMs){
                if(frozenMs_<kFrozenMinMs&&elapsed>=kFrozenMinMs)recentCheck_=Anchor{in.nowMs,raw};
                frozenMs_=elapsed;
                if(fall>kFrozenSlackMs)for(std::size_t i=0;i<pendingCount_;++i)pending_[i].until+=dt;
            }
            else{resumed=frozenMs_>=kFrozenMinMs;frozenMs_=0;clockAnchor_=Anchor{in.nowMs,raw};recentCheck_.reset();}
        }
        expirePending(in.nowMs);
        // A cut at the queue head is heard only after the endpoint has played what it holds; until
        // then the measurement still contains it.
        const double e=raw+pendingMs();
        if(frozenMs_>=kFrozenMinMs){lateSince_.reset();out.ratioPpm=ratio_;++stats_.frozenUpdates;return out;}
        if(resumed){lastGap_=in.nowMs;++stats_.clockStalls;}
        const bool gap=in.gap||resumed;
        // Light smoothing (about 3 updates); a gap is a real step, so take it as it is.
        filtered_=!filtered_||gap?e:*filtered_+0.35*(e-*filtered_);
        const double f=*filtered_;
        const bool resetAllowed=in.nowMs-lastReset_>=config_.resetSpacingMs;
        // Early needs the raw error early too: a cut heard before its pending deadline (a short endpoint
        // plays it sooner than the margin) is counted twice for a moment and must not read as early.
        const double fu=f-in.plannedMs,rawu=raw-in.plannedMs; // without the planned part (0 unless the reserve changed)
        if(resetAllowed&&(fu<-config_.lateResetMs||(fu>config_.earlyResetMs&&rawu>config_.earlyResetMs))){
            out.reset=true;lastReset_=in.nowMs;++stats_.resets;ratio_=0;filtered_.reset();lateSince_.reset();pendingCount_=0;clockAnchor_.reset();recentCheck_.reset();frozenMs_=0;
            return out;
        }
        const double headroom=std::max(0.0,in.queuedMs-in.reserveMs);
        const double late=std::max(0.0,-f);
        // Ratio: proportional, bounded, slewed; never speeds up into the reserve.
        double target=std::clamp(f*config_.gainPpmPerMs,-config_.maxRatioPpm,config_.maxRatioPpm);
        if(target<0&&headroom<=0)target=0;
        const double step=config_.ratioSlewPpmPerSecond*dt/1000;
        ratio_+=std::clamp(target-ratio_,-step,step);
        if(ratio_<0&&headroom<=0)ratio_=0;
        out.ratioPpm=ratio_;
        // Drops: only the part of the excess the reserve can give.
        const double excess=std::min(late,headroom);
        if(late>config_.deadbandMs){if(!lateSince_)lateSince_=in.nowMs;}
        else lateSince_.reset();
        if(excess<=config_.deadbandMs||pendingCount_==pending_.size())return out;
        nextPendingUntil_=in.nowMs+in.endpointMs+kPendingMarginMs;
        if(lastGap_&&in.nowMs-*lastGap_<=config_.afterGapMs){
            // The listener has just heard a dropout and a fade-in: the cut is masked.
            out.dropMs=out.loudDropMs=excess;return out;
        }
        out.dropMs=std::min(excess,config_.maxQuietDropMs);
        if(excess>=config_.loudDropMinExcessMs&&lateSince_&&in.nowMs-*lateSince_>=config_.loudDropPersistMs
            &&(!lastLoud_||in.nowMs-*lastLoud_>=config_.loudDropIntervalMs))
            out.loudDropMs=std::min(excess,config_.loudDropMs);
        return out;
    }
    // Report what the caller actually removed after the last update (0: nothing). The cut stays
    // pending until the endpoint padding reported with that update has played.
    void dropped(double nowMs,double ms,bool quiet,bool afterGap){
        if(ms<=0||pendingCount_==pending_.size())return;
        stats_.droppedMs+=ms;
        if(afterGap)++stats_.gapDrops;else if(quiet)++stats_.quietDrops;else{++stats_.loudDrops;lastLoud_=nowMs;}
        pending_[pendingCount_++]={ms,std::max(nextPendingUntil_,nowMs)};
        if(filtered_)*filtered_+=ms; // the filter state moves with the measurements it will see
    }
    bool afterGap(double nowMs)const{return lastGap_&&nowMs-*lastGap_<=config_.afterGapMs;}
    // Engine latency between padding and the device clock is not reported: one period of margin.
    static constexpr double kPendingMarginMs=12;
    // Device clock stall: error falling by at least this share of the elapsed time, for this long.
    static constexpr double kFrozenFraction=0.8,kFrozenMaxFraction=1.2,kFrozenMinMs=30,kFrozenSlackMs=3,kResumeWindowMs=10;
private:
    void expirePending(double nowMs){
        std::size_t kept=0;
        for(std::size_t i=0;i<pendingCount_;++i)if(nowMs<pending_[i].until)pending_[kept++]=pending_[i];
        pendingCount_=kept;
    }
    struct Pending {double ms=0,until=0;};
    AudioLatencyConfig config_;
    struct Anchor {double at=0,error=0;};
    std::optional<double> filtered_,lastUpdate_,lateSince_,lastGap_,lastLoud_;std::optional<Anchor> clockAnchor_,recentCheck_;double frozenMs_=0;
    std::array<Pending,32> pending_{};std::size_t pendingCount_=0;
    double ratio_=0,lastReset_=-1e300,nextPendingUntil_=0;
    Stats stats_;
};

// libswresample ends a compensation after its distance (48000 samples here, about 1 s), so a
// correction must be set again before then. Due when the value changes, or every kRearmMs while
// it is not zero (the original path set it every 250 ms).
class AudioRatioArming {
public:
    void reset(){last_=0;at_.reset();}
    bool due(double nowMs,int delta){
        if(at_&&delta==last_&&(delta==0||nowMs-*at_<kRearmMs))return false;
        last_=delta;at_=nowMs;return true;
    }
    static constexpr double kRearmMs=250;
private:
    int last_=0;std::optional<double> at_;
};

// Root mean square of `frames` interleaved frames.
inline double audioWindowRms(const float* samples,std::size_t frames,unsigned channels){
    if(!samples||!frames||!channels)return 0;
    double sum=0;const std::size_t count=frames*channels;
    for(std::size_t i=0;i<count;++i){const double v=samples[i];sum+=v*v;}
    return std::sqrt(sum/static_cast<double>(count));
}

// Removes `drop` frames from the head of an interleaved buffer without a step: the `fade` frames that
// would have followed the head are crossfaded (equal gain) into the frames after the cut. The caller
// then discards the first `drop` frames. Needs drop + fade frames; returns false (nothing changed)
// otherwise.
inline bool audioCrossfadeSplice(float* samples,std::size_t frames,unsigned channels,std::size_t drop,std::size_t fade){
    if(!samples||!channels||!drop||drop+fade>frames)return false;
    // Backwards: a write at drop+i never lands on a frame a later step still reads (drop < fade allowed).
    for(std::size_t i=fade;i-->0;){
        const float w=static_cast<float>(static_cast<double>(i+1)/static_cast<double>(fade+1));
        for(unsigned c=0;c<channels;++c){
            float& after=samples[(drop+i)*channels+c];
            after=samples[i*channels+c]*(1.0f-w)+after*w;
        }
    }
    return true;
}

} // namespace veyra::sink
