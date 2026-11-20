// SPDX-License-Identifier: GPL-3.0-or-later
// Linux model: decoded PS5 frame -> Present -> scanout, for today's presentation path and the
// Slipstream presentation experiments (docs/PRESENTATION_LATENCY.md).
//
//   g++ -std=c++20 -O2 -o present_sim tools/remoteplay_latency_bench/present_sim.cpp
//   present_sim [--table] [--minutes N] > present_sim_results.json
//
// A 59.94 fps stream arrives over a wired-like or a Wi-Fi-like network (jitter and bursts are
// assumptions written below), is decoded in 1-3 ms by one serial decoder, and is presented on a
// display of 180 (179.952), 144, 120 or 60 Hz. Rules modelled:
//   composed      today's VSync path when DWM composes the window: one-deep display queue (the engine
//                 waits on the latency object before reading the next frame, newest frame wins), and
//                 DWM shows a present one refresh after the vblank it was composed at.
//   independent   the same path when the window reaches independent flip (fullscreen arm).
//   vrr-vsync     VSync with VRR active and independent flip: the panel starts a refresh when the frame
//                 is ready, no sooner than 1/maxHz after the previous start; below minHz it repeats.
//   tearing-vrr   VEYRA_EXP_PRESENT_TEARING with VRR active: sync interval 0 + ALLOW_TEARING; a frame
//                 ready within 1/maxHz of the previous refresh start is flipped at once (counted torn).
//   late-<us>     VEYRA_EXP_PRESENT_LATE (modelled only): independent flip, the engine wakes at
//                 vblank - margin, takes the newest decoded frame and presents it; older unshown
//                 frames are dropped; a present that misses its vblank shows one refresh later.
// CPU/GPU between pickup and the Present call: 0.4-1.2 ms; a flip must be queued 0.5 ms before its
// vblank (latch); a timer wake is late by 0-0.2 ms. All of these are assumptions, not measurements.
// Deterministic: own PRNG and distributions, so every build prints the same bytes.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed):s(seed?seed:1){}
    uint64_t next(){s^=s<<13;s^=s>>7;s^=s<<17;return s;}
    double uniform(){return double(next()>>11)*(1.0/9007199254740992.0);}
    double range(double a,double b){return a+(b-a)*uniform();}
    double normal(){const double u=std::max(uniform(),1e-12),v=uniform();return std::sqrt(-2*std::log(u))*std::cos(6.283185307179586*v);}
    double exponential(double mean){return -mean*std::log(std::max(1e-12,1-uniform()));}
};

struct Frame { double arrival=0,decoded=0; };
struct Network { const char* name; };

std::vector<Frame> makeStream(bool wifi,double minutes,uint64_t seed){
    Rng r(seed);
    const double interval=1000.0/59.94;
    const std::size_t n=std::size_t(minutes*60.0*59.94);
    std::vector<Frame> frames(n);
    double lastArrival=-1e9,lastDecoded=-1e9;
    for(std::size_t i=0;i<n;++i){
        const double ideal=100.0+double(i)*interval;
        double delay;
        if(wifi){
            // Wi-Fi-like: 1 ms floor, exponential jitter (mean 1.5 ms), 3 % of frames held 5-25 ms
            // (aggregation / retries); frames behind a held one arrive right after it (clumps).
            delay=1.0+r.exponential(1.5);
            if(r.uniform()<0.03)delay+=r.range(5,25);
        }else{
            // Wired-like: 0.3 ms floor, |N(0, 0.15)| jitter, 0.2 % of frames 1-4 ms late.
            delay=0.3+std::fabs(r.normal())*0.15;
            if(r.uniform()<0.002)delay+=r.range(1,4);
        }
        const double arrival=std::max(ideal+delay,lastArrival+0.05);
        const double decoded=std::max(arrival,lastDecoded)+r.range(1,3);
        frames[i]={arrival,decoded};lastArrival=arrival;lastDecoded=decoded;
    }
    return frames;
}

struct Result {
    std::vector<double> total,presentToScanout;
    std::size_t frames=0,shown=0,dropped=0,missed=0,torn=0;
};

double percentile(std::vector<double> v,double q){
    if(v.empty())return -1;
    std::sort(v.begin(),v.end());
    const std::size_t index=std::size_t(std::ceil(double(v.size())*q/100.0));
    return v[std::min(v.size()-1,index?index-1:0)];
}

constexpr double kLatch=0.5,kVrrLatch=0.3,kMinVrrHz=48.0;
double proc(Rng& r){return r.range(0.4,1.2);}
double wake(Rng& r){return r.range(0.0,0.2);}

// First vblank at or after t. Vblanks at phase + k*period.
double vblankAtOrAfter(double t,double phase,double period){return phase+std::ceil((t-phase)/period)*period;}

// composed / independent: the engine reads a frame only when the display queue is empty (latency
// object, maximum latency 1, waited before the read); a frame waiting meanwhile is replaced by a newer one.
Result queued(const std::vector<Frame>& f,double hz,bool composed,uint64_t seed){
    Rng r(seed);Result out;out.frames=f.size();
    const double period=1000.0/hz,phase=0.37*period;
    double freeAt=0;
    for(std::size_t i=0;i<f.size();++i){
        const double d=f[i].decoded;
        if(d<freeAt&&i+1<f.size()&&f[i+1].decoded<=freeAt){++out.dropped;continue;}
        const double pickup=d<freeAt?freeAt+wake(r):d;
        const double present=pickup+proc(r);
        const double vblank=vblankAtOrAfter(present+kLatch,phase,period);
        const double scanout=vblank+(composed?period:0);
        freeAt=vblank;
        ++out.shown;out.total.push_back(scanout-f[i].arrival);out.presentToScanout.push_back(scanout-present);
    }
    return out;
}

// VRR: a refresh starts when a frame is ready (after kVrrLatch), at least 1/hz after the previous
// start; with no frame for 1/kMinVrrHz the panel repeats the old picture (a forced refresh).
Result vrr(const std::vector<Frame>& f,double hz,bool tearing,uint64_t seed){
    Rng r(seed);Result out;out.frames=f.size();
    const double minPeriod=1000.0/hz,maxPeriod=1000.0/std::min(kMinVrrHz,hz);
    double lastStart=-1e9,freeAt=0;
    for(std::size_t i=0;i<f.size();++i){
        const double d=f[i].decoded;
        if(!tearing&&d<freeAt&&i+1<f.size()&&f[i+1].decoded<=freeAt){++out.dropped;continue;}
        const double pickup=(!tearing&&d<freeAt)?freeAt+wake(r):d;
        const double present=pickup+proc(r);
        const double ready=present+kVrrLatch;
        // Forced refreshes since the last start.
        if(ready-lastStart>maxPeriod&&lastStart>-1e8)lastStart+=std::floor((ready-lastStart)/maxPeriod)*maxPeriod;
        double scanout;
        if(ready>=lastStart+minPeriod){scanout=ready;lastStart=ready;}
        else if(tearing){scanout=ready;++out.torn;}
        else{scanout=lastStart+minPeriod;lastStart=scanout;}
        freeAt=scanout;
        ++out.shown;out.total.push_back(scanout-f[i].arrival);out.presentToScanout.push_back(scanout-present);
    }
    return out;
}

// Just in time: at every vblank V the engine wakes at V - margin, takes the newest decoded frame that
// was not shown yet (older ones are dropped) and presents it.
Result late(const std::vector<Frame>& f,double hz,double marginMs,uint64_t seed){
    Rng r(seed);Result out;out.frames=f.size();
    const double period=1000.0/hz,phase=0.37*period;
    std::size_t next=0; // first frame not yet taken
    double v=vblankAtOrAfter(f.front().decoded,phase,period);
    double busyUntil=0; // a missed present is still queued for the following vblank
    while(next<f.size()){
        const double wakeAt=v-marginMs+wake(r);
        if(wakeAt<busyUntil){v+=period;continue;}
        std::size_t newest=next;bool any=false;
        while(newest<f.size()&&f[newest].decoded<=wakeAt){any=true;++newest;}
        if(!any){v+=period;continue;}
        const std::size_t take=newest-1;
        out.dropped+=take-next;
        const double present=wakeAt+proc(r);
        double scanout=v;
        if(present+kLatch>v){++out.missed;scanout=vblankAtOrAfter(present+kLatch,phase,period);}
        busyUntil=scanout;
        ++out.shown;out.total.push_back(scanout-f[take].arrival);out.presentToScanout.push_back(scanout-present);
        next=newest;v+=period;
    }
    return out;
}

bool firstRun=true;
void print(const char* network,double hz,const std::string& rule,const Result& r,bool table){
    const double p50=percentile(r.total,50),p95=percentile(r.total,95),p99=percentile(r.total,99);
    const double s50=percentile(r.presentToScanout,50),s95=percentile(r.presentToScanout,95);
    if(table)std::printf("| %s | %.0f | %s | %.2f | %.2f | %.2f | %.2f | %.2f | %zu | %zu | %zu |\n",network,hz,rule.c_str(),p50,p95,p99,s50,s95,r.dropped,r.missed,r.torn);
    else std::printf("%s{\"network\":\"%s\",\"hz\":%.3f,\"rule\":\"%s\",\"frames\":%zu,\"shown\":%zu,\"dropped\":%zu,\"missedVblank\":%zu,\"torn\":%zu,"
        "\"arrivalToScanoutP50\":%.3f,\"arrivalToScanoutP95\":%.3f,\"arrivalToScanoutP99\":%.3f,\"presentToScanoutP50\":%.3f,\"presentToScanoutP95\":%.3f}",
        firstRun?"":",\n",network,hz,rule.c_str(),r.frames,r.shown,r.dropped,r.missed,r.torn,p50,p95,p99,s50,s95);
    firstRun=false;
}
}

int main(int argc,char** argv){
    bool table=false;double minutes=10;
    for(int i=1;i<argc;++i){
        if(!std::strcmp(argv[i],"--table"))table=true;
        else if(!std::strcmp(argv[i],"--minutes")&&i+1<argc)minutes=std::atof(argv[++i]);
        else{std::fprintf(stderr,"usage: present_sim [--table] [--minutes N]\n");return 2;}
    }
    if(!table)std::printf("{\"model\":\"LINUX MODEL of decoded frame -> Present -> scanout (tools/remoteplay_latency_bench/present_sim.cpp); not a measurement\",\n\"units\":\"ms; arrival = network arrival of the frame, scanout = start of the refresh that shows it\",\n\"runs\":[\n");
    if(table)std::printf("| network | Hz | rule | arrival->scanout P50 | P95 | P99 | present->scanout P50 | P95 | dropped | missed vblank | torn |\n|---|---|---|---|---|---|---|---|---|---|---|\n");
    const double rates[]={179.952,144.0,120.0,60.0};
    for(int net=0;net<2;++net){
        const bool wifi=net==1;const char* name=wifi?"wifi-like":"wired-like";
        const auto frames=makeStream(wifi,minutes,wifi?0x5EEDF00Dull:0xC0FFEEull);
        for(double hz:rates){
            const uint64_t seed=uint64_t(hz*1000)+uint64_t(net);
            print(name,hz,"composed",queued(frames,hz,true,seed),table);
            print(name,hz,"independent",queued(frames,hz,false,seed),table);
            print(name,hz,"vrr-vsync",vrr(frames,hz,false,seed),table);
            print(name,hz,"tearing-vrr",vrr(frames,hz,true,seed),table);
            for(double margin:{0.8,1.2,1.6,2.5}){
                char rule[32];std::snprintf(rule,sizeof(rule),"late-%.0fus",margin*1000);
                print(name,hz,rule,late(frames,hz,margin,seed),table);
            }
        }
    }
    if(!table)std::printf("\n]}\n");
    return 0;
}
