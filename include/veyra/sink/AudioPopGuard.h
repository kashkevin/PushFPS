// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// PS5 Remote Play audio: pop diagnostics and the two sprint-4 experiments (docs/AUDIO.md, round 3).
// Portable (no Windows, no allocation after construction); used by CaptureAudioSession.cpp and
// WasapiAudioSink.cpp, tested in tests/remoteplay/AudioPopTests.inc and modelled in AudioPathModel.h.
//
// - AudioStatsWindow: counters and fixed-size histograms for the [remoteplay-audio] line written with
//   VEYRA_EXP_RP_STATS=1 (pops, queue depth, arrival gaps, endpoint padding). Owned by one thread at a
//   time (the session mutex the callers already hold); nothing here locks.
// - AudioEdgeSmoother (VEYRA_EXP_RP_AUDIO_SMOOTH): when the endpoint is certain to run dry before the next
//   pump, a short synthetic fade-out continues the waveform (mirror of the last written frames under a
//   raised-cosine envelope) instead of an abrupt stop. It is written only then, so steady state is
//   untouched (no added latency); the real samples are never changed.
// - AudioAdaptivePlayout (VEYRA_EXP_RP_AUDIO_ADAPTIVE): the playout reserve grows after an underrun to
//   cover the arrival gaps seen (bounded), and shrinks slowly once gaps have been short for a while. With
//   no underrun it stays at its base, so a clean link keeps today's delay.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace veyra::sink {

// Raised-cosine envelope: 0 -> 1 over `total` steps, never exactly 0 or 1 inside (no step at either end).
// position 0 is the first sample after silence; position total-1 the last faded one.
inline float raisedCosineRise(std::size_t position,std::size_t total){
    if(!total||position>=total)return 1.0f;
    const double x=(static_cast<double>(position)+1.0)/(static_cast<double>(total)+1.0);
    return static_cast<float>(0.5-0.5*std::cos(3.141592653589793*x));
}

// Largest change between neighbouring samples of one channel, including the step from `before` (the
// sample played just before the block; 0 for silence) and, if `after` is set, into it.
inline double largestSampleStep(const float* samples,std::size_t frames,unsigned channels,unsigned channel,double before,double after,bool hasAfter){
    double previous=before,worst=0;
    for(std::size_t i=0;i<frames;++i){const double v=samples[i*channels+channel];worst=std::max(worst,std::abs(v-previous));previous=v;}
    if(hasAfter)worst=std::max(worst,std::abs(after-previous));
    return worst;
}

class AudioEdgeSmoother {
public:
    static constexpr unsigned kMaxChannels=8;
    static constexpr std::size_t kHistoryFrames=512;   // >= the 4 ms fade up to 128 kHz
    struct Config {std::size_t fadeFrames=192;};       // 4 ms at 48 kHz (3-5 ms asked); setFadeMs follows the rate
    AudioEdgeSmoother()=default;
    explicit AudioEdgeSmoother(Config config):config_(config){}
    const Config& config()const{return config_;}
    void reset(){count_=0;head_=0;tailed_=false;}
    // Real frames just written to the endpoint (after gain and fades), interleaved.
    void remember(const float* samples,std::size_t frames,unsigned channels){
        if(!frames||!channels||channels>kMaxChannels)return;
        if(channels!=channels_){channels_=channels;count_=0;head_=0;}
        const std::size_t from=frames>kHistoryFrames?frames-kHistoryFrames:0;
        for(std::size_t i=from;i<frames;++i){
            for(unsigned c=0;c<channels;++c)history_[head_*kMaxChannels+c]=samples[i*channels+c];
            head_=(head_+1)%kHistoryFrames;count_=std::min(count_+1,kHistoryFrames);
        }
        tailed_=false;
    }
    // The endpoint holds `paddingFrames` before this write and gets `writtenFrames` real frames now; the
    // next write comes about one engine period later (event-driven shared mode: one pump per period). Below
    // one period after the write, the engine reaches the end of the data before that: a gap is certain.
    static bool willStarve(std::uint64_t paddingFrames,std::uint64_t writtenFrames,double periodFrames){
        return periodFrames>0&&static_cast<double>(paddingFrames+writtenFrames)<periodFrames;
    }
    // Asked before this pump's real frames are remembered: due once per run of real data (realFrames > 0
    // starts a new run), and only with something to continue from. With no new frames the endpoint must
    // still hold some: once it has run dry (the owner woke late) the cut has already been heard, and a tail
    // started then would itself be a step out of silence (review of round 3).
    bool due(std::uint64_t paddingFrames,std::uint64_t realFrames,double periodFrames)const{
        return willStarve(paddingFrames,realFrames,periodFrames)&&(realFrames>0||(paddingFrames>0&&!tailed_&&count_>0));
    }
    // The fade in time, at the rate the frames are played (4 ms: 192 frames at 48 kHz, 176 at 44.1 kHz).
    void setFadeMs(double ms,double rate){
        if(std::isfinite(ms)&&std::isfinite(rate)&&ms>0&&rate>0)config_.fadeFrames=std::clamp<std::size_t>(static_cast<std::size_t>(std::lround(ms*rate/1000)),1,kHistoryFrames);
    }
    // Writes up to min(maxFrames, fadeFrames) frames that continue the last remembered frames: their
    // mirror image (value-continuous at the joint) under a falling raised-cosine envelope that ends near
    // zero. Returns the frames written; marks the run as tailed (the next real data must fade in).
    std::size_t writeTail(float* out,std::size_t maxFrames,unsigned channels){
        if(channels!=channels_||!count_||!maxFrames)return 0;
        const std::size_t frames=std::min(maxFrames,config_.fadeFrames);
        for(std::size_t i=0;i<frames;++i){
            // history index of the frame i+1 steps back from the newest (mirror), clamped to what is kept
            const std::size_t back=std::min(i+1,count_-1);
            const std::size_t index=(head_+kHistoryFrames-1-back)%kHistoryFrames;
            const float envelope=raisedCosineRise(frames-1-i,frames);
            for(unsigned c=0;c<channels;++c)out[i*channels+c]=history_[index*kMaxChannels+c]*envelope;
        }
        tailed_=true;
        return frames;
    }
    bool tailed()const{return tailed_;}
private:
    Config config_{};
    std::array<float,kHistoryFrames*kMaxChannels> history_{};
    std::size_t count_=0,head_=0;unsigned channels_=0;bool tailed_=false;
};

// Quantile with forgetting of per-packet relative arrival delay (arrival minus PTS, above its recent lower
// envelope): the shape of WebRTC NetEQ's delay manager, re-implemented from its public description; no code
// copied. Every value is added with weight 1, older ones fade by `forget` per value, so the estimate follows the
// link over roughly 1 / (1 - forget) packets (2000 = about 20 s at 100 Opus frames/s). 1 ms bins up to 255 ms.
// No allocation; constant work per value apart from a rare renormalisation.
class AudioGapQuantile {
public:
    static constexpr std::size_t kBins=256;
    explicit AudioGapQuantile(double forget=0.9995):forget_(forget){}
    void clear(){bins_.fill(0);weight_=1;total_=0;count_=0;}
    void add(double gapMs){
        if(!std::isfinite(gapMs))return;
        const auto bin=std::min(kBins-1,static_cast<std::size_t>(std::max(0.0,gapMs)));
        weight_/=forget_;bins_[bin]+=weight_;total_+=weight_;++count_;
        if(weight_>1e12){for(auto& b:bins_)b/=weight_;total_/=weight_;weight_=1;}
    }
    std::uint64_t count()const{return count_;}
    // Upper edge (ms) of the bin holding quantile q; -1 before any gap.
    double quantile(double q)const{
        if(!count_||total_<=0)return -1;
        const double want=std::clamp(q,0.0,1.0)*total_;double seen=0;
        for(std::size_t i=0;i<kBins;++i){seen+=bins_[i];if(seen>=want)return static_cast<double>(i+1);}
        return static_cast<double>(kBins);
    }
private:
    double forget_;std::array<double,kBins> bins_{};double weight_=1,total_=0;std::uint64_t count_=0;
};

class AudioAdaptivePlayout {
public:
    struct Config {
        double maxMs=80;        // upper bound of the reserve
        double growStepMs=10;   // at least this much more after an underrun
        double marginMs=5;      // above the largest recent arrival gap
        double holdMs=10000;    // no underrun for this long before shrinking
        double shrinkMsPerS=2;  // then back towards max(base, recent gaps + margin) this fast
        double windowMs=10000;  // "recent" = the current and the previous window of arrival gaps
        double gapEvidenceMs=1000;   // an underrun counts only this soon after the gap that explains it
        // Phase 2 (research doc E/I2 #2): also raise the reserve before any underrun, from the forgetting quantile
        // of relative arrival delay: reserve = quantile + delayMarginMs (so delays up to 15 ms stay at today's
        // 20 ms reserve). 0.995: about one packet in 200 may still come later than the reserve covers. Needs
        // minSamples values first.
        bool proactive=true;double quantile=0.995,delayMarginMs=5;std::uint64_t minSamples=200;
    };
    AudioAdaptivePlayout()=default;
    explicit AudioAdaptivePlayout(Config config):config_(config){}
    const Config& config()const{return config_;}
    // A new stream (or reconnect): back to the base.
    void reset(double nowMs,double baseMs){base_=baseMs;target_=baseMs;lastUnderrun_=-1e300;windowStart_=nowMs;current_=previous_=0;lastUpdate_=nowMs;external_=0;ask_=0;delays_.clear();lastGap_=0;lastGapAt_=-1e300;pendingUnderrun_=-1e300;}
    // SmartLink (phase 2): the reserve the network decision asks for, 0 = none (stale, observe mode or "leave").
    // Followed at once upwards (bounded by maxMs), released at shrinkMsPerS; the result is the larger of this
    // and the reserve this class keeps itself, so the decision can only add to the safety this class gives.
    void request(double askMs){ask_=std::isfinite(askMs)&&askMs>0?std::min(askMs,config_.maxMs):0;}
    // The base follows today's reserve (it depends on the endpoint opened); the extra above it is kept.
    void setBase(double baseMs){const double extra=std::max(0.0,target_-base_);base_=baseMs;target_=std::max(baseMs,std::min(config_.maxMs,baseMs+extra));}
    // Gaps above maxMs (a pause, a stall no reserve could cover) are ignored (review of round 3).
    void arrivalGap(double nowMs,double gapMs){
        if(!std::isfinite(nowMs)||!std::isfinite(gapMs)||gapMs<=0||gapMs>config_.maxMs)return;
        roll(nowMs);current_=std::max(current_,gapMs);
        if(nowMs-lastGapAt_>config_.gapEvidenceMs||gapMs>=lastGap_){lastGap_=gapMs;lastGapAt_=nowMs;}
        // The gap that starved the endpoint is only known when its late packet arrives, after the underrun.
        if(nowMs-pendingUnderrun_<=config_.gapEvidenceMs)grow(nowMs,gapMs);
    }
    // One packet's arrival delay above the recent lower envelope (ms).
    void arrivalDelay(double delayMs){delays_.add(delayMs);}
    // What the delay statistics alone ask for (today's base when they ask nothing or are not ready).
    double proactiveMs()const{
        if(!config_.proactive||delays_.count()<config_.minSamples)return base_;
        return std::clamp(delays_.quantile(config_.quantile)+config_.delayMarginMs,base_,std::max(base_,config_.maxMs));
    }
    // Grows only when the underrun follows (within gapEvidenceMs) an arrival gap that a larger reserve would
    // have covered; an underrun with no such gap (device, scheduling, a pause) changes nothing (review of
    // round 3: before, any underrun added 10 ms, and one every 10 s held the reserve at its maximum).
    // The explaining gap may come just before (a clump ended the gap, then the margin ran out) or just after
    // (the gap is still open when the endpoint runs dry; its late packet arrives later).
    void underrun(double nowMs){
        if(!std::isfinite(nowMs))return;
        roll(nowMs);pendingUnderrun_=nowMs;
        if(nowMs-lastGapAt_<=config_.gapEvidenceMs)grow(nowMs,lastGap_);
    }
    // Once per audio-owner loop; returns the reserve in ms.
    double update(double nowMs){
        if(!std::isfinite(nowMs))return targetMs();
        roll(nowMs);
        const double dt=std::max(0.0,nowMs-lastUpdate_);lastUpdate_=nowMs;
        const double proactive=proactiveMs();
        if(proactive>target_)target_=proactive;   // raise at once on evidence
        if(nowMs-lastUnderrun_>=config_.holdMs){
            const double floor=std::max({base_,proactive,std::min(config_.maxMs,recentGapMs()+config_.marginMs)});
            if(target_>floor)target_=std::max(floor,target_-config_.shrinkMsPerS*dt/1000);
        }
        if(ask_>=external_)external_=ask_;
        else external_=std::max(ask_,external_-config_.shrinkMsPerS*dt/1000);
        return targetMs();
    }
    double targetMs()const{return std::max(target_,external_);}
    double extraMs()const{return std::max(0.0,targetMs()-base_);}
    // Which rule sets the reserve now: 0 = today's base, 1 = this class (underruns), 2 = SmartLink.
    int source()const{return external_>target_&&external_>base_?2:target_>base_?1:0;}
    double recentGapMs()const{return std::max(current_,previous_);}
private:
    void roll(double nowMs){
        if(nowMs-windowStart_<config_.windowMs)return;
        previous_=nowMs-windowStart_<2*config_.windowMs?current_:0;current_=0;windowStart_=nowMs;
    }
    void grow(double nowMs,double gapMs){
        const double need=gapMs+config_.marginMs;
        if(need<=target_)return;
        target_=std::min(config_.maxMs,std::max({target_+config_.growStepMs,need,base_}));
        lastUnderrun_=nowMs;pendingUnderrun_=-1e300;
    }
    Config config_{};AudioGapQuantile delays_;double pendingUnderrun_=-1e300;
    double base_=20,target_=20,lastUnderrun_=-1e300,windowStart_=0,current_=0,previous_=0,lastUpdate_=0,external_=0,ask_=0,lastGap_=0,lastGapAt_=-1e300;
};

// Fixed-size millisecond histogram (0.5 ms bins up to 250 ms; larger values land in the last bin but keep
// the exact maximum and minimum).
class AudioMsHistogram {
public:
    static constexpr std::size_t kBins=500;
    void clear(){bins_.fill(0);count_=0;max_=0;min_=0;}
    void add(double ms){
        if(!std::isfinite(ms))return;
        ms=std::max(0.0,ms);
        if(!count_){max_=ms;min_=ms;}else{max_=std::max(max_,ms);min_=std::min(min_,ms);}
        ++count_;++bins_[std::min(kBins-1,static_cast<std::size_t>(ms*2))];
    }
    std::uint64_t count()const{return count_;}
    double max()const{return count_?max_:-1;}
    double min()const{return count_?min_:-1;}
    // Upper edge of the bin holding the p-quantile, capped by the exact maximum; -1 when empty.
    double quantile(double p)const{
        if(!count_)return -1;
        const auto want=std::max<std::uint64_t>(1,static_cast<std::uint64_t>(std::ceil(std::clamp(p,0.0,1.0)*static_cast<double>(count_))));
        std::uint64_t seen=0;
        for(std::size_t i=0;i<kBins;++i){seen+=bins_[i];if(seen>=want)return i+1==kBins?max_:std::min(max_,static_cast<double>(i+1)/2);}
        return max_;
    }
private:
    std::array<std::uint32_t,kBins> bins_{};std::uint64_t count_=0;double max_=0,min_=0;
};

// One [remoteplay-audio] statistics window. pops = underrun starts + hard resets (the contract's "audible
// discontinuities", whatever their edge); unfadedCuts = underrun starts that stopped on a real sample with no
// fade (the clicks; reported as `clicks=`, so SMOOTH's effect is visible: with it, only late wakes remain).
class AudioStatsWindow {
public:
    struct Snapshot {
        std::uint64_t underrunStarts=0,hardResets=0,unfadedCuts=0,tails=0,pops=0;
        double queueP50=-1,queueP95=-1,queueMin=-1,gapP95=-1,gapMax=-1,paddingMin=-1;
        std::uint64_t queueSamples=0,arrivals=0;
        double aboveTargetPct=-1;   // share of queue samples more than 5 ms above the reserve in force; -1 = none
    };
    void underrunStart(){++underrunStarts_;}
    void hardReset(){++hardResets_;}
    void unfadedCut(){++unfadedCuts_;}
    void tail(){++tails_;}
    void queue(double ms){queue_.add(ms);}
    // Phase 2: one queue sample against the reserve in force; above = more than 5 ms over it (piled-up delay).
    void queue(double ms,double targetMs){queue_.add(ms);if(std::isfinite(ms)&&std::isfinite(targetMs)){++judged_;if(ms>targetMs+5)++above_;}}
    void arrivalGap(double ms){gap_.add(ms);}
    void padding(double ms){padding_.add(ms);}
    Snapshot take(){
        Snapshot s;
        s.underrunStarts=underrunStarts_;s.hardResets=hardResets_;s.unfadedCuts=unfadedCuts_;s.tails=tails_;
        s.pops=underrunStarts_+hardResets_;
        s.queueP50=queue_.quantile(.5);s.queueP95=queue_.quantile(.95);s.queueMin=queue_.min();s.queueSamples=queue_.count();
        s.gapP95=gap_.quantile(.95);s.gapMax=gap_.max();s.arrivals=gap_.count();s.paddingMin=padding_.min();
        s.aboveTargetPct=judged_?100.0*static_cast<double>(above_)/static_cast<double>(judged_):-1;
        underrunStarts_=hardResets_=unfadedCuts_=tails_=0;queue_.clear();gap_.clear();padding_.clear();judged_=above_=0;
        return s;
    }
private:
    std::uint64_t underrunStarts_=0,hardResets_=0,unfadedCuts_=0,tails_=0,judged_=0,above_=0;
    AudioMsHistogram queue_,gap_,padding_;
};

} // namespace veyra::sink
