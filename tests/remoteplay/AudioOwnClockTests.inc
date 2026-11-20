// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// VEYRA_EXP_RP_AUDIO_OWN_CLOCK (include/veyra/sink/AudioOwnClock.h): unit tests of the target, and the BENCH2
// situation replayed on the LINUX MODEL of the audio path (include/veyra/sink/AudioPathModel.h, virtual time):
// a stream that delivers about 59 frames a second while its video timestamps advance as index / 60.
// A model, not a measurement of PushFPS, a PS5 or a headset.
#include "veyra/sink/AudioOwnClock.h"
#include "veyra/sink/AudioPathModel.h"
#include <cmath>
#include <cstdio>
namespace audio_own_clock_tests {
namespace ocm=veyra::sink::audio_model;
inline void ocPrint(const char* name,const char* rule,const ocm::Result& r){
    std::printf("[audio-own-clock] %-34s %-9s resets %3d targetMax %6.1f ptsLagMax %6.1f lat %5.1f/%5.1f/%6.1f |A/V| %5.1f/%5.1f/%6.1f silence %5.0f pitchMax %4.0f\n",
        name,rule,r.resets,r.targetMaxMs,r.stampLagMaxMs,r.latMed,r.latP95,r.latMax,r.avMed,r.avP95,r.avMax,r.silenceMs,r.maxPitchPpm);
}
inline ocm::Config ocBench2(double deliveredHz,double seconds=60){
    ocm::Config c;c.seconds=seconds;c.videoMs=12;c.videoHz=deliveredHz;c.videoStampHz=60;return c;
}
}

TEST(audio_own_clock_target_is_the_median_local_delay){
    veyra::sink::AudioOwnClockTarget t;
    CHECK(!t.known()&&t.update(0,false)==0);
    t.observeVideo(1012,1000);
    CHECK(t.known()&&t.update(10,false)==12);
    // One late frame (a stall) does not move a median of nine; readings that cannot be a delay are refused.
    for(int i=0;i<8;++i)t.observeVideo(2000+12+i,2000+i);
    t.observeVideo(3300,3000);t.observeVideo(4000,5000);t.observeVideo(9000,1000);t.observeVideo(std::nan(""),1);
    CHECK(t.medianMs()==12&&t.rejected()==3&&t.accepted()==10);
    CHECK(t.update(20,true)==12);
}

TEST(audio_own_clock_target_moves_slower_than_the_trim_while_audio_plays){
    veyra::sink::AudioOwnClockTarget t;
    for(int i=0;i<9;++i)t.observeVideo(100+12,100);
    CHECK(t.update(1000,true)==12);
    // Frame generation switched on: the picture is now shown 30 ms after it arrived.
    for(int i=0;i<9;++i)t.observeVideo(2000+30,2000);
    double previous=t.update(2000,true);
    for(int ms=2010;ms<=7000;ms+=10){
        const double now=t.update(ms,true);
        CHECK(now>=previous&&now-previous<=0.0401);   // 4 ms/s, under the 5 ms/s the trim can follow
        previous=now;
    }
    CHECK(std::abs(previous-30)<1e-9);
    // A long pause between two calls is one second's worth at most; while audio is silent the target jumps.
    for(int i=0;i<9;++i)t.observeVideo(8000+80,8000);
    CHECK(std::abs(t.update(60000,true)-34)<1e-9);
    CHECK(t.update(60010,false)==80);
    // Never more than the cap, whatever the pipeline does.
    for(int i=0;i<9;++i)t.observeVideo(9000+900,9000);
    CHECK(t.update(60020,false)==250);
    t.reset();CHECK(!t.known()&&t.targetMs()==0);
}

TEST(audio_own_clock_ring_finds_the_arrival_of_a_shown_frame){
    veyra::sink::PresentedArrivalRing ring;
    CHECK(ring.find(166667)==0);
    ring.note(166667,5000000);ring.note(333333,5169000);ring.note(500000,0);
    CHECK(ring.find(166667)==5000000&&ring.find(333300)==5169000);   // within the double-ms rounding
    CHECK(ring.find(250000)==0&&ring.find(500000)==0);               // a generated frame's stamp; no arrival known
    for(int i=0;i<64;++i)ring.note(1000000+i*166667,6000000+i*169000);
    CHECK(ring.find(166667)==0&&ring.find(1000000+63*166667)==6000000+63*169000);
    ring.clear();CHECK(ring.find(1000000)==0);
}

TEST(audio_own_clock_bench2_stream_stamped_as_60_delivering_59){
    using namespace audio_own_clock_tests;
    // BENCH2: 58.5-59.4 delivered frames per second for a 60 profile. Default rule: the target follows the
    // lagging video timestamp, the trim cannot, the 60 ms rule restarts the audio again and again.
    for(double hz:{59.4,59.0,58.5}){
        auto c=ocBench2(hz);
        const auto today=ocm::simulate(c,ocm::Rule::Today);
        c.ownClock=true;
        const auto own=ocm::simulate(c,ocm::Rule::Today);
        char name[64];std::snprintf(name,sizeof(name),"%.1f fps stamped as 60, 60 s",hz);
        ocPrint(name,"default",today);ocPrint("","own-clock",own);
        CHECK(today.stampLagMaxMs>200&&today.targetMaxMs>200&&today.latMax>200);   // the BENCH2 picture: hundreds of ms within a minute
        // At 59.4 the lag grows about as fast as the trim's 5 ms/s, so the model's audio just follows it late;
        // from 59.0 down it cannot, and the 60 ms rule restarts it (BENCH2: 3-10 restarts a minute).
        if(hz<=59.0)CHECK(today.resets>=3);
        // Own clock: the same stream, no restart, no silence, the delay bounded by the picture's own delay plus
        // the scheduling reserve (20 ms) and the arrival jitter.
        CHECK(own.resets==0&&own.silenceMs==0&&own.underruns==0);
        CHECK(own.targetMaxMs<c.videoMs+3&&own.latMax<c.videoMs+20+12);
        CHECK(own.maxPitchPpm<=5000+21&&!own.futureAudio&&!own.nonFinite);
    }
}

TEST(audio_own_clock_changes_nothing_a_true_timestamp_did_not_already_give){
    using namespace audio_own_clock_tests;
    // With a true video timestamp (the model's old assumption) both rules play at the same delay.
    for(double video:{35.0,12.0}){
        ocm::Config c;c.seconds=60;c.videoMs=video;
        const auto today=ocm::simulate(c,ocm::Rule::Today);
        c.ownClock=true;
        const auto own=ocm::simulate(c,ocm::Rule::Today);
        ocPrint(video>20?"true timestamps, video 35 ms":"true timestamps, video 12 ms","default",today);ocPrint("","own-clock",own);
        CHECK(today.resets==0&&own.resets==0&&own.silenceMs==0);
        CHECK(std::abs(own.latMed-today.latMed)<3&&std::abs(own.latP95-today.latP95)<4);
    }
}

TEST(audio_own_clock_follows_a_real_change_of_the_picture_delay_without_a_restart){
    using namespace audio_own_clock_tests;
    // Frame generation switched on mid-stream: +25 ms of picture delay after a 200 ms rebuild pause.
    auto c=ocBench2(59.0,90);c.ownClock=true;
    ocm::Event e;e.kind=ocm::Event::VideoDelay;e.atMs=30000;e.value=25;c.events.push_back(e);
    const auto own=ocm::simulate(c,ocm::Rule::Today);
    ocPrint("59 fps, +25 ms picture delay at 30 s","own-clock",own);
    CHECK(own.resets==0&&own.silenceMs==0);
    CHECK(own.targetMaxMs>c.videoMs+20&&own.targetMaxMs<c.videoMs+25+3);
    // The experiment's continuous controller on the same clock: no restart either.
    const auto fast=ocm::simulate(c,ocm::Rule::Fast);
    ocPrint("","own+fast",fast);
    CHECK(fast.resets==0&&!fast.futureAudio&&!fast.nonFinite);
}

TEST(audio_own_clock_a_network_stall_is_still_dropped_not_played_late){
    using namespace audio_own_clock_tests;
    // The queue bound is the existing one: audio more than 60 ms behind its schedule is restarted from fresh
    // sound, so a stall does not stay as delay. One 300 ms stall with everything delivered afterwards.
    auto c=ocBench2(59.0,60);c.ownClock=true;
    ocm::Event e;e.kind=ocm::Event::HeldStall;e.atMs=30000;e.lenMs=300;c.events.push_back(e);
    const auto own=ocm::simulate(c,ocm::Rule::Today);
    ocPrint("59 fps, one 300 ms held stall","own-clock",own);
    CHECK(own.resets<=2&&own.unrecovered==0);
    CHECK(own.lastQuarterMedSigned<25&&own.lastQuarterMedSigned>-5);
}
