// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Soak and property tests of the PS5 Remote Play audio path on the LINUX MODEL in
// include/veyra/sink/AudioPathModel.h (virtual time; not a measurement of PushFPS, a PS5 or a headset).
// "Today" = the default rule (switches unset); "fast" = VEYRA_EXP_RP_AUDIO_FAST_RECOVER; "low" = that plus
// VEYRA_EXP_RP_AUDIO_LOW_BUFFER. Durations here are kept short enough for the sanitizer builds; the 2 h and
// 6 h sessions and the full table are in tools/remoteplay_latency_bench/audio_sim.cpp.
#include "veyra/sink/AudioPathModel.h"
#include <cstdio>
#include <string>
#include <vector>
namespace audio_soak_tests {
namespace m=veyra::sink::audio_model;
inline void print(const char* name,const char* rule,const m::Result& r){
    std::printf("[audio-soak] %-30s %-5s |A/V| %5.1f/%5.1f/%6.1f lat %5.1f/%5.1f sil %7.0f resets %3d cut %6.0f loud/s %4.1f rec %6.0f unrec %d pitch %4.0f q %5.1f->%5.1f\n",
        name,rule,r.avMed,r.avP95,r.avMax,r.latMed,r.latP95,r.silenceMs,r.resets,r.cutMs,r.maxLoudCutPerSecondMs,r.recoverMaxMs,r.unrecovered,r.maxPitchPpm,r.firstQuarterMedSigned,r.lastQuarterMedSigned);
}
// Properties every experimental run must keep, whatever happens around it.
inline bool sane(const m::Result& r){
    return !r.futureAudio&&!r.nonFinite&&r.maxLoudCutPerSecondMs<=50.5&&r.maxPitchPpm<=5000+21&&r.playedMs>0;
}
inline m::Config base(double seconds,std::uint64_t seed=1){m::Config c;c.seconds=seconds;c.seed=seed;return c;}
inline std::vector<m::Event> every(m::Event::Kind kind,double fromMs,double toMs,double stepMs,double lenMs){
    std::vector<m::Event> out;for(double t=fromMs;t<toMs;t+=stepMs){m::Event e;e.kind=kind;e.atMs=t;e.lenMs=lenMs;out.push_back(e);}return out;
}
}
using namespace audio_soak_tests;

TEST(audio_model_steady_state_follows_the_video_delay){
    for(double video:{35.0,15.0}){
        auto c=base(120);c.videoMs=video;
        const auto today=m::simulate(c,m::Rule::Today),fast=m::simulate(c,m::Rule::Fast),low=m::simulate(c,m::Rule::FastLowBuffer);
        print(video>20?"steady, video 35 ms":"steady, video 15 ms","today",today);print("","fast",fast);print("","low",low);
        for(const auto* r:{&today,&fast,&low}){CHECK(r->resets==0&&r->silenceMs==0&&r->underruns==0&&r->cutMs<3&&r->avP95<4);}
        CHECK(sane(fast)&&sane(low));
        // Latency is the video delay plus the arrival jitter of the PTS anchor (2-3.5 ms here).
        CHECK(std::abs(fast.latMed-(video+c.netBaseMs))<4);
    }
    // Video faster than the 20 ms scheduling reserve: only the low buffer (14 ms reserve) gets closer.
    auto c=base(60);c.videoMs=10;
    const auto fast=m::simulate(c,m::Rule::Fast),low=m::simulate(c,m::Rule::FastLowBuffer);
    CHECK(fast.avMed>=4&&low.avMed<fast.avMed-2&&low.latMed<fast.latMed-2);
}

TEST(audio_model_clock_drift_does_not_accumulate_with_fast_recovery){
    // 10 min per case here (sanitizer builds); 2 h and 6 h runs are in audio_sim's table.
    for(double ppm:{50.0,-50.0,200.0,-200.0,500.0,-500.0}){
        auto c=base(600);c.sourcePpm=ppm;c.wanderPpm=std::abs(ppm)/5;c.wanderPeriodS=300;
        const auto today=m::simulate(c,m::Rule::Today),fast=m::simulate(c,m::Rule::Fast);
        char name[48];std::snprintf(name,sizeof name,"drift %+.0f ppm, 10 min",ppm);print(name,"today",today);print("","fast",fast);
        CHECK(sane(fast));
        CHECK(fast.resets==0&&fast.avP95<4.5&&fast.lastHourP2P<5);
        CHECK(std::abs(fast.lastQuarterMedSigned-fast.firstQuarterMedSigned)<1.5);
        if(std::abs(ppm)>=200)CHECK(today.avP95>15);   // finding 1: today's rule drifts (documented, unchanged)
    }
}

TEST(audio_model_device_clock_and_hidden_latency){
    for(double ppm:{300.0,-300.0}){
        auto c=base(600);c.devices[0].clockPpm=ppm;
        const auto fast=m::simulate(c,m::Rule::Fast);
        CHECK(sane(fast)&&fast.avP95<3&&fast.resets==0);
    }
    // Bluetooth-like 150 ms after the device clock: invisible to every rule (finding 9).
    auto c=base(120);c.devices[0].hiddenMs=150;
    const auto today=m::simulate(c,m::Rule::Today),fast=m::simulate(c,m::Rule::Fast);
    CHECK(fast.avP95<3&&fast.earMed>145&&today.earMed>145);
}

TEST(audio_model_repeated_stalls_do_not_ratchet){
    struct Case {const char* name;m::Event::Kind kind;double len;};
    const Case cases[]={{"loss stall 200 ms / 30 s",m::Event::LossStall,200},{"loss stall 1 s / 30 s",m::Event::LossStall,1000},
        {"held 60 ms / 30 s",m::Event::HeldStall,60},{"held 5 s / 30 s",m::Event::HeldStall,5000}};
    for(const auto& k:cases){
        auto c=base(600);c.signal=m::Signal::Speech;c.events=every(k.kind,10000,590000,30000,k.len);
        const auto today=m::simulate(c,m::Rule::Today),fast=m::simulate(c,m::Rule::Fast);
        print(k.name,"today",today);print("","fast",fast);
        CHECK(sane(fast)&&fast.unrecovered==0&&fast.recoverMaxMs<1500);
        CHECK(std::abs(fast.lastQuarterMedSigned-fast.firstQuarterMedSigned)<2&&fast.avMed<3);
        CHECK(fast.avP95<=today.avP95+0.5);
    }
    // Finding 11 (today): a loss stall leaves audio about 90 ms late until the next discontinuity.
    auto c=base(120);c.events={{m::Event::LossStall,20000,200}};
    CHECK(m::simulate(c,m::Rule::Today).lastQuarterMedSigned>80&&std::abs(m::simulate(c,m::Rule::Fast).lastQuarterMedSigned)<3);
}

TEST(audio_model_reconnect_then_loss_inside_the_learning_window){
    // Review round 2 defect: a discontinuity in AudioArrivalDrift's first 2 s was learned as the
    // reference, keeping audio ~90 ms late for the whole session.
    auto c=base(60);c.events={{m::Event::Reconnect,5000,100},{m::Event::LossStall,6000,200}};
    const auto fast=m::simulate(c,m::Rule::Fast);print("reconnect + early loss","fast",fast);
    CHECK(sane(fast)&&std::abs(fast.lastQuarterMedSigned)<3&&fast.unrecovered==0&&fast.recoverMaxMs<1500);
    auto plain=base(60);plain.events={{m::Event::Reconnect,20000,3000}};
    for(auto rule:{m::Rule::Today,m::Rule::Fast}){const auto r=m::simulate(plain,rule);CHECK(r.avP95<4&&r.unrecovered==0&&r.recoverMaxMs<1000);}
}

TEST(audio_model_device_events_at_awkward_moments){
    m::Device usb;usb.clockPpm=60;usb.hiddenMs=1;
    m::Device resampled=usb;resampled.lowLatencyCapable=false;resampled.clockPpm=-90;   // 44.1 kHz mix format
    struct Case {const char* name;std::vector<m::Event> events;};
    const std::vector<Case> cases={
        {"default device switch",{{m::Event::DeviceSwitch,20000,0,1}}},
        {"unplug 4 s, replug",{{m::Event::Unplug,20000,4000,1}}},
        {"format change to 44.1 kHz",{{m::Event::DeviceSwitch,20000,0,2}}},
        {"switch mid-cut (after a stall)",{{m::Event::EndpointStall,20000,200},{m::Event::DeviceSwitch,20260,0,1}}},
        {"switch mid-ratio (FG toggled)",{{m::Event::VideoDelay,20000,0,0,17},{m::Event::DeviceSwitch,21000,0,2}}},
        {"switch during a network stall",{{m::Event::LossStall,20000,1000},{m::Event::DeviceSwitch,20500,0,1}}},
        {"unplug during a held stall",{{m::Event::HeldStall,20000,2000},{m::Event::Unplug,20300,1500,2}}},
        {"device clock stall 300 ms",{{m::Event::EndpointStall,20000,300}}},
        // Review round 2 defect: a burst re-anchor while the endpoint is down is dropped by push() with
        // its discontinuity flag, so the reset after the reopen did not rebase AudioArrivalDrift and the
        // new anchor was followed at 2 ms/s (about 45 s for 90 ms).
        {"held stall ending while unplugged",{{m::Event::Unplug,24202,5000,1},{m::Event::HeldStall,27176,500}}},
    };
    for(const auto& k:cases){
        auto c=base(60);c.devices={m::Device{},usb,resampled};c.signal=m::Signal::Tone;c.events=k.events;
        const auto today=m::simulate(c,m::Rule::Today),fast=m::simulate(c,m::Rule::Fast),low=m::simulate(c,m::Rule::FastLowBuffer);
        print(k.name,"today",today);print("","fast",fast);print("","low",low);
        CHECK(sane(fast)&&sane(low)&&fast.unrecovered==0&&low.unrecovered==0);
        CHECK(fast.recoverMaxMs<1500&&low.recoverMaxMs<1500);
        CHECK(std::abs(fast.lastQuarterMedSigned)<3&&std::abs(low.lastQuarterMedSigned)<3);
    }
    // A device clock stall is heard as a dropout: the delay it added goes at once (one masked cut),
    // not as 5 ms cuts in sound over seconds.
    auto c=base(60);c.signal=m::Signal::Tone;c.events={{m::Event::EndpointStall,20000,300}};
    const auto fast=m::simulate(c,m::Rule::Fast);
    CHECK(fast.loudCuts==0&&fast.gapCuts>=1&&fast.maxCutMs>=250&&fast.recoverMaxMs<300);
}

TEST(audio_model_underrun_storm_and_low_buffer_fallback){
    auto c=base(120);c.signal=m::Signal::Music;
    c.events=every(m::Event::ThreadStall,20000,30000,200,25);
    const auto today=m::simulate(c,m::Rule::Today),fast=m::simulate(c,m::Rule::Fast),low=m::simulate(c,m::Rule::FastLowBuffer);
    print("thread stalls 25 ms / 200 ms, 10 s","today",today);print("","fast",fast);print("","low",low);
    CHECK(sane(fast)&&sane(low));
    CHECK(fast.unrecovered==0&&low.unrecovered==0&&fast.recoverMaxMs<1000&&low.recoverMaxMs<1000);
    CHECK(fast.avP95<=today.avP95&&std::abs(low.lastQuarterMedSigned)<3);
    // Random 1 % loss: single concealed frames, never a reset.
    auto lossy=base(300);lossy.randomLoss=0.01;
    for(auto rule:{m::Rule::Fast,m::Rule::FastLowBuffer}){const auto r=m::simulate(lossy,rule);CHECK(sane(r)&&r.resets==0&&r.avP95<3);}
}

TEST(audio_model_frame_generation_toggled_every_minute){
    for(auto signal:{m::Signal::Tone,m::Signal::Speech}){
        auto c=base(600);c.signal=signal;
        for(double t=60000;t<590000;t+=60000)c.events.push_back({m::Event::VideoDelay,t,0,0,static_cast<int>(t/60000)%2?17.0:0.0});
        const auto today=m::simulate(c,m::Rule::Today),fast=m::simulate(c,m::Rule::Fast);
        print(signal==m::Signal::Tone?"FG toggled / min, tone":"FG toggled / min, speech","today",today);print("","fast",fast);
        CHECK(sane(fast)&&fast.resets==0&&fast.unrecovered==0);
        CHECK(fast.recoverMaxMs<2500&&fast.recoverMaxMs<today.recoverMaxMs);
        CHECK(std::abs(fast.lastQuarterMedSigned-fast.firstQuarterMedSigned)<1.5);
    }
}

TEST(audio_model_random_mixes_keep_every_property){
    // Fixed seeds; every event kind, drift, wander, jitter and three devices at random (randomMix).
    int fastBetter=0;
    for(std::uint64_t seed=1;seed<=8;++seed){
        const auto c=m::randomMix(seed,180);
        const auto today=m::simulate(c,m::Rule::Today),fast=m::simulate(c,m::Rule::Fast),low=m::simulate(c,m::Rule::FastLowBuffer);
        CHECK(sane(fast)&&sane(low)&&!today.futureAudio&&!today.nonFinite);
        CHECK(fast.unrecovered==0&&low.unrecovered==0);
        // Below the 20 ms reserve the floor keeps audio up to (reserve + anchor jitter - video) late.
        const double floorMs=std::max(0.0,22-c.videoMs-c.fgExtraMs);
        CHECK(fast.avMed<5+floorMs&&low.avMed<5+floorMs&&fast.avP95<15+floorMs&&low.avP95<15+floorMs);
        CHECK(fast.avP95<=today.avP95+1);
        fastBetter+=fast.avP95<today.avP95?1:0;
    }
    CHECK(fastBetter>=7);
}

TEST(audio_crossfade_splice_on_waveforms_adds_no_step){
    // The controller's 5 ms loud cut (240 frames, 2.5 ms fade = 120) in a tone, a chirp and noise: the
    // largest sample-to-sample step around the splice stays near what the signal itself has.
    constexpr double pi=3.141592653589793;
    for(int kind=0;kind<3;++kind){
        std::vector<float> s(4800);veyra::sink::audio_model::Rng g(9);
        for(std::size_t i=0;i<s.size();++i){
            const double t=static_cast<double>(i)/48000;
            s[i]=static_cast<float>(kind==0?0.5*std::sin(2*pi*1000*t):kind==1?0.5*std::sin(2*pi*(200*t+20000*t*t)):0.3*(g.next()*2-1));
        }
        double natural=0;for(std::size_t i=1;i<s.size();++i)natural=std::max(natural,static_cast<double>(std::abs(s[i]-s[i-1])));
        std::vector<float> cut(s);
        CHECK(veyra::sink::audioCrossfadeSplice(cut.data()+1000,cut.size()-1000,1,240,120));
        // Played: s[0..999], then cut[1240..].
        double step=std::abs(static_cast<double>(cut[1240])-static_cast<double>(s[999]));
        for(std::size_t i=1241;i<1400;++i)step=std::max(step,static_cast<double>(std::abs(cut[i]-cut[i-1])));
        CHECK(step<=natural*1.25+1e-3);
    }
}

TEST(audio_model_short_device_clock_stalls_recover_at_once){
    // Reviewer's case: three short device clock stalls in sparse sound. Quiet cuts made while the clock
    // stood still must wait for it (their deadlines move with the stall); otherwise they expire unheard,
    // are cut again and audio ends early for seconds (P95 4.4 ms, recovery 179 ms with that mutation).
    for(double len:{60.0,100.0}){
        auto c=base(60);c.signal=m::Signal::Sparse;
        c.events={{m::Event::EndpointStall,20000,len},{m::Event::EndpointStall,30000,len},{m::Event::EndpointStall,40000,len}};
        const auto fast=m::simulate(c,m::Rule::Fast);
        CHECK(sane(fast)&&fast.avP95<1.5&&fast.recoverMaxMs<100&&fast.resets==0);
    }
}

TEST(audio_model_low_buffer_survives_device_clock_stalls_every_3_s){
    // Review round 2: with a 6 ms endpoint the masked cut after a stall was heard before its pending
    // deadline, counted twice, read as audio 60 ms early and reset after every stall (391 resets / 20 min).
    auto c=base(180);c.signal=m::Signal::Tone;c.events=every(m::Event::EndpointStall,10000,170000,3000,100);
    const auto fast=m::simulate(c,m::Rule::Fast),low=m::simulate(c,m::Rule::FastLowBuffer);
    print("device clock stall 100 ms / 3 s","fast",fast);print("","low",low);
    CHECK(sane(fast)&&sane(low)&&fast.resets==0&&low.resets==0&&low.silenceMs<50);
}

