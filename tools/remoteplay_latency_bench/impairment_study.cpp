// SPDX-License-Identifier: GPL-3.0-only
// Linux model (stream "network", 21 Nov 2026): the owner's impairment list (no loss, 0.1 / 0.5 / 1 % and a
// 3 % diagnostic case, short bursts, duplication, reordering with jitter, delayed packets, a loaded-PC
// proxy) run through the same model as reorder_study (ReorderStudy.h: the library's reorder-queue rule,
// checked against the C source by reorder_library_check.sh; FEC as "any 24 of 27 packets"), comparing the
// receive-side choices that exist as switches: the library's own 16 ms wait (the default), the LAN
// profile's 2 ms, the adaptive wait, early loss; and for each, the original decoder queue against
// VEYRA_EXP_RP_BACKLOG. Per condition and choice: frames lost (never shown: the picture holds the last one
// until a keyframe), keyframe waits, stutters (a gap of more than two frame intervals between frames
// handed to the decoder), the delay the wait added to delivered frames (median, P95, P99), and what the
// dominant-cause rule of [slipstream-net] (StreamBottleneck.h) names in the impaired and the clean
// five-second intervals.
//
//   impairment_study [--seconds N] [--seeds N] > impairment_study_results.json
//
// Everything is an assumption, written into the output. No PS5, no network, no Windows machine, no GPU:
// it compares rules on the same made-up arrivals and predicts nothing about the owner's LAN.
#include "veyra/remoteplay/ReorderStudy.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
using namespace veyra::remoteplay;
using namespace veyra::remoteplay::study;
namespace {
struct Named {const char* name;const char* what;Policy policy;};
const Named kPolicies[]={
    {"default_16ms","the library's own 16 ms wait (no switch)",{16000,false,false}},
    {"lan_2ms","VEYRA_EXP_RP_LAN_PROFILE=1 (2 ms wait)",{2000,false,false}},
    {"adaptive_from_2ms","VEYRA_EXP_RP_LAN_PROFILE=1 VEYRA_EXP_RP_REORDER_ADAPTIVE=1",{2000,true,false}},
    {"early_loss_16ms","VEYRA_EXP_RP_EARLY_LOSS=1 (16 ms wait)",{16000,false,true}},
    {"early_loss_2ms","VEYRA_EXP_RP_LAN_PROFILE=1 VEYRA_EXP_RP_EARLY_LOSS=1",{2000,false,true}}};
struct Summary {
    std::uint64_t frames=0,delivered=0,lostEarly=0,lostLink=0,keyframeEpisodes=0,stutters=0;
    std::uint64_t ingressRecoveries=0,backlogRecoveries=0;
    std::vector<std::int64_t> added;
    std::map<std::string,unsigned> impaired,clean;
    std::uint64_t lost()const{return lostEarly+lostLink;}
};
double pct(std::vector<std::int64_t> v,double p){
    if(v.empty())return 0;
    std::sort(v.begin(),v.end());
    const auto rank=static_cast<std::size_t>(std::ceil(p*static_cast<double>(v.size())));
    return static_cast<double>(v[(std::min)(v.size()-1,rank?rank-1:0)])/1000.0;
}
void accumulate(const Condition& c,const RunResult& r,Summary& s){
    std::int64_t lastRequest=-kRequestSpacingUs,lastHand=-1;
    for(std::size_t f=0;f<r.frames.size();++f){
        const auto& o=r.frames[f];const std::int64_t sent=frameSentUs(static_cast<std::int64_t>(f));
        ++s.frames;
        if(o.handUs>=0){
            ++s.delivered;s.added.push_back(o.handUs-o.possibleUs);
            if(lastHand>=0&&o.handUs-lastHand>2*kFrameUs)++s.stutters;
            lastHand=o.handUs;
            continue;
        }
        if(o.possibleUs>=0)++s.lostEarly;else ++s.lostLink;
        if(sent-lastRequest>=kRequestSpacingUs){++s.keyframeEpisodes;lastRequest=sent;}
    }
    s.ingressRecoveries+=r.ingressRecoveries;s.backlogRecoveries+=r.backlogRecoveries;
    for(const auto& h:r.intervals){
        const std::int64_t intervalEnd=kStartUs+static_cast<std::int64_t>(h.sequence)*kIntervalUs;
        const bool impaired=c.seconds>0&&intervalEnd>c.windowStartUs()&&intervalEnd-kIntervalUs<c.windowEndUs();
        const auto cause=streamCauseName(classifyStream(studyEvidence(h)).cause);
        ++(impaired?s.impaired:s.clean)[cause];
    }
}
std::string counts(const std::map<std::string,unsigned>& m){
    std::string out="{";bool first=true;char b[96];
    for(const auto& [name,count]:m){std::snprintf(b,sizeof b,"%s\"%s\": %u",first?"":", ",name.c_str(),count);out+=b;first=false;}
    return out+"}";
}
}
int main(int argc,char** argv){
    unsigned seconds=120,seeds=3;
    for(int i=1;i+1<argc;i+=2){
        if(!std::strcmp(argv[i],"--seconds"))seconds=static_cast<unsigned>(std::strtoul(argv[i+1],nullptr,10));
        else if(!std::strcmp(argv[i],"--seeds"))seeds=static_cast<unsigned>(std::strtoul(argv[i+1],nullptr,10));
        else{std::fprintf(stderr,"usage: impairment_study [--seconds N] [--seeds N]\n");return 2;}
    }
    if(seconds<100||seconds>3600||seeds<1||seeds>50||argc%2==0){std::fprintf(stderr,"usage: impairment_study [--seconds 100..3600] [--seeds 1..50]\n");return 2;}
    const unsigned frames=static_cast<unsigned>(static_cast<std::int64_t>(seconds)*1000000/kFrameUs);
    std::printf("{\n  \"label\": \"Linux model (simulation, not a PS5 measurement)\",\n"
        "  \"what\": \"Made-up packet arrival traces through the library's reorder-queue rule (model checked against the C source), FEC as any 24 of 27 packets, a simplified decoder; every receive-side switch that exists, on the same arrivals. No PS5, no network, no Windows machine, no GPU.\",\n"
        "  \"assumptions\": {\"framesPerSecond\": 59.94, \"sourcePacketsPerFrame\": %u, \"fecPacketsPerFrame\": 3, \"frameSentOverMs\": 2.0, \"baseDelayMs\": 0.3, \"decodeMs\": 3, "
        "\"impairment\": \"from 30 s into each run for 60 s, clean before and after\", \"lost\": \"frames never handed to the decoder (the picture holds the last frame until a keyframe)\", "
        "\"keyframeEpisodes500ms\": \"lost frames merged within PushFPS's 500 ms keyframe request spacing: each is a frozen picture\", "
        "\"stutters\": \"a gap of more than two frame intervals between frames handed to the decoder\", "
        "\"addedDelay\": \"hand-over minus the arrival of the frame's 24th distinct packet at the socket\", "
        "\"ingressRecoveries / backlogRecoveries\": \"decoder queue drops (each a keyframe request): the original queue / VEYRA_EXP_RP_BACKLOG\", "
        "\"verdicts\": \"the [slipstream-net] dominant-cause rule (StreamBottleneck.h) per five-second interval, from what the model's counters show\"},\n"
        "  \"secondsPerSeed\": %u, \"seeds\": %u,\n  \"choices\": {",kSourceUnits,seconds,seeds);
    for(std::size_t p=0;p<sizeof(kPolicies)/sizeof(kPolicies[0]);++p)std::printf("%s\"%s\": \"%s\"",p?", ":"",kPolicies[p].name,kPolicies[p].what);
    std::printf("},\n  \"conditions\": {\n");
    const auto all=impairmentConditions();
    constexpr std::size_t policyCount=sizeof(kPolicies)/sizeof(kPolicies[0]);
    for(std::size_t ci=0;ci<all.size();++ci){
        const auto& c=all[ci];
        Summary s[policyCount];
        for(unsigned seed=1;seed<=seeds;++seed){
            const auto trace=makeTrace(c,90000u+1000u*static_cast<unsigned>(ci+1)+seed,frames);
            for(std::size_t p=0;p<policyCount;++p)accumulate(c,runPolicy(c,trace,kPolicies[p].policy),s[p]);
        }
        std::printf("    \"%s\": {\n      \"condition\": \"%s\",\n",c.name,c.description);
        for(std::size_t p=0;p<policyCount;++p){
            const auto& t=s[p];
            std::printf("      \"%s\": {\"frames\": %llu, \"lost\": %llu, \"givenUpTooEarly\": %llu, \"lostByTheLink\": %llu, \"keyframeEpisodes500ms\": %llu, \"stutters\": %llu, "
                "\"addedDelayMs\": {\"median\": %.2f, \"p95\": %.2f, \"p99\": %.2f}, \"ingressRecoveries\": %llu, \"backlogRecoveries\": %llu, "
                "\"verdictsImpaired\": %s, \"verdictsClean\": %s}%s\n",
                kPolicies[p].name,static_cast<unsigned long long>(t.frames),static_cast<unsigned long long>(t.lost()),static_cast<unsigned long long>(t.lostEarly),
                static_cast<unsigned long long>(t.lostLink),static_cast<unsigned long long>(t.keyframeEpisodes),static_cast<unsigned long long>(t.stutters),
                pct(t.added,0.5),pct(t.added,0.95),pct(t.added,0.99),static_cast<unsigned long long>(t.ingressRecoveries),static_cast<unsigned long long>(t.backlogRecoveries),
                counts(t.impaired).c_str(),counts(t.clean).c_str(),p+1==policyCount?"":",");
        }
        std::printf("    }%s\n",ci+1==all.size()?"":",");
    }
    std::printf("  }\n}\n");
    return 0;
}
