// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// SmartLink predictor: lightweight statistics over the recent Sample windows
// (SmartLinkTypes.h) -> the REGIME the link is in, and from it the chance of a
// stall soon (`stallRisk`). Portable, header-only, O(1) per sample, no
// allocation, no locking (one owner thread). Unverified on Windows and on a
// PS5; calibrated only in the Linux simulator (tools/smartlink_sim).
//
// What it predicts, and what it does not (docs/SMARTLINK_RESEARCH.md I3, F):
// no client-side signal gives seconds of warning of a SINGLE Wi-Fi drought
// (the cause sits in MAC contention and the access point's queue, which the
// PC cannot see). What is predictable is the regime: spikes that recur with a
// stable period, congestion that shows a rising delay trend before loss, a
// link that has been stalling in clusters. `stallRisk` is therefore "how likely
// a stall is in the next kHorizonMs given the regime", not a forecast of one
// spike; acting on it means keeping a margin while the regime lasts.
//
// "A stall" is a frame gap that exceeds the nominal frame interval by more
// than kStallExcessMs. Ingredients, each a probability, combined as
// independent causes (risk = 1 - prod(1 - p)):
//   - hazard: how often stalls happened (exponentially weighted rates over
//     about a minute and over a few seconds: Wi-Fi trouble comes in clusters);
//   - periodic spikes: a period check of the last spike times (the interval
//     that most other intervals are a whole multiple of, within 12 %); the
//     risk is raised from shortly before the next expected spike to shortly
//     after it, and the pattern is dropped after four expected spikes that
//     did not come;
//   - queue building: a least-squares trendline slope of the frames' arrival
//     lateness (Sample::frameLateMeanMs) against an adaptive threshold, the
//     GCC delay-trend idea re-implemented from its description
//     (draft-ietf-rmcat-gcc-02 section 5; research doc E), plus one-sided CUSUM
//     detectors on the console round trip and on the frame-gap excess;
//   - the recent loss rate.
// The Wi-Fi signal is NOT part of the risk (research doc I2 #6: a profile hint
// only, never by itself a reason to change a buffer); it feeds the Regime
// (WeakSignal needs trouble in the stream as well) and the profile advice.
//
// The learned comparison model (SmartLinkModel.h) sees the same `features`;
// it is used only when ModelKind::Learned is selected (never the default).
#include "SmartLinkModel.h"
#include "SmartLinkTypes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace veyra::remoteplay::smartlink {

class Predictor {
public:
    // T: 25 ms beyond the nominal interval = 2.5 intervals shown instead of one
    // at 60 fps: a visible hitch (the stutter metric counts > 1.5 intervals; this
    // is deliberately the more severe kind, the one worth acting on). Also above
    // what the largest reorder step a wired LAN uses (2-16 ms) explains.
    static constexpr double kStallExcessMs=25;
    // H: 2 s. Long enough for an action to take effect (the reorder wait reaches
    // the library at its next look, ~250 ms; the audio reserve grows at most
    // 5 ms per second, docs/AUDIO.md finding 2), short enough that "soon" means
    // something: the simulator's periodic Wi-Fi spikes repeat every 5-30 s.
    static constexpr double kHorizonMs=2000;
    static constexpr double kSlowTauMs=60000,kFastTauMs=5000;   // hazard rates
    static constexpr double kMeanTauMs=10000;                   // "mean jitter"
    static constexpr double kQuantileTauMs=60000;               // P95 of the excess
    static constexpr double kRttTauMs=30000;                    // CUSUM reference
    static constexpr double kSignalFastTauMs=10000,kSignalSlowTauMs=60000;
    static constexpr double kEventSeparationMs=600;             // windows closer than this belong to one spike
    static constexpr double kMaxStepMs=5000;                    // longer pauses count as 5 s (a suspended PC is not 1 h of evidence)
    static constexpr double kSummaryMs=5000;                    // the log line's period
    static constexpr std::int64_t kRebaseAfterRejects=25;       // 5 s of backwards time: take the new clock
    static constexpr std::int64_t kMaxCount=1000000;
    static constexpr std::size_t kEvents=12,kRing=40,kTrend=20; // trendline over the last 20 windows (~4 s; GCC uses 20 packet groups)
    // Adaptive slope threshold in ms/s: start, bounds, and how fast it follows the slope up / down.
    static constexpr double kTrendGammaStart=10,kTrendGammaMin=3,kTrendGammaMax=100,kTrendUpPerS=0.5,kTrendDownPerS=0.05;
    // Histogram edges (ms) for the streaming quantile of the excess.
    static constexpr std::array<double,20> kEdges{1,2,3,5,8,12,16,20,25,30,40,50,60,80,100,150,200,300,500,1000};

    explicit Predictor(ModelKind model=ModelKind::Statistics)noexcept:model_(model){}
    void reset()noexcept{const auto m=model_;*this=Predictor(m);}
    ModelKind model()const noexcept{return model_;}

    Prediction update(const Sample& s)noexcept{
        Prediction p;
        // --- time ---
        double dt=0;
        if(s.endMs>=0){
            if(have_&&s.endMs<=lastEnd_){
                // Time went backwards or stood still: rejected, unless it keeps
                // doing so (a clock that was reset): then the new clock is taken.
                if(++rejects_<kRebaseAfterRejects){p=last_;p.usable=false;return p;}
                lastEnd_=s.endMs;rejects_=0;dt=known(s.windowMs)?(std::min)(s.windowMs,kMaxStepMs):200.0;
            }else{
                dt=have_?static_cast<double>(s.endMs-lastEnd_):(known(s.windowMs)?s.windowMs:200.0);
                lastEnd_=s.endMs;rejects_=0;
            }
        }else if(known(s.windowMs)&&s.windowMs>0){
            dt=s.windowMs;lastEnd_+=static_cast<std::int64_t>(std::llround((std::min)(s.windowMs,kMaxStepMs)));
        }else{p=last_;p.usable=false;return p;}
        if(!(dt>0)||!known(s.windowMs)||s.windowMs<=0){
            // A window of 0 ms (or none) carries no counts worth reading; time still advances.
            if(dt>0){now_+=(std::min)(dt,kMaxStepMs);}
            have_=true;p=last_;p.usable=false;return p;
        }
        dt=(std::min)(dt,kMaxStepMs);
        now_+=dt;have_=true;++samples_;
        const double win=(std::min)(s.windowMs,kMaxStepMs);

        // --- the excess of the largest frame gap over the interval ---
        double excess=-1;
        if(known(s.frameGapExcessMs))excess=s.frameGapExcessMs;
        else if(known(s.frameGapMaxMs))excess=(std::max)(0.0,s.frameGapMaxMs-1000.0/60.0);
        const bool interrupted=s.framesArrived==0&&win>=100;
        if(interrupted)excess=(std::max)(excess,win);
        if(excess>10000)excess=10000;

        const double slow=std::exp(-dt/kSlowTauMs),fast=std::exp(-dt/kFastTauMs),mean=std::exp(-dt/kMeanTauMs),q=std::exp(-dt/kQuantileTauMs);
        slowCount_*=slow;fastCount_*=fast;
        if(excess>=0){
            excessMean_=excessMean_*mean+(1-mean)*(std::min)(excess,200.0);
            for(auto& b:hist_)b*=q;
            std::size_t bucket=0;while(bucket<kEdges.size()&&excess>kEdges[bucket])++bucket;
            hist_[bucket]+=1;
            // CUSUM on the per-window excess (clipped): a rising queue.
            const double x=(std::min)(excess,60.0);
            gapCusum_=(std::max)(0.0,gapCusum_*fast+x-excessMean_-4.0);
            if(gapCusum_>60){gapAlarmUntil_=now_+3000;gapCusum_=0;}
            if(excess>kStallExcessMs){
                if(now_-lastSpikeWindow_>kEventSeparationMs+win){
                    slowCount_+=1;fastCount_+=1;
                    events_[eventNext_]=now_;eventNext_=(eventNext_+1)%kEvents;if(eventCount_<kEvents)++eventCount_;
                    lastEvent_=now_;
                }
                lastSpikeWindow_=now_;
            }
        }
        // --- queue building: trendline slope of the arrival lateness ---
        if(known(s.frameLateMeanMs)&&s.frameLateMeanMs<1e7){
            smoothLate_=lateHave_?0.9*smoothLate_+0.1*s.frameLateMeanMs:s.frameLateMeanMs;lateHave_=true;
            trendT_[trendNext_]=now_;trendY_[trendNext_]=smoothLate_;trendNext_=(trendNext_+1)%kTrend;if(trendCount_<kTrend)++trendCount_;
            if(trendCount_>=kTrend/2){
                double mt=0,my=0;for(std::size_t i=0;i<trendCount_;++i){mt+=trendT_[i];my+=trendY_[i];}
                mt/=static_cast<double>(trendCount_);my/=static_cast<double>(trendCount_);
                double sxy=0,sxx=0;for(std::size_t i=0;i<trendCount_;++i){sxy+=(trendT_[i]-mt)*(trendY_[i]-my);sxx+=(trendT_[i]-mt)*(trendT_[i]-mt);}
                const double slope=sxx>0?sxy/sxx*1000.0:0.0; // ms of lateness per second
                // Adaptive threshold: follows the slope's own noise, fast up, slow down (GCC's shape).
                const double k=std::abs(slope)>trendGamma_?kTrendUpPerS:kTrendDownPerS;
                if(std::abs(slope)-trendGamma_<=15.0)trendGamma_+=k*(dt/1000.0)*(std::abs(slope)-trendGamma_);
                trendGamma_=std::clamp(trendGamma_,kTrendGammaMin,kTrendGammaMax);
                trendOver_=slope>trendGamma_?trendOver_+1:0;
                if(trendOver_>=3){trendAlarmUntil_=now_+3000;trendOver_=0;}
                trendSlope_=slope;
            }
        }
        // --- round trip ---
        if(known(s.rttMs)&&s.rttMs<60000){
            const double r=s.rttMs,a=1-std::exp(-dt/kRttTauMs);
            if(!rttHave_){rttMean_=r;rttDev_=0;rttHave_=true;}
            const double k=(std::max)(2.0,0.5*rttDev_),h=(std::max)(20.0,4.0*rttDev_);
            rttCusum_=(std::max)(0.0,rttCusum_+r-rttMean_-k);
            if(rttCusum_>h){rttAlarmUntil_=now_+3000;rttCusum_=0;}
            rttMean_+=a*(r-rttMean_);rttDev_+=a*(std::abs(r-rttMean_)-rttDev_);
            rttLast_=r;
        }
        // --- loss ---
        if(known(s.packetsLost)&&known(s.packetsReceived)){
            const double total=static_cast<double>(s.packetsLost)+static_cast<double>(s.packetsReceived);
            const double share=total>0?static_cast<double>(s.packetsLost)/total:0.0;
            lossRate_=lossRate_*fast+(1-fast)*(std::min)(share,1.0);
        }
        // --- Wi-Fi signal ---
        double signal=-1;
        if(known(s.wifiSignalPercent))signal=(std::min)(s.wifiSignalPercent,100.0);
        else if(rssiKnown(s.wifiRssiDbm))signal=std::clamp(2.0*(s.wifiRssiDbm+100.0),0.0,100.0); // the usual dBm-to-percent mapping
        if(signal>=0){
            if(!signalHave_){signalFast_=signalSlow_=signal;signalHave_=true;}
            const double af=1-std::exp(-dt/kSignalFastTauMs),as=1-std::exp(-dt/kSignalSlowTauMs);
            signalFast_+=af*(signal-signalFast_);signalSlow_+=as*(signal-signalSlow_);
        }
        // --- ring for the 5 s summary ---
        Recent& r=ring_[ringNext_];ringNext_=(ringNext_+1)%kRing;if(ringCount_<kRing)++ringCount_;
        // Counts are clipped (a window cannot hold more than a million packets) so the 5 s sums never overflow.
        r={now_,excess,known(s.rttMs)?s.rttMs:-1.0,known(s.packetsLate)?(std::min)(s.packetsLate,kMaxCount):0,known(s.packetsLost)?(std::min)(s.packetsLost,kMaxCount):0};

        // --- periodic spikes ---
        periodic();

        // --- combine ---
        const double slowRate=slowCount_/kSlowTauMs,fastRate=fastCount_/kFastTauMs;
        const double pSlow=1-std::exp(-kHorizonMs*slowRate),pFast=1-std::exp(-kHorizonMs*fastRate*0.5);
        double pPeriodic=0;
        if(period_>0){
            const double tol=tolerance(period_);
            if(nextSpike_-now_<=kHorizonMs+tol&&now_-nextSpike_<=tol)pPeriodic=0.9*periodConfidence_;
        }
        const double pRtt=now_<rttAlarmUntil_?0.35:0.0,pGap=now_<gapAlarmUntil_?0.35:0.0;
        const double pLoss=(std::min)(0.5,lossRate_*50.0);
        const double pTrend=now_<trendAlarmUntil_?0.35:0.0;
        const double trend=signalHave_?(signalFast_-signalSlow_)*60000.0/(kSignalSlowTauMs-kSignalFastTauMs):0.0;

        Features f{};
        f[0]=(std::min)((std::max)(excess,0.0)/100.0,1.0);
        f[1]=(std::min)(excessMean_/20.0,1.0);
        f[2]=pSlow;
        f[3]=1-std::exp(-kHorizonMs*fastRate);
        f[4]=pPeriodic;
        f[5]=(std::min)(quantile(0.95)/100.0,1.0);
        f[6]=now_<rttAlarmUntil_?1.0:0.0;
        f[7]=now_<gapAlarmUntil_||now_<trendAlarmUntil_?1.0:0.0;
        f[8]=(std::min)(lossRate_*50.0,1.0);
        f[9]=signalHave_?std::clamp((60.0-signalFast_)/60.0,0.0,1.0):0.0;

        double risk=0;
        if(model_==ModelKind::Learned)risk=learnedRisk(f);
        else{
            double keep=1;
            for(double x:{pSlow,pFast,pPeriodic,pRtt,pGap,pTrend,pLoss})keep*=1-std::clamp(x,0.0,1.0);
            risk=1-keep;
        }
        if(!std::isfinite(risk))risk=0;
        p.stallRisk=std::clamp(risk,0.0,1.0);
        p.features=f;p.usable=true;
        p.jitterMeanMs=excessMean_;
        p.gapExcessP95Ms=samples_>0?quantile(0.95):-1;
        p.rttMeanMs=rttHave_?rttMean_:-1;
        p.spikePeriodMs=period_>0?period_:-1;
        p.msToNextSpike=period_>0?(std::max)(0.0,nextSpike_-now_):-1;
        p.signalTrendPerMin=trend;
        double gmax=-1,rmax=-1;
        for(std::size_t i=0;i<ringCount_;++i){
            if(now_-ring_[i].at>=kSummaryMs)continue;
            gmax=(std::max)(gmax,ring_[i].excess);rmax=(std::max)(rmax,ring_[i].rtt);
        }
        p.gapExcessMaxMs=gmax;p.rttMaxMs=rmax;
        // --- regime ---
        Regime g=Regime::Stable;
        const bool trouble=quantile(0.95)>kStallExcessMs*0.5||lossRate_>0.002||fastCount_>0.5;
        if(samples_<10)g=Regime::Unknown;
        if(interrupted)g=Regime::Interrupted;
        else if(samples_>=10){
            if(signalHave_&&(signalFast_<40||trend<-10)&&trouble)g=Regime::WeakSignal;
            // A round-trip alarm alone is not congestion: the console's round trip on
            // Wi-Fi can swing 2-79 ms with every frame on time (BENCH1). It needs
            // trouble in the stream too (gaps, loss or a fast stall count).
            else if((now_<rttAlarmUntil_&&trouble)||now_<gapAlarmUntil_||now_<trendAlarmUntil_||lossRate_>0.01)g=Regime::Congested;
            else if(period_>0&&periodConfidence_>=0.75)g=Regime::PeriodicSpikes;
        }
        p.regime=g;
        last_=p;
        return p;
    }
    // Current values, for the controller's summary.
    double nowMs()const noexcept{return now_;}
    double lossRate()const noexcept{return lossRate_;}
    double signal()const noexcept{return signalHave_?signalFast_:-1;}
    double lastRtt()const noexcept{return rttLast_;}
    // Spike events (excess above kStallExcessMs, one per cluster) since `sinceMs`.
    unsigned spikesSince(double sinceMs)const noexcept{
        unsigned n=0;for(std::size_t i=0;i<eventCount_;++i)if(events_[i]>sinceMs)++n;return n;
    }
    double lastSpikeMs()const noexcept{return lastEvent_;}
    double trendSlopeMsPerS()const noexcept{return trendSlope_;}
    std::int64_t lateSince(double sinceMs)const noexcept{std::int64_t n=0;for(std::size_t i=0;i<ringCount_;++i)if(ring_[i].at>sinceMs)n+=ring_[i].late;return n;}
    std::int64_t lostSince(double sinceMs)const noexcept{std::int64_t n=0;for(std::size_t i=0;i<ringCount_;++i)if(ring_[i].at>sinceMs)n+=ring_[i].lost;return n;}
    // Streaming quantile of the excess (upper edge of the bucket), about the last minute.
    double quantile(double fraction)const noexcept{
        double total=0;for(double b:hist_)total+=b;
        if(total<=0)return 0;
        double sum=0;
        for(std::size_t i=0;i<hist_.size();++i){
            sum+=hist_[i];
            if(sum>=fraction*total)return i==0?0.0:(i<kEdges.size()?kEdges[i]:kEdges.back()*2);
        }
        return kEdges.back()*2;
    }
private:
    static double tolerance(double period)noexcept{return (std::max)(300.0,0.12*period);}
    static double learnedRisk(const Features& f)noexcept{
        double z=model::kBias;
        for(std::size_t i=0;i<kFeatureCount;++i)z+=model::kWeights[i]*f[i];
        z=std::clamp(z,-30.0,30.0);
        return 1.0/(1.0+std::exp(-z));
    }
    void periodic()noexcept{
        period_=0;periodConfidence_=0;
        if(eventCount_<5)return;
        // Event times in order, newest last.
        std::array<double,kEvents> t{};
        for(std::size_t i=0;i<eventCount_;++i)t[i]=events_[(eventNext_+kEvents-eventCount_+i)%kEvents];
        std::array<double,kEvents> iv{};std::size_t n=0;
        for(std::size_t i=1;i<eventCount_;++i)iv[n++]=t[i]-t[i-1];
        // Candidate period: the smallest interval that is consistent with most
        // others (intervals near a whole multiple of it, up to 3x, count).
        double best=0,bestShare=0;
        for(std::size_t c=0;c<n;++c){
            const double m=iv[c];
            if(m<1500||m>300000)continue;
            std::size_t ok=0;double sum=0,weight=0;
            for(std::size_t i=0;i<n;++i){
                const double k=std::round(iv[i]/m);
                if(k<1||k>3)continue;
                if(std::abs(iv[i]-k*m)<=tolerance(m)*k){++ok;sum+=iv[i];weight+=k;}
            }
            const double share=static_cast<double>(ok)/static_cast<double>(n);
            if(share>bestShare+1e-9||(std::abs(share-bestShare)<1e-9&&m<best)){bestShare=share;best=weight>0?sum/weight:m;}
        }
        if(bestShare<0.75||best<=0)return;
        period_=best;periodConfidence_=bestShare;
        const double last=t[eventCount_-1];
        double next=last+best;
        const double tol=tolerance(best);
        // Roll forward past expected spikes that did not come (at most a few: then the pattern is gone).
        int missed=0;
        while(now_-next>tol&&missed<4){next+=best;++missed;}
        if(missed>=4){period_=0;periodConfidence_=0;return;}
        if(missed>0)periodConfidence_*=std::pow(0.7,missed);
        nextSpike_=next;
    }
    struct Recent {double at=-1e18,excess=-1,rtt=-1;std::int64_t late=0,lost=0;};
    ModelKind model_=ModelKind::Statistics;
    Prediction last_{};
    bool have_=false,rttHave_=false,signalHave_=false;
    std::int64_t lastEnd_=0,rejects_=0;
    double now_=0;std::uint64_t samples_=0;
    double slowCount_=0,fastCount_=0,excessMean_=0,gapCusum_=0,gapAlarmUntil_=-1;
    double lastSpikeWindow_=-1e18,lastEvent_=-1e18;
    std::array<double,kEdges.size()+1> hist_{};
    double rttMean_=0,rttDev_=0,rttCusum_=0,rttAlarmUntil_=-1,rttLast_=-1;
    double lossRate_=0,signalFast_=0,signalSlow_=0;
    std::array<double,kEvents> events_{};std::size_t eventNext_=0,eventCount_=0;
    double period_=0,periodConfidence_=0,nextSpike_=0;
    bool lateHave_=false;double smoothLate_=0,trendGamma_=kTrendGammaStart,trendSlope_=0,trendAlarmUntil_=-1;unsigned trendOver_=0;
    std::array<double,kTrend> trendT_{},trendY_{};std::size_t trendNext_=0,trendCount_=0;
    std::array<Recent,kRing> ring_{};std::size_t ringNext_=0,ringCount_=0;
};
} // namespace veyra::remoteplay::smartlink
