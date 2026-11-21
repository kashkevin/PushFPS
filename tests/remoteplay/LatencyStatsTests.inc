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
    CHECK(lst::pressLine(3,p)=="n=3 sendToArrivalMs=40.25 arrivalToScanoutMs=-1 totalMs=49.75 phaseMs=3.00 arrivalToPresentMs=9.50 outcome=answered");
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
// Review B1: a press whose picture was moving is not measurable: never a time, left out of the statistics, counted.
TEST(latencyStatsPictureMovingPresses){
    CHECK(lst::pressLine(4,lst::pictureMovingPress())=="n=4 sendToArrivalMs=-1 arrivalToScanoutMs=-1 totalMs=-1 phaseMs=-1 arrivalToPresentMs=-1 outcome=picture-moving");
    std::vector<lst::PressSplit> presses;
    for(int i=0;i<10;++i){lst::PressSplit q;q.sendToArrivalMs=40.0+i;q.arrivalToPresentMs=9;q.totalMs=q.sendToArrivalMs+9;presses.push_back(q);}
    auto withMoving=presses;
    for(int i=0;i<4;++i)withMoving.push_back(lst::pictureMovingPress());
    const auto a=lst::summarizeTest(presses,14,lst::Button::Options),b=lst::summarizeTest(withMoving,14,lst::Button::Options);
    CHECK(b.answered==10&&b.unmeasurable==4&&a.unmeasurable==0);
    CHECK(b.total.n==10&&b.console.n==10&&b.pc.n==10&&b.total.p50==a.total.p50&&b.console.p95==a.console.p95);
    CHECK(lst::testLine(b).find(" pcBasis=present unmeasurable=4 ")!=std::string::npos);
    CHECK(lst::testLine(b).rfind("trials=14 answered=10 button=options",0)==0);
    // Stop early when most presses so far could not be measured, never before six.
    CHECK(!lst::mostlyUnmeasurable(5,5));
    CHECK(lst::mostlyUnmeasurable(4,6)&&!lst::mostlyUnmeasurable(3,6));
    CHECK(lst::mostlyUnmeasurable(101,200)&&!lst::mostlyUnmeasurable(100,200));
}
// Review delta1 blocker 1: sham presses (nothing pressed, judged like a press). The bound: a scene whose shams answer
// 20 % of the time is accepted under 5 % of the time; shams never enter a latency statistic.
TEST(latencyStatsShamPresses){
    CHECK(lst::shamCount(8)==14&&lst::shamCount(56)==14&&lst::shamCount(57)==15&&lst::shamCount(500)==125);
    CHECK(lstNear(lst::binomialCdf(0,14,0.2),std::pow(0.8,14),1e-12)&&lst::binomialCdf(14,14,0.2)==1.0&&lst::binomialCdf(-1,5,0.2)==0.0);
    CHECK(lst::shamAnswersAllowed(5)==-1&&lst::shamAnswersAllowed(13)==-1&&lst::shamAnswersAllowed(14)==0);
    CHECK(lst::shamAnswersAllowed(20)==0&&lst::shamAnswersAllowed(25)==1&&lst::shamAnswersAllowed(30)==2&&lst::shamAnswersAllowed(125)==17);
    for(int n=14;n<=200;++n){const int k=lst::shamAnswersAllowed(n);CHECK(k>=0&&lst::binomialCdf(k,n,0.2)<0.05&&lst::binomialCdf(k+1,n,0.2)>=0.05);}
    CHECK(lst::shamValid(14,0)&&!lst::shamValid(14,1)&&!lst::shamValid(13,0)&&lst::shamValid(125,17)&&!lst::shamValid(125,18));
    // Simulated acceptance (deterministic): a 20 % scene with 14 shams passes under 5 % of 20000 tests; a quiet one always.
    lst::Random r(5);int accepted=0;
    for(int t=0;t<20000;++t){int a=0;for(int i=0;i<14;++i)a+=r.unit()<0.2?1:0;accepted+=lst::shamValid(14,a)?1:0;}
    CHECK(accepted<1000&&accepted>600);   // expected 0.8^14 = 4.4 %
    // The schedule: the right counts, a permutation that changes with the seed, shams spread over the test.
    lst::Random s1(1),s2(2);
    const auto a=lst::shamSchedule(s1,8,14),b=lst::shamSchedule(s2,8,14);
    int shams=0;for(bool x:a)shams+=x?1:0;
    CHECK(a.size()==22&&shams==14&&a!=b);
    lst::Random s3(3);int firstHalf=0;const auto big=lst::shamSchedule(s3,500,125);
    for(std::size_t i=0;i<big.size()/2;++i)firstHalf+=big[i]?1:0;
    CHECK(firstHalf>40&&firstHalf<85);
    // Outcomes and lines; shams are left out of every statistic and decide validity.
    CHECK(lst::pressLine(2,lst::shamPress(false,false))=="n=2 sendToArrivalMs=-1 arrivalToScanoutMs=-1 totalMs=-1 phaseMs=-1 arrivalToPresentMs=-1 outcome=sham-clean");
    CHECK(lst::pressLine(3,lst::shamPress(true,false,116.5))=="n=3 sendToArrivalMs=-1 arrivalToScanoutMs=-1 totalMs=116.50 phaseMs=-1 arrivalToPresentMs=-1 outcome=sham-answered");
    CHECK(lst::pressLine(4,lst::shamPress(false,true)).find("outcome=sham-moving")!=std::string::npos);
    std::vector<lst::PressSplit> presses;
    for(int i=0;i<12;++i){lst::PressSplit q;q.sendToArrivalMs=40.0+i;q.arrivalToPresentMs=9;q.totalMs=q.sendToArrivalMs+9;presses.push_back(q);}
    const auto plain=lst::summarizeTest(presses,12,lst::Button::Options);
    auto quiet=presses;for(int i=0;i<14;++i)quiet.push_back(lst::shamPress(false,false));
    const auto ok=lst::summarizeTest(quiet,12,lst::Button::Options);
    CHECK(ok.valid&&ok.shams==14&&ok.shamAnswered==0&&ok.answered==12&&ok.total.n==12&&ok.total.p50==plain.total.p50&&ok.console.p95==plain.console.p95);
    CHECK(lst::testLine(ok).find(" shams=14 shamAnswered=0 shamMoving=0 valid=1")!=std::string::npos);
    auto noisy=quiet;noisy[13]=lst::shamPress(true,false,90);   // one of 14 shams answered
    const auto bad=lst::summarizeTest(noisy,12,lst::Button::Options);
    CHECK(!bad.valid&&bad.shamAnswered==1&&bad.answered==12&&bad.total.n==0&&std::isnan(bad.total.p50)&&bad.basis==lst::PcBasis::None);
    const auto badLine=lst::testLine(bad);
    CHECK(badLine.find("consoleP50=-1 consoleP95=-1 pcP50=-1 pcP95=-1 totalP50=-1 totalP95=-1")!=std::string::npos&&badLine.find(" valid=0")!=std::string::npos);
    // Shams that could not be judged do not count as judged: 14 shams with one moving leaves 13, too few to tell.
    auto moving=quiet;moving[13]=lst::shamPress(false,true);
    CHECK(!lst::summarizeTest(moving,12,lst::Button::Options).valid);
    // The PS button has no shams: its line has no sham keys.
    CHECK(lst::testLine(lst::summarizeTest(presses,12,lst::Button::Ps)).find("shams=")==std::string::npos);
    CHECK(std::string(lst::kInvalidReason)=="picture-changes-by-itself");
}
} // namespace
