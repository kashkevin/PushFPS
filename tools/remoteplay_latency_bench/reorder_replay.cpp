// SPDX-License-Identifier: GPL-3.0-only
// The other half of reorder_library_check.sh: replays a packet arrival trace
// through include/veyra/remoteplay/ReorderQueueModel.h and prints the same
// lines as reorder_library_check.c does from the library itself, with the
// per-frame late-packet evidence (LateFrameEvidenceModel).
//   reorder_replay --generate SEED   writes a trace (reordering, loss, bursts,
//                                    duplicates, live wait changes, index and
//                                    frame index wrap, frames of 6-60 packets)
//   reorder_replay --study NAME SEED [SECONDS]
//                                    writes the trace of a study condition
//                                    (ReorderStudy.h, Wi-Fi ones included)
//   reorder_replay --model           reads a trace on stdin
// Trace format: see reorder_library_check.c.
#include "veyra/remoteplay/ReorderQueueModel.h"
#include "veyra/remoteplay/ReorderStudy.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
using namespace veyra::remoteplay;
namespace {
void print(char kind,std::uint64_t at,std::uint64_t value,const std::array<std::uint64_t,4>& frame){
    if(kind=='P')std::printf("P %llu %llu %llu %llu %llu %llu\n",static_cast<unsigned long long>(at),static_cast<unsigned long long>(value),
        static_cast<unsigned long long>(frame[0]),static_cast<unsigned long long>(frame[1]),static_cast<unsigned long long>(frame[2]),static_cast<unsigned long long>(frame[3]));
    else std::printf("%c %llu %llu\n",kind,static_cast<unsigned long long>(at),static_cast<unsigned long long>(value));
}
int generate(unsigned seed){
    std::mt19937_64 rng(seed);
    const auto pick=[&](std::uint64_t n){return static_cast<std::uint64_t>(rng()%n);};
    struct Event {std::uint64_t at;char kind;std::uint64_t value;std::array<std::uint64_t,4> frame{};};
    std::vector<Event> events;
    static constexpr std::uint32_t waits[]={2000,3000,4000,6000,8000,12000,16000,500,20000};
    std::printf("S %u\n",static_cast<unsigned>(seed%3==0?0u:waits[seed%7]));
    std::printf("O %u\n",static_cast<unsigned>(seed%4)); // CHIAKI_VEYRA_OPTION_*: none, wrap fix, early loss, both
    std::uint64_t sent=1000000; // the library uses 0 for "not waiting"
    std::uint16_t index=static_cast<std::uint16_t>(65000u+seed%400u); // wraps early
    // Frames: a generator of its own, so the packet times are those of the
    // traces before the frame fields were added. Packet i (counting bursts)
    // belongs to frame frameOf[i]; the frame index wraps early too.
    std::mt19937_64 layout(seed*7919u+1u);
    std::vector<std::uint32_t> frameOf,unitOf;std::vector<std::uint16_t> frameTotal,frameFec;
    for(std::uint32_t f=0;frameOf.size()<62000;++f){
        const auto total=static_cast<std::uint16_t>(6+layout()%55),fec=static_cast<std::uint16_t>(layout()%(total/4+1));
        frameTotal.push_back(total);frameFec.push_back(fec);
        for(unsigned u=0;u<total;++u){frameOf.push_back(f);unitOf.push_back(u);}
    }
    const std::uint32_t frameBase=65500u+seed%30u;
    const auto frameFields=[&](unsigned i){
        const std::size_t k=(std::min)(static_cast<std::size_t>(i),frameOf.size()-1);
        const std::uint32_t f=frameOf[k];
        return std::array<std::uint64_t,4>{static_cast<std::uint16_t>(frameBase+f),frameTotal[f],frameFec[f],unitOf[k]};
    };
    const unsigned reorderIn=static_cast<unsigned>(20+pick(200)),lossIn=static_cast<unsigned>(50+pick(400));
    for(unsigned i=0;i<60000;++i,++index){
        sent+=20+pick(300);
        if(pick(4000)==0)sent+=pick(40000); // a pause
        if(pick(lossIn)==0){
            if(pick(6)==0){const unsigned burst=static_cast<unsigned>(1+pick(90));i+=burst;index=static_cast<std::uint16_t>(index+burst);} // a burst, sometimes past the queue's 64 slots
            continue;
        }
        std::uint64_t at=sent;
        if(pick(reorderIn)==0)at+=200+pick(pick(8)==0?40000:7000);
        events.push_back({at,'P',index,frameFields(i)});
        if(pick(700)==0)events.push_back({at+pick(3000),'P',index,frameFields(i)}); // a duplicate
        if(pick(3000)==0)events.push_back({sent,'W',waits[pick(9)]});
    }
    std::stable_sort(events.begin(),events.end(),[](const Event& a,const Event& b){return a.at<b.at;});
    for(const auto& e:events)print(e.kind,e.at,e.value,e.frame);
    return 0;
}
// Hand-made traces for the reviewers' cases (reorder_library_check.sh checks the outcome too).
int caseTrace(const char* name){
    if(!std::strcmp(name,"late_duplicate")||!std::strcmp(name,"late_duplicate_once")){
        // Frame 7: 6 packets, 1 FEC (5 needed). Packets 0-2 in order, 3 late (given up, then
        // arrives, twice unless "_once"), 4-5 lost; frame 8 starts after. It can never be
        // completed (4 of 5 at most): no frame lost to a late packet.
        std::printf("S 2000\nO 0\n");
        std::uint64_t t=1000000;
        for(unsigned u=0;u<3;++u)print('P',t+=100,100+u,{7,6,1,u});
        print('P',t+=100,106,{8,6,1,0});        // the next frame: packets 103-105 are missing
        print('P',t+=100,107,{8,6,1,1});
        t+=5000;print('P',t,108,{8,6,1,2});      // the 2 ms wait has run out: 103-105 given up
        print('P',t+=100,103,{7,6,1,3});         // late
        if(!std::strcmp(name,"late_duplicate"))print('P',t+=100,103,{7,6,1,3}); // the same packet again
        for(unsigned u=3;u<6;++u)print('P',t+=100,106+u,{8,6,1,u});
        return 0;
    }
    if(!std::strcmp(name,"wrap")||!std::strcmp(name,"wrap_fixed")){
        // The queue ends exactly at 65535 when packet 0 arrives in time (65534 is late).
        std::printf("S 4000\nO %u\n",!std::strcmp(name,"wrap_fixed")?1u:0u);
        const std::uint64_t at[]={1000000,1000100,1000200,1000300};
        const unsigned idx[]={65533,65535,0,65534};
        for(unsigned i=0;i<4;++i)print('P',at[i],idx[i],{static_cast<std::uint64_t>(idx[i]==0?1:0),27,3,static_cast<std::uint64_t>(idx[i]==0?0:idx[i]-65533)});
        return 0;
    }
    std::fprintf(stderr,"unknown case %s\n",name);
    return 2;
}
int studyTrace(const char* name,unsigned seed,double seconds){
    auto conditions=study::studyConditions();
    for(const auto& w:study::wifiConditions())conditions.push_back(w);
    for(const auto& c:conditions){
        if(std::strcmp(c.name,name)!=0)continue;
        static constexpr std::uint32_t waits[]={2000,4000,8000,16000};
        std::printf("S %u\nO %u\n",static_cast<unsigned>(waits[seed%4]),static_cast<unsigned>(seed%4));
        const auto frames=static_cast<unsigned>(seconds*1e6/static_cast<double>(study::kFrameUs));
        const auto trace=study::makeTrace(c,seed,frames);
        const unsigned units=c.units();
        for(const auto& a:trace.arrivals)
            print('P',static_cast<std::uint64_t>(a.atUs),static_cast<std::uint16_t>(a.packet),{static_cast<std::uint16_t>(a.packet/units),units,c.fecUnits,a.packet%units});
        return 0;
    }
    std::fprintf(stderr,"unknown condition %s\n",name);
    return 2;
}
int model(){
    char kind=0;unsigned long long a=0,b=0;
    if(std::scanf(" %c %llu",&kind,&a)!=2||kind!='S')return 2;
    unsigned long long options=0;
    if(std::scanf(" %c %llu",&kind,&options)!=2||kind!='O')return 2;
    ReorderQueueModel::Options o;o.wrapFix=(options&1u)!=0;o.earlyLoss=(options&2u)!=0;
    ReorderQueueModel queue(a?static_cast<std::uint32_t>(a):16000u,o);
    std::vector<ReorderQueueModel::Released> out;
    std::int64_t now=0;
    // Frame fields by packet index (the queue releases indexes only); the
    // same stand-in for the video receiver as reorder_library_check.c.
    struct Fields {std::uint16_t frame=0,total=0,fec=0;};
    std::array<Fields,65536> fields{};
    LateFrameEvidenceModel evidence;std::uint64_t fecFailed=0;
    bool started=false,handed=false;std::uint16_t current=0;unsigned released=0;
    const auto gt16=[](std::uint16_t x,std::uint16_t y){return x!=y&&static_cast<std::uint16_t>(x-y)<32768u;};
    const auto print=[&]{
        for(const auto& r:out){
            std::printf("R %u %lld\n",static_cast<unsigned>(r.seq),static_cast<long long>(r.atUs));
            const Fields f=fields[r.seq];
            evidence.unit(f.frame,f.total,f.fec,false);
            if(started&&gt16(current,f.frame))continue;
            if(!started||gt16(f.frame,current)){
                if(started&&!handed){++fecFailed;evidence.failed(current);}
                started=true;current=f.frame;released=0;handed=false;
            }
            if(++released>=static_cast<unsigned>(f.total-f.fec))handed=true;
        }
        out.clear();
    };
    const auto timers=[&](std::int64_t until){
        for(;;){
            const auto wake=queue.nextWakeUs(now);
            if(!wake||*wake>until)return;
            queue.timer(*wake,out);now=queue.nowUs();print();
        }
    };
    while(std::scanf(" %c %llu %llu",&kind,&a,&b)==3){
        timers(static_cast<std::int64_t>(a));
        now=static_cast<std::int64_t>(a);
        if(kind=='W')queue.setTimeoutUs(static_cast<std::uint32_t>(b));
        else if(kind=='P'){
            unsigned long long frame=0,total=0,fec=0,unit=0;
            if(std::scanf("%llu %llu %llu %llu",&frame,&total,&fec,&unit)!=4)return 2;
            const auto seq=static_cast<std::uint16_t>(b);
            const Fields f{static_cast<std::uint16_t>(frame),static_cast<std::uint16_t>(total),static_cast<std::uint16_t>(fec)};
            const ReorderQueueModel::Unit u{f.frame,static_cast<std::uint16_t>(unit),f.total,f.fec};
            // A packet given up on keeps its fields: a late copy has the same ones.
            if(queue.packet(now,seq,out,u))evidence.unit(f.frame,f.total,f.fec,true);
            fields[seq]=f;
            print();
        }
        else return 2;
    }
    timers(INT64_MAX/2);
    const auto& c=queue.counters();
    std::printf("C videoPackets=%llu gaps=%llu timeouts=%llu skipped=%llu dropped=%llu late=%llu overshootUs=%llu windowUs=%u fecFailed=%llu lateFramesFailed=%llu earlyCuts=%llu earlyWrong=%llu lateness=",
        static_cast<unsigned long long>(c.videoPackets),static_cast<unsigned long long>(c.gaps),static_cast<unsigned long long>(c.timeouts),
        static_cast<unsigned long long>(c.skipped),static_cast<unsigned long long>(c.dropped),static_cast<unsigned long long>(c.lateArrivals),
        static_cast<unsigned long long>(c.overshootUs),static_cast<unsigned>(c.reorderTimeoutUs),
        static_cast<unsigned long long>(fecFailed),static_cast<unsigned long long>(evidence.lateFramesFailed()),
        static_cast<unsigned long long>(c.earlyCuts),static_cast<unsigned long long>(c.earlyWrong));
    for(std::size_t i=0;i<c.lateness.size();++i)std::printf("%s%llu",i?",":"",static_cast<unsigned long long>(c.lateness[i]));
    std::printf("\n");
    return 0;
}
}
int main(int argc,char** argv){
    if(argc==3&&!std::strcmp(argv[1],"--generate"))return generate(static_cast<unsigned>(std::strtoul(argv[2],nullptr,10)));
    if(argc==2&&!std::strcmp(argv[1],"--model"))return model();
    if(argc==3&&!std::strcmp(argv[1],"--case"))return caseTrace(argv[2]);
    if((argc==4||argc==5)&&!std::strcmp(argv[1],"--study"))return studyTrace(argv[2],static_cast<unsigned>(std::strtoul(argv[3],nullptr,10)),argc==5?std::strtod(argv[4],nullptr):120.0);
    std::fprintf(stderr,"usage: reorder_replay --generate SEED | --study NAME SEED [SECONDS] | --model < trace\n");
    return 2;
}
