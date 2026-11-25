// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "sync" (Package 9 evidence): the picture term of the default audio sync rule
// (include/veyra/remoteplay/AudioPictureTerm.h). Unit tests, the production RemotePlayClock driven at the console
// rates of Package 9, and the LINUX MODEL of the audio path (include/veyra/sink/AudioPathModel.h, virtual time).
// Models and code-level tests, not a measurement of PushFPS, a PS5 or a headset.
#include "veyra/remoteplay/AudioPictureTerm.h"
#include "veyra/sink/AudioPathModel.h"
#include "veyra/sink/CaptureSyncTarget.h"
#include <cmath>
#include <cstdio>
#include <random>
namespace audio_picture_term_tests {
namespace apm=veyra::sink::audio_model;
inline void aptPrint(const char* name,const char* rule,const apm::Result& r){
    std::printf("[audio-picture-term] %-30s %-8s resets %3d targetMax %7.1f ptsLagMax %7.1f lat %6.1f/%6.1f/%7.1f |A/V| %6.1f/%6.1f/%7.1f silence %5.0f\n",
        name,rule,r.resets,r.targetMaxMs,r.stampLagMaxMs,r.latMed,r.latP95,r.latMax,r.avMed,r.avP95,r.avMax,r.silenceMs);
}
// Package 9: wire indexes per second over the whole run (measure-c-30@131516, c-15@132311, c-15@131353,
// c-50@131634, c-50@132027, c-100@131909), each stamped as 60 by RemotePlayClock.
constexpr double kPackage9Rates[]={57.46,57.69,58.60,59.03,59.37,59.84};
}

TEST(audio_picture_term_uses_the_arrival_when_the_estimate_lags_and_never_less_than_the_pts){
    veyra::remoteplay::AudioPictureTerm t;
    const double origin=1000;
    // Nothing known yet, a generated frame: the PTS as it is.
    CHECK(t.observe(500,origin+600,0,origin)==500&&!t.known());
    // A real frame stamped 300 ms behind its arrival (arrival 1800 on the axis), shown 5 ms after it arrived.
    CHECK(t.observe(1500,origin+1805,origin+1800,origin)==1800&&t.known()&&t.lagMs()==300);
    // The generated frame shown after it carries the same correction.
    CHECK(t.observe(1508,origin+1813,0,origin)==1808);
    // An estimate ahead of the arrival is never moved back: the PTS stays (audio is never made later).
    CHECK(t.observe(2000,origin+2004,origin+1990,origin)==2000&&t.lagMs()==0);
    CHECK(t.observe(2010,origin+2014,0,origin)==2010);
    // Arrivals that cannot be this frame's are ignored and the last good correction stays.
    t.observe(3000,origin+3110,origin+3100,origin);CHECK(t.lagMs()==100);
    const auto accepted=t.accepted();
    CHECK(t.observe(3020,origin+3120,origin+3200,origin)==3120);       // arrived after it was shown
    CHECK(t.observe(3030,origin+3130,origin-1,origin)==3130);          // before the session's origin
    CHECK(t.observe(3040,origin+9200,origin+9100,origin)==9100&&t.lagMs()==6060); // 6 s behind: still this frame's
    t.observe(3045,origin+3150,origin+3145,origin);CHECK(t.lagMs()==100);
    CHECK(t.observe(3050,origin+3150,origin+3140,0)==3150);            // no origin (not connected)
    CHECK(t.observe(3060,std::nan(""),origin+3140,origin)==3160);
    CHECK(t.accepted()==accepted+2&&t.rejected()==4);
    CHECK(std::isnan(t.observe(std::nan(""),origin+1,origin+1,origin)));
    t.reset();CHECK(!t.known()&&t.lagMs()==0&&t.observe(3070,origin+3170,0,origin)==3070);
}

TEST(audio_picture_term_target_is_never_larger_than_todays_for_any_observation_sequence){
    // For Remote Play no arrival is passed to the audio rule, so CaptureSyncTarget::observe hands each frame's raw
    // value on unfiltered (clamped 0..1500 afterwards, then the 60 ms restart rule checked every 250 ms). Fed the
    // same frames, the corrected term can only lower each requested target: sound is never scheduled later.
    std::mt19937 rng(9091);
    for(int run=0;run<200;++run){
        veyra::sink::CaptureSyncTarget today,fixed;veyra::remoteplay::AudioPictureTerm term;
        const double origin=5000;double lag=0,pts=0;
        for(int i=0;i<600;++i){
            pts+=16.0+double(rng()%200)/100.0;
            lag=std::max(-5.0,lag+double(int(rng()%41)-20)/10.0);            // the estimate wanders round the arrival
            if(rng()%97==0)lag=double(rng()%3000);                           // and jumps
            const double arrival=origin+pts+lag,host=arrival+double(rng()%4000)/100.0,ingress=origin+double(rng()%300)/100.0;
            const bool real=rng()%3!=0;
            const double given=term.observe(pts,host,real?arrival:0,origin);
            CHECK(given>=pts);
            const auto a=today.observe(host-pts-ingress,host,std::nullopt);
            const auto b=fixed.observe(host-given-ingress,host,std::nullopt);
            CHECK(b.targetMs<=a.targetMs);
            if(real&&lag>=0)CHECK(std::abs(given-(arrival-origin))<1e-6);
        }
    }
}

TEST(audio_picture_term_with_the_production_clock_at_package9_rates){
    // The real RemotePlayClock stamps a console that sends fewer than 60 frames a second; the audio target today
    // follows its lag up to the 1500 ms cap, the corrected term stays the local picture delay (5 ms here).
    for(double fps:audio_picture_term_tests::kPackage9Rates){
        RemotePlayClock clock;const HostTime origin=10000000;clock.reset(origin,60);
        veyra::sink::CaptureSyncTarget today,fixed;veyra::remoteplay::AudioPictureTerm term;
        double todayEnd=0,fixedMax=0,lagEnd=0;
        for(std::uint64_t i=0;i<std::uint64_t(fps*70);++i){
            const HostTime arrival=origin+5000000+HostTime(double(i)*1e7/fps)+HostTime((i*7919)%20000);
            const auto stamp=clock.video(i+1,arrival);CHECK(stamp&&!stamp->discontinuity);
            const double ptsMs=double(stamp->pts100ns)/1e4,hostMs=double(arrival)/1e4+5.0,arrivalMs=double(arrival)/1e4,originMs=double(origin)/1e4;
            const double ingress=originMs;   // audio arriving with no extra delay over its own floor
            todayEnd=today.observe(hostMs-ptsMs-ingress,hostMs,std::nullopt).targetMs;
            const double given=term.observe(ptsMs,hostMs,arrivalMs,originMs);
            fixedMax=std::max(fixedMax,fixed.observe(hostMs-given-ingress,hostMs,std::nullopt).targetMs);
            lagEnd=arrivalMs-originMs-ptsMs;
        }
        std::printf("[audio-picture-term] production clock %.2f fps stamped as 60, 70 s: PTS lag at end %.1f ms, today's target %.1f ms, corrected target max %.1f ms\n",fps,lagEnd,todayEnd,fixedMax);
        if(fps<59.5)CHECK(lagEnd>100&&todayEnd>100);   // Package 9: 338-2257 ms/min of lag below 59.7 frames/s
        CHECK(fixedMax<=5.0+1e-6);                        // never more than the picture's own 5 ms
    }
}

TEST(audio_picture_term_holds_through_a_twenty_minute_session){
    // Review of 21 Nov: the default clock's lag is unbounded, so the correction must hold however large it gets.
    // Production clock, 1200 s at the four Package 9 rates below 59.7: the lag reaches 6-50 s, the target stays the
    // picture's own 5 ms on every frame, and today's rule sits at its 1500 ms cap.
    for(double fps:{57.46,58.60,59.03,59.37}){
        RemotePlayClock clock;const HostTime origin=10000000;clock.reset(origin,60);
        veyra::sink::CaptureSyncTarget today,fixed;veyra::remoteplay::AudioPictureTerm term;
        double fixedMax=0,lagEnd=0,todayEnd=0;
        for(std::uint64_t i=0;i<std::uint64_t(fps*1200);++i){
            const HostTime arrival=origin+5000000+HostTime(double(i)*1e7/fps)+HostTime((i*7919)%20000);
            const auto stamp=clock.video(i+1,arrival);CHECK(stamp&&!stamp->discontinuity);
            const double ptsMs=double(stamp->pts100ns)/1e4,arrivalMs=double(arrival)/1e4,hostMs=arrivalMs+5.0,originMs=double(origin)/1e4;
            const double given=term.observe(ptsMs,hostMs,i%3?arrivalMs:0.0,originMs);   // every third frame a generated one
            fixedMax=std::max(fixedMax,fixed.observe(hostMs-given-originMs,hostMs,std::nullopt).targetMs);
            todayEnd=std::clamp(today.observe(hostMs-ptsMs-originMs,hostMs,std::nullopt).targetMs,0.0,1500.0);
            lagEnd=arrivalMs-originMs-ptsMs;
        }
        std::printf("[audio-picture-term] production clock %.2f fps, 1200 s: PTS lag at end %.0f ms, today's target %.0f ms (capped), corrected target max %.2f ms\n",fps,lagEnd,todayEnd,fixedMax);
        CHECK(lagEnd>5000&&todayEnd==1500.0);
        CHECK(fixedMax<8.0);   // a frame without its arrival carries the last real frame's lag, which grows < 1 ms a frame here
        CHECK(term.rejected()==0);
    }
}

TEST(audio_picture_term_package9_replay_on_the_audio_model){
    using namespace audio_picture_term_tests;
    // Today: the BENCH2 / Package 9 picture (hundreds of ms of target, restarts). Corrected: no restart, the
    // latency and the visible A/V offset never above today's, and the offset bounded by arrival jitter.
    for(double hz:kPackage9Rates){
        apm::Config c;c.seconds=70;c.videoMs=12;c.videoHz=hz;c.videoStampHz=60;
        const auto today=apm::simulate(c,apm::Rule::Today);
        c.pictureFix=true;
        const auto fixed=apm::simulate(c,apm::Rule::Today);
        char name[64];std::snprintf(name,sizeof(name),"%.2f fps stamped as 60, 70 s",hz);
        aptPrint(name,"today",today);aptPrint("","picture",fixed);
        if(hz<59.5)CHECK(today.targetMaxMs>200&&today.latMax>200);
        CHECK(fixed.resets==0&&fixed.silenceMs==0&&fixed.underruns==0&&!fixed.futureAudio&&!fixed.nonFinite);
        CHECK(fixed.targetMaxMs<c.videoMs+3);
        CHECK(fixed.latMed<=today.latMed+0.5&&fixed.latP95<=today.latP95+0.5&&fixed.latMax<=today.latMax+0.5);
        CHECK(fixed.avMed<=today.avMed+0.5&&fixed.avP95<=today.avP95+0.5);
        CHECK(fixed.avP95<6);
    }
}

TEST(audio_picture_term_changes_nothing_when_the_timestamp_is_true){
    using namespace audio_picture_term_tests;
    // A true video timestamp (lag 0): the corrected term is the PTS itself, the model run is identical.
    for(double video:{35.0,12.0}){
        apm::Config c;c.seconds=60;c.videoMs=video;
        const auto today=apm::simulate(c,apm::Rule::Today);
        c.pictureFix=true;
        const auto fixed=apm::simulate(c,apm::Rule::Today);
        CHECK(fixed.resets==today.resets&&fixed.latMed==today.latMed&&fixed.latP95==today.latP95&&fixed.latMax==today.latMax);
        CHECK(fixed.avMed==today.avMed&&fixed.avP95==today.avP95&&fixed.silenceMs==today.silenceMs);
    }
    // The fast-recovery experiment waits by the same term and gets the same correction.
    apm::Config c;c.seconds=70;c.videoMs=12;c.videoHz=58.6;c.videoStampHz=60;c.pictureFix=true;
    const auto fast=apm::simulate(c,apm::Rule::Fast);
    aptPrint("58.60 fps stamped as 60, fast","picture",fast);
    CHECK(fast.resets==0&&fast.targetMaxMs<c.videoMs+3&&!fast.futureAudio&&!fast.nonFinite);
}
