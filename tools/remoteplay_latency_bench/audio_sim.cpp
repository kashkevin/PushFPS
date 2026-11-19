// SPDX-License-Identifier: GPL-3.0-or-later
// Linux model: the PS5 Remote Play audio path under made-up clocks, networks and device events.
//
//   audio_sim [--quick] > audio_sim_results.json
//
// Runs every scenario of the matrix (include/veyra/sink/AudioPathModel.h) with today's rule (switches
// unset), VEYRA_EXP_RP_AUDIO_FAST_RECOVER ("fast") and fast + VEYRA_EXP_RP_AUDIO_LOW_BUFFER ("low"), and
// prints one JSON object per scenario and rule. --quick shortens the 2 h / 6 h sessions to 20 min (for a
// smoke run; the committed file is the full run). Single-threaded, deterministic: the same build prints
// the same bytes.
//
// Everything about the console, the network, the PC and the headset is an assumption written into the
// scenario names: no PS5, no network and no Windows machine was involved. The numbers compare the rules
// on the same model, nothing more; they are not latencies anyone measured.
#include "veyra/sink/AudioPathModel.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace m=veyra::sink::audio_model;

namespace {
struct Scenario {std::string name;m::Config config;bool low=true;};

std::vector<m::Event> every(m::Event::Kind kind,double fromMs,double toMs,double stepMs,double lenMs){
    std::vector<m::Event> out;for(double t=fromMs;t<toMs;t+=stepMs){m::Event e;e.kind=kind;e.atMs=t;e.lenMs=lenMs;out.push_back(e);}return out;
}

std::vector<Scenario> matrix(bool quick){
    std::vector<Scenario> s;
    auto add=[&](std::string name,double seconds,auto&& edit,bool low=true){m::Config c;c.seconds=seconds;edit(c);s.push_back({std::move(name),c,low});};
    const double longS=quick?1200:7200,veryLongS=quick?1200:21600,hourS=quick?1200:3600;
    add("steady, video 35 ms",120,[](m::Config&){});
    add("steady, video 15 ms",120,[](m::Config& c){c.videoMs=15;});
    add("steady, video 10 ms (below the 20 ms reserve)",120,[](m::Config& c){c.videoMs=10;});
    add("steady, frame generation on (+17 ms, 120 presentations/s)",120,[](m::Config& c){c.fgExtraMs=17;});
    for(double ppm:{50.0,-50.0,200.0,-200.0,500.0,-500.0}){
        char name[96];std::snprintf(name,sizeof name,"console clock %+.0f ppm (wander %.0f), 2 h",ppm,std::abs(ppm)/5);
        add(name,longS,[ppm](m::Config& c){c.sourcePpm=ppm;c.wanderPpm=std::abs(ppm)/5;c.wanderPeriodS=300;},false);
    }
    for(double ppm:{200.0,-500.0}){
        char name[96];std::snprintf(name,sizeof name,"console clock %+.0f ppm (wander %.0f), 6 h",ppm,std::abs(ppm)/5);
        add(name,veryLongS,[ppm](m::Config& c){c.sourcePpm=ppm;c.wanderPpm=std::abs(ppm)/5;c.wanderPeriodS=300;},false);
    }
    add("device clock +300 ppm",600,[](m::Config& c){c.devices[0].clockPpm=300;});
    add("device clock -300 ppm",600,[](m::Config& c){c.devices[0].clockPpm=-300;});
    add("Bluetooth-like device, 150 ms after its clock",120,[](m::Config& c){c.devices[0].hiddenMs=150;});
    for(double len:{100.0,200.0,500.0,1000.0,5000.0}){
        char name[96];std::snprintf(name,sizeof name,"loss stall %.0f ms every 30 s, 1 h",len);
        add(name,hourS,[len,hourS](m::Config& c){c.signal=m::Signal::Speech;c.events=every(m::Event::LossStall,10000,hourS*1000-10000,30000,len);},false);
    }
    for(double len:{25.0,60.0,200.0,1000.0,5000.0}){
        char name[96];std::snprintf(name,sizeof name,"held stall %.0f ms every 30 s, 1 h",len);
        add(name,hourS,[len,hourS](m::Config& c){c.signal=m::Signal::Speech;c.events=every(m::Event::HeldStall,10000,hourS*1000-10000,30000,len);},false);
    }
    add("random loss 1 %",300,[](m::Config& c){c.randomLoss=0.01;});
    add("random loss 5 %",300,[](m::Config& c){c.randomLoss=0.05;});
    add("loss stall 100 ms every 3 s, 10 min",600,[](m::Config& c){c.signal=m::Signal::Speech;c.events=every(m::Event::LossStall,10000,590000,3000,100);});
    add("reconnect 3 s",120,[](m::Config& c){c.events={{m::Event::Reconnect,20000,3000}};});
    add("reconnect, loss stall 1 s later",120,[](m::Config& c){c.events={{m::Event::Reconnect,5000,100},{m::Event::LossStall,6000,200}};});
    m::Device usb;usb.clockPpm=60;usb.hiddenMs=1;
    m::Device resampled=usb;resampled.lowLatencyCapable=false;resampled.clockPpm=-90;
    auto devices=[usb,resampled](m::Config& c,std::vector<m::Event> events){c.devices={m::Device{},usb,resampled};c.signal=m::Signal::Tone;c.events=std::move(events);};
    add("default device switch (tone)",60,[&](m::Config& c){devices(c,{{m::Event::DeviceSwitch,20000,0,1}});});
    add("unplug 4 s, replug (tone)",60,[&](m::Config& c){devices(c,{{m::Event::Unplug,20000,4000,1}});});
    add("format change to 44.1 kHz, no low-latency period (tone)",60,[&](m::Config& c){devices(c,{{m::Event::DeviceSwitch,20000,0,2}});});
    add("channel count change (reopen, tone)",60,[&](m::Config& c){devices(c,{{m::Event::DeviceSwitch,20000,0,0}});});
    add("device switch mid-cut, after a clock stall (tone)",60,[&](m::Config& c){devices(c,{{m::Event::EndpointStall,20000,200},{m::Event::DeviceSwitch,20260,0,1}});});
    add("device switch mid-ratio, FG on (tone)",60,[&](m::Config& c){devices(c,{{m::Event::VideoDelay,20000,0,0,17},{m::Event::DeviceSwitch,21000,0,2}});});
    add("device switch during a 1 s network stall (tone)",60,[&](m::Config& c){devices(c,{{m::Event::LossStall,20000,1000},{m::Event::DeviceSwitch,20500,0,1}});});
    add("unplug during a held 2 s stall (tone)",60,[&](m::Config& c){devices(c,{{m::Event::HeldStall,20000,2000},{m::Event::Unplug,20300,1500,2}});});
    add("device clock stall 300 ms (tone)",60,[&](m::Config& c){devices(c,{{m::Event::EndpointStall,20000,300}});});
    add("device clock stall 100 ms every 30 s, 1 h (tone)",hourS,[&](m::Config& c){devices(c,every(m::Event::EndpointStall,10000,hourS*1000-10000,30000,100));});
    add("device clock stall 100 ms every 3 s, 20 min (tone)",1200,[&](m::Config& c){devices(c,every(m::Event::EndpointStall,10000,1190000,3000,100));});
    add("audio thread stalls 25 ms every 200 ms for 10 s",120,[](m::Config& c){c.events=every(m::Event::ThreadStall,20000,30000,200,25);});
    add("audio thread stalls 8 ms every 50 ms for 10 s",120,[](m::Config& c){c.events=every(m::Event::ThreadStall,20000,30000,50,8);});
    for(auto signal:{m::Signal::Tone,m::Signal::Speech}){
        add(signal==m::Signal::Tone?"FG toggled every minute, 1 h (tone)":"FG toggled every minute, 1 h (speech)",hourS,[signal,hourS](m::Config& c){
            c.signal=signal;for(double t=60000;t<hourS*1000-10000;t+=60000)c.events.push_back({m::Event::VideoDelay,t,0,0,static_cast<int>(t/60000)%2?17.0:0.0});});
    }
    add("everything: +200 ppm, 0.5 % loss, stalls, FG, device events, 1 h",hourS,[&](m::Config& c){
        c.sourcePpm=200;c.wanderPpm=40;c.randomLoss=0.005;c.devices={m::Device{},usb,resampled};c.signal=m::Signal::Speech;
        for(double t=15000;t<hourS*1000-20000;t+=60000){
            c.events.push_back({m::Event::LossStall,t,500});c.events.push_back({m::Event::HeldStall,t+15000,60});
            c.events.push_back({m::Event::VideoDelay,t+30000,0,0,static_cast<int>(t/60000)%2?17.0:0.0});
            c.events.push_back({m::Event::DeviceSwitch,t+45000,0,static_cast<int>(t/60000)%3});
        }});
    return s;
}

void print(const char* sep,const std::string& name,const char* rule,const m::Result& r){
    std::printf("%s{\"scenario\":\"%s\",\"rule\":\"%s\",\"av_ms\":[%.1f,%.1f,%.1f],\"heard_av_median_ms\":%.1f,\"latency_ms\":[%.1f,%.1f,%.1f],"
        "\"silence_ms\":%.0f,\"resets\":%d,\"underruns\":%d,\"cut_ms\":%.0f,\"cuts\":{\"loud\":%d,\"quiet\":%d,\"after_gap\":%d},\"max_cut_ms\":%.1f,"
        "\"max_loud_cut_ms_per_s\":%.1f,\"discarded_ms\":%.0f,\"max_pitch_ppm\":%.0f,\"ms_above_1000_ppm\":%.0f,\"recover_max_ms\":%.0f,\"unrecovered\":%d,"
        "\"signed_median_first_last_quarter_ms\":[%.2f,%.2f],\"last_hour_p2p_ms\":%.2f,\"future_audio\":%s,\"non_finite\":%s}",
        sep,name.c_str(),rule,r.avMed,r.avP95,r.avMax,r.earMed,r.latMed,r.latP95,r.latMax,r.silenceMs,r.resets,r.underruns,r.cutMs,r.loudCuts,r.quietCuts,r.gapCuts,r.maxCutMs,
        r.maxLoudCutPerSecondMs,r.discardedMs,r.maxPitchPpm,r.msAbove1000Ppm,r.recoverMaxMs,r.unrecovered,r.firstQuarterMedSigned,r.lastQuarterMedSigned,r.lastHourP2P,
        r.futureAudio?"true":"false",r.nonFinite?"true":"false");
}
}

int main(int argc,char** argv){
    bool quick=false,fuzz=false;
    for(int i=1;i<argc;++i){if(!std::strcmp(argv[i],"--quick"))quick=true;else if(!std::strcmp(argv[i],"--fuzz"))fuzz=true;else{std::fprintf(stderr,"usage: audio_sim [--quick] [--fuzz]\n");return 2;}}
    std::printf("{\"model\":\"LINUX MODEL of the PS5 Remote Play audio path (include/veyra/sink/AudioPathModel.h); not a measurement\",\n");
    std::printf("\"units\":\"ms; av = |play - (arrival + video delay)| median/P95/max after 3 s; latency = play - lower-envelope arrival\",\n\"runs\":[\n");
    const char* sep="";
    if(fuzz){
        // Costs next to the A/V gain: every mix holds at least one stall of 100 ms or more, so "fast has the
        // lower P95" is mostly one mechanism (finding 11), not independent wins.
        int lowerP95=0,moreSilence=0,moreUnderruns=0;double maxMaskedCut=0;
        for(std::uint64_t seed=1;seed<=100;++seed){
            const auto c=m::randomMix(seed,quick?120:600);const std::string name="random mix seed "+std::to_string(seed);
            const auto today=m::simulate(c,m::Rule::Today),fast=m::simulate(c,m::Rule::Fast);
            print(sep,name,"today",today);sep=",\n";
            print(sep,name,"fast",fast);print(sep,name,"low",m::simulate(c,m::Rule::FastLowBuffer));
            lowerP95+=fast.avP95<today.avP95?1:0;moreSilence+=fast.silenceMs>today.silenceMs?1:0;
            moreUnderruns+=fast.underruns>today.underruns?1:0;maxMaskedCut=std::max(maxMaskedCut,fast.maxCutMs);
        }
        std::printf("%s{\"summary\":\"fast vs today over 100 mixes\",\"fast_lower_p95\":%d,\"fast_more_silence\":%d,\"fast_more_underruns\":%d,\"fast_max_single_cut_ms\":%.0f}",sep,lowerP95,moreSilence,moreUnderruns,maxMaskedCut);
    }else for(const auto& s:matrix(quick)){
        print(sep,s.name,"today",m::simulate(s.config,m::Rule::Today));sep=",\n";
        print(sep,s.name,"fast",m::simulate(s.config,m::Rule::Fast));
        if(s.low)print(sep,s.name,"low",m::simulate(s.config,m::Rule::FastLowBuffer));
        std::fflush(stdout);
    }
    std::printf("\n]}\n");
    return 0;
}
