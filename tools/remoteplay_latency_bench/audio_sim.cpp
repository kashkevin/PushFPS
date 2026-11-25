// SPDX-License-Identifier: GPL-3.0-or-later
// Linux model: the PS5 Remote Play audio path under made-up clocks, networks and device events.
//
//   audio_sim [--quick] > audio_sim_results.json
//   audio_sim --pops > audio_pop_results.json     (round 3: Wi-Fi-like arrival, VEYRA_EXP_RP_AUDIO_SMOOTH/ADAPTIVE)
//   audio_sim --continuity > audio_continuity_results.json   (round 4: 12 h of repeated disturbances, SmartLink)
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

// Round 3: today's rule with and without VEYRA_EXP_RP_AUDIO_SMOOTH / _ADAPTIVE under wired-like and Wi-Fi-like
// arrival (bursts with nothing delivered, then a clump). Per minute after warm-up; latency = play - lower-
// envelope arrival (the delay audio adds), av = |play - (arrival + video delay)|. Video delay 15 ms (the
// first session's in-app counters were about 10 ms arrival->Present; the rest is a guess), music-like sound.
int pops(){
    struct Net {const char* name;double every,minMs,maxMs,jitter;};
    const Net nets[]={{"wired-like (jitter 1.5 ms, no bursts)",0,0,0,1.5},{"wired-like, jitter 4 ms",0,0,0,4},
        {"Wi-Fi light (20-40 ms bursts, every 2 s)",2000,20,40,1.5},{"Wi-Fi typical (20-60 ms bursts, every 1 s)",1000,20,60,1.5},
        {"Wi-Fi heavy (20-80 ms bursts, every 0.5 s)",500,20,80,1.5}};
    const char* names[]={"today","smooth","adaptive","smooth+adaptive"};
    std::printf("{\"model\":\"LINUX MODEL of the PS5 Remote Play audio path (include/veyra/sink/AudioPathModel.h), round 3; not a measurement\",\n");
    std::printf("\"units\":\"per minute after 3 s; clicks = underruns that start right after loud real sound (no fade-out); latency = play - lower-envelope arrival (ms, P50/P95); av = |A/V| P95 ms; video delay 15 ms; 600 s, seeds 1-3 summed\",\n\"runs\":[\n");
    const char* sep="";
    for(const auto& n:nets)for(int mode=0;mode<4;++mode){
        double clicks=0,gaps=0,resets=0,tails=0,silence=0,minutes=0,latP50=0,latP95=0,avP95=0,playMax=0;bool future=false;
        for(std::uint64_t seed=1;seed<=3;++seed){
            m::Config c;c.seconds=600;c.seed=seed;c.videoMs=15;c.netJitterMs=n.jitter;c.wifiEveryMs=n.every;c.wifiMinMs=n.minMs;c.wifiMaxMs=n.maxMs;
            c.smooth=(mode&1)!=0;c.adaptive=(mode&2)!=0;
            const auto r=m::simulate(c,m::Rule::Today);
            clicks+=r.clicks;gaps+=r.gaps;resets+=r.resets;tails+=r.tails;silence+=r.silenceMs;minutes+=(c.seconds*1000-c.warmupMs)/60000;
            latP50+=r.latMed/3;latP95+=r.latP95/3;avP95+=r.avP95/3;playMax=std::max(playMax,r.playoutMaxMs);future=future||r.futureAudio;
        }
        std::printf("%s{\"network\":\"%s\",\"rule\":\"%s\",\"clicks_per_min\":%.2f,\"underruns_per_min\":%.2f,\"resets_per_min\":%.2f,\"tails_per_min\":%.2f,\"silence_ms_per_min\":%.0f,"
            "\"latency_ms\":[%.1f,%.1f],\"av_p95_ms\":%.1f,\"playout_max_ms\":%.0f,\"future_audio\":%s}",
            sep,n.name,names[mode],clicks/minutes,gaps/minutes,resets/minutes,tails/minutes,silence/minutes,latP50,latP95,avP95,playMax,future?"true":"false");
        sep=",\n";std::fflush(stdout);
    }
    std::printf("\n]}\n");
    return 0;
}

// Round 4 (SmartLink phase 2): 12 simulated hours, 2 minutes of Wi-Fi-like bursts (20-60 ms, every 1 s) at the
// start of every 10 minutes, with and without a +100 ppm console clock. Per cycle: seconds after the bursts end
// until the 10 s mean A/V offset is within 1.5 ms of the same rule's clean-link floor ("return"), and that
// offset over the last 5 minutes of the cycle ("end"); first and last cycle show any ratchet.
int continuity(){
    struct Arm {const char* name;m::Rule rule;bool smooth,adaptive;double ask;};
    const Arm arms[]={{"today",m::Rule::Today,false,false,0},{"fast",m::Rule::Fast,false,false,0},
        {"fast+smooth+adaptive",m::Rule::Fast,true,true,0},{"fast+smooth+adaptive+smartlink(60 ms)",m::Rule::Fast,true,true,60}};
    std::printf("{\"model\":\"LINUX MODEL of the PS5 Remote Play audio path (include/veyra/sink/AudioPathModel.h), round 4; not a measurement\",\n");
    std::printf("\"units\":\"12 h; bursts 20-60 ms every 1 s for 2 min of every 10 min; video delay 15 ms; return_s = seconds after the bursts end until the 10 s mean signed A/V offset is within 1.5 ms of the clean-link floor (-1 = not within the cycle); end_ms = mean offset over the cycle's last 5 min minus the floor\",\n\"runs\":[\n");
    const char* sep="";
    for(double ppm:{0.0,100.0})for(const auto& a:arms){
        m::Config c;c.seconds=43200;c.videoMs=15;c.sourcePpm=ppm;c.smooth=a.smooth;c.adaptive=a.adaptive;
        m::Config clean=c;
        c.wifiEveryMs=1000;c.wifiMinMs=20;c.wifiMaxMs=60;c.wifiPeriodMs=600000;c.wifiOnMs=120000;c.smartlinkAskMs=a.ask;
        const auto r=m::simulate(c,a.rule),f=m::simulate(clean,a.rule);
        auto mean=[](const m::Result& x,std::size_t from,std::size_t to){double sum=0;int n=0;for(std::size_t i=from;i<to&&i<x.perSecondSigned.size();++i)if(std::isfinite(x.perSecondSigned[i])){sum+=x.perSecondSigned[i];++n;}return n?sum/n:std::nan("");};
        double worstReturn=0;int never=0;double firstEnd=0,lastEnd=0,maxEnd=-1e9;
        for(std::size_t cycle=0;cycle<72;++cycle){
            const std::size_t start=cycle*600,burstEnd=start+120;
            int ret=-1;
            for(std::size_t s2=burstEnd;s2+10<=start+600;++s2){const double floorNow=mean(f,s2,s2+10);if(std::abs(mean(r,s2,s2+10)-floorNow)<1.5){ret=static_cast<int>(s2-burstEnd);break;}}
            if(ret<0)++never;else worstReturn=std::max(worstReturn,static_cast<double>(ret));
            const double end=mean(r,start+300,start+600)-mean(f,start+300,start+600);
            if(cycle==0){firstEnd=end;}
            if(cycle==71){lastEnd=end;}
            maxEnd=std::max(maxEnd,end);
        }
        const double minutes=(c.seconds*1000-c.warmupMs)/60000;
        std::printf("%s{\"rule\":\"%s\",\"console_ppm\":%.0f,\"return_s_worst\":%.0f,\"cycles_not_returned\":%d,\"end_ms_first_last_max\":[%.2f,%.2f,%.2f],"
            "\"clicks_per_min\":%.2f,\"underruns_per_min\":%.2f,\"resets\":%d,\"latency_ms\":[%.1f,%.1f],\"clean_floor_latency_ms\":%.1f,\"reserve_max_ms\":%.0f,\"future_audio\":%s,\"non_finite\":%s}",
            sep,a.name,ppm,worstReturn,never,firstEnd,lastEnd,maxEnd,r.clicks/minutes,r.gaps/minutes,r.resets,r.latMed,r.latP95,f.latMed,r.playoutMaxMs,r.futureAudio?"true":"false",r.nonFinite?"true":"false");
        sep=",\n";std::fflush(stdout);
    }
    std::printf("\n]}\n");
    return 0;
}

int main(int argc,char** argv){
    bool quick=false,fuzz=false;
    for(int i=1;i<argc;++i){if(!std::strcmp(argv[i],"--quick"))quick=true;else if(!std::strcmp(argv[i],"--fuzz"))fuzz=true;else if(!std::strcmp(argv[i],"--pops"))return pops();else if(!std::strcmp(argv[i],"--continuity"))return continuity();else{std::fprintf(stderr,"usage: audio_sim [--quick] [--fuzz] [--pops] [--continuity]\n");return 2;}}
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
