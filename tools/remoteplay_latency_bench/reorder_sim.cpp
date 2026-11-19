// SPDX-License-Identifier: GPL-3.0-only
// Linux model: what the video reorder wait costs and saves under made-up links.
//
//   reorder_sim [--seconds N] [--seeds N] > reorder_sim_results.json
//
// Replays packet arrival traces through the library's reorder-queue rule
// (include/veyra/remoteplay/ReorderQueueModel.h, compared with the C source by
// reorder_library_check.sh) for a fixed 2 ms wait, a fixed 16 ms wait and the
// adaptive controller (include/veyra/remoteplay/ReorderWindow.h, started at
// 2 ms and at 16 ms; and, for comparison only, the rule that widens on late
// packets alone), and counts per scenario:
//   - frames whose hand-over to the decoder was held by the wait, and by how
//     much (against the moment enough of the frame's packets had reached the
//     socket);
//   - frames given up although enough of their packets did arrive;
//   - frames that could not be completed at all, each of which makes the
//     library ask the console for a keyframe.
//
// Everything about the stream and the links is an assumption, written into the
// output: no PS5, no network and no Windows machine was involved, and nothing
// here predicts what a real console or LAN does. The numbers compare the three
// rules with each other on the same arrivals, nothing more.
#include "veyra/remoteplay/ReorderQueueModel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
using namespace veyra::remoteplay;
namespace {
// Assumed stream: 59.94 frames a second, every frame 24 source and 3 FEC
// packets sent evenly over its first 2 ms (about 1600 packets a second).
constexpr std::int64_t kFrameUs=16683,kSendSpreadUs=2000,kBaseDelayUs=300;
constexpr unsigned kSourceUnits=24,kFecUnits=3,kUnits=kSourceUnits+kFecUnits;
constexpr std::int64_t kDelayedUs=500;       // "held by the wait": handed over at least this much later than possible
constexpr std::int64_t kRequestSpacingUs=500000; // PushFPS sends at most one keyframe request per 500 ms

// Own generator, so that a trace does not depend on the standard library.
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed):state(seed*0x9E3779B97F4A7C15ull+0x1234567ull){}
    std::uint64_t next(){std::uint64_t z=(state+=0x9E3779B97F4A7C15ull);z=(z^(z>>30))*0xBF58476D1CE4E5B9ull;z=(z^(z>>27))*0x94D049BB133111EBull;return z^(z>>31);}
    double unit(){return static_cast<double>(next()>>11)*(1.0/9007199254740992.0);} // [0,1)
    bool chance(double p){return p>0&&unit()<p;}
    double between(double low,double high){return low+(high-low)*unit();}
    double exponential(double mean){return mean<=0?0:-mean*std::log(1.0-unit());}
};
// What a link does to a packet sent at a given time.
struct Link {
    double jitterMeanUs=50,jitterCapUs=1000; // queueing delay, order kept
    double loss=0;                           // independent loss
    double burstStart=0,burstMeanPackets=0;  // loss bursts: chance to start per packet, mean length
    double reorder=0,reorderMinUs=0,reorderMaxUs=0; // a packet takes a slower path and overtakes nothing
};
struct Scenario {
    const char* name;
    const char* description;
    Link normal;
    Link disturbed;                  // used between disturbedFrom and disturbedTo (fractions of the run)
    double disturbedFrom=2,disturbedTo=2;
};
const Scenario kScenarios[]={
    {"clean_lan","queueing delay 0.05 ms mean; no loss, no reordering",
        {50,1000,0,0,0,0,0,0},{},2,2},
    {"sustained_light_jitter","queueing delay 0.4 ms mean (cap 3 ms); loss 0.01 %; 0.05 % of packets 0.2-1.5 ms late",
        {400,3000,0.0001,0,0,0.0005,200,1500},{},2,2},
    {"temporary_congestion","as clean_lan, but for the middle sixth of the run: queueing delay 3 ms mean (cap 20 ms), loss 1 %, 2 % of packets 1-10 ms late",
        {50,1000,0,0,0,0,0,0},{3000,20000,0.01,0,0,0.02,1000,10000},0.25,0.4167},
    {"pure_reordering","no loss; 2 % of packets 0.2-5 ms late",
        {50,1000,0,0,0,0.02,200,5000},{},2,2},
    {"burst_loss","no reordering; loss bursts of 12 packets on average, about one every 5 s",
        {50,1000,0,1.0/8000.0,12,0,0,0},{},2,2},
    {"mixed","queueing delay 0.3 ms mean (cap 3 ms); loss 0.3 %; bursts of 10 packets about every 12 s; 1 % of packets 0.2-6 ms late",
        {300,3000,0.003,1.0/20000.0,10,0.01,200,6000},{},2,2},
};
struct Arrival {std::int64_t atUs;std::uint32_t packet;}; // packet: index since the start, not wrapped
std::vector<Arrival> makeTrace(const Scenario& s,std::uint64_t seed,unsigned frames){
    Rng rng(seed);std::vector<Arrival> out;out.reserve(static_cast<std::size_t>(frames)*kUnits);
    std::int64_t fifo=0;unsigned burstLeft=0;
    for(unsigned f=0;f<frames;++f){
        const double where=static_cast<double>(f)/static_cast<double>(frames);
        const Link& link=(where>=s.disturbedFrom&&where<s.disturbedTo)?s.disturbed:s.normal;
        for(unsigned u=0;u<kUnits;++u){
            const std::uint32_t packet=f*kUnits+u;
            const std::int64_t sent=1000000+static_cast<std::int64_t>(f)*kFrameUs+static_cast<std::int64_t>(u)*kSendSpreadUs/kUnits;
            if(burstLeft==0&&rng.chance(link.burstStart))burstLeft=1+static_cast<unsigned>(rng.exponential(link.burstMeanPackets));
            if(burstLeft>0){--burstLeft;continue;}
            if(rng.chance(link.loss))continue;
            if(rng.chance(link.reorder)){
                out.push_back({sent+kBaseDelayUs+static_cast<std::int64_t>(rng.between(link.reorderMinUs,link.reorderMaxUs)),packet});
                continue;
            }
            const double jitter=(std::min)(rng.exponential(link.jitterMeanUs),link.jitterCapUs);
            fifo=(std::max)(fifo+10,sent+kBaseDelayUs+static_cast<std::int64_t>(jitter));
            out.push_back({fifo,packet});
        }
    }
    std::stable_sort(out.begin(),out.end(),[](const Arrival& a,const Arrival& b){return a.atUs<b.atUs;});
    return out;
}
struct Totals {
    std::uint64_t frames=0,delivered=0,lostEarly=0,lostLink=0,keyframeEpisodes=0;
    std::uint64_t gaps=0,timeouts=0,lateArrivals=0,windowChanges=0,rollbacks=0,faults=0;
    std::vector<std::int64_t> added;        // every delivered frame
    std::map<std::uint32_t,double> secondsAt; // adaptive: time spent at each wait
    std::uint32_t finalWindowUs=0;
};
enum class Policy {Fixed2,Fixed16,AdaptiveFrom2,AdaptiveFrom16,LateOnlyFrom2};
constexpr Policy kPolicies[]={Policy::Fixed2,Policy::Fixed16,Policy::AdaptiveFrom2,Policy::AdaptiveFrom16,Policy::LateOnlyFrom2};
constexpr std::size_t kPolicyCount=sizeof(kPolicies)/sizeof(kPolicies[0]);
const char* policyName(Policy p){
    return p==Policy::Fixed2?"fixed_2ms":p==Policy::Fixed16?"fixed_16ms":p==Policy::AdaptiveFrom2?"adaptive_from_2ms":p==Policy::AdaptiveFrom16?"adaptive_from_16ms":"comparison_widen_on_late_packets_alone_from_2ms";
}
bool isAdaptive(Policy p){return p!=Policy::Fixed2&&p!=Policy::Fixed16;}
void run(const std::vector<Arrival>& trace,unsigned frames,Policy policy,Totals& t){
    const std::uint32_t start=(policy==Policy::Fixed16||policy==Policy::AdaptiveFrom16)?16000u:2000u;
    const bool adaptive=isAdaptive(policy);
    ReorderQueueModel queue(start);ReorderWindowController controller(start,policy!=Policy::LateOnlyFrom2);
    std::uint64_t fecFailed=0; // frames begun and not completed: what the library counts as FEC failures
    std::vector<ReorderQueueModel::Released> out;
    // When could each frame first have been completed: the arrival of its
    // kSourceUnits-th distinct packet (any order: what an order-free FEC
    // assembly would need). -1: never, the link lost too much of it.
    std::vector<std::int64_t> possible(frames,-1);
    {
        std::vector<unsigned> have(frames,0);std::vector<std::uint8_t> seen(static_cast<std::size_t>(frames)*kUnits,0);
        for(const auto& a:trace){
            if(seen[a.packet])continue;
            seen[a.packet]=1;
            const unsigned f=a.packet/kUnits;
            if(++have[f]==kSourceUnits)possible[f]=a.atUs;
        }
    }
    std::vector<std::uint8_t> deliveredFrame(frames,0);
    // The frame processor: one frame at a time; handed over when enough units
    // have come out of the reorder queue; given up when the next frame starts.
    std::int64_t current=-1;unsigned units=0;bool handed=false;
    std::uint32_t lastPacket=0;bool anyReleased=false;std::uint16_t lastSeq=0;
    const auto release=[&](const ReorderQueueModel::Released& r){
        const std::uint32_t packet=anyReleased?lastPacket+static_cast<std::uint16_t>(r.seq-lastSeq):static_cast<std::uint32_t>(r.seq);
        anyReleased=true;lastPacket=packet;lastSeq=r.seq;
        const std::int64_t f=packet/kUnits;
        if(f>=static_cast<std::int64_t>(frames)||f<current)return;
        if(f>current){if(current>=0&&!handed)++fecFailed;current=f;units=0;handed=false;}
        if(++units>=kSourceUnits&&!handed){
            handed=true;deliveredFrame[static_cast<std::size_t>(f)]=1;
            t.added.push_back(r.atUs-possible[static_cast<std::size_t>(f)]);
        }
    };
    std::int64_t now=0,nextLook=1250000,windowSince=1000000;
    const auto drain=[&]{for(const auto& r:out)release(r);out.clear();};
    const auto look=[&](std::int64_t at){
        if(!adaptive)return;
        const auto before=controller.windowUs();
        ReorderEvidence evidence=queue.counters();evidence.fecFailed=fecFailed;
        const auto d=controller.update(at*10,evidence);
        if(d.changed){
            t.secondsAt[before]+=static_cast<double>(at-windowSince)/1e6;windowSince=at;
            queue.setTimeoutUs(d.windowUs);++t.windowChanges;
        }
    };
    const auto advance=[&](std::int64_t until){
        for(;;){
            const auto wake=queue.nextWakeUs(now);
            const std::int64_t next=wake?*wake:INT64_MAX;
            // The owner's look is not the Takion thread's: `now` stays the time of
            // that thread's last look at its queue, which its timed wait counts from.
            if(nextLook<next&&nextLook<=until){look(nextLook);nextLook+=250000;continue;}
            if(!wake||*wake>until)break;
            queue.timer(*wake,out);now=queue.nowUs();drain();
        }
    };
    for(const auto& a:trace){
        advance(a.atUs);
        now=a.atUs;
        queue.packet(now,static_cast<std::uint16_t>(a.packet),out);drain();
    }
    const std::int64_t end=1000000+static_cast<std::int64_t>(frames)*kFrameUs+100000;
    advance(end);
    if(adaptive)t.secondsAt[controller.windowUs()]+=static_cast<double>(end-windowSince)/1e6;
    std::int64_t lastRequest=-kRequestSpacingUs;
    for(unsigned f=0;f<frames;++f){
        ++t.frames;
        if(deliveredFrame[f]){++t.delivered;continue;}
        if(possible[f]>=0)++t.lostEarly;else ++t.lostLink;
        const std::int64_t at=1000000+static_cast<std::int64_t>(f)*kFrameUs;
        if(at-lastRequest>=kRequestSpacingUs){++t.keyframeEpisodes;lastRequest=at;}
    }
    t.gaps+=queue.counters().gaps;t.timeouts+=queue.counters().timeouts;t.lateArrivals+=queue.counters().lateArrivals;
    t.faults+=controller.faults();t.rollbacks+=controller.rollbacks();t.finalWindowUs=adaptive?controller.windowUs():start;
}
double percentile(std::vector<std::int64_t>& sorted,double p){
    if(sorted.empty())return 0;
    const auto rank=static_cast<std::size_t>(std::ceil(p*static_cast<double>(sorted.size())));
    return static_cast<double>(sorted[(std::min)(sorted.size()-1,rank?rank-1:0)])/1000.0;
}
void print(const Totals& in,Policy policy,bool last){
    Totals t=in;
    std::sort(t.added.begin(),t.added.end());
    std::vector<std::int64_t> held;
    for(const auto v:t.added)if(v>=kDelayedUs)held.push_back(v);
    double sum=0;for(const auto v:t.added)sum+=static_cast<double>(v);
    std::printf("      \"%s\": {\"frames\": %llu, \"delivered\": %llu, \"framesHeldByTheWait\": %llu, "
        "\"heldDelayMs\": {\"median\": %.2f, \"p95\": %.2f, \"p99\": %.2f}, "
        "\"allFramesAddedDelayMs\": {\"mean\": %.4f, \"p99\": %.2f, \"p999\": %.2f, \"max\": %.2f}, "
        "\"framesGivenUpTooEarly\": %llu, \"framesTheLinkLost\": %llu, \"keyframeRequestsImplied\": %llu, \"keyframeEpisodes500ms\": %llu, "
        "\"reorderGaps\": %llu, \"reorderTimeouts\": %llu, \"lateArrivals\": %llu",
        policyName(policy),static_cast<unsigned long long>(t.frames),static_cast<unsigned long long>(t.delivered),static_cast<unsigned long long>(held.size()),
        percentile(held,0.5),percentile(held,0.95),percentile(held,0.99),
        t.added.empty()?0.0:sum/static_cast<double>(t.added.size())/1000.0,percentile(t.added,0.99),percentile(t.added,0.999),percentile(t.added,1.0),
        static_cast<unsigned long long>(t.lostEarly),static_cast<unsigned long long>(t.lostLink),static_cast<unsigned long long>(t.lostEarly+t.lostLink),
        static_cast<unsigned long long>(t.keyframeEpisodes),
        static_cast<unsigned long long>(t.gaps),static_cast<unsigned long long>(t.timeouts),static_cast<unsigned long long>(t.lateArrivals));
    if(isAdaptive(policy)){
        std::printf(", \"windowChanges\": %llu, \"rollbacks\": %llu, \"controllerFaults\": %llu, \"finalWindowUsLastSeed\": %u, \"secondsAtWindowUs\": {",
            static_cast<unsigned long long>(t.windowChanges),static_cast<unsigned long long>(t.rollbacks),static_cast<unsigned long long>(t.faults),t.finalWindowUs);
        bool first=true;
        for(const auto& [us,seconds]:t.secondsAt){std::printf("%s\"%u\": %.1f",first?"":", ",us,seconds);first=false;}
        std::printf("}");
    }
    std::printf("}%s\n",last?"":",");
}
}
int main(int argc,char** argv){
    unsigned seconds=120,seeds=5;
    for(int i=1;i+1<argc;i+=2){
        if(!std::strcmp(argv[i],"--seconds"))seconds=static_cast<unsigned>(std::strtoul(argv[i+1],nullptr,10));
        else if(!std::strcmp(argv[i],"--seeds"))seeds=static_cast<unsigned>(std::strtoul(argv[i+1],nullptr,10));
        else{std::fprintf(stderr,"usage: reorder_sim [--seconds N] [--seeds N]\n");return 2;}
    }
    if(seconds<10||seconds>3600||seeds<1||seeds>50||argc%2==0){std::fprintf(stderr,"usage: reorder_sim [--seconds 10..3600] [--seeds 1..50]\n");return 2;}
    const unsigned frames=static_cast<unsigned>(static_cast<std::int64_t>(seconds)*1000000/kFrameUs);
    std::printf("{\n  \"label\": \"Linux model\",\n"
        "  \"what\": \"The library's video reorder-queue rule (model checked against the C source) replayed on made-up packet arrival traces. No PS5, no network, no Windows machine. Compares the rules with each other; predicts nothing about a real console or LAN.\",\n"
        "  \"assumptions\": {\"framesPerSecond\": 59.94, \"sourcePacketsPerFrame\": %u, \"fecPacketsPerFrame\": %u, \"frameSentOverMs\": %.1f, \"baseDelayMs\": %.1f, "
        "\"frameHandedOverWhen\": \"%u of its packets have left the reorder queue; given up when the next frame's first packet leaves it\", "
        "\"addedDelay\": \"hand-over minus the arrival of the frame's %u-th distinct packet at the socket\", "
        "\"heldByTheWait\": \"added delay of at least %.1f ms\", "
        "\"keyframeRequestsImplied\": \"one per frame not handed over (the library asks on every frame FEC cannot complete); keyframeEpisodes500ms merges those within 500 ms, PushFPS's own request spacing\", "
        "\"adaptive\": \"ReorderWindowController read every 250 ms; from_2ms as with VEYRA_EXP_RP_LAN_PROFILE, from_16ms as with the adaptive switch alone; comparison_widen_on_late_packets_alone is the rule without the lost-frame condition and the rollback, which the app does not use\"},\n"
        "  \"secondsPerSeed\": %u, \"seeds\": %u,\n  \"scenarios\": {\n",
        kSourceUnits,kFecUnits,static_cast<double>(kSendSpreadUs)/1000.0,static_cast<double>(kBaseDelayUs)/1000.0,kSourceUnits,kSourceUnits,static_cast<double>(kDelayedUs)/1000.0,seconds,seeds);
    const std::size_t count=sizeof(kScenarios)/sizeof(kScenarios[0]);
    for(std::size_t s=0;s<count;++s){
        std::printf("    \"%s\": {\n      \"link\": \"%s\",\n",kScenarios[s].name,kScenarios[s].description);
        Totals totals[kPolicyCount];
        for(unsigned seed=1;seed<=seeds;++seed){
            const auto trace=makeTrace(kScenarios[s],1000u*static_cast<unsigned>(s+1)+seed,frames);
            for(std::size_t p=0;p<kPolicyCount;++p)run(trace,frames,kPolicies[p],totals[p]);
        }
        for(std::size_t p=0;p<kPolicyCount;++p)print(totals[p],kPolicies[p],p+1==kPolicyCount);
        std::printf("    }%s\n",s+1==count?"":",");
    }
    std::printf("  }\n}\n");
    return 0;
}
