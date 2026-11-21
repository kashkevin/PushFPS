// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Sprint 4 audio round 3: pop diagnostics (AudioStatsWindow), VEYRA_EXP_RP_AUDIO_SMOOTH (AudioEdgeSmoother)
// and VEYRA_EXP_RP_AUDIO_ADAPTIVE (AudioAdaptivePlayout), include/veyra/sink/AudioPopGuard.h, plus the
// Wi-Fi-like arrival rows of the LINUX MODEL (AudioPathModel.h). Waveform tests run the same edge functions
// the renderer calls (AudioGain.h, AudioPopGuard.h) on a full-scale tone; nothing here is a measurement.
#include "veyra/sink/AudioPopGuard.h"
#include "veyra/sink/AudioGain.h"
#include "veyra/sink/AudioPathModel.h"
#include <cmath>
#include <cstdio>
#include <vector>
namespace audio_pop_tests {
namespace m=veyra::sink::audio_model;
using veyra::sink::AudioEdgeSmoother;using veyra::sink::AudioAdaptivePlayout;using veyra::sink::AudioStatsWindow;
// A full-scale stereo 1 kHz tone (phase-shifted right channel): neighbouring samples differ by at most
// 2*pi*1000/48000 = 0.131. "No pop" below = no step larger than that natural bound plus a small slack.
constexpr double kToneStep=2*3.141592653589793*1000/48000;
inline std::vector<float> tone(std::size_t first,std::size_t frames){
    std::vector<float> v(frames*2);
    for(std::size_t i=0;i<frames;++i){const double t=static_cast<double>(first+i)/48000;
        v[2*i]=static_cast<float>(std::sin(2*3.141592653589793*1000*t+0.3));v[2*i+1]=static_cast<float>(std::cos(2*3.141592653589793*1000*t+0.3));}
    return v;
}
inline double worstStep(const std::vector<float>& played){
    double worst=0;
    for(unsigned c=0;c<2;++c)worst=std::max(worst,veyra::sink::largestSampleStep(played.data(),played.size()/2,2,c,played[c],0.0,false));
    return worst;
}
// What the endpoint plays around one underrun, as WasapiAudioSink.cpp pumpOnce writes it: real frames,
// (SMOOTH) the tail, `silence` frames of nothing, then the next real frames with the 5 ms linear fade-in
// (fadePcmHead) the renderer arms after a detected gap or a tail. resumeFadeIn=false: a driver whose clock
// stops while empty, where today's gap detection arms no fade-in (CaptureAudioDsp.h observeEmpty).
inline std::vector<float> underrunEdge(bool smooth,std::size_t silence,bool resumeFadeIn,std::size_t stopAt=4801){
    std::vector<float> played;AudioEdgeSmoother edge;
    const auto before=tone(0,stopAt);played=before;edge.remember(before.data(),stopAt,2);
    if(smooth){std::vector<float> tail(AudioEdgeSmoother::kHistoryFrames*2);const auto n=edge.writeTail(tail.data(),AudioEdgeSmoother::kHistoryFrames,2);played.insert(played.end(),tail.begin(),tail.begin()+static_cast<std::ptrdiff_t>(n*2));}
    played.insert(played.end(),silence*2,0.0f);
    auto after=tone(stopAt+silence,960);
    std::size_t remaining=resumeFadeIn||smooth?240:0;veyra::sink::fadePcmHead(after.data(),960,2,remaining);
    played.insert(played.end(),after.begin(),after.end());
    return played;
}
}
using namespace audio_pop_tests;

TEST(audio_pop_raised_cosine_is_monotone_and_never_steps_at_its_ends){
    for(std::size_t total:{std::size_t{1},std::size_t{2},std::size_t{192},std::size_t{240}}){
        float previous=0;
        for(std::size_t i=0;i<total;++i){const float v=veyra::sink::raisedCosineRise(i,total);CHECK(v>0&&v<1);CHECK(v>previous);previous=v;}
        CHECK(veyra::sink::raisedCosineRise(total,total)==1.0f);
        if(total>=192){CHECK(veyra::sink::raisedCosineRise(0,total)<0.001f);CHECK(veyra::sink::raisedCosineRise(total-1,total)>0.999f);}
    }
}

TEST(audio_pop_today_an_underrun_on_loud_sound_is_a_step_and_smooth_makes_it_none){
    // Today (code reading, WasapiAudioSink.cpp pumpOnce): the endpoint plays the last real frame, then
    // silence, with no fade-out; the resume is faded in. The worst step is the last sample's value.
    for(std::size_t stop:{std::size_t{4801},std::size_t{4812},std::size_t{4830}}){
        const auto today=underrunEdge(false,480,true,stop);
        const auto last=std::max(std::abs(tone(stop-1,1)[0]),std::abs(tone(stop-1,1)[1]));
        CHECK(worstStep(today)>=last-1e-6);CHECK(last>0.3);
        const auto smooth=underrunEdge(true,480,true,stop);
        CHECK(worstStep(smooth)<=kToneStep+0.01);
    }
}

TEST(audio_pop_resume_without_a_detected_gap_steps_today_and_not_with_smooth){
    // Drivers whose clock stops while empty: today's tracker needs two empty observations a period apart,
    // so a gap shorter than that resumes without the fade-in (a step at the resume).
    const auto resumeAt=[](bool smooth){return 4801+(smooth?AudioEdgeSmoother::Config{}.fadeFrames:0)+240;};
    const auto today=underrunEdge(false,240,false);
    const auto first=tone(4801+240,1);
    CHECK(std::abs(today[resumeAt(false)*2+1])>0.3&&std::abs(today[resumeAt(false)*2+1]-first[1])<1e-6);   // straight from silence
    const auto smooth=underrunEdge(true,240,false);
    CHECK(std::abs(smooth[resumeAt(true)*2+1])<=1.0/240+1e-6);
    CHECK(worstStep(smooth)<=kToneStep+0.01);
}

TEST(audio_pop_smooth_tail_is_value_continuous_and_ends_near_zero){
    AudioEdgeSmoother edge;
    const auto block=tone(0,480);edge.remember(block.data(),480,2);
    std::vector<float> tail(400*2,9.0f);
    const auto n=edge.writeTail(tail.data(),400,2);
    CHECK(n==edge.config().fadeFrames);CHECK(edge.tailed());
    for(unsigned c=0;c<2;++c){
        CHECK(std::abs(tail[c]-block[479*2+c])<=kToneStep+0.01);   // the joint
        CHECK(std::abs(tail[(n-1)*2+c])<0.001);                     // into silence
    }
    CHECK(tail[n*2]==9.0f);                                        // nothing written beyond n
    // A short history (fewer frames than the fade) still continues without a step.
    AudioEdgeSmoother shortEdge;const auto few=tone(100,3);shortEdge.remember(few.data(),3,2);
    std::vector<float> t2(192*2);const auto k=shortEdge.writeTail(t2.data(),192,2);
    CHECK(k==192);for(unsigned c=0;c<2;++c){CHECK(std::abs(t2[c]-few[2*2+c])<=kToneStep+0.01);CHECK(std::abs(t2[191*2+c])<0.001);}
    // Limited free space: the fade gets shorter but still ends near zero.
    AudioEdgeSmoother limited;limited.remember(block.data(),480,2);std::vector<float> t3(48*2);
    CHECK(limited.writeTail(t3.data(),48,2)==48);CHECK(std::abs(t3[47*2])<0.01&&std::abs(t3[47*2+1])<0.01);
}

TEST(audio_pop_smooth_tail_is_due_only_when_a_gap_is_certain){
    AudioEdgeSmoother edge;
    CHECK(!edge.due(0,0,480));                       // nothing to continue from yet
    CHECK(edge.due(0,100,480));                      // ... but this pump's own frames will be
    const auto block=tone(0,480);edge.remember(block.data(),480,2);
    CHECK(!edge.due(480,0,480));                     // exactly one period left: the next pump comes in time
    CHECK(!edge.due(300,180,480));
    CHECK(!edge.due(960,240,480));                   // steady state: well above a period
    CHECK(edge.due(300,179,480));
    CHECK(edge.due(1,0,480));
    // Review blocker: the owner woke late and the endpoint is already empty: the cut was heard; a tail now
    // would start at the last sample's value out of silence. Not due.
    CHECK(!edge.due(0,0,480));
    std::vector<float> tail(192*2);edge.writeTail(tail.data(),192,2);
    CHECK(!edge.due(1,0,480));                       // once per run of real data
    CHECK(edge.due(0,10,480));                       // new real frames start a new run
    edge.remember(block.data(),10,2);CHECK(!edge.tailed());CHECK(edge.due(5,0,480));
    CHECK(!AudioEdgeSmoother::willStarve(0,0,0));    // unknown period: never
    // A channel count it cannot hold is ignored, not written out of bounds.
    AudioEdgeSmoother wide;std::vector<float> nine(9*4,0.5f);wide.remember(nine.data(),4,9);CHECK(!wide.due(0,0,480));
    std::vector<float> out(9*8,7.0f);CHECK(wide.writeTail(out.data(),8,9)==0&&out[0]==7.0f);
}

TEST(audio_pop_reset_paths_of_today_are_already_step_free){
    // fadeAndReset: 5 ms linear tail from the current gain (fadePcmTail), then the endpoint restarts with
    // the gain ramp from 0 (stopAndReset sets smoothedGain_=0; applyPcmGain moves 1/240 per frame).
    auto before=tone(0,4800);std::vector<float> played(before.begin(),before.end());
    auto tail=tone(4800,240);veyra::sink::fadePcmTail(tail.data(),240,2,1.0f);played.insert(played.end(),tail.begin(),tail.end());
    played.insert(played.end(),960u*2,0.0f);
    auto after=tone(9000,960);float gain=0;veyra::sink::applyPcmGain(after.data(),960,2,1.0f,gain);played.insert(played.end(),after.begin(),after.end());
    CHECK(worstStep(played)<=kToneStep+0.01);
    // Volume changes ramp too (5 ms full scale).
    auto loud=tone(1,960);float g=1;veyra::sink::applyPcmGain(loud.data(),960,2,0.0f,g);
    CHECK(g==0.0f);
    for(unsigned c=0;c<2;++c)CHECK(veyra::sink::largestSampleStep(loud.data(),960,2,c,tone(0,1)[c],0,false)<=kToneStep+1.0/240+0.01);
}

TEST(audio_pop_adaptive_stays_at_today_s_reserve_without_underruns){
    AudioAdaptivePlayout a;a.reset(0,20);
    for(double t=0;t<600000;t+=10){a.arrivalGap(t,10+((static_cast<long long>(t)/10)%7==0?6.0:0.0));CHECK(a.update(t)==20);}
    CHECK(a.extraMs()==0);
}

TEST(audio_pop_adaptive_grows_fast_bounded_and_shrinks_slowly){
    AudioAdaptivePlayout a;a.reset(0,20);
    a.arrivalGap(100,55);a.underrun(110);
    CHECK(std::abs(a.targetMs()-60)<1e-9);           // the largest recent gap + 5 ms margin
    a.underrun(200);CHECK(std::abs(a.targetMs()-60)<1e-9);   // the same gap is already covered: no growth
    a.arrivalGap(250,58);a.underrun(260);CHECK(std::abs(a.targetMs()-70)<1e-9);   // a larger one: +10 at least
    a.arrivalGap(300,400);a.underrun(310);CHECK(std::abs(a.targetMs()-70)<1e-9);  // a pause above the bound: ignored
    a.arrivalGap(320,79);a.underrun(330);CHECK(a.targetMs()==80);                  // bounded
    // Held for 10 s after the last underrun, then at most 2 ms/s, towards the recent gaps + margin.
    double t=310,previous=a.update(t);
    for(;t<310+9990;t+=10){a.arrivalGap(t,10);const double v=a.update(t);CHECK(v==80);previous=v;}
    for(;t<200000;t+=10){a.arrivalGap(t,10);const double v=a.update(t);CHECK(v<=previous+1e-12);CHECK(previous-v<=2.0*10/1000+1e-9);CHECK(v>=20);previous=v;}
    CHECK(std::abs(a.targetMs()-20)<1e-9);           // gaps short again: back at the base, never below
    // While large gaps are still recent, it shrinks only to them.
    AudioAdaptivePlayout b;b.reset(0,20);b.arrivalGap(0,45);b.underrun(0);b.arrivalGap(1,51);b.underrun(1);b.arrivalGap(2,61);b.underrun(2);b.arrivalGap(3,71);b.underrun(3);
    CHECK(b.targetMs()==80);
    for(double u=0;u<19000;u+=10){b.arrivalGap(u,45);b.update(u);}
    CHECK(b.targetMs()<80&&b.targetMs()>=50-1e-9);
    // A larger base (other endpoint) lifts the floor at once; reset returns to it.
    const double extra=b.extraMs();b.setBase(30);CHECK(std::abs(b.extraMs()-std::min(extra,50.0))<1e-9);
    b.reset(20000,20);CHECK(b.targetMs()==20&&b.extraMs()==0);
    AudioAdaptivePlayout c;c.reset(0,20);c.setBase(14);CHECK(c.targetMs()==14&&c.extraMs()==0);   // low buffer: no extra
}

TEST(audio_pop_stats_window_counts_and_resets){
    AudioStatsWindow w;
    auto empty=w.take();CHECK(empty.pops==0&&empty.queueP50==-1&&empty.gapMax==-1&&empty.paddingMin==-1);
    w.underrunStart();w.underrunStart();w.hardReset();w.unfadedCut();w.tail();
    for(int i=1;i<=100;++i){w.queue(i);w.arrivalGap(i<=95?10:60+i);w.padding(5+i);}
    const auto s=w.take();
    CHECK(s.underrunStarts==2&&s.hardResets==1&&s.unfadedCuts==1&&s.tails==1&&s.pops==3);
    CHECK(s.queueMin==1&&std::abs(s.queueP50-50)<=0.5&&std::abs(s.queueP95-95)<=0.5&&s.queueSamples==100);
    CHECK(std::abs(s.gapP95-10)<=0.5&&s.gapMax==160&&s.arrivals==100);
    CHECK(s.paddingMin==6);
    const auto again=w.take();CHECK(again.pops==0&&again.queueSamples==0&&again.gapMax==-1);
    // Out-of-range and non-finite values do not corrupt the window.
    w.queue(1e9);w.queue(-5);w.queue(std::nan(""));const auto big=w.take();CHECK(big.queueSamples==2&&big.queueMin==0&&big.queueP95==1e9);
}

// ---- the path model (LINUX MODEL, AudioPathModel.h round 3) -------------------------------------------
namespace audio_pop_tests {
inline m::Config popConfig(double seconds,double every,double lo,double hi,bool smooth,bool adaptive,std::uint64_t seed=1){
    m::Config c;c.seconds=seconds;c.seed=seed;c.videoMs=15;c.wifiEveryMs=every;c.wifiMinMs=lo;c.wifiMaxMs=hi;c.smooth=smooth;c.adaptive=adaptive;return c;
}
inline void printPop(const char* name,const m::Result& r){
    std::printf("[audio-pop] %-26s clicks %4d gaps %4d resets %3d tails %4d sil %6.0f lat %5.1f/%5.1f av95 %5.1f playout %4.0f\n",
        name,r.clicks,r.gaps,r.resets,r.tails,r.silenceMs,r.latMed,r.latP95,r.avP95,r.playoutMaxMs);
}
}

TEST(audio_pop_model_clean_link_keeps_today_s_delay_with_both_switches){
    for(double jitter:{1.5,4.0}){
        auto base=popConfig(120,0,0,0,false,false);base.netJitterMs=jitter;
        const auto today=m::simulate(base,m::Rule::Today);
        for(int mode=1;mode<4;++mode){
            auto c=base;c.smooth=(mode&1)!=0;c.adaptive=(mode&2)!=0;
            const auto r=m::simulate(c,m::Rule::Today);
            CHECK(r.latMed==today.latMed&&r.latP95==today.latP95&&r.avP95==today.avP95);
            CHECK(r.gaps==0&&r.tails==0&&r.resets==0&&r.silenceMs==0);
            CHECK(!c.adaptive||r.playoutMaxMs==20);
        }
    }
}

TEST(audio_pop_model_wifi_bursts_click_today_and_not_with_smooth){
    for(std::uint64_t seed:{1u,2u}){
        const auto today=m::simulate(popConfig(120,1000,20,60,false,false,seed),m::Rule::Today);
        const auto smooth=m::simulate(popConfig(120,1000,20,60,true,false,seed),m::Rule::Today);
        if(seed==1){printPop("wifi typical today",today);printPop("wifi typical smooth",smooth);}
        CHECK(today.clicks>=10);CHECK(today.gaps>=today.clicks);
        CHECK(smooth.clicks==0);CHECK(smooth.tails>=smooth.gaps);CHECK(!smooth.futureAudio&&!smooth.nonFinite);
        // Zero added delay in steady state: a tail is written only when a gap is certain (+4 ms at most then).
        CHECK(smooth.latMed<=today.latMed+4);
    }
}

TEST(audio_pop_model_adaptive_removes_most_underruns_at_a_bounded_cost){
    for(double every:{2000.0,1000.0}){
        const auto today=m::simulate(popConfig(180,every,20,every>1500?40:60,false,false),m::Rule::Today);
        const auto adaptive=m::simulate(popConfig(180,every,20,every>1500?40:60,false,true),m::Rule::Today);
        const auto both=m::simulate(popConfig(180,every,20,every>1500?40:60,true,true),m::Rule::Today);
        printPop(every>1500?"wifi light adaptive":"wifi typical adaptive",adaptive);printPop("  smooth+adaptive",both);
        CHECK(adaptive.gaps*4<=today.gaps);CHECK(both.clicks==0);
        CHECK(adaptive.playoutMaxMs<=80&&both.playoutMaxMs<=80);
        CHECK(adaptive.latP95<=80+15+5);                 // reserve bound + video delay + one block of slack
        CHECK(!adaptive.futureAudio&&!both.futureAudio&&!adaptive.nonFinite);
    }
}

TEST(audio_pop_model_adaptive_shrinks_back_after_the_bursts_stop){
    // Bursts for the first minute only, then a clean link: the extra delay drains back to today's.
    auto c=popConfig(240,1000,20,60,false,true);c.wifiUntilMs=60000;
    const auto r=m::simulate(c,m::Rule::Today);
    const auto clean=m::simulate(popConfig(240,0,0,0,false,false),m::Rule::Today);
    CHECK(r.playoutMaxMs>40);
    const auto& a=r.perSecondSigned;const auto& b=clean.perSecondSigned;
    CHECK(a.size()==b.size()&&a.size()>=240);
    for(std::size_t i=a.size()-30;i<a.size();++i){CHECK(std::isfinite(a[i])&&std::abs(a[i]-b[i])<2);}
    // ... and not quickly: 10 s after the last burst it is still above today's (hold, then 2 ms/s).
    CHECK(a[70]-b[70]>5);
}

// ---- phase 2: SmartLink's audio reserve request (AudioAdaptivePlayout::request) ------------------------
TEST(audio_pop_smartlink_request_raises_at_once_releases_slowly_and_is_bounded){
    AudioAdaptivePlayout a;a.reset(0,20);
    a.request(0);CHECK(a.update(0)==20&&a.source()==0&&a.extraMs()==0);
    a.request(45);CHECK(a.update(10)==45&&a.source()==2);
    a.request(500);CHECK(a.update(20)==80);                  // never above the bound
    a.request(std::nan(""));double previous=a.update(30);   // nothing asked any more: released at 2 ms/s
    CHECK(previous<=80&&previous>79.9);
    for(double t=40;t<60000;t+=10){const double v=a.update(t);CHECK(v<=previous+1e-12&&previous-v<=0.02+1e-9&&v>=20);previous=v;}
    CHECK(a.targetMs()==20&&a.source()==0);
    // The request never lowers the class's own reserve.
    AudioAdaptivePlayout b;b.reset(0,20);b.arrivalGap(0,55);b.underrun(0);CHECK(b.targetMs()==60);
    b.request(30);CHECK(b.update(10)==60&&b.source()==1);
    b.request(70);CHECK(b.update(20)==70&&b.source()==2);
    b.reset(30,20);CHECK(b.targetMs()==20&&b.source()==0);
}

TEST(audio_pop_stats_window_time_above_target){
    AudioStatsWindow w;CHECK(w.take().aboveTargetPct==-1);
    for(int i=0;i<100;++i)w.queue(i<25?40.0:20.0,20.0);
    const auto s=w.take();CHECK(std::abs(s.aboveTargetPct-25)<1e-9);CHECK(s.queueSamples==100);
    w.queue(25.0,20.0);CHECK(w.take().aboveTargetPct==0);   // within 5 ms: not counted
}

// ---- phase 2 on the model: audio continuity arm = FAST_RECOVER + SMOOTH + ADAPTIVE (+ SmartLink apply) ----
namespace audio_pop_tests {
// Repeated disturbances: Wi-Fi bursts (20-60 ms, every 1 s) in the first `onMs` of every `periodMs`.
inline m::Config continuity(double seconds,double periodMs,double onMs,double askMs,bool smooth=true,bool adaptive=true){
    m::Config c=popConfig(seconds,1000,20,60,smooth,adaptive);c.wifiPeriodMs=periodMs;c.wifiOnMs=onMs;c.smartlinkAskMs=askMs;return c;
}
// Mean signed A/V offset (ms) over seconds [from, to) of a run; NaN seconds skipped.
inline double meanOffset(const m::Result& r,std::size_t from,std::size_t to){
    double sum=0;int n=0;for(std::size_t i=from;i<to&&i<r.perSecondSigned.size();++i)if(std::isfinite(r.perSecondSigned[i])){sum+=r.perSecondSigned[i];++n;}
    return n?sum/n:std::nan("");
}
}

TEST(audio_pop_model_smartlink_unset_stale_or_zero_is_exactly_phase_one){
    // An ask of 0 (stale, observe, "leave") must leave every number as without SmartLink, clean or bursty.
    for(double every:{0.0,1000.0}){
        auto base=popConfig(120,every,20,60,true,true);const auto without=m::simulate(base,m::Rule::Fast);
        auto zero=base;zero.smartlinkAskMs=60;zero.smartlinkDetectMs=1e12;   // asked never in time: always 0
        const auto with=m::simulate(zero,m::Rule::Fast);
        CHECK(with.latMed==without.latMed&&with.latP95==without.latP95&&with.gaps==without.gaps&&with.resets==without.resets&&with.silenceMs==without.silenceMs);
    }
    // And a clean link with SmartLink asking nothing keeps today's delay under every rule.
    for(auto rule:{m::Rule::Today,m::Rule::Fast}){
        auto c=popConfig(120,0,0,0,false,false);const auto today=m::simulate(c,rule);
        c.smartlinkAskMs=60;c.smooth=c.adaptive=true;const auto r=m::simulate(c,rule);
        CHECK(r.latMed==today.latMed&&r.playoutMaxMs==20&&r.gaps==0);
    }
}

TEST(audio_pop_model_delay_returns_to_the_floor_after_each_disturbance_and_never_ratchets){
    // 40 min: 2 min of bursts every 10 min. Floor = the same run's clean-link offset (no bursts at all).
    const auto clean=m::simulate(popConfig(2400,0,0,0,true,true),m::Rule::Fast);
    auto noBursts=continuity(2400,600000,0,0);    // never disturbed: the same arm on a clean link
    const auto cleanArm=m::simulate(noBursts,m::Rule::Fast);
    CHECK(cleanArm.latMed==clean.latMed);
    for(double ask:{0.0,60.0}){
        const auto r=m::simulate(continuity(2400,600000,120000,ask),m::Rule::Fast);
        CHECK(r.playoutMaxMs>20);CHECK(r.clicks==0&&!r.futureAudio&&!r.nonFinite);
        const double floor=meanOffset(clean,60,2400);
        std::vector<double> ends;
        for(std::size_t cycle=0;cycle<4;++cycle){
            const std::size_t end=cycle*600+600;
            // 90 s after the bursts end (120 s on, so from second 210 of the cycle) the mean is back at the floor.
            const double after=meanOffset(r,cycle*600+210,end);
            CHECK(std::abs(after-floor)<1.5);ends.push_back(after);
            // ... and the disturbance itself did raise it (so the test sees a return, not nothing).
            CHECK(meanOffset(r,cycle*600+60,cycle*600+120)-floor>10);
        }
        CHECK(std::abs(ends.back()-ends.front())<0.5);   // no ratchet from one cycle to the next
    }
}

TEST(audio_pop_model_continuity_arm_recovers_from_stalls_and_drift){
    // Finding 11 (today: about 91 ms late for good after a 200 ms stall) and finding 1 (drift accumulates):
    // the continuity arm (fast + smooth + adaptive) removes both.
    auto stalls=popConfig(300,0,0,0,true,true);stalls.signal=m::Signal::Speech;
    for(double t=10000;t<290000;t+=30000)stalls.events.push_back({m::Event::LossStall,t,200});
    auto todayStalls=stalls;todayStalls.smooth=todayStalls.adaptive=false;
    const auto today=m::simulate(todayStalls,m::Rule::Today),arm=m::simulate(stalls,m::Rule::Fast);
    CHECK(today.avMed>80);CHECK(arm.avMed<3&&arm.avP95<15&&std::abs(arm.lastQuarterMedSigned-arm.firstQuarterMedSigned)<3);
    // (adaptive treats each stall's underrun as a disturbance: up to 30 ms reserve for a while, then back.)
    for(double ppm:{100.0,-100.0}){
        auto drift=popConfig(600,0,0,0,true,true);drift.sourcePpm=ppm;
        auto todayDrift=drift;todayDrift.smooth=todayDrift.adaptive=false;
        const auto t0=m::simulate(todayDrift,m::Rule::Today),a=m::simulate(drift,m::Rule::Fast);
        if(ppm>0)CHECK(t0.lastQuarterMedSigned-t0.firstQuarterMedSigned>20);   // today: piles up (finding 1)
        CHECK(std::abs(a.lastQuarterMedSigned-a.firstQuarterMedSigned)<2&&a.clicks==0&&a.resets==0);
        CHECK(t0.clicks<=1&&t0.resets==0);   // and today's resampling trim itself never clicks
    }
}

TEST(audio_pop_gap_quantile_forgets_and_the_reserve_rises_before_any_underrun){
    veyra::sink::AudioGapQuantile q(0.999);CHECK(q.quantile(.97)==-1);
    for(int i=0;i<1000;++i)q.add(i%50==0?60.0:10.0);           // 2 % at 60 ms
    CHECK(q.quantile(.5)==11&&q.quantile(.97)==11&&q.quantile(.99)==61);
    for(int i=0;i<5000;++i)q.add(10);                            // forgotten after about 1000 values each
    CHECK(q.quantile(.999)==11);
    q.add(std::nan(""));q.add(1e9);CHECK(q.quantile(1.0)==256);
    // Proactive raise: 2 % of packets 45 ms late -> reserve 46 + 5 = 51 ms with no underrun; small delays keep 20.
    AudioAdaptivePlayout a;a.reset(0,20);double t=0;
    for(int i=0;i<300;++i){a.arrivalDelay(i%3);CHECK(a.update(t)==20);t+=10;}
    for(int i=0;i<1000;++i){a.arrivalDelay(i%50==0?45.0:1.0);a.update(t);t+=10;}
    CHECK(std::abs(a.targetMs()-51)<1e-9&&a.source()==1);
    // Clean again: back to today's reserve, at most 2 ms/s, once the quantile forgets.
    double previous=a.targetMs();
    for(int i=0;i<12000;++i){a.arrivalDelay(1);const double v=a.update(t);CHECK(v<=previous+1e-12&&previous-v<=0.02+1e-9);previous=v;t+=10;}
    CHECK(a.targetMs()==20);
    AudioAdaptivePlayout::Config off;off.proactive=false;AudioAdaptivePlayout b(off);b.reset(0,20);
    for(int i=0;i<1000;++i){b.arrivalDelay(i%50==0?45.0:1.0);b.update(i*10.0);}
    CHECK(b.targetMs()==20);                                     // without it: only after an underrun
}

// ---- the review of round 3, kept as permanent tests (reviewer's adversarial cases) -------------------------
TEST(audio_pop_review_late_wake_tail_never_starts_from_silence){
    AudioEdgeSmoother s;const auto t=tone(0,4801);s.remember(t.data(),4801,2);
    CHECK(!s.due(300,480,480));                      // the previous pump queued a period: no tail then
    CHECK(!s.due(0,0,480));                          // engine ran dry before the late pump: no tail now
    // Tail written, next real data in time: the joint is near zero (a dip, not a click).
    AudioEdgeSmoother e;e.remember(t.data(),4801,2);std::vector<float> tail(192*2);e.writeTail(tail.data(),192,2);
    auto b=tone(4801,480);std::size_t rem=240;veyra::sink::fadePcmHead(b.data(),480,2,rem);
    CHECK(std::abs(tail[2*191])<0.01&&std::abs(b[0])<0.01);
    // 15 kHz mono: the mirror joint is within the signal's own largest step.
    AudioEdgeSmoother h;std::vector<float> v(1000);for(std::size_t i=0;i<1000;++i)v[i]=static_cast<float>(std::sin(2*3.141592653589793*15000*static_cast<double>(i)/48000));
    h.remember(v.data(),1000,1);std::vector<float> out(192);h.writeTail(out.data(),192,1);
    double natural=0;for(std::size_t i=1;i<1000;++i)natural=std::max(natural,static_cast<double>(std::abs(v[i]-v[i-1])));
    CHECK(std::abs(out[0]-v[999])<=natural+1e-6);
    // One remembered frame: its value held and faded; a channel change gives no tail.
    AudioEdgeSmoother one;float f[2]={0.9f,-0.9f};one.remember(f,1,2);std::vector<float> o(384);
    CHECK(one.writeTail(o.data(),192,2)==192&&std::abs(o[0]-0.9f)<0.01f&&std::abs(o[2*191])<0.01f);
    std::vector<float> six(192*6);CHECK(one.writeTail(six.data(),192,6)==0);
    // Time-based fade: 4 ms at 44.1 kHz and 96 kHz.
    AudioEdgeSmoother r;r.setFadeMs(4,44100);CHECK(r.config().fadeFrames==176);r.setFadeMs(4,96000);CHECK(r.config().fadeFrames==384);
    r.setFadeMs(std::nan(""),48000);CHECK(r.config().fadeFrames==384);
}

TEST(audio_pop_review_adaptive_ignores_pauses_and_unexplained_underruns){
    // A 3 s pause and one underrun: nothing (before the fix: 80 ms for 49 s).
    AudioAdaptivePlayout p;p.reset(0,20);p.arrivalGap(1000,3000);p.underrun(1001);CHECK(p.targetMs()==20);
    // 11 h of underruns every 9.9 s with only 12 ms gaps (a reserve could not have helped): stays at the base.
    AudioAdaptivePlayout q;q.reset(0,20);double t=0;
    for(int k=0;k<4000;++k){for(int j=0;j<990;++j){t+=10;q.arrivalGap(t,12);q.update(t);}q.underrun(t);}
    CHECK(q.targetMs()==20);
    // An underrun long after its gap is not explained by it.
    AudioAdaptivePlayout r;r.reset(0,20);r.arrivalGap(0,50);r.update(1500);r.underrun(1500);CHECK(r.targetMs()==20);
    // The usual order on Wi-Fi: the endpoint runs dry during the gap, the late packet (and its gap) comes after.
    AudioAdaptivePlayout late;late.reset(0,20);late.underrun(100);CHECK(late.targetMs()==20);late.arrivalGap(140,45);CHECK(late.targetMs()==50);
    late.arrivalGap(1400,60);CHECK(late.targetMs()==50);   // no underrun pending any more: no growth
    // Non-finite and negative inputs.
    AudioAdaptivePlayout n;n.reset(0,20);n.arrivalGap(1,std::nan(""));n.arrivalGap(2,-5);n.underrun(3);n.update(std::nan(""));n.underrun(std::nan(""));
    CHECK(std::isfinite(n.targetMs())&&n.targetMs()==20);
    veyra::sink::AudioMsHistogram hist;hist.add(std::nan(""));hist.add(1e9);hist.add(-3);CHECK(hist.count()==2&&hist.max()==1e9&&hist.quantile(.5)>=0);
}

TEST(audio_pop_review_product_drain_is_not_a_second_cause_in_the_model){
    // The product converts input until the converted queue holds the endpoint capacity + 10 ms, then pumps;
    // the model converted everything at once. Draining as the product does changes the Wi-Fi results little.
    for(auto rule:{m::Rule::Today,m::Rule::Fast}){
        auto c=popConfig(300,1000,20,60,false,false);const auto all=m::simulate(c,rule);
        c.productDrain=true;const auto product=m::simulate(c,rule);
        CHECK(std::abs(product.gaps-all.gaps)*10<=all.gaps+10);
        CHECK(std::abs(product.latMed-all.latMed)<2);CHECK(!product.futureAudio&&!product.nonFinite);
    }
}

TEST(audio_pop_review_long_episodes_no_ratchet_and_bounded){
    // The reviewer's harness, shortened to 2 h for the sanitizer builds (12 h: audio_sim --continuity and
    // the reviewer's own run): a 60 s Wi-Fi episode (12 held stalls of 20-80 ms) every 30 min, clean between.
    m::Config c;c.seconds=7200;c.seed=5;c.videoMs=15;c.adaptive=true;c.productDrain=true;
    unsigned s=7;
    for(double ep=600e3;ep<c.seconds*1000;ep+=1800e3)for(int k=0;k<12;++k){
        s=s*1103515245u+12345u;const double len=20+60*static_cast<double>((s>>8)%1000)/1000.0;
        c.events.push_back({m::Event::HeldStall,ep+k*2500,len});
    }
    const auto r=m::simulate(c,m::Rule::Today);
    CHECK(r.playoutMaxMs<=80&&!r.futureAudio&&!r.nonFinite);
    // The 3 minutes before each episode (clean for 25+ min): the same offset each time.
    std::vector<double> before;
    for(double ep=600e3+1800e3;ep<c.seconds*1000;ep+=1800e3)before.push_back(meanOffset(r,static_cast<std::size_t>(ep/1000)-180,static_cast<std::size_t>(ep/1000)));
    CHECK(before.size()>=3);
    for(const double v:before)CHECK(std::abs(v-before.front())<1.0);
}

// ---- review audio2 fix 1: a reserve raise (SmartLink's ask, ADAPTIVE) is not trouble (model) ----
TEST(audio_pop_model_reserve_raise_never_forces_a_reset){
    // The decision asks during "trouble" audio never sees (1 ms bursts, 2 min of every 10): 1 simulated hour.
    const auto askOnly=[](double ask,bool before){
        m::Config c;c.seconds=3600;c.videoMs=15;c.wifiEveryMs=1000;c.wifiMinMs=1;c.wifiMaxMs=1;c.wifiPeriodMs=600000;c.wifiOnMs=120000;
        c.smartlinkAskMs=ask;c.noRaiseAllowance=before;return c;};
    for(auto rule:{m::Rule::Today,m::Rule::Fast})for(double ask:{40.0,60.0,70.0,80.0}){
        const auto r=m::simulate(askOnly(ask,false),rule);
        CHECK(r.resets==0&&r.gaps==0&&std::abs(r.playoutMaxMs-ask)<1e-6);
    }
    // The reviewer's reproduction: before the fix an ask of 80 reset the audio on its own.
    CHECK(m::simulate(askOnly(80,true),m::Rule::Today).resets>0);
    CHECK(m::simulate(askOnly(80,true),m::Rule::Fast).resets>0);
    // With the reserve never changed the allowance is inert: identical with and without it.
    for(auto rule:{m::Rule::Today,m::Rule::Fast}){
        m::Config c;c.seconds=600;c.videoMs=15;c.wifiEveryMs=1000;c.wifiMinMs=20;c.wifiMaxMs=60;
        const auto a=m::simulate(c,rule);c.noRaiseAllowance=true;const auto b=m::simulate(c,rule);
        CHECK(a.resets==b.resets&&a.gaps==b.gaps&&a.clicks==b.clicks&&a.latMed==b.latMed&&a.latP95==b.latP95);
    }
}
TEST(audio_pop_reserve_change_allowance_keeps_real_trouble){
    veyra::sink::ReserveChangeAllowance a;a.anchor(0);
    CHECK(a.update(0,70)==0);                       // no change: the whole error counts (original test)
    CHECK(a.update(60,62)==60);                     // raised 60: the jump is planned
    CHECK(a.update(60,40)==40);                     // the trim works it off: the allowance follows down
    CHECK(a.update(60,140)==40);                    // never grows with the error: 100 ms of real trouble remain
    CHECK(a.update(0,-20)==-20);                    // a release is planned too, both ways
    a.anchor(80);CHECK(a.update(80,65)==0);         // a re-anchor starts from the schedule as it is
    CHECK(a.update(std::nan(""),5)==0);
}
// Review audio2 (adv2.cpp): the reserve stays within [base, 80] for any ask, releases at 2 ms/s,
// two controllers take the maximum, and 12-15 ms gaps never grow it.
TEST(audio_pop_adaptive_playout_bounds_release_and_maximum){
    const auto run=[](AudioAdaptivePlayout& p,double& t,double until,double ask){double mx=0;for(;t<until;t+=2){p.setBase(20);p.request(ask);mx=std::max(mx,p.update(t));}return mx;};
    for(double ask:{0.0,19.0,21.0,80.0,81.0,10000.0,-5.0,std::nan(""),static_cast<double>(INFINITY)}){
        AudioAdaptivePlayout p;p.reset(0,20);double t=0;const double mx=run(p,t,5000,ask);
        CHECK(mx>=20-1e-9&&mx<=80+1e-9);
    }
    {AudioAdaptivePlayout r;r.reset(0,20);double t=0;run(r,t,1000,80);const double from=t;
     while(r.targetMs()>20+1e-6&&t<from+120000){r.setBase(20);r.request(0);r.update(t);t+=2;}
     CHECK(std::abs((t-from)/1000-30)<0.5);}
    {AudioAdaptivePlayout p;p.reset(0,20);double t=0;
     for(int i=0;i<10;++i){t+=500;p.arrivalGap(t,75);p.underrun(t+1);p.request(80);p.update(t+2);}
     CHECK(p.targetMs()<=80+1e-9);}
    {AudioAdaptivePlayout p;p.reset(0,20);double t=0;for(int k=0;k<600;++k){t+=1000;p.arrivalGap(t,14);p.underrun(t+5);p.update(t+6);}
     CHECK(std::abs(p.targetMs()-20)<1e-9);}
}
