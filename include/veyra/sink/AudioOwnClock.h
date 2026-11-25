// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// PS5 Remote Play audio on its own clocks (experiment VEYRA_EXP_RP_AUDIO_OWN_CLOCK, sprint 5).
//
// Why: the default path waits for the picture by `present time - video PTS`. The Remote Play video PTS is
// frame index / nominal rate (RemotePlayClock::video, slew at most 0.5 %); when the console delivers fewer
// frames than the nominal rate (BENCH2: about 59 for a 60 profile) that PTS falls behind real time by
// 6-13 ms every second, the audio target follows it (249-537 ms after 40 s), the 5000 ppm trim cannot keep
// up and the 60 ms rule fades and restarts the audio 3-10 times a minute.
//
// Rule (re-implemented from what moonlight-qt does, app/streaming/audio/renderers/sdlaud.cpp submitAudio:
// audio is scheduled from its own arrival, never from a video timestamp): the only video term is the LOCAL
// picture delay, `present time - arrival time of the same frame`, two readings of the one host clock, so a
// wrong timestamp cannot enter. Its median over the last frames is followed slowly enough for the existing
// rate trim, so a change of that delay (frame generation switched on, a heavier effect) never needs a restart.
// Audio arrival (ArrivalClockMapping) and the device clock (the existing trim) are unchanged.
//
// Portable, allocation-free, one owner thread.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace veyra::sink {

class AudioOwnClockTarget {
public:
    struct Config {
        double maxMs=250;          // never hold audio longer than this for the picture
        double slewMsPerSecond=4;  // below the trim's capacity (5000 ppm = 5 ms/s), so the error stays small
        double maxSampleMs=1000;   // a frame shown later than this after it arrived is a stall, not a pipeline delay
    };
    AudioOwnClockTarget()=default;
    explicit AudioOwnClockTarget(const Config& config):config_(config){}

    void reset(){count_=cursor_=0;timed_=seeded_=false;targetMs_=medianMs_=0;lastUpdateMs_=0;accepted_=rejected_=0;}

    // A real (not generated) frame was shown: host time of its Present and of its network arrival, ms on
    // one clock. Readings that cannot be a pipeline delay are counted and ignored.
    void observeVideo(double presentedHostMs,double arrivalHostMs){
        const double local=presentedHostMs-arrivalHostMs;
        if(!std::isfinite(local)||local<0||local>config_.maxSampleMs){++rejected_;return;}
        samples_[cursor_]=local;cursor_=(cursor_+1)%samples_.size();count_=std::min(count_+1,samples_.size());
        auto sorted=samples_;std::sort(sorted.begin(),sorted.begin()+static_cast<std::ptrdiff_t>(count_));
        medianMs_=std::min(sorted[count_/2],config_.maxMs);++accepted_;
    }

    // The schedule's video term for `nowMs`. While audio is not playing (before the first start, after a
    // restart for another reason) the target takes the median at once: nothing is heard, so nothing can
    // click. While it plays, the target moves at most slewMsPerSecond. Without any reading it stays where
    // it is (0 at the start): audio never waits for a picture that does not come.
    double update(double nowMs,bool audioPlaying){
        const double dt=timed_?std::clamp(nowMs-lastUpdateMs_,0.0,1000.0):0.0;
        lastUpdateMs_=nowMs;timed_=true;
        if(!count_)return targetMs_;
        if(!audioPlaying||!seeded_){targetMs_=medianMs_;seeded_=true;return targetMs_;}
        const double step=config_.slewMsPerSecond*dt/1000.0;
        targetMs_+=std::clamp(medianMs_-targetMs_,-step,step);
        return targetMs_;
    }

    bool known()const{return count_>0;}
    double targetMs()const{return targetMs_;}
    double medianMs()const{return medianMs_;}
    std::uint64_t accepted()const{return accepted_;}
    std::uint64_t rejected()const{return rejected_;}
    const Config& config()const{return config_;}

private:
    Config config_{};
    std::array<double,9> samples_{};
    std::size_t count_=0,cursor_=0;
    bool timed_=false,seeded_=false;
    double targetMs_=0,medianMs_=0,lastUpdateMs_=0;
    std::uint64_t accepted_=0,rejected_=0;
};

} // namespace veyra::sink
