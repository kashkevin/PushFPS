// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// PS5 Remote Play: the picture time the audio schedule waits for (stream "sync", Package 9 evidence).
//
// Why: the automatic audio sync (CaptureAudioSession.cpp, mode 0) delays sound by
// `present time - video PTS - audio ingress`. The Remote Play video PTS is a local estimate
// (RemotePlayClock::video): wire index / 60 with at most 0.5 % correction per frame. When the console sends
// fewer than 59.7 frames a second (Package 9: 56.4-59.5 per 10 s window at 15-50 Mbit/s, no index gaps, no
// loss) that PTS falls behind the frame's real arrival by exactly the deficit (1000 - 16.75 x frames/s ms
// every second) and the audio waited for it: 1292.8 ms (15 Mbit/s) at the end of a 70 s run, 1500 ms (the
// cap) in two runs, 1-5 hard restarts of the sound per 10 s window. The picture itself is not delayed by it: a
// stream re-anchors its presentation on every real frame (EngineController.cpp, liveTimeline.resetPair).
//
// Rule: for a shown REAL frame whose arrival time is known, the time handed to the audio schedule is the later
// of its PTS and its arrival on the same axis (origin = the session's common origin, RemotePlaySource). The lag
// found that way (arrival - PTS, never negative) is kept and added to the PTS of the generated frames shown
// after it. So the audio target is `present - max(PTS, arrival)`:
//  - never larger than today's `present - PTS` (audio is never later than before), and
//  - equal to the local picture delay `present - arrival` whenever the estimate lags the arrival, which is the
//    delay the picture really had on the one host clock (audio PTS is arrival-anchored too: RemotePlaySource::
//    pullAudio), so sound and picture keep the relation they arrived with.
// An arrival that cannot belong to this frame (not finite, before the session's origin or after the present) is
// ignored: the PTS is used as it is today. There is no bound on the lag itself: the default clock's lag is
// unbounded (it grows as long as the console sends fewer than 59.7 frames a second), and a bound would switch the
// correction off in a long session (review of 21 Nov: after 133-899 s at 57.5-59.4 frames/s for a 5 s bound).
// A reset (video reset, new connection) forgets the lag. The term acts at every console rate, also at the nominal
// one (there the lag is the few ms the lower envelope leaves). VEYRA_TEST_RP_LEGACY_AUDIO_PICTURE=1 restores the
// term as it was (A/B only).
//
// Portable, allocation-free; one caller thread (the engine's presentation step), state written nowhere else.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace veyra::remoteplay {

class AudioPictureTerm {
public:
    void reset() noexcept {lagMs_=0;known_=false;}

    // One shown frame. ptsMs: its PTS (ms on the session axis). hostMs: present time, arrivalHostMs: arrival of a
    // shown REAL frame's data (0 for a generated or re-read frame), originHostMs: the session's common origin; the
    // three on the one host clock, ms. Returns the time the audio schedule should be given (>= ptsMs, finite).
    double observe(double ptsMs,double hostMs,double arrivalHostMs,double originHostMs) noexcept {
        if(!std::isfinite(ptsMs))return ptsMs;
        if(arrivalHostMs>0){
            const double lag=(arrivalHostMs-originHostMs)-ptsMs;
            const bool usable=std::isfinite(arrivalHostMs)&&std::isfinite(hostMs)&&std::isfinite(originHostMs)&&
                originHostMs>0&&arrivalHostMs>=originHostMs&&arrivalHostMs<=hostMs&&std::isfinite(lag);
            if(usable){lagMs_=std::max(0.0,lag);known_=true;++accepted_;}
            else ++rejected_;
        }
        return known_?ptsMs+lagMs_:ptsMs;
    }

    double lagMs() const noexcept {return known_?lagMs_:0.0;}
    bool known() const noexcept {return known_;}
    std::uint64_t accepted() const noexcept {return accepted_;}
    std::uint64_t rejected() const noexcept {return rejected_;}

private:
    double lagMs_=0;
    bool known_=false;
    std::uint64_t accepted_=0,rejected_=0;
};

} // namespace veyra::remoteplay
