// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Remote Play audio playout (include/veyra/sink/AudioLatencyController.h), included by CoreTests.cpp.
// The last test is a LINUX MODEL of the whole audio path (console clock, network, Chiaki concealment,
// AudioIngress, feeder, playout queue, a WASAPI-like endpoint) run with today's rule and with the new
// one. Its numbers are a model of the code as read, not a measurement of PushFPS or a PS5.
#include "veyra/sink/AudioLatencyController.h"
#include "veyra/sink/ArrivalClockMapping.h"
#include "veyra/sink/CaptureAudioDsp.h"   // the original correction rule, modelled below
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
namespace audio_sync_tests {
using namespace veyra::sink;
AudioLatencyController::Input at(double now,std::optional<double> error,double queued=60,bool gap=false){
    AudioLatencyController::Input in;in.nowMs=now;in.errorMs=error;in.queuedMs=queued;in.reserveMs=20;in.gap=gap;return in;
}
}
using namespace audio_sync_tests;

TEST(audio_video_delay_filter_takes_the_median_and_forgets_old_frames){
    AudioVideoDelayFilter f;CHECK(!f.value(0));
    const double values[]={30,31,90,29,30};double t=0;
    for(double v:values){f.observe(t,v);t+=16.7;}
    CHECK(f.value(t)&&*f.value(t)==30);           // one outlier does not move it
    CHECK(!f.value(t+AudioVideoDelayFilter::kMaxAgeMs+100));
    for(int i=0;i<9;++i){f.observe(t,45);t+=8.3;} // frame generation turned on: follows within 9 frames
    CHECK(*f.value(t)==45);
}

TEST(audio_arrival_drift_follows_a_clock_difference_without_jumps){
    AudioArrivalDrift d;
    // Console audio 200 ppm fast: PTS run ahead, arrival minus PTS falls 0.2 ms per second.
    double previous=0,maxStep=0;
    for(int ms=0;ms<120000;ms+=10){
        const double ingress=5-0.0002*ms;
        const double offset=d.observe(ms,ingress);
        maxStep=std::max(maxStep,std::abs(offset-previous));previous=offset;
        if(ms<2000)CHECK(offset==0);
    }
    CHECK(std::abs(previous-(-0.0002*(120000-1990)))<0.5);
    CHECK(d.driftPpm()&&std::abs(*d.driftPpm()-200)<20);
    CHECK(maxStep<=AudioArrivalDrift::kMaxSlewMsPerSecond*10/1000+1e-9);
    // A sudden 30 ms step in the 2 s minimum is followed at 2 ms/s, not at once.
    AudioArrivalDrift s;for(int ms=0;ms<=3000;ms+=10)s.observe(ms,5);
    const double before=s.offsetMs();s.observe(3010,35);
    CHECK(s.offsetMs()-before<=0.021);
    // A discontinuity re-anchors 100 ms late: followed at once, then slewed again.
    s.rebase(3020);s.observe(3030,-95);CHECK(s.offsetMs()==-100);
    s.observe(3300,-90);CHECK(std::abs(s.offsetMs()-(-100+0.54))<0.01);
    d.reset();CHECK(d.offsetMs()==0&&!d.driftPpm());
}

TEST(audio_latency_controller_small_errors_use_only_a_bounded_ratio){
    AudioLatencyController c;c.reset(0);
    for(int i=1;i<=500;++i){
        const auto r=c.update(at(i*10.0,(i%2?1.5:-1.5)));
        CHECK(r.dropMs==0&&!r.reset);CHECK(std::abs(r.ratioPpm)<=c.config().maxRatioPpm);
    }
    // Steady 30 ms early: slows down, at most the configured ratio, no reset below the threshold.
    for(int i=0;i<200;++i){const auto r=c.update(at(5000+i*10.0,30));CHECK(!r.reset&&r.dropMs==0);}
    CHECK(c.ratioPpm()==c.config().maxRatioPpm);
}

TEST(audio_latency_controller_drops_the_whole_excess_right_after_a_gap){
    AudioLatencyController c;c.reset(0);
    c.update(at(10,0.0));
    const auto r=c.update(at(20,-35.0,80,true));
    CHECK(std::abs(r.loudDropMs-35)<1e-9&&std::abs(r.dropMs-35)<1e-9);
    c.dropped(20,r.dropMs,false,true);
    CHECK(c.stats().gapDrops==1&&std::abs(c.filteredErrorMs())<1e-9&&std::abs(c.pendingMs()-35)<1e-9);
    // Never below the reserve: 30 ms queued with a 20 ms reserve gives at most 10 ms.
    AudioLatencyController d;d.reset(0);
    const auto limited=d.update(at(10,-35.0,30,true));
    CHECK(std::abs(limited.dropMs-10)<1e-9);
    const auto none=d.update(at(20,-35.0,20,false));
    CHECK(none.dropMs==0&&none.ratioPpm>=0);
}

TEST(audio_latency_controller_cuts_into_sound_only_when_the_excess_persists){
    AudioLatencyController c;c.reset(0);
    auto r=c.update(at(10,-10.0));
    CHECK(r.dropMs>0&&r.loudDropMs==0);          // 10 ms late: quiet windows only
    for(double t=20;t<=140;t+=10){r=c.update(at(t,-30.0));CHECK(r.loudDropMs==0);}
    r=c.update(at(170,-30.0));CHECK(r.loudDropMs==c.config().loudDropMs&&r.dropMs==c.config().maxQuietDropMs);
    c.dropped(170,r.loudDropMs,false,false);
    r=c.update(at(180,-25.0));CHECK(r.loudDropMs==0);   // spacing between cuts
    r=c.update(at(280,-25.0));CHECK(r.loudDropMs==c.config().loudDropMs);
    CHECK(c.stats().loudDrops==1);
}

TEST(audio_latency_controller_resets_only_far_late_or_early_and_spaced){
    AudioLatencyController c;c.reset(-5000);
    auto r=c.update(at(10,-200.0));CHECK(!r.reset);   // the original resets above 60 ms; drops handle this
    r=c.update(at(20,-450.0));CHECK(!r.reset);        // smoothing
    for(double t=30;t<200&&!r.reset;t+=10)r=c.update(at(t,-450.0));
    CHECK(r.reset&&c.stats().resets==1);
    r=c.update(at(300,-450.0));CHECK(!r.reset);       // spaced one second
    AudioLatencyController e;e.reset(-5000);
    for(double t=10;t<200&&!r.reset;t+=10)r=e.update(at(t,80.0));
    CHECK(r.reset);                                   // early: silence must be inserted, as before
}

TEST(audio_latency_controller_holds_without_a_measurement){
    AudioLatencyController c;c.reset(0);
    for(double t=10;t<1000;t+=10)c.update(at(t,-4.0));
    const double ratio=c.ratioPpm();CHECK(ratio<0);
    const auto r=c.update(at(1010,std::nullopt));
    CHECK(r.ratioPpm==ratio&&r.dropMs==0&&!r.reset);
}

TEST(audio_crossfade_splice_has_no_step_and_rms_finds_quiet){
    std::vector<float> s(2*100);
    for(std::size_t i=0;i<100;++i){s[2*i]=0.5f;s[2*i+1]=-0.25f;}
    CHECK(audioCrossfadeSplice(s.data(),100,2,40,20));
    for(std::size_t i=40;i<60;++i)CHECK(std::abs(s[2*i]-0.5f)<1e-6f&&std::abs(s[2*i+1]+0.25f)<1e-6f);
    // A ramp: the first frame after the cut continues from the head, the last one reaches the far side.
    std::vector<float> r(60);for(std::size_t i=0;i<60;++i)r[i]=static_cast<float>(i);
    CHECK(audioCrossfadeSplice(r.data(),60,1,30,10));
    CHECK(std::abs(r[30]-(0.0f*(1-1.0f/11)+30.0f/11))<1e-4f);
    CHECK(r[39]>36.0f&&r[39]<39.0f);
    CHECK(!audioCrossfadeSplice(r.data(),60,1,55,10));
    std::vector<float> quiet(96,0.001f),loud(96,0.3f);
    CHECK(audioWindowRms(quiet.data(),48,2)<AudioLatencyConfig{}.quietRms);
    CHECK(audioWindowRms(loud.data(),48,2)>AudioLatencyConfig{}.quietRms);
    CHECK(audioWindowRms(nullptr,0,2)==0);
}

namespace audio_sync_tests {
// A cut at the queue head is heard only after the endpoint's padding has played. Starts `excess` ms
// late, measures what is playing (the queue head as it was `endpointMs` earlier), applies every cut
// at once to the head and the ratio per period. Returns the earliest lateness heard (negative = early).
double earliestHeard(double excess,double endpointMs,double periodMs,bool gapFirst){
    AudioLatencyController c;c.reset(0);
    std::vector<double> head;double lateness=excess,earliest=excess;
    for(double t=0;t<3000;t+=1){
        head.push_back(lateness);
        const auto heardIndex=static_cast<std::size_t>(std::max(0.0,t-endpointMs));
        const double heard=head[heardIndex];earliest=std::min(earliest,heard);
        if(std::fmod(t,periodMs)>=1)continue;
        AudioLatencyController::Input in;in.nowMs=t;in.errorMs=-heard;in.queuedMs=100;in.endpointMs=endpointMs;in.reserveMs=20;in.gap=gapFirst&&t==0;
        const auto d=c.update(in);
        if(d.dropMs>0){lateness-=d.dropMs;c.dropped(t,d.dropMs,true,c.afterGap(t));}  // quiet content: every cut allowed
        lateness+=d.ratioPpm*1e-6*periodMs;
    }
    return std::min(earliest,head.back());
}
}
TEST(audio_latency_controller_never_ends_early_after_cuts){
    // The three cases the review found (30 ms excess): endpoint 20 / period 10, 10 / 2.7, 20 / 2.
    for(bool gap:{false,true}){
        CHECK(earliestHeard(30,20,10,gap)>-2.0);
        CHECK(earliestHeard(30,10,2.7,gap)>-2.0);
        CHECK(earliestHeard(30,20,2,gap)>-2.0);
        CHECK(earliestHeard(30,20,10,gap)<3.0);   // and the excess is still removed
    }
}

TEST(audio_ratio_arming_sets_again_before_swresample_ends_it){
    AudioRatioArming a;
    CHECK(a.due(0,-10));CHECK(!a.due(100,-10));CHECK(a.due(250,-10));   // re-armed every 250 ms
    CHECK(a.due(260,-12));CHECK(a.due(270,0));CHECK(!a.due(5000,0));    // zero is set once
    a.reset();CHECK(a.due(0,0));CHECK(!a.due(1,0));CHECK(a.due(2,3));
}

TEST(audio_crossfade_splice_shorter_than_its_fade_reads_original_samples){
    const std::size_t frames=400,drop=96,fade=120;
    std::vector<float> s(frames),ref(frames);
    for(std::size_t i=0;i<frames;++i)s[i]=ref[i]=static_cast<float>(i%37)-18.0f;
    CHECK(audioCrossfadeSplice(s.data(),frames,1,drop,fade));
    for(std::size_t i=0;i<fade;++i){
        const float w=static_cast<float>(static_cast<double>(i+1)/static_cast<double>(fade+1));
        CHECK(std::abs(s[drop+i]-(ref[i]*(1.0f-w)+ref[drop+i]*w))<1e-4f);
    }
}

namespace audio_sync_tests {
// ---- Linux model --------------------------------------------------------------------------------
struct Rng {std::uint64_t s=0x9E3779B97F4A7C15ull;double next(){s^=s<<13;s^=s>>7;s^=s<<17;return static_cast<double>(s>>11)*(1.0/9007199254740992.0);}};
struct NetEvent {enum Kind {Loss,Hold,Device} kind;double at,len;};
struct Scenario {const char* name;double seconds=120,driftPpm=0,videoMs=35;std::vector<NetEvent> events;};
struct Policy {bool fresh=false,lowBuffer=false;};
struct Result {double latMed=0,latP95=0,latMax=0,avMed=0,avP95=0,avMax=0,silenceMs=0,droppedMs=0;int resets=0;};
struct Unit {double pts,ideal,len;bool loud;};   // one source millisecond (len = output ms)
struct Block {double push,pts,ideal;bool loud[10];bool disc;};
double pct(std::vector<double> v,double p){if(v.empty())return 0;std::sort(v.begin(),v.end());return v[static_cast<std::size_t>(p*static_cast<double>(v.size()-1))];}

// Console frames (10 ms), network, Chiaki concealment, AudioIngress (100 ms), feeder and PTS anchoring.
std::vector<Block> arrivals(const Scenario& sc,Rng& rng){
    struct Frame {double prod,arrive;bool lost,plc;bool loud[10];};
    std::vector<Frame> frames;bool loud=true;double runLeft=0;
    const double period=10.0/(1.0+sc.driftPpm*1e-6);
    for(double t=0;t<sc.seconds*1000;t+=period){
        Frame f{t,t+2+1.5*rng.next(),false,false,{}};
        for(bool& l:f.loud){if(runLeft<=0){loud=!loud;runLeft=loud?-400*std::log(1-rng.next()):-80*std::log(1-rng.next());}l=loud;runLeft-=1;}
        for(const auto& e:sc.events){
            if(e.kind==NetEvent::Loss&&f.prod>=e.at&&f.prod<e.at+e.len)f.lost=true;
            if(e.kind==NetEvent::Hold&&f.arrive>=e.at&&f.arrive<e.at+e.len)f.arrive=e.at+e.len+0.1*rng.next();
        }
        frames.push_back(f);
    }
    // Chiaki: a lost frame is concealed when the next frames arrive (one burst, in order).
    for(std::size_t i=0;i<frames.size();++i)if(frames[i].lost){
        std::size_t j=i;while(j<frames.size()&&frames[j].lost)++j;
        const double when=j<frames.size()?frames[j].arrive:frames[i].prod;
        for(std::size_t k=i;k<j;++k){frames[k].arrive=when;frames[k].plc=true;for(bool& l:frames[k].loud)l=k-i<2;}
        i=j;
    }
    std::vector<Block> out;double anchor=0,anchorIndex=0;bool anchored=false;std::size_t i=0;
    while(i<frames.size()){
        std::size_t j=i;while(j<frames.size()&&frames[j].arrive==frames[i].arrive)++j;  // one burst
        const std::size_t keep=std::min<std::size_t>(j-i,10);                          // AudioIngress 100 ms
        bool disc=!anchored||keep<j-i;
        for(std::size_t k=j-keep;k<j;++k){
            if(disc){anchor=frames[k].arrive;anchorIndex=static_cast<double>(k);anchored=true;}
            Block b{frames[k].arrive+2*rng.next(),anchor+10*(static_cast<double>(k)-anchorIndex),frames[k].prod+2,{},disc};
            for(int m=0;m<10;++m)b.loud[m]=frames[k].loud[m];
            out.push_back(b);disc=false;
        }
        i=j;
    }
    std::sort(out.begin(),out.end(),[](const Block& a,const Block& b){return a.push<b.push;});
    return out;
}

Result simulate(const Scenario& sc,Policy policy){
    Rng rng;const auto blocks=arrivals(sc,rng);
    const double capacity=policy.lowBuffer?6:20,period=policy.lowBuffer?3:10;
    const double reserve=policy.lowBuffer?14:std::min(capacity,20.0);
    std::deque<Unit> queue,endpoint;double padding=0;bool started=false,gap=false;
    ArrivalClockMapping ingressClock;double ingress=0;
    AudioVideoDelayFilter videoFilter;AudioArrivalDrift drift;AudioLatencyController controller;
    AudioRatioArming arming;double setPpm=0,armedUntil=-1,filtered=0,nextCorrection=0,lastReset=-1e9,starveSince=-1,lastPush=0,downUntil=-1,nextVideo=0,lastVideo=sc.videoMs;
    std::size_t next=0;std::vector<double> lat,av;Result r;
    auto queuedMs=[&]{double q=0;for(const auto& u:queue)q+=u.len;return q;};
    auto restart=[&](bool clear){if(clear)queue.clear();endpoint.clear();padding=0;started=false;setPpm=0;armedUntil=-1;filtered=0;++r.resets;};
    auto pump=[&]{
        double free=capacity-padding;
        while(free>1e-9&&!queue.empty()){
            Unit& u=queue.front();const double take=std::min(free,u.len);
            endpoint.push_back({u.pts,u.ideal,take,u.loud});padding+=take;free-=take;
            if(take<u.len){const double media=take/u.len;u.pts+=media;u.ideal+=media;u.len-=take;}else queue.pop_front();
        }
    };
    for(double t=0;t<sc.seconds*1000;t+=1){
        for(const auto& e:sc.events)if(e.kind==NetEvent::Device&&t==e.at){restart(true);downUntil=t+(policy.fresh?50:500);}
        if(t>=nextVideo){lastVideo=sc.videoMs+2*rng.next()-1;videoFilter.observe(t,lastVideo);nextVideo+=1000.0/60;}
        while(next<blocks.size()&&blocks[next].push<=t){
            const Block& b=blocks[next++];
            if(t<downUntil)continue;
            if(b.disc){restart(true);ingressClock.reset();if(policy.fresh)drift.rebase(t);}
            ingress=ingressClock.observe(b.push,b.pts);lastPush=t;
            const double ppm=t<armedUntil?setPpm:0;   // compensation ends after its distance unless set again
            for(int m=0;m<10;++m)queue.push_back({b.pts+m,b.ideal+m,1.0+ppm*1e-6,b.loud[m]});
        }
        double mapping=0;
        if(policy.fresh){const double wanted=videoFilter.value(t).value_or(lastVideo)+drift.observe(t,ingress);mapping=ingress+std::clamp(wanted-ingress,0.0,1500.0);}
        else mapping=ingress+std::clamp(lastVideo-ingress,0.0,1500.0);
        // Endpoint plays one millisecond.
        if(started){
            double left=1;
            if(padding<1)gap=true;
            while(left>1e-9&&!endpoint.empty()){
                Unit& u=endpoint.front();const double take=std::min(left,u.len);
                if(t>3000){lat.push_back(t-u.ideal);av.push_back(std::abs(t-(u.ideal+sc.videoMs)));}
                left-=take;padding-=take;
                if(take<u.len){const double media=take/u.len;u.pts+=media;u.ideal+=media;u.len-=take;}else endpoint.pop_front();
            }
            if(left>1e-9&&t>3000)r.silenceMs+=left;
            padding=std::max(0.0,padding);
            // Starvation: 30 ms with nothing to play and no input -> fade, clear, re-anchor.
            if(padding<=0&&queue.empty()){if(starveSince<0)starveSince=t;if(t-starveSince>=30&&t-lastPush>=30){restart(true);starveSince=-1;}}
            else starveSince=-1;
        }
        if(!started&&t>3000)r.silenceMs+=1;
        if(!started&&t>=downUntil&&!queue.empty()&&queuedMs()>=reserve){
            const double expired=std::max(0.0,t-mapping-queue.front().pts-5);
            double drop=std::min(expired,queuedMs()-reserve);
            while(drop>=1&&!queue.empty()){drop-=queue.front().len;queue.pop_front();}
            if(!queue.empty()&&t>=mapping+queue.front().pts){started=true;lastReset=t;nextCorrection=t+250;pump();controller.reset(t);arming.reset();}
            continue;
        }
        if(!started||std::fmod(t,period)!=0)continue;
        pump();
        const double queued=queuedMs()+padding;
        std::optional<double> error;
        if(!endpoint.empty())error=endpoint.front().pts-(t-mapping);
        if(!policy.fresh){
            if(!error||t<nextCorrection)continue;
            filtered=0.75*filtered+0.25**error;nextCorrection=t+250;
            if(t-lastReset>1000&&std::abs(*error)>60){restart(false);lastReset=t;continue;}
            const double target=std::clamp(filtered*250,-5000.0,5000.0);
            setPpm=captureSafeCorrectionPpm(target,setPpm,queued,reserve);armedUntil=t+1000;  // set every 250 ms
            continue;
        }
        AudioLatencyController::Input in;in.nowMs=t;in.errorMs=error;in.queuedMs=queued;in.endpointMs=padding;in.reserveMs=reserve;in.gap=gap;gap=false;
        const auto d=controller.update(in);
        if(d.reset){restart(false);lastReset=t;arming.reset();continue;}
        const int delta=int(std::llround(d.ratioPpm*48000/1000000));
        if(arming.due(t,delta)){setPpm=delta*1e6/48000;armedUntil=t+1000;}  // swresample: 48000-sample distance
        const double fade=controller.config().crossfadeMs;
        const double drop=std::floor(std::min(d.dropMs,queuedMs()-fade));
        if(drop>=1){
            bool quiet=true;double seen=0;for(const auto& u:queue){if(seen>=drop)break;quiet=quiet&&!u.loud;seen+=u.len;}
            const double cut=quiet?drop:std::floor(std::min(d.loudDropMs,queuedMs()-fade));
            if((quiet||d.loudDropMs>0)&&cut>=1){
                double left=cut;while(left>1e-9&&!queue.empty()){left-=queue.front().len;queue.pop_front();}
                controller.dropped(t,cut,quiet,controller.afterGap(t));r.droppedMs+=cut;
            }
        }
    }
    r.latMed=pct(lat,.5);r.latP95=pct(lat,.95);r.latMax=pct(lat,1);
    r.avMed=pct(av,.5);r.avP95=pct(av,.95);r.avMax=pct(av,1);
    return r;
}

std::vector<Scenario> scenarios(){
    std::vector<Scenario> s;
    s.push_back({"steady",60,0,35,{}});
    s.push_back({"steady, video 15 ms",60,0,15,{}});
    s.push_back({"25 ms hold x6",90,0,35,{{NetEvent::Hold,10000,25},{NetEvent::Hold,22000,25},{NetEvent::Hold,34000,25},{NetEvent::Hold,46000,25},{NetEvent::Hold,58000,25},{NetEvent::Hold,70000,25}}});
    s.push_back({"200 ms stall (loss)",60,0,35,{{NetEvent::Loss,20000,200}}});
    s.push_back({"200 ms stall (held)",60,0,35,{{NetEvent::Hold,20000,200}}});
    s.push_back({"2 s stall (loss)",60,0,35,{{NetEvent::Loss,20000,2000}}});
    s.push_back({"60 ms bursts every 5 s",90,0,35,{}});
    for(double at=10000;at<85000;at+=5000)s.back().events.push_back({NetEvent::Hold,at,60});
    s.push_back({"+200 ppm drift",300,200,35,{}});
    s.push_back({"-200 ppm drift",300,-200,35,{}});
    s.push_back({"device switch",60,0,35,{{NetEvent::Device,20000,0}}});
    return s;
}
}

TEST(audio_sync_linux_model_new_rule_is_never_worse_and_bounds_drift){
    std::printf("[audio-model] LINUX MODEL, not a measurement. ms: latency = play - arrival (median/P95/max); |A/V| = |play - (arrival + video delay)| (median/P95/max)\n");
    for(const auto& sc:scenarios()){
        const auto a=simulate(sc,{false,false}),b=simulate(sc,{true,false}),c=simulate(sc,{true,true});
        std::printf("[audio-model] %-24s today lat %5.1f/%5.1f/%6.1f av %5.1f/%5.1f/%6.1f sil %6.0f resets %2d | new lat %5.1f/%5.1f/%6.1f av %5.1f/%5.1f/%6.1f sil %6.0f resets %2d drop %5.0f | +low lat %5.1f/%5.1f/%6.1f av %5.1f/%5.1f\n",
            sc.name,a.latMed,a.latP95,a.latMax,a.avMed,a.avP95,a.avMax,a.silenceMs,a.resets,
            b.latMed,b.latP95,b.latMax,b.avMed,b.avP95,b.avMax,b.silenceMs,b.resets,b.droppedMs,c.latMed,c.latP95,c.latMax,c.avMed,c.avP95);
        CHECK(b.avP95<=a.avP95+0.5);
        CHECK(b.resets<=a.resets);
        if(sc.driftPpm!=0||sc.name==std::string("200 ms stall (loss)"))CHECK(b.avP95<10&&a.avP95>15);
    }
}
