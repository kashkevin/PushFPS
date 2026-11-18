// SPDX-License-Identifier: GPL-3.0-only
// Input dispatch model for PS5 Remote Play: time from a pad state change being
// readable on the PC to chiaki_session_set_controller_state() being called.
// A discrete-event simulation in virtual time around the real ControllerQueue.
// It is a model of the two threads' schedules, not a measurement of Windows:
//
//   original : UI polls the pad every 8 ms; the owner loop wakes every
//              `--owner-sleep-ms` (Sleep(1) is 1-2 ms on Windows), decodes a
//              frame when one is there, and sends one queued state per 4 ms tick.
//   new      : UI polls every 4 ms; a changed state wakes the owner, which
//              sends everything pending unless it is inside a decode submit.
//
// Every edge must reach the console exactly once and in order; the run fails otherwise.
#include "veyra/remoteplay/ControllerQueue.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace veyra::remoteplay;
namespace {
using Us=long long; // virtual microseconds
struct Dist{std::vector<double> v;
    void add(double x){v.push_back(x);}
    double mean()const{double s=0;for(double x:v)s+=x;return v.empty()?0:s/double(v.size());}
    double pct(double p){if(v.empty())return 0;std::sort(v.begin(),v.end());return v[std::min(v.size()-1,std::size_t(p*double(v.size())))];}
};
}
int main(int argc,char** argv){
    std::string mode="original";int edges=20000;double ownerSleepMs=1.5,decodeMs=1.0,wakeMs=0.1,pollMs=4;bool motion=true;
    for(int i=1;i<argc;++i){
        if(!std::strcmp(argv[i],"--mode")&&i+1<argc)mode=argv[++i];
        else if(!std::strcmp(argv[i],"--edges")&&i+1<argc)edges=std::atoi(argv[++i]);
        else if(!std::strcmp(argv[i],"--owner-sleep-ms")&&i+1<argc)ownerSleepMs=std::atof(argv[++i]);
        else if(!std::strcmp(argv[i],"--decode-ms")&&i+1<argc)decodeMs=std::atof(argv[++i]);
        else if(!std::strcmp(argv[i],"--wake-ms")&&i+1<argc)wakeMs=std::atof(argv[++i]);
        else if(!std::strcmp(argv[i],"--poll-ms")&&i+1<argc)pollMs=std::clamp(std::atof(argv[++i]),1.0,8.0); // new mode: VEYRA_EXP_RP_PAD_POLL_MS
        else if(!std::strcmp(argv[i],"--no-motion"))motion=false;
    }
    const bool original=mode=="original";
    const Us pollUs=original?8000:Us(pollMs*1000);
    std::mt19937_64 rng(807);
    std::uniform_real_distribution<double> gapMs(40,400),unit(0,1);
    // Button edges: alternate press / release of Cross at human intervals.
    std::vector<Us> edgeAt;edgeAt.reserve(std::size_t(edges));
    Us t=100000;for(int i=0;i<edges;++i){t+=Us(gapMs(rng)*1000);edgeAt.push_back(t);}
    const Us end=t+500000;
    ControllerQueue queue(original?ControllerQueue::Policy::Tick:ControllerQueue::Policy::Immediate);
    // UI poll schedule with up to 0.3 ms of timer jitter.
    Us nextPoll=0,nextPollAt=0;std::size_t edgeSeen=0;std::uint32_t buttons=0;std::int16_t stick=0;
    std::vector<Us> polledAt(std::size_t(edges),-1);
    Us wakeAt=-1; // new: earliest time a push asks the owner to run
    auto poll=[&](Us upTo){
        while(nextPollAt<=upTo){
            const Us at=nextPollAt;
            while(edgeSeen<edgeAt.size()&&edgeAt[edgeSeen]<=at){buttons^=ControllerState::Cross;polledAt[edgeSeen]=at;++edgeSeen;}
            ControllerState s;s.inputActive=true;s.buttons=buttons;
            if(motion){s.leftX=++stick;s.motionValid=true;s.motionTimestampUs=std::uint64_t(at);} // stick/gyro change on every poll
            if(queue.push(at*10,s)&&!original&&(wakeAt<0||at<wakeAt))wakeAt=at;
            nextPoll+=pollUs;nextPollAt=nextPoll+Us(unit(rng)*300);
        }
    };
    // Video: 59.94 fps, +-1.5 ms; each frame costs the owner `decodeMs` of submit time.
    Us nextFrame=16683;auto frameJitter=[&]{return Us((unit(rng)*3.0-1.5)*1000);};Us frameAt=nextFrame+frameJitter();
    Dist sampling,dispatch,total;std::size_t delivered=0;std::uint32_t consoleButtons=0;bool orderOk=true;std::uint64_t submits=0;
    auto submit=[&](Us now,const ControllerState& s){
        ++submits;
        if(s.buttons!=consoleButtons){
            // One edge per submitted change: Chiaki turns it into a history event.
            if(delivered>=edgeAt.size()||polledAt[delivered]<0||polledAt[delivered]>now)orderOk=false;
            else{sampling.add(double(polledAt[delivered]-edgeAt[delivered])/1000);dispatch.add(double(now-polledAt[delivered])/1000);total.add(double(now-edgeAt[delivered])/1000);}
            consoleButtons=s.buttons;++delivered;
        }
    };
    Us now=0,nextController=0;
    std::uniform_real_distribution<double> sleepSpread(0.67,1.33); // e.g. 1.5 ms -> 1.0..2.0 ms
    while(now<end){
        poll(now);
        if(original){
            if(now>=nextController){submit(now,queue.take(now*10));nextController=now+4000;}
        }else{
            bool due=now>=nextController;
            for(unsigned n=0;n<ControllerQueue::kCapacity+1;++n){
                if(!due&&!queue.pending())break;
                submit(now,queue.take(now*10));due=false;nextController=now+4000;
                if(!queue.pending())break;
            }
            wakeAt=-1;
        }
        if(frameAt<=now){now+=Us(decodeMs*1000);nextFrame+=16683;frameAt=nextFrame+frameJitter();continue;}
        if(original){now+=Us(ownerSleepMs*sleepSpread(rng)*1000);continue;}
        // new: sleep until a pad change, a frame, or the 4 ms tick, whichever is first.
        Us wake=std::min(nextController,frameAt+Us(wakeMs*1000));
        // Look ahead for the first poll that queues something new.
        while(nextPollAt<wake){poll(nextPollAt);if(wakeAt>=0){wake=std::min(wake,wakeAt+Us(wakeMs*1000));break;}}
        now=std::max(now+1,wake);
    }
    const bool ok=orderOk&&delivered==edgeAt.size()&&queue.overflows()==0;
    std::printf("{\"bench\":\"input\",\"mode\":\"%s\",\"pollMs\":%lld,\"ownerSleepMs\":%.2f,\"motion\":%s,\"edges\":%zu,\"delivered\":%zu,\"inOrder\":%s,\"overflows\":%llu,\"submitsPerSecond\":%.0f,"
        "\"samplingMeanMs\":%.3f,\"dispatchMeanMs\":%.3f,\"totalMeanMs\":%.3f,\"totalP50Ms\":%.3f,\"totalP95Ms\":%.3f,\"totalP99Ms\":%.3f,\"totalMaxMs\":%.3f}\n",
        mode.c_str(),pollUs/1000,original?ownerSleepMs:0.0,motion?"true":"false",edgeAt.size(),delivered,orderOk?"true":"false",static_cast<unsigned long long>(queue.overflows()),double(submits)/(double(end)/1e6),
        sampling.mean(),dispatch.mean(),total.mean(),total.pct(.50),total.pct(.95),total.pct(.99),total.pct(1.0));
    return ok?0:1;
}
