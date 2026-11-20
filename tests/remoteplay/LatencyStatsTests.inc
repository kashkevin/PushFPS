// SPDX-License-Identifier: GPL-3.0-only
// Latency test statistics (veyra/engine/PushLatencyStats.h). Included by CoreTests.cpp directly above main().
// The samples below are synthetic (a deterministic generator), not measurements.
#include "veyra/engine/PushLatencyStats.h"
#include <cmath>
#include <string>
#include <vector>
namespace {
namespace lst=veyra::engine::latencystats;
bool lstNear(double a,double b,double tol){return std::abs(a-b)<=tol;}
// Normal-ish sample: sum of 12 uniforms (Irwin-Hall), centred, scaled.
std::vector<double> lstSample(std::size_t n,double centre,double sigma,std::uint64_t seed){
    lst::Random r(seed);std::vector<double> v;v.reserve(n);
    for(std::size_t i=0;i<n;++i){double s=0;for(int k=0;k<12;++k)s+=r.unit();v.push_back(centre+(s-6.0)*sigma);}
    return v;
}
TEST(latencyStatsQuantilesAndMedian){
    CHECK(std::isnan(lst::median({})));
    CHECK(lst::median({5.0})==5.0);
    CHECK(lst::median({1.0,3.0})==2.0);
    CHECK(lst::median({9.0,1.0,5.0})==5.0);
    CHECK(lstNear(lst::quantile({0.0,10.0},0.95),9.5,1e-9));
    CHECK(lst::quantile({4.0,1.0,3.0,2.0},0.0)==1.0);
    CHECK(lst::quantile({4.0,1.0,3.0,2.0},1.0)==4.0);
}
TEST(latencyStatsTrimmedMeanIgnoresOutliers){
    std::vector<double> v{10,10,10,10,10,10,10,10,10,500};
    CHECK(lstNear(lst::trimmedMean(v,0.1),10.0,1e-9));   // 1 of 10 cut at each end
    CHECK(lst::trimmedMean(v,0.0)>50.0);
    CHECK(lstNear(lst::robustSigma(v),0.0,1e-9));
}
TEST(latencyStatsBootstrapIsDeterministicAndCoversTheCentre){
    const auto v=lstSample(200,550.0,26.0,7);
    const auto a=lst::bootstrap95(v,lst::Statistic::Median,1000,3);
    const auto b=lst::bootstrap95(v,lst::Statistic::Median,1000,3);
    CHECK(a.valid()&&a.low==b.low&&a.high==b.high);
    CHECK(a.low<550.0+8&&a.high>550.0-8);
    CHECK(a.high-a.low<15.0);                     // ~ +-1.96*1.25*26/sqrt(200) = +-4.5 ms
    CHECK(!lst::bootstrap95({1,2,3},lst::Statistic::Median).valid());
    const auto t=lst::bootstrap95(v,lst::Statistic::TrimmedMean,500,3);
    CHECK(t.valid()&&t.low<t.high);
}
TEST(latencyStatsDetectableDifferenceShrinksWithPresses){
    // A spread like the PS-button test of the first hardware run (sd 26 ms): 12 presses cannot detect a few ms,
    // 500 presses get close to 6 ms; a PC-side spread of 1 ms gets well under 1 ms.
    const auto few=lstSample(12,550.0,26.0,11),many=lstSample(500,550.0,26.0,11),pc=lstSample(500,10.0,1.0,5);
    const double dFew=lst::detectableDifferenceMs(few,lst::bootstrap95(few,lst::Statistic::Median));
    const double dMany=lst::detectableDifferenceMs(many,lst::bootstrap95(many,lst::Statistic::Median));
    const double dPc=lst::detectableDifferenceMs(pc,lst::bootstrap95(pc,lst::Statistic::Median));
    CHECK(dFew>20.0);
    CHECK(dMany<9.0&&dMany>3.0);
    CHECK(dPc<0.5);
    CHECK(lst::pressesForDifference(26.0,5.0)>300);
    CHECK(lst::pressesForDifference(1.0,0.5)<100);
    CHECK(lst::pressesForDifference(0.0,1.0)==-1);
}
TEST(latencyStatsSplitOfOnePress){
    // sent at 1 s, frame arrived 40 ms later, presented 10 ms after arrival, scanout unknown.
    const std::int64_t s=10000000;
    auto p=lst::split(s,s+400000,s+500000,0,s-50000,1000.0/59.94);
    CHECK(lstNear(p.sendToArrivalMs,40.0,1e-9));
    CHECK(lstNear(p.arrivalToPresentMs,10.0,1e-9));
    CHECK(p.arrivalToScanoutMs==-1);
    CHECK(lstNear(p.totalMs,50.0,1e-9));         // send -> present while scanout is unknown
    CHECK(lstNear(p.phaseMs,5.0,1e-9));
    p=lst::split(s,s+400000,s+500000,s+600000,s-250000,1000.0/60.0);
    CHECK(lstNear(p.arrivalToScanoutMs,20.0,1e-9));
    CHECK(lstNear(p.totalMs,60.0,1e-9));
    CHECK(lstNear(p.phaseMs,25.0-1000.0/60.0,1e-9));
    p=lst::split(s,0,s+500000,0,0,16.7);         // arrival not found
    CHECK(p.sendToArrivalMs==-1&&p.arrivalToPresentMs==-1&&lstNear(p.totalMs,50.0,1e-9)&&p.phaseMs==-1);
    p=lst::split(s,s-1,s+500000,0,0,16.7);       // an arrival before the press is not this press's frame
    CHECK(p.sendToArrivalMs==-1);
}
TEST(latencyStatsPressGapsCoverEveryPhase){
    lst::Random r(42);
    const double interval=1000.0/59.94;
    int bins[8]{};
    for(int i=0;i<4000;++i){
        const double g=lst::pressGapMs(r,300.0,interval,4);
        CHECK(g>=300.0&&g<300.0+4*interval);
        ++bins[int(std::fmod(g-300.0,interval)/interval*8)];
    }
    for(int b:bins)CHECK(b>350&&b<650);          // uniform within a frame: no phase is favoured
    lst::Random r2(1);
    CHECK(lst::pressGapMs(r2,100.0,0.0,4)<100.0+4*1000.0/60.0+1e-9);   // unknown interval: 60 fps
}
TEST(latencyStatsButtonsAndLines){
    CHECK(lst::parseButton("")==lst::Button::Ps);
    CHECK(lst::parseButton("ps")==lst::Button::Ps);
    CHECK(lst::parseButton("options")==lst::Button::Options);
    CHECK(lst::parseButton("dpad-right-left")==lst::Button::DpadRightLeft);
    CHECK(!lst::parseButton("PS"));
    CHECK(!lst::parseButton("cross"));
    CHECK(std::string(lst::buttonName(lst::Button::DpadRightLeft))=="dpad-right-left");
    lst::PressSplit p;p.sendToArrivalMs=40.25;p.arrivalToPresentMs=9.5;p.totalMs=49.75;p.phaseMs=3.0;
    CHECK(lst::pressLine(3,p)=="n=3 sendToArrivalMs=40.25 arrivalToScanoutMs=-1 totalMs=49.75 phaseMs=3.00 arrivalToPresentMs=9.50");
    std::vector<lst::PressSplit> presses;
    lst::Random r(9);
    for(int i=0;i<40;++i){lst::PressSplit q;q.sendToArrivalMs=40+r.unit()*10;q.arrivalToPresentMs=9+r.unit();q.totalMs=q.sendToArrivalMs+q.arrivalToPresentMs;presses.push_back(q);}
    const auto t=lst::summarizeTest(presses,42,lst::Button::Options);
    CHECK(t.answered==40&&t.basis==lst::PcBasis::Present);
    const auto line=lst::testLine(t);
    CHECK(line.rfind("trials=42 answered=40 button=options consoleP50=",0)==0);
    for(const char* key:{" consoleP95="," pcP50="," pcP95="," totalP50="," totalP95="," pcBasis=present"," totalDetectable="})CHECK(line.find(key)!=std::string::npos);
    CHECK(line.find(" pcP50=")<line.find(" totalP50=")&&line.find(" totalP95=")<line.find(" pcBasis="));
    const auto empty=lst::summarizeTest({},12,lst::Button::Ps);
    CHECK(lst::testLine(empty).find("consoleP50=-1 consoleP95=-1 pcP50=-1")!=std::string::npos);
    CHECK(lst::number(std::nan(""))=="-1");
}
} // namespace
