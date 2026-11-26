// SPDX-License-Identifier: GPL-3.0-only
// delivery_sim: a LINUX MODEL of PS5 Remote Play frame delivery, from frame
// arrival (takion callback) through decode, the decoded-frame mailbox and the
// engine's pickup to scanout. It is a model, not a measurement: every number
// it prints follows from the parameters below. See docs/FRAME_DELIVERY.md.
//
// Build: g++ -std=c++20 -O2 -Iinclude tools/remoteplay_latency_bench/delivery_sim.cpp -o /tmp/delivery_sim
// Run:   /tmp/delivery_sim [--seconds N] [--seeds N] [--json out.json]
//        /tmp/delivery_sim --gaps gaps.txt      (replay: one inter-arrival gap in ms per line)
//        /tmp/delivery_sim --fit "arrived=300 clumps=6 longGaps=3"   (Wi-Fi model from a [slipstream-delivery] line)
//
// Policy (b) uses the app's own decision code (include/veyra/remoteplay/FrameDelivery.h).
#include "veyra/remoteplay/FrameDelivery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>
using namespace veyra::remoteplay;

namespace {
constexpr double kSourceFps=59.94;
constexpr double kInterval=1000.0/kSourceFps; // ms

// ---------------- arrival processes ----------------
struct Network {
    std::string name;
    double jitterMs=0.25;      // Gaussian jitter of every arrival (not cumulative)
    double skipProb=0.003;     // the console sends no frame for this slot (the stream runs slightly under 60)
    double pairProb=0;         // a lone frame is late by U(pairMin,pairMax) ms: it lands just before the next one
    double pairMin=4,pairMax=14;
    double holdPerSecond=0;    // long link holds: frames are queued for U(holdMin,holdMax) ms, then released together
    double holdMin=60,holdMax=130;
    double shortHoldPerSecond=0; // short holds of U(shortMin,shortMax) ms (2-3 frames released together)
    double shortMin=20,shortMax=45;
    double releaseSpacingMs=0.3; // spacing of a released clump
    std::vector<double> replayGaps; // replay mode: inter-arrival gaps in ms, cycled
};
Network lan(){Network n;n.name="lan";n.jitterMs=0.25;return n;}
// Wi-Fi: chosen so that today's policy at 180 Hz, queue-1 vsync, event-driven
// hand-off replaces ~0.7 % of frames with the shape the owner's logs show:
// rare seconds with several replaced frames (long holds, 1 per 20 s, 80-150 ms),
// plus a low steady rate (short holds, 0.32 per s, 22-48 ms: 2-4 frames released
// together). Lone late frames alone replace nothing with an event-driven engine
// (see docs/FRAME_DELIVERY.md). Fitted by hand to ONE Wi-Fi session's software
// counters (replaced 0.71 %, seconds under 57 presents 4.2 %); not a general
// Wi-Fi model.
Network wifi(){Network n;n.name="wifi";n.jitterMs=1.0;n.pairProb=0.005;n.holdPerSecond=0.05;n.holdMin=80;n.holdMax=150;n.shortHoldPerSecond=0.32;n.shortMin=22;n.shortMax=48;return n;}

struct Arrival{double t;int64_t wire;};
std::vector<Arrival> arrivals(const Network& n,double seconds,std::mt19937_64& rng){
    std::uniform_real_distribution<double> u(0,1);std::normal_distribution<double> g(0,1);
    std::vector<Arrival> out;
    if(!n.replayGaps.empty()){
        double t=100;size_t k=0;int64_t wire=0;
        while(t<seconds*1000){out.push_back({t,wire++});t+=std::max(0.0,n.replayGaps[k++%n.replayGaps.size()]);}
        return out;
    }
    const double holdProbPerFrame=n.holdPerSecond/kSourceFps;
    double holdUntil=-1;
    const int64_t frames=int64_t(seconds*kSourceFps);
    for(int64_t i=0;i<frames;++i){
        const double nominal=100+double(i)*kInterval;
        if(u(rng)<n.skipProb)continue;
        double t=nominal+std::abs(g(rng))*n.jitterMs;
        if(holdUntil<0&&u(rng)<holdProbPerFrame)holdUntil=nominal+n.holdMin+u(rng)*(n.holdMax-n.holdMin);
        if(holdUntil<0&&u(rng)<n.shortHoldPerSecond/kSourceFps)holdUntil=nominal+n.shortMin+u(rng)*(n.shortMax-n.shortMin);
        if(holdUntil>=0){
            if(nominal<holdUntil)t=holdUntil;else holdUntil=-1;
        }
        if(holdUntil<0&&u(rng)<n.pairProb)t=nominal+n.pairMin+u(rng)*(n.pairMax-n.pairMin);
        out.push_back({t,i});
    }
    // A link delivers in order: a frame never overtakes the one before it.
    std::sort(out.begin(),out.end(),[](const Arrival& a,const Arrival& b){return a.wire<b.wire;});
    for(size_t i=1;i<out.size();++i)out[i].t=std::max(out[i].t,out[i-1].t+(out[i].t<=out[i-1].t?n.releaseSpacingMs:0));
    return out;
}

// ---------------- pipeline model ----------------
struct Pipeline {
    double refreshHz=180;bool vsync=true;
    double decodeMin=1,decodeMax=3;     // owner-thread decode, serial
    double processMin=1,processMax=2.5; // engine pickup -> Present call (GPU work included)
    double ownerPollMs=0;               // 0 = event-driven owner, else polling period (legacy: 1 or 15.6)
    double enginePollMs=0;              // 0 = event-driven engine wake, else slice (legacy: 1)
    double scanDelayRefreshes=1;        // vsync: the picture reaches the screen this many refreshes after the
                                        // first vblank after Present (composition). 1 matches the field's
                                        // presentToScanout P50 9-10 ms at 180 Hz (BENCH1 item 4). Display only.
    double extraBlindRefreshes=0;       // vsync: the engine stays blind this many refreshes after that first
                                        // vblank (the queue-1 waitable signalled later). Sensitivity only.
    double wakeMs=0.05;                 // event wake-up latency
};
enum class Policy{Newest,CatchUp,LateNewer,Dejitter};
const char* policyName(Policy p){switch(p){case Policy::Newest:return "a-newest";case Policy::CatchUp:return "b-catchup";case Policy::LateNewer:return "c-late-newer";default:return "d-dejitter";}}

struct Shown{int64_t idx;double scan;};
struct Result {
    std::string network,policy,owner;double refreshHz=0;bool vsync=true;double dejitterMs=0;
    uint64_t arrived=0,shown=0,neverShown=0,replacedAtPublish=0,replacedAtTake=0,caughtUp=0,hitches=0;
    uint64_t seconds=0,lowSeconds=0,lowArrivalSeconds=0,emptyReads=0;
    std::vector<double> shownInterval,arrToScan,fresh,ageOnScreen,replacedAt;
    uint64_t replacedInLow=0;
};
double pct(std::vector<double>& v,double p){if(v.empty())return -1;std::sort(v.begin(),v.end());size_t k=size_t(std::ceil(p*double(v.size())));k=std::clamp<size_t>(k,1,v.size());return v[k-1];}

Result run(const Network& net,const Pipeline& pl,Policy policy,double dejitterMs,double seconds,uint64_t seed){
    std::mt19937_64 rng(seed);std::uniform_real_distribution<double> u(0,1);
    const auto arr=arrivals(net,seconds,rng);
    Result r;r.network=net.name;r.policy=policyName(policy);r.refreshHz=pl.refreshHz;r.vsync=pl.vsync;r.dejitterMs=dejitterMs;
    r.owner=pl.ownerPollMs>0?"poll":"event";
    // Decoder owner: serial, in order.
    const double ownerPhase=u(rng)*std::max(pl.ownerPollMs,1.0);
    std::vector<double> published(arr.size());
    double ownerFree=0;
    for(size_t i=0;i<arr.size();++i){
        double start=arr[i].t+pl.wakeMs;
        // A polling owner that is still busy goes straight on to the next queued
        // sample; an idle one notices it at its next poll.
        if(pl.ownerPollMs>0&&arr[i].t>ownerFree)start=ownerPhase+std::ceil((arr[i].t-ownerPhase)/pl.ownerPollMs)*pl.ownerPollMs;
        start=std::max(start,ownerFree);
        const double done=start+pl.decodeMin+u(rng)*(pl.decodeMax-pl.decodeMin);
        ownerFree=done;published[i]=done;
        if(pl.ownerPollMs==0)++r.emptyReads; // event owner reads once more, finds nothing, waits
    }
    // (d) de-jitter: hold each frame until a smoothed schedule + D.
    if(policy==Policy::Dejitter){
        std::deque<std::pair<int64_t,double>> window; // (wire, offset)
        for(size_t i=0;i<arr.size();++i){
            const double offset=published[i]-double(arr[i].wire)*kInterval;
            window.push_back({arr[i].wire,offset});
            while(!window.empty()&&window.front().first<arr[i].wire-120)window.pop_front();
            double anchor=offset;for(const auto& w:window)anchor=std::min(anchor,w.second);
            published[i]=std::max(published[i],anchor+double(arr[i].wire)*kInterval+dejitterMs);
        }
        for(size_t i=1;i<published.size();++i)published[i]=std::max(published[i],published[i-1]);
    }
    const double period=1000.0/pl.refreshHz,phase=u(rng)*period;
    const auto vblankAtOrAfter=[&](double t){return phase+std::ceil((t-phase)/period)*period;};
    const double enginePhase=u(rng)*std::max(pl.enginePollMs,1.0);
    // Mailbox: the app's DeliverySlots (Newest = one slot; CatchUp = two).
    DeliverySlots<int64_t> slots(policy==Policy::CatchUp||policy==Policy::LateNewer?DeliveryPolicy::CatchUp:DeliveryPolicy::Newest);
    CatchUpGuard guard;
    const auto noMerge=[](int64_t&,int64_t&){};
    DeliveryContext ctx;ctx.refreshHz=pl.refreshHz;ctx.sourceFps=kSourceFps;ctx.frameGeneration=false;
    DeliveryContext never=ctx;never.frameGeneration=true; // (c): catch-up only when free; see below
    std::vector<Shown> shown;std::vector<char> seen(arr.size(),0);
    size_t next=0;double engineFree=0;
    const double end=seconds*1000+100;
    auto toMs=[](double ms){return HostTime(std::llround(ms*10000));};
    while(true){
        // Everything published before the pickup instant is in the mailbox.
        double t=engineFree;
        while(next<published.size()&&published[next]<=t){if(slots.publish(int64_t(next),toMs(arr[next].t),noMerge)){++r.replacedAtPublish;r.replacedAt.push_back(published[next]);}++next;}
        if(!slots.pending()){
            if(next>=published.size())break;
            double wake=published[next]+pl.wakeMs;
            if(pl.enginePollMs>0)wake=enginePhase+std::ceil((published[next]-enginePhase)/pl.enginePollMs)*pl.enginePollMs;
            t=std::max(t,wake);
            while(next<published.size()&&published[next]<=t){if(slots.publish(int64_t(next),toMs(arr[next].t),noMerge)){++r.replacedAtPublish;r.replacedAt.push_back(published[next]);}++next;}
        }
        const double process=pl.processMin+u(rng)*(pl.processMax-pl.processMin);
        bool allow=guard.allows(toMs(t));
        const DeliveryContext* use=&ctx;
        if(policy==Policy::LateNewer){
            // (c) newest wins unless the newer frame would miss the vblank the
            // older one makes anyway. Both are already decoded at this pickup,
            // so the newer frame always makes the same vblank: never true.
            const double older=vblankAtOrAfter(t+process),newer=vblankAtOrAfter(t+process);
            if(!(newer>older))use=&never;
        }
        if(guard.awaitingFollow())guard.followed(toMs(t),pl.refreshHz,kSourceFps);
        auto taken=slots.take(toMs(t),*use,allow,noMerge);
        if(!taken)break;
        if(taken->replaced){++r.replacedAtTake;r.replacedAt.push_back(t);}
        if(taken->caughtUp){++r.caughtUp;guard.caughtUp(toMs(t));}
        double scan,free;
        if(pl.vsync){const double flip=vblankAtOrAfter(t+process+0.3);scan=flip+pl.scanDelayRefreshes*period;free=flip+pl.extraBlindRefreshes*period;}
        else{scan=t+process;free=scan;}
        shown.push_back({taken->value,scan});seen[size_t(taken->value)]=1;
        engineFree=free;
        if(engineFree>end)break;
    }
    r.arrived=arr.size();r.shown=shown.size();
    for(size_t i=0;i<arr.size();++i)if(!seen[i]&&published[i]<end-200)++r.neverShown;
    for(size_t i=1;i<shown.size();++i){const double d=shown[i].scan-shown[i-1].scan;r.shownInterval.push_back(d);if(d>1.5*kInterval)++r.hitches;}
    for(const auto& s:shown)r.arrToScan.push_back(s.scan-arr[size_t(s.idx)].t);
    // Freshness: arrival of every frame -> first scanout of it or a newer one.
    size_t k=0;
    for(size_t i=0;i<arr.size();++i){while(k<shown.size()&&shown[k].idx<int64_t(i))++k;if(k<shown.size())r.fresh.push_back(shown[k].scan-arr[i].t);}
    // Age on screen, sampled every refresh (immediate: every 1 ms).
    const double step=pl.vsync?period:1.0;size_t s=0;
    for(double t=phase+1000;t<end-200&&!shown.empty();t+=step){while(s+1<shown.size()&&shown[s+1].scan<=t)++s;if(shown[s].scan<=t)r.ageOnScreen.push_back(t-arr[size_t(shown[s].idx)].t);}
    // Seconds with fewer than 57 frames shown / 58 arrived (the field metrics).
    std::map<int64_t,int> perShown,perArr;
    for(const auto& x:shown)++perShown[int64_t(x.scan/1000)];
    for(const auto& a:arr)++perArr[int64_t(a.t/1000)];
    for(int64_t sec=1;sec<int64_t(seconds)-1;++sec){++r.seconds;if(perShown[sec]<57)++r.lowSeconds;if(perArr[sec]<58)++r.lowArrivalSeconds;}
    for(double t:r.replacedAt){const auto sec=int64_t(t/1000);if(sec>=1&&sec<int64_t(seconds)-1&&perShown[sec]<57)++r.replacedInLow;}
    return r;
}

struct Row{std::string key;std::vector<Result> runs;};
std::string fmt(double v){char b[32];std::snprintf(b,sizeof b,"%.2f",v);return b;}
std::map<std::string,double> summarize(std::vector<Result>& runs){
    std::map<std::string,double> m;Result all=runs.front();
    for(size_t i=1;i<runs.size();++i){auto& x=runs[i];
        all.arrived+=x.arrived;all.shown+=x.shown;all.neverShown+=x.neverShown;all.replacedAtPublish+=x.replacedAtPublish;all.replacedAtTake+=x.replacedAtTake;
        all.caughtUp+=x.caughtUp;all.replacedInLow+=x.replacedInLow;all.hitches+=x.hitches;all.seconds+=x.seconds;all.lowSeconds+=x.lowSeconds;all.lowArrivalSeconds+=x.lowArrivalSeconds;
        for(auto v:{&Result::shownInterval,&Result::arrToScan,&Result::fresh,&Result::ageOnScreen})(all.*v).insert((all.*v).end(),(x.*v).begin(),(x.*v).end());}
    m["neverShownPct"]=100.0*double(all.neverShown)/double(all.arrived);
    m["replacedPct"]=100.0*double(all.replacedAtPublish+all.replacedAtTake)/double(all.arrived);
    m["caughtUpPct"]=100.0*double(all.caughtUp)/double(all.arrived);
    m["hitchesPerMin"]=double(all.hitches)/(double(all.seconds)/60.0);
    m["lowSecondsPct"]=100.0*double(all.lowSeconds)/double(all.seconds);
    m["replacedInLowSecondsPct"]=all.replacedAtPublish+all.replacedAtTake?100.0*double(all.replacedInLow)/double(all.replacedAtPublish+all.replacedAtTake):0;
    m["lowArrivalSecondsPct"]=100.0*double(all.lowArrivalSeconds)/double(all.seconds);
    m["shownGapP95"]=pct(all.shownInterval,.95);m["shownGapP99"]=pct(all.shownInterval,.99);m["shownGapMax"]=pct(all.shownInterval,1);
    m["arrToScanP50"]=pct(all.arrToScan,.5);m["arrToScanP95"]=pct(all.arrToScan,.95);m["arrToScanP99"]=pct(all.arrToScan,.99);
    m["freshP50"]=pct(all.fresh,.5);m["freshP95"]=pct(all.fresh,.95);m["freshP99"]=pct(all.fresh,.99);
    m["ageP50"]=pct(all.ageOnScreen,.5);m["ageP95"]=pct(all.ageOnScreen,.95);m["ageP99"]=pct(all.ageOnScreen,.99);
    return m;
}
} // namespace

int main(int argc,char** argv){
    double seconds=600;int seeds=3;std::string json,gapsFile,fit,wifiParams;bool quick=false;
    for(int i=1;i<argc;++i){std::string a=argv[i];auto val=[&]{return i+1<argc?std::string(argv[++i]):std::string();};
        if(a=="--seconds")seconds=std::atof(val().c_str());else if(a=="--seeds")seeds=std::atoi(val().c_str());
        else if(a=="--json")json=val();else if(a=="--gaps")gapsFile=val();else if(a=="--fit")fit=val();else if(a=="--wifi")wifiParams=val();else if(a=="--quick")quick=true;
        else{std::fprintf(stderr,"unknown option %s\n",a.c_str());return 2;}}
    std::vector<Network> nets{lan(),wifi()};
    if(!wifiParams.empty()){
        // Override Wi-Fi model parameters: "pairProb=0.01 pairMin=10 pairMax=16 holdPerSecond=0.04 holdMin=70 holdMax=140 jitterMs=1"
        std::istringstream in(wifiParams);std::string tok;Network& n=nets[1];
        while(in>>tok){auto eq=tok.find('=');if(eq==std::string::npos)continue;const auto k=tok.substr(0,eq);const double v=std::atof(tok.substr(eq+1).c_str());
            if(k=="pairProb")n.pairProb=v;else if(k=="pairMin")n.pairMin=v;else if(k=="pairMax")n.pairMax=v;else if(k=="holdPerSecond")n.holdPerSecond=v;
            else if(k=="holdMin")n.holdMin=v;else if(k=="holdMax")n.holdMax=v;else if(k=="jitterMs")n.jitterMs=v;else if(k=="shortHoldPerSecond")n.shortHoldPerSecond=v;else if(k=="shortMin")n.shortMin=v;else if(k=="shortMax")n.shortMax=v;else if(k=="skipProb")n.skipProb=v;}
    }
    if(!gapsFile.empty()){Network n;n.name="replay";std::ifstream f(gapsFile);double g;while(f>>g)n.replayGaps.push_back(g);
        if(n.replayGaps.empty()){std::fprintf(stderr,"no gaps in %s\n",gapsFile.c_str());return 2;}nets={n};}
    if(!fit.empty()){
        // Wi-Fi model from one [slipstream-delivery] line: long gaps -> holds, clumps outside holds -> lone late frames.
        std::map<std::string,double> kv;std::istringstream in(fit);std::string tok;
        while(in>>tok){auto eq=tok.find('=');if(eq!=std::string::npos)kv[tok.substr(0,eq)]=std::atof(tok.substr(eq+1).c_str());}
        const double arrived=std::max(1.0,kv["arrived"]),window=arrived/kSourceFps;
        Network n=wifi();n.name="fit";n.holdPerSecond=kv["longGaps"]/window;
        const double holdClumps=n.holdPerSecond*window*((n.holdMin+n.holdMax)/2/kInterval);
        n.pairProb=std::max(0.0,kv["clumps"]-holdClumps)/arrived;nets={n};
        std::printf("fit: holdPerSecond=%.4f pairProb=%.4f (from arrived=%.0f clumps=%.0f longGaps=%.0f)\n",n.holdPerSecond,n.pairProb,arrived,kv["clumps"],kv["longGaps"]);
    }
    std::printf("MODEL (Linux simulation, not a measurement). source %.2f fps, %d seeds x %.0f s per row.\n",kSourceFps,seeds,seconds);
    std::printf("%-6s %-10s %4s %-4s %-13s %6s %6s %6s %6s %6s | %6s %6s %6s | %6s %6s %6s | %6s %6s | %6s %6s\n",
        "net","owner","Hz","pres","policy","never%","repl%","catch%","low%s","hitch/m","gapP95","gapP99","gapMax","a2sP50","a2sP95","a2sP99","frP50","frP99","ageP50","ageP95");
    std::ostringstream js;js<<"{\n \"label\": \"MODEL: Linux simulation of PS5 Remote Play frame delivery (tools/remoteplay_latency_bench/delivery_sim.cpp); not a measurement\",\n \"rows\": [\n";
    bool firstRow=true;
    auto emit=[&](const Network& net,const Pipeline& pl,Policy pol,double dj){
        std::vector<Result> runs;for(int s=0;s<seeds;++s)runs.push_back(run(net,pl,pol,dj,seconds,0x5eed0000ull+uint64_t(s)*7919ull+uint64_t(pl.refreshHz)));
        auto m=summarize(runs);
        std::string name=policyName(pol);if(pol==Policy::Dejitter)name+="-"+fmt(dj).substr(0,fmt(dj).find('.'))+"ms";
        std::string owner=pl.ownerPollMs>0?(pl.ownerPollMs>2?"p15.6":"p1"):"event";
        if(pl.vsync&&pl.extraBlindRefreshes!=0)owner+="/b"+fmt(pl.extraBlindRefreshes).substr(0,1);
        if(pl.processMax>5)owner+="/slow";
        std::printf("%-6s %-10s %4.0f %-4s %-13s %6.2f %6.2f %6.2f %6.2f %6.2f | %6.2f %6.2f %6.2f | %6.2f %6.2f %6.2f | %6.2f %6.2f | %6.2f %6.2f\n",
            net.name.c_str(),owner.c_str(),pl.refreshHz,pl.vsync?"vs1":"imm",name.c_str(),m["neverShownPct"],m["replacedPct"],m["caughtUpPct"],m["lowSecondsPct"],m["hitchesPerMin"],
            m["shownGapP95"],m["shownGapP99"],m["shownGapMax"],m["arrToScanP50"],m["arrToScanP95"],m["arrToScanP99"],m["freshP50"],m["freshP99"],m["ageP50"],m["ageP95"]);
        js<<(firstRow?"":",\n")<<"  {\"network\":\""<<net.name<<"\",\"owner\":\""<<owner<<"\",\"refreshHz\":"<<pl.refreshHz<<",\"presentation\":\""<<(pl.vsync?"vsync-queue1":"immediate")<<"\",\"policy\":\""<<name<<"\"";
        for(auto& [k,v]:m)js<<",\""<<k<<"\":"<<fmt(v);
        js<<"}";firstRow=false;
    };
    for(const auto& net:nets){
        if(quick){Pipeline pl;emit(net,pl,Policy::Newest,0);emit(net,pl,Policy::CatchUp,0);continue;}
        for(double hz:{60.0,120.0,144.0,180.0,240.0})for(bool vs:{true,false}){
            Pipeline pl;pl.refreshHz=hz;pl.vsync=vs;
            for(Policy p:{Policy::Newest,Policy::CatchUp,Policy::LateNewer})emit(net,pl,p,0);
            emit(net,pl,Policy::Dejitter,4);emit(net,pl,Policy::Dejitter,8);
        }
        // Legacy timing (VEYRA_TEST_RP_LEGACY_TIMING): polling owner and engine.
        for(double poll:{1.0,15.6}){Pipeline pl;pl.ownerPollMs=poll;pl.enginePollMs=1;emit(net,pl,Policy::Newest,0);}
        // Calibration grid: how long the engine stays blind after the flip
        // (0-2 extra refreshes) against the owner's wake (event / 1 ms / 15.6 ms polls).
        for(double blind:{0.0,1.0,2.0})for(double poll:{0.0,1.0,15.6}){Pipeline pl;pl.extraBlindRefreshes=blind;pl.ownerPollMs=poll;pl.enginePollMs=poll>0?1:0;emit(net,pl,Policy::Newest,0);}
        // A slow engine (super resolution): processing 6-9 ms at 180 Hz.
        {Pipeline pl;pl.processMin=6;pl.processMax=9;emit(net,pl,Policy::Newest,0);emit(net,pl,Policy::CatchUp,0);}
    }
    // VideoIngress expiry race (fixed only under VEYRA_EXP_RP_BACKLOG): the
    // owner reads the clock, then takes the inbox lock; a sample pushed into an
    // EMPTY queue in between is called expired (recovery + keyframe request).
    // Expected rate = empty-queue reads/s x arrival rate x window.
    if(quick){js<<"\n ]\n}\n";if(!json.empty()){std::ofstream f(json);f<<js.str();}return 0;}
    std::printf("\nexpiry race (analytic): empty-queue reads/s ~ 60 (after each decode) + 250 (4 ms controller ticks) = 310\n");
    for(double w_us:{0.1,1.0,10.0}){const double perHour=310.0*kSourceFps*w_us*1e-6*3600;
        std::printf("  window %.1f us -> %.2f recoveries per hour (each: queue dropped, picture frozen until the next keyframe)\n",w_us,perHour);}
    js<<"\n ]\n}\n";
    if(!json.empty()){std::ofstream f(json);f<<js.str();}
    return 0;
}
