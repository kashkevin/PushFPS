// SPDX-License-Identifier: GPL-3.0-only
// Linux model: which reorder wait (fixed 2, 4, 8, 16 ms or the adaptive rule)
// does best under which made-up link condition, and what is left after the
// condition ends.
//
//   reorder_study [--seconds N] [--seeds N] > reorder_study_results.json
//   reorder_study --wifi [--seconds N] [--seeds N] > reorder_study_wifi_results.json
//     the Wi-Fi-like conditions (ReorderStudy.h wifiConditions), with decoder
//     queue drops (original queue and VEYRA_EXP_RP_BACKLOG) per policy
//
// Conditions and runner: include/veyra/remoteplay/ReorderStudy.h. Every
// condition is clean before and after its impairment window. Per condition
// and policy: frames lost (given up although complete at the socket, and lost
// by the link), keyframe requests (one per lost frame; and merged within
// PushFPS's 500 ms spacing), the delay the wait added to delivered frames
// (median, P95, P99), and the time after the impairment until the frames'
// whole delay (send to hand-over) is back at its baseline. "best": among the
// fixed waits losing at most max(2, 2 % of the fewest) frames more than the
// fewest, the lowest P99 added delay; "tiedFixed": those within 0.05 ms of
// it. The adaptive rule is "within margin" when it loses
// at most max(5, 10 %) more frames than that and its P99 added delay is at
// most 1 ms above it.
//
// Everything is an assumption, written into the output. No PS5, no network,
// no Windows machine; nothing here predicts what a real console or LAN does.
#include "veyra/remoteplay/ReorderStudy.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
using namespace veyra::remoteplay;
using namespace veyra::remoteplay::study;
namespace {
struct Named {const char* name;Policy policy;};
const Named kPolicies[]={{"fixed_2ms",{2000,false,false}},{"fixed_4ms",{4000,false,false}},{"fixed_8ms",{8000,false,false}},{"fixed_16ms",{16000,false,false}},{"adaptive_from_2ms",{2000,true,false}}};
// --wifi also: VEYRA_EXP_RP_EARLY_LOSS with the fixed 2 ms and 16 ms waits (after the adaptive rule, so the
// best-fixed and margin columns stay those of the four plain fixed waits and the adaptive rule).
const Named kWifiPolicies[]={{"fixed_2ms",{2000,false,false}},{"fixed_4ms",{4000,false,false}},{"fixed_8ms",{8000,false,false}},{"fixed_16ms",{16000,false,false}},{"adaptive_from_2ms",{2000,true,false}},
    {"early_loss_2ms",{2000,false,true}},{"early_loss_16ms",{16000,false,true}}};
constexpr std::size_t kFixedCount=4,kAdaptiveIndex=4;
struct Summary {
    std::uint64_t frames=0,delivered=0,lostEarly=0,lostLink=0,keyframeEpisodes=0,windowChanges=0;
    std::vector<std::int64_t> added;
    std::vector<double> backMs;   // per seed: time after the window until the whole delay is at baseline
    std::vector<double> residualMs; // per seed: P99 whole delay 10-20 s after the window, minus baseline
    std::vector<double> floorBackMs; // adaptive: time after the window until the wait is back at its start
    std::map<std::string,unsigned> classes; // bottleneck named in impaired intervals
    std::uint64_t ingressRecoveries=0,backlogRecoveries=0;
    std::uint64_t lost()const{return lostEarly+lostLink;}
};
double pct(std::vector<std::int64_t> v,double p){
    if(v.empty())return 0;
    std::sort(v.begin(),v.end());
    const auto rank=static_cast<std::size_t>(std::ceil(p*static_cast<double>(v.size())));
    return static_cast<double>(v[(std::min)(v.size()-1,rank?rank-1:0)])/1000.0;
}
void accumulate(const Condition& c,const RunResult& r,Summary& s){
    std::int64_t last=-kRequestSpacingUs;
    std::vector<std::int64_t> base;
    const std::int64_t end=c.seconds>0?c.windowEndUs():c.windowStartUs();
    for(std::size_t f=0;f<r.frames.size();++f){
        const auto& o=r.frames[f];const std::int64_t sent=frameSentUs(static_cast<std::int64_t>(f));
        ++s.frames;
        if(o.handUs>=0){
            ++s.delivered;s.added.push_back(o.handUs-o.possibleUs);
            if(sent>=kStartUs+5000000&&sent<c.windowStartUs())base.push_back(o.handUs-sent);
            continue;
        }
        if(o.possibleUs>=0)++s.lostEarly;else ++s.lostLink;
        if(sent-last>=kRequestSpacingUs){++s.keyframeEpisodes;last=sent;}
    }
    s.windowChanges+=r.windows.size()-1;
    s.ingressRecoveries+=r.ingressRecoveries;s.backlogRecoveries+=r.backlogRecoveries;
    std::sort(base.begin(),base.end());
    const std::int64_t baseline=base.empty()?0:base[base.size()/2];
    // Back at baseline: the first frame from which every frame of the next
    // second is delivered with a whole delay at most 1 ms above the baseline.
    std::int64_t back=-1;
    const std::size_t first=static_cast<std::size_t>((end-kStartUs+kFrameUs-1)/kFrameUs);
    std::size_t good=0;
    for(std::size_t f=first;f<r.frames.size();++f){
        const auto& o=r.frames[f];
        const bool ok=o.handUs>=0&&o.handUs-frameSentUs(static_cast<std::int64_t>(f))<=baseline+1000;
        good=ok?good+1:0;
        if(good*static_cast<std::size_t>(kFrameUs)>=1000000u){back=frameSentUs(static_cast<std::int64_t>(f+1-good))-end;break;}
    }
    s.backMs.push_back(back<0?-1.0:static_cast<double>((std::max)(back,std::int64_t{0}))/1000.0);
    std::vector<std::int64_t> after;
    for(std::size_t f=0;f<r.frames.size();++f){
        const std::int64_t sent=frameSentUs(static_cast<std::int64_t>(f));
        if(sent>=end+10000000&&sent<end+20000000&&r.frames[f].handUs>=0)after.push_back(r.frames[f].handUs-sent-baseline);
    }
    s.residualMs.push_back(pct(after,0.99));
    if(r.windows.size()>1){
        std::int64_t at=-1;
        for(const auto& w:r.windows)if(w.atUs>=end&&w.windowUs==r.windows.front().windowUs){at=w.atUs;break;}
        if(r.windows.back().windowUs!=r.windows.front().windowUs)at=-1;
        else if(at<0)at=end; // never moved after the window
        s.floorBackMs.push_back(at<0?-1.0:static_cast<double>(at-end)/1000.0);
    }else s.floorBackMs.push_back(0);
    for(const auto& h:r.intervals){
        const std::int64_t intervalEnd=kStartUs+static_cast<std::int64_t>(h.sequence)*kIntervalUs;
        if(c.seconds>0&&intervalEnd>c.windowStartUs()&&intervalEnd-kIntervalUs<end)++s.classes[bottleneckName(h.bottleneck)];
    }
}
std::string list(const std::vector<double>& v){
    std::string out="[";char b[32];
    for(std::size_t i=0;i<v.size();++i){std::snprintf(b,sizeof b,"%s%.1f",i?", ":"",v[i]);out+=b;}
    return out+"]";
}
}
int main(int argc,char** argv){
    unsigned seconds=120,seeds=3;
    bool wifi=false;
    if(argc>1&&!std::strcmp(argv[1],"--wifi")){wifi=true;--argc;++argv;}
    for(int i=1;i+1<argc;i+=2){
        if(!std::strcmp(argv[i],"--seconds"))seconds=static_cast<unsigned>(std::strtoul(argv[i+1],nullptr,10));
        else if(!std::strcmp(argv[i],"--seeds"))seeds=static_cast<unsigned>(std::strtoul(argv[i+1],nullptr,10));
        else{std::fprintf(stderr,"usage: reorder_study [--seconds N] [--seeds N]\n");return 2;}
    }
    if(seconds<100||seconds>3600||seeds<1||seeds>50||argc%2==0){std::fprintf(stderr,"usage: reorder_study [--seconds 100..3600] [--seeds 1..50]\n");return 2;}
    const unsigned frames=static_cast<unsigned>(static_cast<std::int64_t>(seconds)*1000000/kFrameUs);
    std::printf("{\n  \"label\": \"Linux model\",\n"
        "  \"what\": \"The library's video reorder-queue rule (model checked against the C source) replayed on made-up packet arrival traces, fixed waits against the adaptive rule. No PS5, no network, no Windows machine; predicts nothing about a real console or LAN.\",\n"
        "  \"assumptions\": {\"framesPerSecond\": 59.94, \"sourcePacketsPerFrame\": %u, \"fecPacketsPerFrame\": \"3 unless the condition says none\", \"frameSentOverMs\": 2.0, \"baseDelayMs\": 0.3, "
        "\"impairment\": \"from 30 s into each run, clean before and after\", \"addedDelay\": \"hand-over minus the arrival of the frame's 24th distinct packet at the socket\", "
        "\"backToBaseline\": \"after the impairment window, until every frame of one second is delivered with send-to-hand-over delay at most 1 ms above the median of 5-30 s\", "
        "\"keyframeRequests\": \"one per lost frame (the library asks on every frame FEC cannot complete); episodes500ms merges those within PushFPS's 500 ms spacing\", "
        "\"best\": \"among fixed waits losing at most max(2, 2 %% of the fewest) frames more than the fewest, the lowest P99 added delay (the shorter wait on equal P99); tiedFixed lists those candidates within 0.05 ms of that P99\", \"margin\": \"adaptive within max(5, 10 %%) lost frames and 1 ms P99 added delay of the best fixed wait\"},\n"
        "  \"secondsPerSeed\": %u, \"seeds\": %u,\n",kSourceUnits,seconds,seeds);
    if(wifi)std::printf("  \"wifi\": \"Wi-Fi-like link models invented from the general shape of 802.11 behaviour (stalls then clumps, link-layer retries arriving out of order, rate dips); not measured on any network. ingressRecoveries / backlogRecoveries: decoder queue drops, each a keyframe request in the app, with the original queue (a fifth waiting frame) and with VEYRA_EXP_RP_BACKLOG (16 frames, 100 ms), simplified decoder at 3 ms a frame\",\n");
    std::printf("  \"conditions\": {\n");
    const auto all=wifi?wifiConditions():studyConditions();
    unsigned within=0;
    for(std::size_t ci=0;ci<all.size();++ci){
        const auto& c=all[ci];
        const Named* policies=wifi?kWifiPolicies:kPolicies;
        const std::size_t policyCount=wifi?sizeof(kWifiPolicies)/sizeof(kWifiPolicies[0]):sizeof(kPolicies)/sizeof(kPolicies[0]);
        Summary s[sizeof(kWifiPolicies)/sizeof(kWifiPolicies[0])];
        for(unsigned seed=1;seed<=seeds;++seed){
            const auto trace=makeTrace(c,(wifi?50000u:0u)+1000u*static_cast<unsigned>(ci+1)+seed,frames);
            for(std::size_t p=0;p<policyCount;++p)accumulate(c,runPolicy(c,trace,policies[p].policy),s[p]);
        }
        // Best fixed wait: among those losing at most max(2, 2 % of the fewest)
        // frames more than the fewest, the lowest P99 added delay (the shorter
        // wait on equal P99). Tied: every such candidate within 0.05 ms of that P99.
        std::uint64_t fewest=s[0].lost();
        for(std::size_t p=1;p<kFixedCount;++p)fewest=(std::min)(fewest,s[p].lost());
        const std::uint64_t tolerance=(std::max)(std::uint64_t{2},fewest/50);
        std::size_t best=kFixedCount;
        for(std::size_t p=0;p<kFixedCount;++p){
            if(s[p].lost()>fewest+tolerance)continue;
            if(best==kFixedCount||pct(s[p].added,0.99)<pct(s[best].added,0.99))best=p;
        }
        std::string tied="[";
        for(std::size_t p=0;p<kFixedCount;++p){
            if(s[p].lost()>fewest+tolerance||pct(s[p].added,0.99)>pct(s[best].added,0.99)+0.05)continue;
            if(tied.size()>1)tied+=", ";
            tied+="\"";tied+=kPolicies[p].name;tied+="\"";
        }
        tied+="]";
        const auto& a=s[kAdaptiveIndex];
        const bool lostOk=a.lost()<=s[best].lost()+(std::max)(std::uint64_t{5},s[best].lost()/10);
        const bool delayOk=pct(a.added,0.99)<=pct(s[best].added,0.99)+1.0;
        within+=lostOk&&delayOk;
        std::printf("    \"%s\": {\n      \"condition\": \"%s\",\n      \"bestFixed\": \"%s\", \"tiedFixed\": %s, \"adaptiveWithinMargin\": %s, \"adaptiveLostOk\": %s, \"adaptiveDelayOk\": %s,\n",
            c.name,c.description,kPolicies[best].name,tied.c_str(),lostOk&&delayOk?"true":"false",lostOk?"true":"false",delayOk?"true":"false");
        for(std::size_t p=0;p<policyCount;++p){
            const auto& t=s[p];
            std::printf("      \"%s\": {\"frames\": %llu, \"lost\": %llu, \"givenUpTooEarly\": %llu, \"lostByTheLink\": %llu, \"keyframeRequests\": %llu, \"keyframeEpisodes500ms\": %llu, "
                "\"addedDelayMs\": {\"median\": %.2f, \"p95\": %.2f, \"p99\": %.2f}, \"backToBaselineMsPerSeed\": %s, \"residualP99MsPerSeed\": %s",
                policies[p].name,static_cast<unsigned long long>(t.frames),static_cast<unsigned long long>(t.lost()),static_cast<unsigned long long>(t.lostEarly),
                static_cast<unsigned long long>(t.lostLink),static_cast<unsigned long long>(t.lost()),static_cast<unsigned long long>(t.keyframeEpisodes),
                pct(t.added,0.5),pct(t.added,0.95),pct(t.added,0.99),list(t.backMs).c_str(),list(t.residualMs).c_str());
            if(wifi)std::printf(", \"ingressRecoveries\": %llu, \"backlogRecoveries\": %llu",static_cast<unsigned long long>(t.ingressRecoveries),static_cast<unsigned long long>(t.backlogRecoveries));
            if(policies[p].policy.adaptive)std::printf(", \"waitChanges\": %llu, \"waitBackAtStartMsPerSeed\": %s",static_cast<unsigned long long>(t.windowChanges),list(t.floorBackMs).c_str());
            if(p==0){
                std::printf(", \"bottleneckInImpairedIntervals\": {");
                bool first=true;for(const auto& [name,count]:t.classes){std::printf("%s\"%s\": %u",first?"":", ",name.c_str(),count);first=false;}
                std::printf("}");
            }
            std::printf("}%s\n",p+1==policyCount?"":",");
        }
        std::printf("    }%s\n",ci+1==all.size()?"":",");
    }
    std::printf("  },\n  \"adaptiveWithinMarginIn\": \"%u of %zu conditions\"\n}\n",within,all.size());
    return 0;
}
