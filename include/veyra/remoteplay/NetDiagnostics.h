// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// [slipstream-net]: one five-second window of a PS5 stream's reception, from the
// socket to the decoder, with every kind of loss kept apart (stream "network",
// 21 Nov 2026; field table in docs/PS5_REMOTE_PLAY_EXPERIMENTS.md, contract in
// S5_NET_API.md). Portable and header-only: the session owner feeds it counters it
// already has, plus two read-only system counters on Windows; nothing here changes
// what is received, sent or shown. Only with VEYRA_EXP_RP_STATS=1.
//
// What the counts are (chiaki patch 0004 and our own queues; docs/drafts/NETWORK_AUDIT.md):
// - missing: units a frame lacked when the next frame began (the library's
//   congestion accounting). It INCLUDES packets that came after the reorder wait
//   had given them up (late) and packets this PC's socket dropped.
// - missingNotLate = missing - late: the upper bound of packets that never reached
//   the library (lost on the wire, or by this PC's socket buffer or adapter).
// - udpInErrors: system-wide UDP datagrams Windows could not deliver (every program,
//   every socket); 0 proves no UDP receive drop on this PC in the window, more is an
//   upper bound. nic*: the adapter that routes to the console. -1 = not read.
// - netToDecode: complete frame handed over by the library -> decoder returned the
//   picture (queue wait + decode submission; the GPU decode itself finishes behind a
//   fence the engine waits on). Per decoded frame, P50 / P95 / P99.
// - jitter: RFC 3550 style per FRAME (Takion packets carry no sender time).
#include "ConnectionHealth.h"
#include "StreamBottleneck.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace veyra::remoteplay {
// Read-only counters of this PC (GetUdpStatisticsEx, GetIfEntry2 on Windows); cumulative.
struct SystemNetCounters {
    bool udpKnown=false;
    std::uint64_t udpInDatagrams=0,udpInErrors=0;
    bool nicKnown=false;
    std::uint64_t nicInPackets=0,nicInDiscards=0,nicInErrors=0;
};
// The system's own route to the console (GetBestRoute2): a next hop of "none" is on-link.
enum class RouteHop : std::uint8_t {Unknown,OnLink,Gateway};
inline const char* routeHopName(RouteHop h)noexcept{return h==RouteHop::OnLink?"on-link":h==RouteHop::Gateway?"gateway":"unknown";}
struct NetSnapshot {
    bool valid=false;
    std::uint64_t sequence=0;
    HealthSnapshot health;              // loss, reorder, FEC, jitter, Mbit/s, fps, console view, route
    std::string codec;                  // as requested at connect
    std::uint32_t width=0,height=0;     // of the newest decoded picture; 0 = none in the window
    std::uint64_t resolutionChanges=0;  // picture size changed (console profile switch or reconnect)
    double requestedKbps=0,kbps=0;
    std::optional<double> kbpsMin1s,kbpsMax1s; // whole one-second windows inside this window
    std::uint64_t missing=0,late=0,missingNotLate=0,dropOther=0;
    bool counterWrap=false;             // missing / missingNotLate / lossPct unavailable in this window (-1 in the line)
    std::optional<double> fecRecoveryPct; // frames FEC completed of frames that needed it
    std::uint64_t framesLost=0,ingressDropped=0,frames=0;
    std::optional<double> netToDecodeP50Ms,netToDecodeP95Ms,netToDecodeP99Ms;
    double gapMaxMs=0;                  // longest time between two complete frames handed over
    std::uint64_t burstMax=0;           // most frames handed over within 4 ms of each other in a row
    double waitingIdrMs=0;              // time in the window with no decodable picture (waiting for a keyframe)
    std::optional<std::uint64_t> udpInDatagrams,udpInErrors,nicInPackets,nicDiscards,nicErrors;
    RouteHop hop=RouteHop::Unknown;
    StreamVerdict verdict;              // network and decoder evidence only (the engine's is the panel's)
    NetworkEvidence evidence;           // what the verdict saw, for a caller that adds the engine's
};
class NetDiagnostics {
public:
    static constexpr HostTime kInterval100ns=ConnectionHealth::kInterval100ns; // 5 s
    static constexpr HostTime kBurstGap100ns=40000;   // 4 ms: frames closer than this came together
    static constexpr std::size_t kMaxSamples=2048;     // per window; 5 s of 240 frames a second fit easily
    NetDiagnostics(){samples_.reserve(kMaxSamples);}
    // A new connection attempt. Counters restart; the grade inside the health part is kept.
    void begin(HostTime now,const RouteAssessment& route,RouteHop hop,double requestedKbps,std::string_view codec){
        health_.begin(now,route);start_=now;hop_=hop;requestedKbps_=requestedKbps;codec_=std::string(codec);
        havePrevious_=false;haveSystem_=false;samples_.clear();lastArrival_=-1;run_=0;burstMax_=0;gapMax100ns_=0;
        secondStart_=-1;secondBytes_=0;kbpsMin_.reset();kbpsMax_.reset();waitingSince_=-1;waiting100ns_=0;
        resolutionChanges_=0;haveSize_=false;width_=height_=0;frames_=0;sequence_=0;latest_={};
    }
    // The session's received bytes (cumulative), every loop pass: one-second rates.
    void observeBytes(HostTime now,std::uint64_t receivedBytes)noexcept{
        if(secondStart_<0||now<secondStart_||receivedBytes<secondBytes_){secondStart_=now;secondBytes_=receivedBytes;return;}
        const HostTime elapsed=now-secondStart_;
        if(elapsed<10000000)return;
        const double kbps=static_cast<double>(receivedBytes-secondBytes_)*8.0/1000.0/(static_cast<double>(elapsed)/10000000.0);
        kbpsMin_=kbpsMin_?(std::min)(*kbpsMin_,kbps):kbps;
        kbpsMax_=kbpsMax_?(std::max)(*kbpsMax_,kbps):kbps;
        secondStart_=now;secondBytes_=receivedBytes;
    }
    // Every loop pass: the ingress queue's depth (its peak is reported per window).
    void observeIngressDepth(std::size_t depth)noexcept{health_.observeIngressDepth(depth);}
    // Every loop pass: whether the ingress waits for a keyframe (nothing can be decoded).
    void observeWaitingIdr(HostTime now,bool waiting)noexcept{
        if(waiting&&waitingSince_<0){waitingSince_=now;return;}
        if(!waiting&&waitingSince_>=0){if(now>waitingSince_)waiting100ns_+=now-waitingSince_;waitingSince_=-1;}
    }
    // Each decoded frame: when the library handed it over, when the decoder returned it, its size.
    void frameDecoded(HostTime arrival,HostTime decoded,std::uint32_t width,std::uint32_t height){
        ++frames_;
        if(arrival>0&&decoded>=arrival&&samples_.size()<kMaxSamples)samples_.push_back(static_cast<double>(decoded-arrival)/10000.0);
        if(arrival>0){
            if(lastArrival_>0&&arrival>=lastArrival_){
                const HostTime gap=arrival-lastArrival_;
                gapMax100ns_=(std::max)(gapMax100ns_,gap);
                run_=gap<kBurstGap100ns?run_+1:1;
            }else run_=1;
            burstMax_=(std::max)(burstMax_,run_);
            lastArrival_=arrival;
        }
        if(width&&height){
            if(haveSize_&&(width!=width_||height!=height_))++resolutionChanges_;
            haveSize_=true;width_=width;height_=height;
        }
    }
    bool due(HostTime now)const noexcept{return now<start_||now-start_>=kInterval100ns;}
    std::optional<NetSnapshot> sample(HostTime now,const HealthCounters& c,const SystemNetCounters& system){
        if(now<start_){start_=now;return {};}
        if(now-start_<kInterval100ns)return {};
        const auto h=health_.sample(now,c);
        if(!h)return {};
        const auto delta=[](std::uint64_t current,std::uint64_t before)noexcept{return current>=before?current-before:current;};
        NetSnapshot s;s.valid=true;s.sequence=++sequence_;s.health=*h;s.codec=codec_;s.hop=hop_;
        s.width=width_;s.height=height_;s.resolutionChanges=resolutionChanges_;
        s.requestedKbps=requestedKbps_;s.kbps=h->receivedMbps*1000.0;s.kbpsMin1s=kbpsMin_;s.kbpsMax1s=kbpsMax_;
        s.counterWrap=h->counterWrap; // the loss count of this window is not usable (ConnectionHealth.h)
        s.missing=h->packetsLost;s.late=h->lateArrivals;
        s.missingNotLate=s.missing>s.late?s.missing-s.late:0;
        s.dropOther=h->late>h->lateArrivals?h->late-h->lateArrivals:0; // duplicates or queue overflow (no own counter)
        const auto needed=h->fecRecovered+h->fecFailed;
        if(needed)s.fecRecoveryPct=100.0*static_cast<double>(h->fecRecovered)/static_cast<double>(needed);
        s.framesLost=havePrevious_?delta(c.framesLost,previous_.framesLost):c.framesLost;
        s.ingressDropped=havePrevious_?delta(c.ingressDropped,previous_.ingressDropped):c.ingressDropped;
        s.frames=frames_;
        if(!samples_.empty()){
            std::sort(samples_.begin(),samples_.end());
            const auto at=[&](double q){const auto i=static_cast<std::size_t>(std::ceil(q*static_cast<double>(samples_.size())))-1u;return samples_[(std::min)(i,samples_.size()-1u)];};
            s.netToDecodeP50Ms=at(0.50);s.netToDecodeP95Ms=at(0.95);s.netToDecodeP99Ms=at(0.99);
        }
        s.gapMaxMs=static_cast<double>(gapMax100ns_)/10000.0;s.burstMax=burstMax_;
        HostTime waiting=waiting100ns_;
        if(waitingSince_>=0&&now>waitingSince_){waiting+=now-waitingSince_;waitingSince_=now;}
        s.waitingIdrMs=static_cast<double>(waiting)/10000.0;
        if(system.udpKnown&&haveSystem_&&previousSystem_.udpKnown&&system.udpInErrors>=previousSystem_.udpInErrors&&system.udpInDatagrams>=previousSystem_.udpInDatagrams){
            s.udpInErrors=system.udpInErrors-previousSystem_.udpInErrors;s.udpInDatagrams=system.udpInDatagrams-previousSystem_.udpInDatagrams;
        }
        if(system.nicKnown&&haveSystem_&&previousSystem_.nicKnown&&system.nicInDiscards>=previousSystem_.nicInDiscards&&system.nicInErrors>=previousSystem_.nicInErrors&&system.nicInPackets>=previousSystem_.nicInPackets){
            s.nicDiscards=system.nicInDiscards-previousSystem_.nicInDiscards;s.nicErrors=system.nicInErrors-previousSystem_.nicInErrors;
            s.nicInPackets=system.nicInPackets-previousSystem_.nicInPackets;
        }
        s.evidence=evidenceOf(s);
        s.verdict=classifyStream(s.evidence);
        previous_=c;havePrevious_=true;previousSystem_=system;haveSystem_=true;
        start_=now;samples_.clear();run_=lastArrival_>0?1:0;burstMax_=run_;gapMax100ns_=0;kbpsMin_.reset();kbpsMax_.reset();
        waiting100ns_=0;resolutionChanges_=0;frames_=0;latest_=s;
        return s;
    }
    // The first window's system counters are only a starting point: call once at begin() time too.
    void primeSystem(const SystemNetCounters& system)noexcept{previousSystem_=system;haveSystem_=true;}
    const NetSnapshot& latest()const noexcept{return latest_;}
    static NetworkEvidence evidenceOf(const NetSnapshot& s)noexcept{
        NetworkEvidence n;n.valid=s.valid;
        n.packets=s.health.packetsReceived+s.health.packetsLost;n.missingNotLate=s.missingNotLate;n.lateArrivals=s.late;n.countsKnown=!s.counterWrap;
        n.fecFailed=s.health.fecFailed;n.framesLost=s.framesLost;n.ingressDropped=s.ingressDropped;n.frames=s.frames;
        n.mailboxOverwrites=s.health.mailboxOverwrites;n.udpInErrors=s.udpInErrors;n.nicDiscards=s.nicDiscards;
        n.reorderTimeouts=s.health.reorderTimeouts;n.reorderOvershootMs=s.health.reorderOvershootMs;
        n.jitterMs=s.health.frameJitterMs;n.jitterPeakMs=s.health.frameJitterPeakMs;n.fps=s.health.receivedFps;
        n.decodeP95Ms=s.health.decodeP95Ms;n.kbps=s.kbps;n.requestedKbps=s.requestedKbps;n.consoleTargetKbps=s.health.console.targetKbps;
        return n;
    }
private:
    ConnectionHealth health_;
    HealthCounters previous_{};bool havePrevious_=false;
    SystemNetCounters previousSystem_{};bool haveSystem_=false;
    HostTime start_=0;RouteHop hop_=RouteHop::Unknown;double requestedKbps_=0;std::string codec_;
    std::vector<double> samples_;
    HostTime lastArrival_=-1,gapMax100ns_=0;std::uint64_t run_=0,burstMax_=0;
    HostTime secondStart_=-1;std::uint64_t secondBytes_=0;std::optional<double> kbpsMin_,kbpsMax_;
    HostTime waitingSince_=-1,waiting100ns_=0;
    std::uint64_t resolutionChanges_=0,frames_=0;bool haveSize_=false;std::uint32_t width_=0,height_=0;
    std::uint64_t sequence_=0;NetSnapshot latest_{};
};
// The text after "[slipstream-net] ". Fixed keys, numbers and fixed words only: no
// addresses, names or paths. -1 = not available (never 0 for "unknown").
inline std::string formatNetLine(const NetSnapshot& s){
    const auto opt=[](const std::optional<double>& v){return v?*v:-1.0;};
    const auto optCount=[](const std::optional<std::uint64_t>& v){return v?static_cast<long long>(*v):-1LL;};
    const auto& h=s.health;
    char text[1400];
    std::snprintf(text,sizeof(text),
        "window=%.1f codec=%s resolution=%ux%u resolutionChanges=%llu fps=%.1f requestedKbps=%.0f kbps=%.0f kbpsMin1s=%.0f kbpsMax1s=%.0f consoleTargetKbps=%.0f "
        "packets=%llu missing=%lld late=%llu missingNotLate=%lld lossPct=%.3f reordered=%llu dropOther=%llu reorderTimeouts=%llu "
        "fecRecovered=%llu fecFailed=%llu fecRecoveryPct=%.1f framesLost=%llu recoveries=%llu keyframeRequests=%llu ingressDropped=%llu ingressPeak=%llu mailboxOverwrites=%llu frames=%llu "
        "jitterMs=%.2f jitterPeakMs=%.2f gapMaxMs=%.1f burstMax=%llu netToDecodeP50Ms=%.2f netToDecodeP95Ms=%.2f netToDecodeP99Ms=%.2f decodeP95Ms=%.2f waitingIdrMs=%.0f "
        "udpInDatagrams=%lld udpInErrors=%lld nicPackets=%lld nicDiscards=%lld nicErrors=%lld route=%s routeClass=%s family=%s",
        h.seconds,s.codec.empty()?"unknown":s.codec.c_str(),s.width,s.height,static_cast<unsigned long long>(s.resolutionChanges),h.receivedFps,
        s.requestedKbps,s.kbps,opt(s.kbpsMin1s),opt(s.kbpsMax1s),h.console.targetKbps<0?-1.0:h.console.targetKbps,
        static_cast<unsigned long long>(h.packetsReceived+h.packetsLost),s.counterWrap?-1LL:static_cast<long long>(s.missing),static_cast<unsigned long long>(s.late),
        s.counterWrap?-1LL:static_cast<long long>(s.missingNotLate),s.counterWrap?-1.0:h.lossPercent,static_cast<unsigned long long>(h.reordered),static_cast<unsigned long long>(s.dropOther),
        static_cast<unsigned long long>(h.reorderTimeouts),static_cast<unsigned long long>(h.fecRecovered),static_cast<unsigned long long>(h.fecFailed),opt(s.fecRecoveryPct),
        static_cast<unsigned long long>(s.framesLost),static_cast<unsigned long long>(h.decoderResets),static_cast<unsigned long long>(h.keyframeRequests),
        static_cast<unsigned long long>(s.ingressDropped),static_cast<unsigned long long>(h.ingressPeak),static_cast<unsigned long long>(h.mailboxOverwrites),
        static_cast<unsigned long long>(s.frames),h.frameJitterMs,h.frameJitterPeakMs,s.gapMaxMs,static_cast<unsigned long long>(s.burstMax),
        opt(s.netToDecodeP50Ms),opt(s.netToDecodeP95Ms),opt(s.netToDecodeP99Ms),opt(h.decodeP95Ms),s.waitingIdrMs,
        optCount(s.udpInDatagrams),optCount(s.udpInErrors),optCount(s.nicInPackets),optCount(s.nicDiscards),optCount(s.nicErrors),
        routeHopName(s.hop),routeClassName(h.route),h.route.addressKnown?"ipv4":"unknown");
    std::string line(text);
    line+=" bottleneck=";line+=streamCauseName(s.verdict.cause);
    line+=" signs=";line+=formatStreamCauseSigns(s.verdict.signs);
    line+=s.counterWrap?" counterWrap=1":" counterWrap=0";
    line+=" (software counters; udp*: whole PC; nic*: the console's adapter; -1 = not available)";
    return line;
}
} // namespace veyra::remoteplay
