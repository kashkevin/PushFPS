// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Linux model of the video reorder wait under made-up link conditions, for
// tools/remoteplay_latency_bench/reorder_study.cpp and the unit tests only:
// the application never uses it. Portable and header-only.
//
// A packet arrival trace (a stream of 59.94 frames a second, every frame 24
// source packets plus FEC packets sent over 2 ms) is made for one condition:
// clean, displaced packets (reordering), Gilbert-Elliott loss bursts, a
// bottleneck whose queue builds up and drains, path stalls (jitter spikes),
// interruptions, a bitrate above the link's capacity, a receive thread that
// wakes late, a slow decoder. Every condition is clean before and after an
// impairment window, so what remains after it can be seen. The trace runs
// through the library's reorder-queue rule (ReorderQueueModel.h, compared
// with the C source by reorder_library_check.sh) under a fixed wait or the
// adaptive controller (ReorderWindow.h), and the result is per frame (when it
// could have been complete, when it was handed over) and per five-second
// interval (the health snapshot ConnectionHealth.h would have seen).
//
// Everything about the stream and the links is an assumption. Nothing here
// was measured on a PS5, a network or a Windows machine, and nothing predicts
// what a real console or LAN does: it compares rules on the same arrivals.
#include "ConnectionHealth.h"
#include "ReorderQueueModel.h"
#include "ReorderWindow.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>
namespace veyra::remoteplay::study {
inline constexpr std::int64_t kFrameUs=16683,kSendSpreadUs=2000,kBaseDelayUs=300,kStartUs=1000000;
inline constexpr std::int64_t kIntervalUs=5000000,kLookUs=250000;
inline constexpr unsigned kSourceUnits=24;
inline constexpr std::int64_t kRequestSpacingUs=500000; // PushFPS sends at most one keyframe request per 500 ms

// Own generator, so that a trace does not depend on the standard library.
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed)noexcept:state(seed*0x9E3779B97F4A7C15ull+0x1234567ull){}
    std::uint64_t next()noexcept{std::uint64_t z=(state+=0x9E3779B97F4A7C15ull);z=(z^(z>>30))*0xBF58476D1CE4E5B9ull;z=(z^(z>>27))*0x94D049BB133111EBull;return z^(z>>31);}
    double unit()noexcept{return static_cast<double>(next()>>11)*(1.0/9007199254740992.0);} // [0,1)
    bool chance(double p)noexcept{return p>0&&unit()<p;}
    double between(double low,double high)noexcept{return low+(high-low)*unit();}
    double exponential(double mean)noexcept{return mean<=0?0:-mean*std::log(1.0-unit());}
};

struct Condition {
    const char* name="";
    const char* description="";
    unsigned fecUnits=3;                          // FEC packets per frame (0: a frame needs every source packet)
    double fromSeconds=30,seconds=0;              // the impairment window, from the start of the run
    double reorderShare=0,reorderMinUs=0,reorderMaxUs=0; // displaced packets: held back by a uniform delay after the queue
    double geEnter=0,geMeanBurst=0,geLossInBad=1; // Gilbert-Elliott: chance per packet to enter "bad", mean packets in it, loss there
    double capacityPps=0;std::int64_t bufferUs=0; // a bottleneck serving this many packets a second, drop-tail beyond bufferUs of queue
    double spikePerFrame=0,spikeMinUs=0,spikeMaxUs=0; // the path stalls (order kept)
    bool outage=false;                            // everything in the window is lost
    std::int64_t lateWakeUs=0;                    // the receive thread wakes this much after its wait ends
    std::int64_t decodeUs=3000,slowDecodeUs=0;    // decode time per frame; slowDecodeUs in the window
    unsigned units()const noexcept{return kSourceUnits+fecUnits;}
    std::int64_t windowStartUs()const noexcept{return kStartUs+static_cast<std::int64_t>(fromSeconds*1e6);}
    std::int64_t windowEndUs()const noexcept{return windowStartUs()+static_cast<std::int64_t>(seconds*1e6);}
    bool impaired(std::int64_t sentUs)const noexcept{return seconds>0&&sentUs>=windowStartUs()&&sentUs<windowEndUs();}
};

// The conditions of the study (reorder_study.cpp) and of the tests.
inline std::vector<Condition> studyConditions(){
    std::vector<Condition> c;
    const auto add=[&](Condition x){c.push_back(x);};
    Condition x;
    x={};x.name="clean_wired_lan";x.description="queueing delay 0.05 ms mean (cap 1 ms); no loss, no reordering";add(x);
    x={};x.name="light_reordering";x.description="60 s: 0.5 % of packets 1-4 ms late (uniform)";x.seconds=60;x.reorderShare=0.005;x.reorderMinUs=1000;x.reorderMaxUs=4000;add(x);
    x={};x.name="heavy_reordering";x.description="60 s: 5 % of packets 1-12 ms late (uniform)";x.seconds=60;x.reorderShare=0.05;x.reorderMinUs=1000;x.reorderMaxUs=12000;add(x);
    x={};x.name="burst_loss_ge2_fec";x.description="60 s: Gilbert-Elliott bursts, mean 2 packets, one starts every ~2.5 s; 3 FEC packets per frame";x.seconds=60;x.geEnter=1.0/4000;x.geMeanBurst=2;add(x);
    x={};x.name="burst_loss_ge5_fec";x.description="60 s: Gilbert-Elliott bursts, mean 5 packets, every ~2.5 s; 3 FEC packets per frame";x.seconds=60;x.geEnter=1.0/4000;x.geMeanBurst=5;add(x);
    x={};x.name="burst_loss_ge15_fec";x.description="60 s: Gilbert-Elliott bursts, mean 15 packets, every ~2.5 s; 3 FEC packets per frame";x.seconds=60;x.geEnter=1.0/4000;x.geMeanBurst=15;add(x);
    x={};x.name="burst_loss_ge2_nofec";x.description="as burst_loss_ge2_fec without FEC packets (a frame needs all 24)";x.seconds=60;x.geEnter=1.0/4000;x.geMeanBurst=2;x.fecUnits=0;add(x);
    x={};x.name="burst_loss_ge5_nofec";x.description="as burst_loss_ge5_fec without FEC packets";x.seconds=60;x.geEnter=1.0/4000;x.geMeanBurst=5;x.fecUnits=0;add(x);
    x={};x.name="congestion_queue";x.description="1.5 s: a bottleneck at 1550 packets/s (the stream needs ~1620), 100 ms buffer: its queue builds up, then drains";x.seconds=1.5;x.capacityPps=1550;x.bufferUs=100000;add(x);
    x={};x.name="congestion_with_reordering";x.description="10 s: queue as congestion_queue (1550 packets/s, 100 ms buffer) and 2 % of packets 1-10 ms late on another path";x.seconds=10;x.capacityPps=1550;x.bufferUs=100000;x.reorderShare=0.02;x.reorderMinUs=1000;x.reorderMaxUs=10000;add(x);
    x={};x.name="jitter_spikes";x.description="60 s: in 2 % of frames the path stalls 5-30 ms (order kept)";x.seconds=60;x.spikePerFrame=0.02;x.spikeMinUs=5000;x.spikeMaxUs=30000;add(x);
    x={};x.name="interruption_1s";x.description="every packet sent in 1 s is lost";x.seconds=1;x.outage=true;add(x);
    x={};x.name="interruption_5s";x.description="every packet sent in 5 s is lost";x.seconds=5;x.outage=true;add(x);
    x={};x.name="bandwidth_step";x.description="10 s: the link serves 1400 packets/s (below the stream's ~1620) with a 30 ms buffer: queue, then steady drops";x.seconds=10;x.capacityPps=1400;x.bufferUs=30000;add(x);
    x={};x.name="late_receive_thread";x.description="60 s: single packets lost (0.2 %), and the receive thread wakes 4 ms after its wait ends";x.seconds=60;x.geEnter=0.002;x.geMeanBurst=1;x.lateWakeUs=4000;add(x);
    x={};x.name="slow_decoder";x.description="60 s: decoding takes 18 ms a frame (longer than a frame interval); the link is clean";x.seconds=60;x.slowDecodeUs=18000;add(x);
    return c;
}
struct Arrival {std::int64_t atUs;std::uint32_t packet;}; // packet: index since the start, not wrapped
struct Trace {
    std::vector<Arrival> arrivals;          // sorted by time
    std::vector<std::int64_t> lostSentUs;   // send times of packets the link lost
    unsigned frames=0;
};
inline std::int64_t frameSentUs(std::int64_t frame)noexcept{return kStartUs+frame*kFrameUs;}

// firstFrame / firstPacket: where this trace continues a longer run (the
// impairment window then counts from the frame firstFrame was sent).
inline Trace makeTrace(const Condition& c,std::uint64_t seed,unsigned frames,std::int64_t firstFrame=0,std::uint32_t firstPacket=0){
    Rng rng(seed);Trace t;t.frames=frames;
    const unsigned units=c.units();
    t.arrivals.reserve(static_cast<std::size_t>(frames)*units);
    std::int64_t lastFifo=0,stallUntil=0;bool bad=false;
    const std::int64_t shift=firstFrame*kFrameUs;
    for(unsigned f=0;f<frames;++f){
        const std::int64_t frameSent=frameSentUs(firstFrame+static_cast<std::int64_t>(f));
        const bool impaired=c.impaired(frameSent-shift);
        if(impaired&&rng.chance(c.spikePerFrame))stallUntil=frameSent+static_cast<std::int64_t>(rng.between(c.spikeMinUs,c.spikeMaxUs));
        for(unsigned u=0;u<units;++u){
            const std::uint32_t packet=firstPacket+f*units+u;
            const std::int64_t sent=frameSent+static_cast<std::int64_t>(u)*kSendSpreadUs/static_cast<std::int64_t>(units);
            const bool on=c.impaired(sent-shift);
            if(on&&c.outage){t.lostSentUs.push_back(sent);continue;}
            if(on&&c.geEnter>0){
                if(bad){if(rng.chance(1.0/(std::max)(1.0,c.geMeanBurst)))bad=false;}
                else if(rng.chance(c.geEnter))bad=true;
                if(bad&&rng.chance(c.geLossInBad)){t.lostSentUs.push_back(sent);continue;}
            }else bad=false;
            // A displaced packet passes the same queue and is then held back
            // (another path behind it, a retransmitting hop): it arrives that
            // much after its place in the order, never ahead of the queue.
            const bool displaced=on&&rng.chance(c.reorderShare);
            const double jitter=(std::min)(rng.exponential(50.0),1000.0);
            const std::int64_t ready=sent+kBaseDelayUs+static_cast<std::int64_t>(jitter);
            const std::int64_t service=on&&c.capacityPps>0?static_cast<std::int64_t>(1e6/c.capacityPps):10;
            std::int64_t at=(std::max)(lastFifo+service,ready);
            if(on&&stallUntil>ready)at=(std::max)(at,stallUntil+service);
            if(on&&c.bufferUs>0&&at-ready>c.bufferUs){t.lostSentUs.push_back(sent);continue;} // drop-tail
            lastFifo=at;
            if(displaced)at+=static_cast<std::int64_t>(rng.between(c.reorderMinUs,c.reorderMaxUs));
            t.arrivals.push_back({at,packet});
        }
    }
    std::stable_sort(t.arrivals.begin(),t.arrivals.end(),[](const Arrival& a,const Arrival& b){return a.atUs<b.atUs;});
    return t;
}

struct Policy {
    std::uint32_t startUs=2000;
    bool adaptive=false;
};
struct FrameOutcome {
    std::int64_t possibleUs=-1; // the arrival of its kSourceUnits-th distinct packet; -1: the link lost too much
    std::int64_t handUs=-1;     // handed to the decoder; -1: given up
};
struct WindowChange {std::int64_t atUs;std::uint32_t windowUs;};
struct RunResult {
    std::vector<FrameOutcome> frames;
    std::vector<HealthSnapshot> intervals; // one per five seconds, as ConnectionHealth would have reported
    std::vector<WindowChange> windows;     // the wait over time (the first entry is the start)
    ReorderQueueModel::Counters counters{};
    std::uint64_t fecFailed=0;
    unsigned rollbacks=0,faults=0;
};

inline RunResult runPolicy(const Condition& c,const Trace& trace,const Policy& policy){
    const unsigned units=c.units(),frames=trace.frames;
    RunResult r;r.frames.assign(frames,FrameOutcome{});
    {
        std::vector<unsigned> have(frames,0);std::vector<std::uint8_t> seen(static_cast<std::size_t>(frames)*units,0);
        for(const auto& a:trace.arrivals){
            if(seen[a.packet])continue;
            seen[a.packet]=1;
            const unsigned f=a.packet/units;
            if(++have[f]==kSourceUnits)r.frames[f].possibleUs=a.atUs;
        }
    }
    ReorderQueueModel queue(policy.startUs);ReorderWindowController controller(policy.startUs);
    r.windows.push_back({0,policy.startUs});
    std::vector<ReorderQueueModel::Released> out;
    std::int64_t current=-1;unsigned got=0;bool handed=false;
    std::uint32_t lastPacket=0;bool anyReleased=false;std::uint16_t lastSeq=0;
    // Decoder (for the health interval only): frames decoded one after the
    // other; a queue of more than four is dropped as VideoIngress does.
    std::deque<std::int64_t> decodeStarts;std::int64_t decoderFree=0;std::size_t intervalPeak=0;
    FrameJitter jitter;std::uint64_t handedTotal=0;
    const auto decode=[&](std::int64_t at){
        while(!decodeStarts.empty()&&decodeStarts.front()<=at)decodeStarts.pop_front();
        if(decodeStarts.size()>=4){decodeStarts.clear();decoderFree=at;} // recovery: the queue is dropped
        const std::int64_t cost=c.impaired(at)&&c.slowDecodeUs>0?c.slowDecodeUs:c.decodeUs;
        const std::int64_t start=(std::max)(at,decoderFree);
        decoderFree=start+cost;
        if(start>at)decodeStarts.push_back(start);
        intervalPeak=(std::max)(intervalPeak,decodeStarts.size()+(start>at?0u:1u));
    };
    const auto release=[&](const ReorderQueueModel::Released& x){
        const std::uint32_t packet=anyReleased?lastPacket+static_cast<std::uint16_t>(x.seq-lastSeq):static_cast<std::uint32_t>(x.seq);
        anyReleased=true;lastPacket=packet;lastSeq=x.seq;
        const std::int64_t f=packet/units;
        if(f>=static_cast<std::int64_t>(frames)||f<current)return;
        if(f>current){if(current>=0&&!handed)++r.fecFailed;current=f;got=0;handed=false;}
        if(++got>=kSourceUnits&&!handed){
            handed=true;r.frames[static_cast<std::size_t>(f)].handUs=x.atUs;++handedTotal;
            jitter.observe(static_cast<std::uint64_t>(f),x.atUs*10,60);
            decode(x.atUs);
        }
    };
    const auto drain=[&]{for(const auto& x:out)release(x);out.clear();};
    // Health intervals: counters at each boundary.
    ReorderQueueModel::Counters before{};std::uint64_t failedBefore=0,handedBefore=0,linkLostBefore=0;
    std::size_t lostIndex=0,arrivalsInInterval=0;
    std::int64_t nextInterval=kStartUs+kIntervalUs,nextLook=kStartUs+kLookUs,now=0;
    const auto closeInterval=[&](std::int64_t at){
        HealthSnapshot s;s.valid=true;s.seconds=static_cast<double>(kIntervalUs)/1e6;s.sequence=r.intervals.size()+1;
        while(lostIndex<trace.lostSentUs.size()&&trace.lostSentUs[lostIndex]<at)++lostIndex;
        const std::uint64_t linkLost=lostIndex;
        s.packetsReceived=arrivalsInInterval;s.packetsLost=linkLost-linkLostBefore;
        const auto total=s.packetsReceived+s.packetsLost;
        s.lossPercent=total?100.0*static_cast<double>(s.packetsLost)/static_cast<double>(total):0.0;
        const auto& k=queue.counters();
        s.reorderTimeouts=k.timeouts-before.timeouts;s.reorderSkipped=k.skipped-before.skipped;
        s.reordered=(k.gaps-before.gaps)-s.reorderTimeouts;s.late=k.dropped-before.dropped;s.lateArrivals=k.lateArrivals-before.lateArrivals;
        s.fecFailed=r.fecFailed-failedBefore;s.networkFrameDrops=s.fecFailed;
        s.reorderOvershootMs=s.reorderTimeouts?static_cast<double>(k.overshootUs-before.overshootUs)/1000.0/static_cast<double>(s.reorderTimeouts):0.0;
        s.frameJitterMs=jitter.jitterMs();s.frameJitterPeakMs=jitter.peakMs();
        s.receivedFps=static_cast<double>(handedTotal-handedBefore)/s.seconds;
        s.ingressPeak=intervalPeak;s.ingressDepth=decodeStarts.size();
        s.decodeP95Ms=static_cast<double>(c.impaired(at-1)&&c.slowDecodeUs>0?c.slowDecodeUs:c.decodeUs)/1000.0;
        s.bottleneckSigns=bottleneckSigns(s);s.bottleneck=classifyBottleneck(s.bottleneckSigns);
        s.intervalGrade=ConnectionHealth::gradeInterval(s);s.grade=s.intervalGrade;
        r.intervals.push_back(s);
        before=k;failedBefore=r.fecFailed;handedBefore=handedTotal;linkLostBefore=linkLost;arrivalsInInterval=0;intervalPeak=0;
    };
    const auto look=[&](std::int64_t at){
        if(!policy.adaptive)return;
        ReorderEvidence e=queue.counters();e.fecFailed=r.fecFailed;
        const auto d=controller.update(at*10,e);
        if(d.changed){queue.setTimeoutUs(d.windowUs);r.windows.push_back({at,d.windowUs});}
    };
    const auto advance=[&](std::int64_t until){
        for(;;){
            const auto wake=queue.nextWakeUs(now);
            std::int64_t next=wake?*wake:(std::numeric_limits<std::int64_t>::max)();
            if(wake&&c.lateWakeUs>0&&c.impaired(*wake))next+=c.lateWakeUs; // the receive thread wakes late
            const std::int64_t boundary=(std::min)(nextLook,nextInterval);
            if(boundary<next&&boundary<=until){
                if(nextLook<=nextInterval){look(nextLook);nextLook+=kLookUs;}
                else{closeInterval(nextInterval);nextInterval+=kIntervalUs;}
                continue;
            }
            if(!wake||next>until)break;
            queue.timer(next,out);now=queue.nowUs();drain();
        }
    };
    for(const auto& a:trace.arrivals){
        advance(a.atUs);
        now=a.atUs;++arrivalsInInterval;
        queue.packet(now,static_cast<std::uint16_t>(a.packet),out);drain();
    }
    advance(frameSentUs(frames)+100000);
    r.counters=queue.counters();r.rollbacks=controller.rollbacks();r.faults=controller.faults();
    return r;
}
} // namespace veyra::remoteplay::study
