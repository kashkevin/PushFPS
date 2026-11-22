// SPDX-License-Identifier: GPL-3.0-only
// SmartLink simulator (LINUX MODEL; never a measurement). Build:
//   g++ -std=c++20 -O2 -Iinclude tools/smartlink_sim/smartlink_sim.cpp -o smartlink_sim
// Modes:
//   smartlink_sim [--seeds N] [--lag-ms L] [--json results.json] [--md tables.md]
//       every scenario x policy x seed; SmartLink's decisions take effect L ms late (default 250, as the library's look) (include/veyra/remoteplay/SmartLinkSim.h); acceptance checks at the end
//   smartlink_sim --study [--seeds N] [--model-out SmartLinkModel.h] [--json study.json] [--md study.md]
//       statistics vs learned comparison: trains the logistic model on the training families (policy slipstream_rc4,
//       SmartLink observing), evaluates on held-out seeds of those families and on families not seen in training
//   smartlink_sim --replay trace.csv
//       a recorded trace (docs/SMARTLINK.md, "Trace file") through the predictor and the controller
//   smartlink_sim --bench
//       cost per sample of the predictor (statistics / learned) and of Engine::update on this machine
// Single-threaded; deterministic for given seeds.
#include "veyra/remoteplay/SmartLinkSim.h"
#include "veyra/remoteplay/SmartLinkTrace.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
using namespace veyra::remoteplay::smartlink;
namespace {
std::int64_t g_lagUs=sim::RunOptions::kDefaultApplyLagUs; // --lag-ms
std::string fmt(double v,int p=2){char b[64];std::snprintf(b,sizeof b,"%.*f",p,v);return b;}
struct Agg {
    double delivered=0,givenUp=0,frames=0,stutterPerMin=0,requests=0,audioGapsPerMin=0,audioMean=0,audioP95=0,changes=0,leave=-1,maxPerMin=0,giveUpP95=0;
    std::vector<float> added;std::array<double,5> wait{};unsigned runs=0;bool neverLeft=false;
};
const char* kWaitNames[5]={"2","4","8","16","other"};

int table(unsigned seeds,const std::string& jsonPath,const std::string& mdPath){
    const auto sc=sim::scenarios();
    std::map<std::string,std::map<std::string,Agg>> all;
    std::vector<std::string> failures;
    for(const auto& c:sc){
        for(unsigned seed=1;seed<=seeds;++seed){
            const auto path=sim::makePath(c,seed);
            for(const auto& p:sim::kPolicies){
                sim::RunOptions o;o.seed=seed;o.applyLagUs=g_lagUs;auto r=sim::run(c,path,p.kind,o);
                auto& m=r.metrics;auto& a=all[c.name][p.name];
                a.delivered+=static_cast<double>(m.delivered);a.givenUp+=static_cast<double>(m.givenUp);a.frames+=static_cast<double>(m.frames);
                a.stutterPerMin+=static_cast<double>(m.stutters)/m.minutes;a.requests+=static_cast<double>(m.recoveryRequests);
                a.audioGapsPerMin+=static_cast<double>(m.audioGaps)/m.minutes;a.audioMean+=m.audioMeanMs;a.audioP95+=m.audioP95Ms;
                a.changes+=static_cast<double>(m.settingChanges);a.maxPerMin=(std::max)(a.maxPerMin,m.maxChangesPerMinute);a.giveUpP95+=m.giveUpP95Ms;
                for(std::size_t i=0;i<5;++i)a.wait[i]+=m.secondsAtWait[i];
                if(p.kind==sim::PolicyKind::SmartLink||p.kind==sim::PolicyKind::SmartLinkLan||p.kind==sim::PolicyKind::SmartLinkLearned){
                    if(m.leaveAfterS<0)a.neverLeft=true;else a.leave=(std::max)(a.leave,m.leaveAfterS);
                }
                a.added.insert(a.added.end(),m.added.begin(),m.added.end());
                ++a.runs;
            }
        }
    }
    // Summaries.
    std::ostringstream js,md;
    js<<"{\n  \"label\": \"Model result (Linux simulator), not a measurement\",\n  \"seeds\": "<<seeds<<",\n  \"policies\": [";
    for(std::size_t i=0;i<sim::kPolicies.size();++i)js<<(i?",":"")<<"\n    {\"name\": \""<<sim::kPolicies[i].name<<"\", \"description\": \""<<sim::kPolicies[i].description<<"\"}";
    js<<"\n  ],\n  \"scenarios\": [";
    bool firstScenario=true;
    for(const auto& c:sc){
        js<<(firstScenario?"":",")<<"\n    {\"name\": \""<<c.name<<"\", \"description\": \""<<c.description<<"\", \"results\": {";firstScenario=false;
        md<<"\n#### "<<c.name<<"\n\n"<<c.description<<" (clean before 30 s and after 120 s; 240 s per run, "<<seeds<<" seeds).\n\n";
        md<<"Model result (Linux simulator), not a measurement\n\n";
        md<<"| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |\n";
        md<<"|---|---|---|---|---|---|---|---|---|---|---|\n";
        bool firstPolicy=true;
        for(const auto& p:sim::kPolicies){
            auto& a=all[c.name][p.name];const double n=a.runs;
            const double p50=sim::detail::percentile(a.added,0.5),p95=sim::detail::percentile(a.added,0.95),p99=sim::detail::percentile(a.added,0.99);
            std::string waits;for(std::size_t i=0;i<4;++i)waits+=(i?"/":"")+fmt(a.wait[i]/n,0);
            const bool smart=p.kind==sim::PolicyKind::SmartLink||p.kind==sim::PolicyKind::SmartLinkLan||p.kind==sim::PolicyKind::SmartLinkLearned;
            const std::string leave=!smart?"-":a.neverLeft?"never":fmt((std::max)(0.0,a.leave),1);
            md<<"| "<<p.name<<" | "<<fmt(p50)<<" / "<<fmt(p95)<<" / "<<fmt(p99)<<" | "<<fmt(a.delivered/n,1)<<" | "<<fmt(a.givenUp/n,1)<<" | "<<fmt(a.stutterPerMin/n)
              <<" | "<<fmt(a.requests/n,1)<<" | "<<fmt(a.audioGapsPerMin/n)<<" | "<<fmt(a.audioMean/n,1)<<" / "<<fmt(a.audioP95/n,1)<<" | "<<waits<<" | "<<fmt(a.changes/n,1)<<" ("<<fmt(a.maxPerMin,0)<<") | "<<leave<<" |\n";
            js<<(firstPolicy?"":",")<<"\n      \""<<p.name<<"\": {\"addedP50Ms\": "<<fmt(p50,3)<<", \"addedP95Ms\": "<<fmt(p95,3)<<", \"addedP99Ms\": "<<fmt(p99,3)
              <<", \"framesDelivered\": "<<fmt(a.delivered/n,1)<<", \"framesGivenUp\": "<<fmt(a.givenUp/n,1)<<", \"stutterPerMinute\": "<<fmt(a.stutterPerMin/n,3)
              <<", \"recoveryRequests\": "<<fmt(a.requests/n,1)<<", \"audioGapsPerMinute\": "<<fmt(a.audioGapsPerMin/n,3)<<", \"audioReserveMeanMs\": "<<fmt(a.audioMean/n,2)
              <<", \"audioReserveP95Ms\": "<<fmt(a.audioP95/n,2)<<", \"giveUpP95Ms\": "<<fmt(a.giveUpP95/n,2)<<", \"secondsAtWaitMs\": {";
            for(std::size_t i=0;i<5;++i)js<<(i?", ":"")<<"\""<<kWaitNames[i]<<"\": "<<fmt(a.wait[i]/n,1);
            js<<"}, \"settingChanges\": "<<fmt(a.changes/n,1)<<", \"maxChangesPerMinute\": "<<fmt(a.maxPerMin,0)<<", \"backToLeaveAfterS\": "<<(smart?(a.neverLeft?"null":fmt((std::max)(0.0,a.leave),1)):"null")<<"}";
            firstPolicy=false;
            // Acceptance checks.
            if(smart){
                if(a.neverLeft)failures.push_back(std::string(c.name)+"/"+p.name+": an output never returned to leave after the disturbance");
                else if(a.leave>Engine::kReturnBoundMs/1000.0)failures.push_back(std::string(c.name)+"/"+p.name+": back to leave only after "+fmt(a.leave,1)+" s (> the stated bound)");
                if(a.maxPerMin>static_cast<double>(Engine::kReorderChangesPerMinute+Engine::kAudioChangesPerMinute))failures.push_back(std::string(c.name)+"/"+p.name+": too many changes per minute");
            }
        }
        js<<"\n    }}";
    }
    // Wired clean: zero delay, zero changes. SmartLink vs slipstream_rc4: more delay AND fewer frames?
    for(const auto& c:sc){
        auto& b=all[c.name]["slipstream_rc4"];
        for(const char* name:{"smartlink_apply","smartlink_apply_lan","smartlink_apply_learned"}){
            auto& a=all[c.name][name];
            const double ap95=sim::detail::percentile(a.added,0.95),bp95=sim::detail::percentile(b.added,0.95);
            if(std::string(c.name)=="wired_clean"&&(sim::detail::percentile(a.added,1.0)>0||a.changes>0))failures.push_back(std::string("wired_clean/")+name+": added delay or changes on a clean wired link");
            if(ap95>bp95+16.0&&a.delivered<b.delivered)failures.push_back(std::string(c.name)+"/"+name+": added-delay P95 above slipstream_rc4's by more than a reorder step AND fewer frames delivered");
            if(ap95>bp95+0.5&&a.delivered<b.delivered)md<<"\nNote: "<<c.name<<"/"<<name<<": P95 added delay "<<fmt(ap95)<<" ms vs "<<fmt(bp95)<<" ms with "<<fmt((b.delivered-a.delivered)/a.runs,1)<<" fewer frames per run.\n";
        }
    }
    js<<"\n  ],\n  \"acceptanceFailures\": [";
    for(std::size_t i=0;i<failures.size();++i)js<<(i?",":"")<<"\n    \""<<failures[i]<<"\"";
    js<<"\n  ]\n}\n";
    md<<"\nAcceptance checks: "<<(failures.empty()?"all passed":"FAILED")<<"\n";
    for(const auto& f:failures)md<<"- "<<f<<"\n";
    if(!jsonPath.empty()){std::ofstream(jsonPath)<<js.str();}
    if(!mdPath.empty()){std::ofstream(mdPath)<<md.str();}
    else std::fputs(md.str().c_str(),stdout);
    for(const auto& f:failures)std::fprintf(stderr,"ACCEPTANCE: %s\n",f.c_str());
    return failures.empty()?0:1;
}

// ---------------- statistics vs learned ----------------
struct Row {Features f{};double stat=0;bool label=false;double excess=0;};
bool trainFamily(const std::string& n){
    for(const char* t:{"wired_clean","wired_rare_loss","rtt_variation","wifi_random_spikes","burst_loss","reordering","congestion","weak_signal"})if(n==t)return true;
    return false;
}
std::vector<std::vector<Row>> collect(bool train,unsigned seedFrom,unsigned seedTo,bool familiesSeen){
    std::vector<std::vector<Row>> runs;
    for(const auto& c:sim::scenarios()){
        if(trainFamily(c.name)!=familiesSeen)continue;
        (void)train;
        for(unsigned seed=seedFrom;seed<=seedTo;++seed){
            const auto path=sim::makePath(c,seed);
            const auto r=sim::run(c,path,sim::PolicyKind::Rc4Default,{true,seed});
            std::vector<Row> rows;
            for(const auto& w:r.windows)rows.push_back({w.features,w.riskStat,w.label,w.excess});
            runs.push_back(std::move(rows));
        }
    }
    return runs;
}
struct Logistic {std::array<double,kFeatureCount> w{};double b=0;
    double p(const Features& f)const{double z=b;for(std::size_t i=0;i<kFeatureCount;++i)z+=w[i]*f[i];z=std::clamp(z,-30.0,30.0);return 1/(1+std::exp(-z));}};
Logistic fit(const std::vector<std::vector<Row>>& runs){
    Logistic m;m.b=-2;
    std::vector<const Row*> rows;for(const auto& r:runs)for(const auto& x:r)rows.push_back(&x);
    const double n=static_cast<double>(rows.size()),l2=1e-3;
    for(int it=0;it<3000;++it){
        std::array<double,kFeatureCount> g{};double gb=0;
        for(const Row* r:rows){const double e=m.p(r->f)-(r->label?1.0:0.0);for(std::size_t i=0;i<kFeatureCount;++i)g[i]+=e*r->f[i];gb+=e;}
        const double lr=2.0;
        for(std::size_t i=0;i<kFeatureCount;++i)m.w[i]-=lr*(g[i]/n+l2*m.w[i]);
        m.b-=lr*gb/n;
    }
    return m;
}
// Depth-2 stump forest (gradient boosting on log-odds, 30 trees, thresholds at feature quantiles).
struct Stump {std::size_t f1=0,f2=0;double t1=0,t2a=0,t2b=0;std::array<double,4> v{};};
struct Forest {std::vector<Stump> trees;double base=0;
    double p(const Features& x)const{double z=base;for(const auto& s:trees){const bool a=x[s.f1]>s.t1;const bool b=a?x[s.f2]>s.t2b:x[s.f2]>s.t2a;z+=s.v[(a?2:0)+(b?1:0)];}z=std::clamp(z,-30.0,30.0);return 1/(1+std::exp(-z));}};
Forest boost(const std::vector<std::vector<Row>>& runs){
    std::vector<const Row*> rows;for(const auto& r:runs)for(const auto& x:r)rows.push_back(&x);
    Forest fo;double pos=0;for(auto r:rows)pos+=r->label;
    const double n=static_cast<double>(rows.size());fo.base=std::log((pos+1)/(n-pos+1));
    std::vector<double> z(rows.size(),fo.base);
    const std::array<double,5> qs{0.02,0.05,0.1,0.3,0.6};
    for(int t=0;t<30;++t){
        std::vector<double> grad(rows.size()),hess(rows.size());
        for(std::size_t i=0;i<rows.size();++i){const double p=1/(1+std::exp(-z[i]));grad[i]=(rows[i]->label?1.0:0.0)-p;hess[i]=(std::max)(p*(1-p),1e-6);}
        Stump best;double bestGain=-1;
        const auto leaf=[&](double g,double h){return g*g/(h+1.0);};
        for(std::size_t f1=0;f1<kFeatureCount;++f1)for(double t1:qs)for(std::size_t f2=0;f2<kFeatureCount;++f2)for(double t2:qs){
            std::array<double,4> g{},h{};
            for(std::size_t i=0;i<rows.size();++i){const bool a=rows[i]->f[f1]>t1;const bool b=rows[i]->f[f2]>t2;const std::size_t k=(a?2:0)+(b?1:0);g[k]+=grad[i];h[k]+=hess[i];}
            double gain=0;for(std::size_t k=0;k<4;++k)gain+=leaf(g[k],h[k]);
            if(gain>bestGain){bestGain=gain;best.f1=f1;best.f2=f2;best.t1=t1;best.t2a=t2;best.t2b=t2;for(std::size_t k=0;k<4;++k)best.v[k]=0.5*g[k]/(h[k]+1.0);}
        }
        fo.trees.push_back(best);
        for(std::size_t i=0;i<rows.size();++i){const bool a=rows[i]->f[best.f1]>best.t1;const bool b=rows[i]->f[best.f2]>best.t2a;z[i]+=best.v[(a?2:0)+(b?1:0)];}
    }
    return fo;
}
struct Eval {double hit5=0,hit10=0,fa5=0,fa10=0,onsetHit=0,leadMedianMs=0,auc=0;std::size_t positives=0,negatives=0,onsets=0;std::array<std::array<double,2>,10> calib{};};
template<class Score> Eval evaluate(const std::vector<std::vector<Row>>& runs,Score score){
    Eval e;std::vector<double> neg,posv;
    for(const auto& r:runs)for(const auto& x:r)(x.label?posv:neg).push_back(score(x));
    e.positives=posv.size();e.negatives=neg.size();
    if(neg.empty()||posv.empty())return e;
    std::vector<double> sorted=neg;std::sort(sorted.begin(),sorted.end());
    const auto thresholdAt=[&](double fa){return sorted[static_cast<std::size_t>((1-fa)*static_cast<double>(sorted.size()-1))];};
    const auto rate=[&](const std::vector<double>& v,double t){double k=0;for(double s:v)if(s>t)++k;return k/static_cast<double>(v.size());};
    const double t5=thresholdAt(0.05),t10=thresholdAt(0.10);
    e.hit5=rate(posv,t5);e.fa5=rate(neg,t5);e.hit10=rate(posv,t10);e.fa10=rate(neg,t10);
    // AUC (rank).
    std::vector<std::pair<double,int>> all;for(double s:neg)all.push_back({s,0});for(double s:posv)all.push_back({s,1});
    std::sort(all.begin(),all.end());double rankSum=0;
    for(std::size_t i=0;i<all.size();){std::size_t j=i;while(j<all.size()&&all[j].first==all[i].first)++j;const double rank=(static_cast<double>(i)+static_cast<double>(j)+1)/2;for(std::size_t k=i;k<j;++k)if(all[k].second)rankSum+=rank;i=j;}
    e.auc=(rankSum-static_cast<double>(posv.size())*(static_cast<double>(posv.size())+1)/2)/(static_cast<double>(posv.size())*static_cast<double>(neg.size()));
    // Onsets: a window above T after 3 windows below; alarm = score above the 10 % false-alarm threshold in the window before.
    std::vector<double> leads;
    for(const auto& r:runs)for(std::size_t i=3;i<r.size();++i){
        if(!(r[i].excess>Predictor::kStallExcessMs)||r[i-1].excess>Predictor::kStallExcessMs||r[i-2].excess>Predictor::kStallExcessMs||r[i-3].excess>Predictor::kStallExcessMs)continue;
        ++e.onsets;
        if(score(r[i-1])>t10){std::size_t j=i-1;while(j>0&&i-j<10&&score(r[j-1])>t10)--j;leads.push_back(static_cast<double>(i-j)*200.0);}
    }
    e.onsetHit=e.onsets?static_cast<double>(leads.size())/static_cast<double>(e.onsets):0;
    if(!leads.empty()){std::sort(leads.begin(),leads.end());e.leadMedianMs=leads[leads.size()/2];}
    for(const auto& r:runs)for(const auto& x:r){const auto b=static_cast<std::size_t>((std::min)(9.0,score(x)*10));e.calib[b][0]+=1;e.calib[b][1]+=x.label;}
    return e;
}
std::string evalJson(const Eval& e){
    std::ostringstream o;
    o<<"{\"positives\": "<<e.positives<<", \"negatives\": "<<e.negatives<<", \"auc\": "<<fmt(e.auc,3)<<", \"hitAt5PctFalseAlarm\": "<<fmt(e.hit5,3)<<", \"falseAlarm5\": "<<fmt(e.fa5,3)
     <<", \"hitAt10PctFalseAlarm\": "<<fmt(e.hit10,3)<<", \"falseAlarm10\": "<<fmt(e.fa10,3)<<", \"stallOnsets\": "<<e.onsets<<", \"onsetsAlarmedBefore\": "<<fmt(e.onsetHit,3)<<", \"medianLeadMs\": "<<fmt(e.leadMedianMs,0)<<", \"calibration\": [";
    for(std::size_t b=0;b<10;++b)o<<(b?", ":"")<<"{\"risk\": \""<<fmt(static_cast<double>(b)/10.0,1)<<"-"<<fmt(static_cast<double>(b+1)/10.0,1)<<"\", \"windows\": "<<fmt(e.calib[b][0],0)<<", \"stallShare\": "<<(e.calib[b][0]>0?fmt(e.calib[b][1]/e.calib[b][0],3):"null")<<"}";
    o<<"]}";return o.str();
}
std::string evalRow(const char* set,const char* model,const Eval& e){
    std::ostringstream o;
    o<<"| "<<set<<" | "<<model<<" | "<<fmt(e.auc,3)<<" | "<<fmt(e.hit5*100,1)<<" % ("<<fmt(e.fa5*100,1)<<" %) | "<<fmt(e.hit10*100,1)<<" % ("<<fmt(e.fa10*100,1)<<" %) | "
     <<fmt(e.onsetHit*100,1)<<" % of "<<e.onsets<<" | "<<fmt(e.leadMedianMs,0)<<" |\n";
    return o.str();
}

int study(unsigned seeds,const std::string& modelOut,const std::string& jsonPath,const std::string& mdPath){
    const auto train=collect(true,1,seeds,true);
    const auto heldSeeds=collect(false,101,100+seeds,true);
    const auto unseen=collect(false,201,200+seeds,false);
    const Logistic lr=fit(train);
    const Forest fo=boost(train);
    const auto stat=[](const Row& r){return r.stat;};
    const auto lrs=[&](const Row& r){return lr.p(r.f);};
    const auto fos=[&](const Row& r){return fo.p(r.f);};
    std::ostringstream md,js;
    md<<"Model result (Linux simulator), not a measurement\n\n";
    md<<"| evaluation set | model | AUC | hit rate at 5 % false alarms (actual) | hit rate at 10 % false alarms (actual) | stall onsets alarmed in the window before | median lead (ms) |\n|---|---|---|---|---|---|---|\n";
    struct Set {const char* name;const std::vector<std::vector<Row>>* runs;};
    js<<"{\n  \"label\": \"Model result (Linux simulator), not a measurement\",\n  \"stallDefinition\": \"frame-gap excess above "<<Predictor::kStallExcessMs<<" ms within the next "<<Predictor::kHorizonMs<<" ms\",\n  \"sets\": {";
    bool first=true;
    for(const Set& s:{Set{"training (seen families, seeds 1..N)",&train},Set{"held-out seeds, seen families",&heldSeeds},Set{"unseen families (periodic, governor case, bandwidth drop, interruptions, reordering + loss)",&unseen}}){
        const auto a=evaluate(*s.runs,stat),b=evaluate(*s.runs,lrs),c=evaluate(*s.runs,fos);
        md<<evalRow(s.name,"statistics",a)<<evalRow(s.name,"logistic",b)<<evalRow(s.name,"stump forest (30 x depth 2)",c);
        js<<(first?"":",")<<"\n    \""<<s.name<<"\": {\"statistics\": "<<evalJson(a)<<", \"logistic\": "<<evalJson(b)<<", \"forest\": "<<evalJson(c)<<"}";first=false;
    }
    js<<"\n  },\n  \"logistic\": {\"bias\": "<<fmt(lr.b,4)<<", \"weights\": [";
    for(std::size_t i=0;i<kFeatureCount;++i)js<<(i?", ":"")<<fmt(lr.w[i],4);
    js<<"]}\n}\n";
    // Calibration of the statistics on the unseen families.
    const auto cal=evaluate(unseen,stat);
    md<<"\nCalibration of the statistics' stallRisk (unseen families; share of windows followed by a stall within 2 s):\n\n| risk | windows | stall share |\n|---|---|---|\n";
    for(std::size_t b=0;b<10;++b)if(cal.calib[b][0]>0)md<<"| "<<fmt(static_cast<double>(b)/10.0,1)<<"-"<<fmt(static_cast<double>(b+1)/10.0,1)<<" | "<<fmt(cal.calib[b][0],0)<<" | "<<fmt(cal.calib[b][1]/cal.calib[b][0]*100,1)<<" % |\n";
    if(!modelOut.empty()){
        std::ofstream o(modelOut);
        o<<"// SPDX-License-Identifier: GPL-3.0-only\n#pragma once\n"
           "// SmartLink learned comparison model: logistic regression over the predictor's\n"
           "// features (SmartLinkTypes.h Features, computed in SmartLinkPredictor.h).\n"
           "// Used ONLY when VEYRA_EXP_RP_SMARTLINK_MODEL=learned; the default is the\n"
           "// statistics. GENERATED by tools/smartlink_sim --study --model-out (trained on\n"
           "// simulated traces of the training families, seeds 1.."<<seeds<<"; a model of a model,\n"
           "// never trained on a real link). Do not edit by hand: re-run the tool.\n"
           "#include <array>\n#include <cstddef>\nnamespace veyra::remoteplay::smartlink::model {\n"
           "inline constexpr double kBias="<<fmt(lr.b,6)<<";\ninline constexpr std::array<double,10> kWeights{";
        for(std::size_t i=0;i<kFeatureCount;++i)o<<(i?",":"")<<fmt(lr.w[i],6);
        o<<"};\n} // namespace veyra::remoteplay::smartlink::model\n";
    }
    if(!jsonPath.empty())std::ofstream(jsonPath)<<js.str();
    if(!mdPath.empty())std::ofstream(mdPath)<<md.str();else std::fputs(md.str().c_str(),stdout);
    return 0;
}

int bench(){
    // A sequence of windows from two scenarios, replayed many times.
    std::vector<Sample> samples;
    for(const auto& c:sim::scenarios()){
        if(std::string(c.name)!="wifi_random_spikes"&&std::string(c.name)!="congestion")continue;
        const auto r=sim::run(c,sim::makePath(c,1),sim::PolicyKind::Rc4Default,{true,1});
        for(const auto& w:r.windows)samples.push_back(w.sample);
    }
    const auto time=[&](auto&& body){
        const auto t0=std::chrono::steady_clock::now();std::size_t n=0;double sink=0;
        for(int rep=0;rep<50;++rep){sink+=body(rep);n+=samples.size();}
        const double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-t0).count()/static_cast<double>(n);
        if(sink==12345.678)std::puts("");
        return ns;
    };
    const double stat=time([&](int rep){Predictor p(ModelKind::Statistics);double s=0;std::int64_t base=static_cast<std::int64_t>(rep)*100000000;for(auto x:samples){x.endMs+=base;s+=p.update(x).stallRisk;}return s;});
    const double learned=time([&](int rep){Predictor p(ModelKind::Learned);double s=0;std::int64_t base=static_cast<std::int64_t>(rep)*100000000;for(auto x:samples){x.endMs+=base;s+=p.update(x).stallRisk;}return s;});
    const double engine=time([&](int rep){Engine e;double s=0;std::int64_t base=static_cast<std::int64_t>(rep)*100000000;for(auto x:samples){x.endMs+=base;s+=e.update(x).stallRisk;}return s;});
    std::printf("{\"label\": \"Linux machine of the simulator (shared 2-core VM, nice 10), not the owner's PC\", \"samples\": %zu, \"predictorStatisticsNsPerSample\": %.0f, \"predictorLearnedNsPerSample\": %.0f, \"engineUpdateNsPerSample\": %.0f}\n",samples.size()*50,stat,learned,engine);
    return 0;
}

int replay(const std::string& file){
    std::ifstream in(file);
    if(!in){std::fprintf(stderr,"cannot open %s\n",file.c_str());return 2;}
    Engine e;std::string line;std::size_t lines=0,used=0;std::int64_t nextLog=-1;
    std::array<std::size_t,5> waits{};std::size_t limited=0,headroom=0,audioRaised=0;
    while(std::getline(in,line)){
        ++lines;Sample s;
        if(!trace::parseTraceLine(line,s))continue;
        ++used;
        const Decision d=e.update(s);
        waits[d.reorderWindowMs==0?0:d.reorderWindowMs==2?1:d.reorderWindowMs==4?2:d.reorderWindowMs==8?3:4]++;
        limited+=d.networkLimited;headroom+=d.linkHeadroom;audioRaised+=d.audioTargetMs>0;
        if(s.endMs>=0&&(nextLog<0||s.endMs>=nextLog)){std::printf("%lld %s\n",static_cast<long long>(s.endMs),std::string(toLogLine(d,Mode::Observe).view()).c_str());nextLog=s.endMs+5000;}
    }
    std::printf("{\"lines\": %zu, \"samples\": %zu, \"windowsAtLeave\": %zu, \"windowsAt2\": %zu, \"windowsAt4\": %zu, \"windowsAt8\": %zu, \"windowsAt16\": %zu, \"networkLimited\": %zu, \"linkHeadroom\": %zu, \"audioRaised\": %zu, \"changes\": %llu}\n",
        lines,used,waits[0],waits[1],waits[2],waits[3],waits[4],limited,headroom,audioRaised,static_cast<unsigned long long>(e.changes()));
    return used?0:1;
}
}
int main(int argc,char** argv){
    unsigned seeds=3;std::string json,md,modelOut,replayFile;bool doStudy=false,doBench=false;
    for(int i=1;i<argc;++i){
        const std::string a=argv[i];
        const auto next=[&]{if(i+1>=argc){std::fprintf(stderr,"%s needs a value\n",a.c_str());std::exit(2);}return std::string(argv[++i]);};
        if(a=="--seeds")seeds=static_cast<unsigned>(std::max(1,std::atoi(next().c_str())));
        else if(a=="--lag-ms")g_lagUs=static_cast<std::int64_t>(std::max(0,std::atoi(next().c_str())))*1000;
        else if(a=="--json")json=next();
        else if(a=="--md")md=next();
        else if(a=="--model-out")modelOut=next();
        else if(a=="--study")doStudy=true;
        else if(a=="--bench")doBench=true;
        else if(a=="--replay")replayFile=next();
        else{std::fprintf(stderr,"usage: smartlink_sim [--seeds N] [--lag-ms L] [--json F] [--md F] | --study [--model-out F] | --bench | --replay trace.csv\n");return 2;}
    }
    if(!replayFile.empty())return replay(replayFile);
    if(doBench)return bench();
    if(doStudy)return study(seeds,modelOut,json,md);
    return table(seeds,json,md);
}
