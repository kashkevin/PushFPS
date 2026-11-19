// SPDX-License-Identifier: GPL-3.0-only
// The other half of reorder_library_check.sh: replays a packet arrival trace
// through include/veyra/remoteplay/ReorderQueueModel.h and prints the same
// lines as reorder_library_check.c does from the library itself.
//   reorder_replay --generate SEED   writes a trace (reordering, loss, bursts,
//                                    duplicates, live wait changes, index wrap)
//   reorder_replay --model           reads a trace on stdin
// Trace format: see reorder_library_check.c.
#include "veyra/remoteplay/ReorderQueueModel.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
using namespace veyra::remoteplay;
namespace {
int generate(unsigned seed){
    std::mt19937_64 rng(seed);
    const auto pick=[&](std::uint64_t n){return static_cast<std::uint64_t>(rng()%n);};
    struct Event {std::uint64_t at;char kind;std::uint64_t value;};
    std::vector<Event> events;
    static constexpr std::uint32_t waits[]={2000,3000,4000,6000,8000,12000,16000,500,20000};
    std::printf("S %u\n",static_cast<unsigned>(seed%3==0?0u:waits[seed%7]));
    std::uint64_t sent=1000000; // the library uses 0 for "not waiting"
    std::uint16_t index=static_cast<std::uint16_t>(65000u+seed%400u); // wraps early
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
        events.push_back({at,'P',index});
        if(pick(700)==0)events.push_back({at+pick(3000),'P',index}); // a duplicate
        if(pick(3000)==0)events.push_back({sent,'W',waits[pick(9)]});
    }
    std::stable_sort(events.begin(),events.end(),[](const Event& a,const Event& b){return a.at<b.at;});
    for(const auto& e:events)std::printf("%c %llu %llu\n",e.kind,static_cast<unsigned long long>(e.at),static_cast<unsigned long long>(e.value));
    return 0;
}
int model(){
    char kind=0;unsigned long long a=0,b=0;
    if(std::scanf(" %c %llu",&kind,&a)!=2||kind!='S')return 2;
    ReorderQueueModel queue(a?static_cast<std::uint32_t>(a):16000u);
    std::vector<ReorderQueueModel::Released> out;
    std::int64_t now=0;
    const auto print=[&]{for(const auto& r:out)std::printf("R %u %lld\n",static_cast<unsigned>(r.seq),static_cast<long long>(r.atUs));out.clear();};
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
        else if(kind=='P'){queue.packet(now,static_cast<std::uint16_t>(b),out);print();}
        else return 2;
    }
    timers(INT64_MAX/2);
    const auto& c=queue.counters();
    std::printf("C videoPackets=%llu gaps=%llu timeouts=%llu skipped=%llu dropped=%llu late=%llu overshootUs=%llu windowUs=%u lateness=",
        static_cast<unsigned long long>(c.videoPackets),static_cast<unsigned long long>(c.gaps),static_cast<unsigned long long>(c.timeouts),
        static_cast<unsigned long long>(c.skipped),static_cast<unsigned long long>(c.dropped),static_cast<unsigned long long>(c.lateArrivals),
        static_cast<unsigned long long>(c.overshootUs),static_cast<unsigned>(c.reorderTimeoutUs));
    for(std::size_t i=0;i<c.lateness.size();++i)std::printf("%s%llu",i?",":"",static_cast<unsigned long long>(c.lateness[i]));
    std::printf("\n");
    return 0;
}
}
int main(int argc,char** argv){
    if(argc==3&&!std::strcmp(argv[1],"--generate"))return generate(static_cast<unsigned>(std::strtoul(argv[2],nullptr,10)));
    if(argc==2&&!std::strcmp(argv[1],"--model"))return model();
    std::fprintf(stderr,"usage: reorder_replay --generate SEED | --model < trace\n");
    return 2;
}
