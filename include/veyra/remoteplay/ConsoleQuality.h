// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The console's own view of the stream: takion CONNECTIONQUALITY (target_bitrate,
// upstream_bitrate, upstream_loss, rtt, loss), which chiaki-ng only logs at VERBOSE.
// Chiaki patch 0004 keeps the newest values as counters (CHIAKI_VEYRA_STAT_CQ_*);
// this turns them into milliseconds and kbit/s. Portable, header-only, pure.
//
// What is NOT known from the code (pinned chiaki-ng, the protobuf and launchspec.c):
//   - the unit of `rtt` (a protobuf double) and of `target_bitrate` / `upstream_bitrate`
//     (uint32). The patch keeps rtt x 1e6 as an integer (TransportCounters::consoleRttMilli,
//     whose name is historical), so a sub-millisecond value sent in seconds survives.
//     The unit is decided by ConsoleUnits, ONE object per session shared by the health
//     line and the SmartLink feed: each new report "supports" a unit when its converted
//     value lies within 10x of a reference measured on our side (the round trip the
//     library measured while connecting, Senkusha; the bitrate requested at launch),
//     and a unit is chosen only after 3 such reports with at least 3 more than any other
//     unit has. Candidate units are 1000x apart, so one value supports at most one.
//     Until then, and whenever a converted value falls outside the plausible range
//     (0.1-2000 ms; 100-1000000 kbit/s), the value is unknown (-1), never a guess.
//   - how often the message arrives. `reports` and `ageMs` let a log say it.
// Unknown until a real PS5 session has been logged with these counters; the round
// trip's scale must still be compared with the on-screen value in a real session.
#include "TransportTuning.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
namespace veyra::remoteplay {
// TransportCounters::consoleRttMilli holds the console's rtt as sent x this.
inline constexpr double kConsoleRttCountsPerUnit=1e6;
inline constexpr double kConsoleRttMinMs=0.1,kConsoleRttMaxMs=2000;
inline constexpr double kConsoleRateMinKbps=100,kConsoleRateMaxKbps=1000000;
struct ConsoleQuality {
    std::uint64_t reports=0;      // messages received since the counters were reset
    double ageMs=-1;              // since the newest message; -1 = none
    double rttMs=-1;              // -1 = unknown (no message, no rtt field, unit not decided, implausible)
    double targetKbps=-1,upstreamKbps=-1;
    double measuredKbps=-1;       // the library's own measured video rate at the newest message
    double upstreamLossPercent=-1;
    std::int64_t loss=-1;         // as sent; meaning unknown
    // The scale used: value x scale = ms (or kbit/s). 0 = not decided.
    double rttScale=0,bitrateScale=0;
};
// The power of 1000 (among the given candidates) that brings value closest to the
// reference on a log scale; 0 when value or reference is not positive.
inline double nearestThousandScale(double value,double reference,double smallest,double largest)noexcept{
    if(!(value>0)||!(reference>0)||!std::isfinite(value)||!std::isfinite(reference))return 0;
    double best=0,bestDistance=0;
    for(double scale=smallest;scale<=largest*1.0001;scale*=1000.0){
        const double distance=std::fabs(std::log(value*scale/reference));
        if(best==0||distance<bestDistance){best=scale;bestDistance=distance;}
    }
    return best;
}
// One unit decision (round trip or bitrate): votes per candidate scale.
struct UnitVote {
    static constexpr int kAgree=3;
    static constexpr double kBand=10.0; // a report supports a unit within 10x of the reference
    double scale=0;          // decided scale; 0 = not yet
    unsigned votes[3]{};     // per candidate (smallest, x1000, x1000000)
    // value as sent, reference in the target unit, candidates smallest, smallest*1000, ...
    void observe(double value,double reference,double smallest,int candidates,double minPlausible,double maxPlausible)noexcept{
        if(scale!=0||!(value>0)||!(reference>0)||!std::isfinite(value)||!std::isfinite(reference))return;
        double candidate=smallest;
        for(int i=0;i<candidates&&i<3;++i,candidate*=1000.0){
            const double converted=value*candidate;
            if(converted<minPlausible||converted>maxPlausible)continue;
            if(std::fabs(std::log10(converted/reference))>std::log10(kBand))continue;
            ++votes[i];
        }
        unsigned best=0,second=0;int bestIndex=-1;
        for(int i=0;i<3;++i){
            if(votes[i]>best){second=best;best=votes[i];bestIndex=i;}
            else if(votes[i]>second)second=votes[i];
        }
        if(bestIndex>=0&&best>=static_cast<unsigned>(kAgree)&&best>=second+static_cast<unsigned>(kAgree))
            scale=smallest*std::pow(1000.0,bestIndex);
    }
};
// The session's units (one per session, kept across reconnects; shared by the
// health line and the SmartLink feed). learn() may be called by several readers
// with the same counters: only a report not seen before counts.
struct ConsoleUnits {
    double rttScale=0,bitrateScale=0; // 0 = not decided
    UnitVote rtt{},rate{};
    std::uint64_t lastReports=0,seen=0; // reports counted so far
    double referenceRttMs=-1,referenceKbps=-1;
    bool logged=false;
    void learn(const TransportCounters& c,double requestedKbps)noexcept{
        if(c.consoleReports==0||c.consoleReports==lastReports)return;
        lastReports=c.consoleReports;++seen;
        // senkushaRttUs 1 = the library's "measured as 0": no reference.
        if(c.senkushaRttUs>1){
            referenceRttMs=static_cast<double>(c.senkushaRttUs)/1000.0;
            if(c.consoleRttMilli!=0)rtt.observe(static_cast<double>(c.consoleRttMilli)/kConsoleRttCountsPerUnit,referenceRttMs,1.0,2,kConsoleRttMinMs,kConsoleRttMaxMs);
        }
        if(requestedKbps>0){
            referenceKbps=requestedKbps;
            if(c.consoleTargetBitrate!=0)rate.observe(static_cast<double>(c.consoleTargetBitrate),requestedKbps,0.001,3,kConsoleRateMinKbps,kConsoleRateMaxKbps);
        }
        rttScale=rtt.scale;bitrateScale=rate.scale;
    }
    // Once per session: the text after "[remoteplay-health] " saying what was chosen
    // (when both are decided, or after 20 reports with one still undecided).
    // Numbers and unit words only. Returns 0 when there is nothing to log (yet).
    std::size_t takeLogLine(char* out,std::size_t size)noexcept{
        if(logged||size==0||seen==0)return 0;
        if(!(rttScale!=0&&bitrateScale!=0)&&seen<20)return 0;
        logged=true;
        const char* rttName=rttScale==1.0?"ms":rttScale==1000.0?"s":"undecided";
        const char* rateName=bitrateScale==0.001?"bit/s":bitrateScale==1.0?"kbit/s":bitrateScale==1000.0?"Mbit/s":"undecided";
        const int n=std::snprintf(out,size,"consoleUnits rtt=%s rate=%s basis=reports:%llu rttVotes:%u/%u rateVotes:%u/%u/%u connectRttMs:%.3f requestedKbps:%.0f",
            rttName,rateName,static_cast<unsigned long long>(seen),rtt.votes[0],rtt.votes[1],rate.votes[0],rate.votes[1],rate.votes[2],referenceRttMs,referenceKbps);
        if(n<=0)return 0;
        return static_cast<std::size_t>(n)<size?static_cast<std::size_t>(n):size-1;
    }
};
// nowUs: the library's monotonic clock now (chiaki_time_now_monotonic_us), the same
// clock as the counters' last-arrival time. requestedKbps: unused for the units now
// (kept for the callers); units: the session's ConsoleUnits; without them (nullptr)
// nothing is decided and the round trip and bitrates stay unknown.
inline ConsoleQuality interpretConsoleQuality(const TransportCounters& c,std::uint64_t nowUs,double requestedKbps,const ConsoleUnits* units=nullptr)noexcept{
    (void)requestedKbps;
    ConsoleQuality q;
    q.reports=c.consoleReports;
    if(c.consoleReports==0)return q;
    if(c.consoleLastUs!=0&&nowUs>=c.consoleLastUs)q.ageMs=static_cast<double>(nowUs-c.consoleLastUs)/1000.0;
    if(units){q.rttScale=units->rttScale;q.bitrateScale=units->bitrateScale;}
    const auto plausible=[](double v,double lo,double hi){return v>=lo&&v<=hi?v:-1.0;};
    if(c.consoleRttMilli!=0&&q.rttScale>0)q.rttMs=plausible(static_cast<double>(c.consoleRttMilli)/kConsoleRttCountsPerUnit*q.rttScale,kConsoleRttMinMs,kConsoleRttMaxMs);
    if(c.consoleTargetBitrate!=0&&q.bitrateScale>0)q.targetKbps=plausible(static_cast<double>(c.consoleTargetBitrate)*q.bitrateScale,kConsoleRateMinKbps,kConsoleRateMaxKbps);
    if(c.consoleUpstreamBitrate!=0&&q.bitrateScale>0)q.upstreamKbps=static_cast<double>(c.consoleUpstreamBitrate)*q.bitrateScale;
    q.measuredKbps=static_cast<double>(c.consoleMeasuredKbps);
    q.upstreamLossPercent=static_cast<double>(c.consoleUpstreamLossPpm)/10000.0;
    q.loss=static_cast<std::int64_t>(c.consoleLoss>0x7fffffffffffffffull?0x7fffffffffffffffull:c.consoleLoss);
    return q;
}
} // namespace veyra::remoteplay
