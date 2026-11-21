// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Live connection health for a PS5 stream (experiment VEYRA_EXP_RP_HEALTH).
// Portable and header-only: the session owner feeds cumulative counters, this
// turns them into one snapshot per five seconds and a grade with hysteresis.
// It observes only; nothing here changes what is sent to the console.
//
// What the numbers are, and are not:
// - loss: the library's own packet accounting (its 200 ms congestion windows),
//   before the 5 % cap it applies to what the console is told.
// - jitter: RFC 3550 interarrival jitter computed per video FRAME, not per
//   packet (Takion packets carry no sender timestamp): arrival of a complete
//   frame against the nominal frame cadence. It therefore includes the
//   console's own encoder pacing, and a 59.94 fps stream against a nominal 60
//   adds a constant 0.017 ms.
// - rtt: only what the library measured once while connecting (Senkusha).
//   When it did not, the value is absent and printed as "not-measured".
// - grade thresholds are provisional: they have not been calibrated on a PS5.
// - bottleneck: which of loss, reordering, jitter, the receive thread waking
//   late, a decoder backlog or a Wi-Fi route the interval points at (pure
//   rules below, provisional thresholds, the local cause named first).
#include "ConsoleQuality.h"
#include "SessionInbox.h"
#include "TransportTuning.h"
#include <algorithm>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
namespace veyra::remoteplay {

// --- Frame interarrival jitter, RFC 3550 section 6.4.1: J += (|D| - J) / 16 ---
class FrameJitter {
public:
    void reset()noexcept{*this={};}
    // index: extended frame index; arrival: local clock, 100 ns; fps: nominal rate.
    void observe(std::uint64_t index,HostTime arrival,std::uint32_t fps)noexcept{
        if(!fps)return;
        if(!have_){have_=true;lastIndex_=index;lastArrival_=arrival;return;}
        if(index<=lastIndex_||arrival<lastArrival_)return; // duplicate, old or a clock step back
        const std::uint64_t frames=(std::min<std::uint64_t>)(index-lastIndex_,1000000u);
        const double sent=static_cast<double>(frames)*10000000.0/static_cast<double>(fps);
        double d=static_cast<double>(arrival-lastArrival_)-sent;if(d<0)d=-d;
        jitter_+=(d-jitter_)/16.0;++samples_;
        // Peak over the last 256..512 frames: two buckets, the older one replaced in turn.
        if(d>peak_[bucket_])peak_[bucket_]=d;
        if(++bucketCount_>=256){bucket_^=1u;peak_[bucket_]=0;bucketCount_=0;}
        lastIndex_=index;lastArrival_=arrival;
    }
    // The same with the 16-bit index the console sends: wraps, duplicates and
    // older frames are handled here.
    void observeWire(std::uint16_t wireIndex,HostTime arrival,std::uint32_t fps)noexcept{
        if(!have_){observe(wireIndex,arrival,fps);return;}
        const auto step=static_cast<std::uint16_t>(wireIndex-static_cast<std::uint16_t>(lastIndex_));
        if(step==0||step>=0x8000u)return;
        observe(lastIndex_+step,arrival,fps);
    }
    double jitterMs()const noexcept{return jitter_/10000.0;}
    double peakMs()const noexcept{return (std::max)(peak_[0],peak_[1])/10000.0;}
    std::uint64_t samples()const noexcept{return samples_;}
private:
    bool have_=false;std::uint64_t lastIndex_=0,samples_=0;HostTime lastArrival_=0;
    double jitter_=0,peak_[2]={0,0};unsigned bucket_=0,bucketCount_=0;
};

// --- Is the console on the same LAN? Pure logic on addresses. ---
enum class LinkKind : std::uint8_t {Unknown,Ethernet,WiFi,Other};
struct LocalInterface {
    std::uint32_t address=0;        // IPv4, host byte order
    std::uint8_t prefixLength=0;    // on-link prefix
    LinkKind kind=LinkKind::Unknown;
    std::uint64_t linkBitsPerSecond=0; // 0 = unknown
};
// Strict dotted quad, as validHost() accepts it: no leading zeros, no port.
inline std::optional<std::uint32_t> parseIpv4(std::string_view text)noexcept{
    std::uint32_t value=0;unsigned parts=0;std::size_t i=0;
    while(i<text.size()){
        if(parts==4)return {};
        std::uint32_t part=0;std::size_t digits=0;
        while(i<text.size()&&text[i]>='0'&&text[i]<='9'){
            if(digits==1&&part==0)return {};
            part=part*10u+static_cast<std::uint32_t>(text[i]-'0');
            if(++digits>3||part>255)return {};
            ++i;
        }
        if(!digits)return {};
        value=(value<<8)|part;++parts;
        if(i<text.size()){if(text[i]!='.'||i+1==text.size())return {};++i;}
    }
    if(parts!=4)return {};
    return value;
}
// RFC 1918, and RFC 3927 link-local (a console cabled straight to the PC).
inline bool privateIpv4(std::uint32_t a)noexcept{
    return (a>>24)==10u||(a>>20)==0xAC1u||(a>>16)==0xC0A8u||(a>>16)==0xA9FEu;
}
inline bool onLink(std::uint32_t peer,const LocalInterface& local)noexcept{
    if(local.prefixLength==0||local.prefixLength>30)return false;
    const std::uint32_t mask=0xFFFFFFFFu<<(32u-local.prefixLength);
    return (peer&mask)==(local.address&mask);
}
struct RouteAssessment {
    bool addressKnown=false; // the host was an IPv4 literal (a name is not resolved here)
    bool privateRange=false,onLink=false;
    LinkKind link=LinkKind::Unknown;
    std::uint64_t linkBitsPerSecond=0;
    bool directLan()const noexcept{return addressKnown&&privateRange&&onLink;}
    bool wifi()const noexcept{return onLink&&link==LinkKind::WiFi;}
    const char* name()const noexcept{
        return !addressKnown?"unknown-host-name":directLan()?"direct-lan":onLink?"on-link-public-address":privateRange?"private-routed":"not-local";
    }
    const char* linkName()const noexcept{
        return link==LinkKind::Ethernet?"ethernet":link==LinkKind::WiFi?"wifi":link==LinkKind::Other?"other":"unknown";
    }
    bool operator==(const RouteAssessment&)const=default;
};
// The class of this PC's side of the route, for log lines and comparisons
// (the kit refuses to compare a wired run with a Wi-Fi one): "wired" = the
// console is on the subnet of an Ethernet adapter of this PC, "wifi" = of a
// Wi-Fi adapter, "unknown" otherwise (a host name, a routed or other adapter,
// nothing assessed yet). The console's own link is not visible from here.
// Virtual switches report as Ethernet.
enum class RouteClass : std::uint8_t {Unknown,Wired,Wifi};
inline RouteClass routeClass(const RouteAssessment& r)noexcept{
    if(!r.onLink)return RouteClass::Unknown;
    return r.link==LinkKind::Ethernet?RouteClass::Wired:r.link==LinkKind::WiFi?RouteClass::Wifi:RouteClass::Unknown;
}
inline const char* routeClassName(RouteClass c)noexcept{return c==RouteClass::Wired?"wired":c==RouteClass::Wifi?"wifi":"unknown";}
inline const char* routeClassName(const RouteAssessment& r)noexcept{return routeClassName(routeClass(r));}
// The interface with the longest matching prefix is the one the system routes through.
inline RouteAssessment assessRoute(std::string_view host,std::span<const LocalInterface> interfaces)noexcept{
    RouteAssessment out;
    const auto peer=parseIpv4(host);
    if(!peer)return out;
    out.addressKnown=true;out.privateRange=privateIpv4(*peer);
    unsigned best=0;
    for(const auto& local:interfaces){
        if(!onLink(*peer,local)||static_cast<unsigned>(local.prefixLength)<=best)continue;
        best=local.prefixLength;out.onLink=true;out.link=local.kind;out.linkBitsPerSecond=local.linkBitsPerSecond;
    }
    return out;
}

// --- Interval aggregation ---
// Everything cumulative since the session attempt began, except the two
// instantaneous values at the end.
struct HealthCounters {
    std::uint64_t packetsReceived=0,packetsLost=0;
    std::uint64_t reorderGaps=0,reorderTimeouts=0,reorderSkipped=0,reorderDropped=0;
    std::uint64_t fecRecovered=0,fecFailed=0,keyframeRequests=0;
    std::uint64_t reorderOvershootUs=0; // summed over reorderTimeouts: how much longer than asked those waits lasted
    std::uint64_t reorderLateArrivals=0; // packets that arrived after the wait had given up on them (patch 0004)
    std::optional<double> decodeP95Ms;  // the inbox's decode time, 95th percentile; absent until measured
    std::uint64_t decoderResets=0;     // ingress recovery episodes (each resets the decoder)
    std::uint64_t framesLost=0;        // frames the library could not complete
    std::uint64_t ingressDropped=0;    // complete frames discarded while recovering
    std::uint64_t framesReceived=0,bytesReceived=0;
    std::uint64_t mailboxOverwrites=0; // decoded frames replaced before the engine took them
    std::size_t ingressDepth=0;
    double frameJitterMs=0,frameJitterPeakMs=0;
    std::optional<double> rttMs;
    ConsoleQuality console; // the newest CONNECTIONQUALITY message (ConsoleQuality.h); instantaneous
};
enum class HealthGrade : std::uint8_t {Unknown,Stable,Unsteady,Unstable};
// What limits the stream in one interval. Bits in HealthSnapshot::bottleneckSigns.
enum class Bottleneck : std::uint8_t {None,Loss,Reordering,Jitter,ReceiveScheduling,DecoderBacklog,WifiRoute};
inline const char* bottleneckName(Bottleneck b)noexcept{
    switch(b){
    case Bottleneck::Loss:return "loss";
    case Bottleneck::Reordering:return "reordering";
    case Bottleneck::Jitter:return "jitter";
    case Bottleneck::ReceiveScheduling:return "receive-scheduling";
    case Bottleneck::DecoderBacklog:return "decoder-backlog";
    case Bottleneck::WifiRoute:return "wifi-route";
    case Bottleneck::None:break;
    }
    return "none";
}
inline constexpr std::uint8_t bottleneckBit(Bottleneck b)noexcept{
    return b==Bottleneck::None?std::uint8_t{0}:static_cast<std::uint8_t>(1u<<(static_cast<unsigned>(b)-1u));
}
inline const char* healthGradeName(HealthGrade g)noexcept{
    return g==HealthGrade::Stable?"stable":g==HealthGrade::Unsteady?"unsteady":g==HealthGrade::Unstable?"unstable":"unknown";
}
struct HealthSnapshot {
    bool valid=false;
    std::uint64_t sequence=0; // intervals reported so far in this session attempt
    double seconds=0;
    std::uint64_t packetsReceived=0,packetsLost=0;double lossPercent=0;
    double frameJitterMs=0,frameJitterPeakMs=0;
    // reordered: gaps at the head of the reorder queue that closed before the
    // wait ran out. late: packets the queue refused (after a skip, duplicates).
    std::uint64_t reordered=0,late=0,reorderTimeouts=0,reorderSkipped=0;
    // lateArrivals: packets that came after the wait had given them up (a
    // wait shorter than the link's reordering, not a loss on the wire).
    std::uint64_t lateArrivals=0;
    std::uint64_t fecRecovered=0,fecFailed=0,keyframeRequests=0,decoderResets=0;
    std::uint64_t networkFrameDrops=0; // library frames lost + frames discarded while recovering
    std::size_t ingressDepth=0,ingressPeak=0;
    std::uint64_t mailboxOverwrites=0;
    double reorderOvershootMs=0;       // mean per reorder timeout in this interval; 0 without timeouts
    std::optional<double> decodeP95Ms;
    double receivedMbps=0,receivedFps=0;
    std::optional<double> rttMs;
    ConsoleQuality console; // the console's own view at the end of the interval (live rtt, target rate)
    HealthGrade intervalGrade=HealthGrade::Unknown; // this interval alone
    HealthGrade grade=HealthGrade::Unknown;         // with hysteresis
    RouteAssessment route;
    std::uint8_t bottleneckSigns=0;    // every sign seen (bottleneckBit)
    Bottleneck bottleneck=Bottleneck::None; // the one named
    std::uint64_t recoveryEvents()const noexcept{return fecRecovered+keyframeRequests+decoderResets;}
};
// --- Bottleneck classification: pure rules on one interval ---
// Provisional thresholds, not calibrated on a PS5 (like the grade):
//   loss               measured loss >= 0.1 %, or a frame FEC could not complete
//                      while no packet arrived after being given up
//   reordering         >= 1 per mille of packets arrived out of order (and >= 10),
//                      or frames FEC could not complete while packets arrived
//                      after the wait had given them up (the wait was too short
//                      for the link's reordering; nothing was lost on the wire)
//   jitter             frame interarrival jitter >= 2 ms, or one frame >= 8 ms
//                      off its cadence (the peak: a stall that the smoothed
//                      value has forgotten by the end of the interval)
//   receive-scheduling reorder waits lasted >= 2 ms longer than asked, over >= 4
//                      waits: the receive (Takion) thread woke late (the library's
//                      own socket wait rounds up to whole ms, so 1 ms is normal)
//   decoder-backlog    the decode time's 95th percentile is >= 0.8 of a frame
//                      interval, or the ingress queue reached 3 frames, except
//                      when the decode time is known to be short and the frames
//                      came in bursts (jitter): then the network released them
//                      together and the decoder caught up
//   wifi-route         this PC reaches the console over Wi-Fi
struct BottleneckThresholds {
    static constexpr double kLossPercent=0.1,kReorderedPerMille=1.0,kJitterMs=2.0,kJitterPeakMs=8.0,kOvershootMs=2.0,kDecodeShare=0.8;
    static constexpr std::uint64_t kMinReordered=10,kMinTimeouts=4;
    static constexpr std::size_t kBacklogDepth=3;
};
inline std::uint8_t bottleneckSigns(const HealthSnapshot& s)noexcept{
    using T=BottleneckThresholds;
    std::uint8_t signs=s.route.wifi()?bottleneckBit(Bottleneck::WifiRoute):std::uint8_t{0};
    if(s.packetsReceived+s.packetsLost==0)return signs;
    const auto set=[&](Bottleneck b){signs=static_cast<std::uint8_t>(signs|bottleneckBit(b));};
    // A frame FEC could not complete is a loss only when nothing came late:
    // with late arrivals, the wait gave up on packets that were on their way.
    const bool givenUpLate=s.fecFailed>=1&&s.lateArrivals>=1;
    if(s.lossPercent>=T::kLossPercent||(s.fecFailed>=1&&!givenUpLate))set(Bottleneck::Loss);
    if((s.reordered>=T::kMinReordered&&static_cast<double>(s.reordered)*1000.0>=T::kReorderedPerMille*static_cast<double>(s.packetsReceived))||givenUpLate)set(Bottleneck::Reordering);
    const bool jitter=s.frameJitterMs>=T::kJitterMs||s.frameJitterPeakMs>=T::kJitterPeakMs;
    if(jitter)set(Bottleneck::Jitter);
    if(s.reorderTimeouts>=T::kMinTimeouts&&s.reorderOvershootMs>=T::kOvershootMs)set(Bottleneck::ReceiveScheduling);
    const double frameMs=s.receivedFps>1.0?1000.0/s.receivedFps:1000.0/60.0;
    const bool slowDecode=s.decodeP95Ms&&*s.decodeP95Ms>=T::kDecodeShare*frameMs;
    const bool burstArrival=jitter&&s.decodeP95Ms&&!slowDecode;
    if(slowDecode||(s.ingressPeak>=T::kBacklogDepth&&!burstArrival))set(Bottleneck::DecoderBacklog);
    return signs;
}
// One name per interval. The local causes first, since they also produce the
// network signs (a late receive thread gives packets up and looks like
// reordering and jitter); then the route when there are network signs on
// Wi-Fi; then loss, reordering, jitter. A clean Wi-Fi link names nothing.
inline Bottleneck classifyBottleneck(std::uint8_t signs)noexcept{
    const auto has=[signs](Bottleneck b){return (signs&bottleneckBit(b))!=0;};
    const bool network=has(Bottleneck::Loss)||has(Bottleneck::Reordering)||has(Bottleneck::Jitter);
    if(has(Bottleneck::ReceiveScheduling))return Bottleneck::ReceiveScheduling;
    if(has(Bottleneck::DecoderBacklog))return Bottleneck::DecoderBacklog;
    if(network&&has(Bottleneck::WifiRoute))return Bottleneck::WifiRoute;
    if(has(Bottleneck::Loss))return Bottleneck::Loss;
    if(has(Bottleneck::Reordering))return Bottleneck::Reordering;
    if(has(Bottleneck::Jitter))return Bottleneck::Jitter;
    return Bottleneck::None;
}
inline Bottleneck classifyBottleneck(const HealthSnapshot& s)noexcept{return classifyBottleneck(bottleneckSigns(s));}
// "loss+jitter", or "none".
inline std::string formatBottleneckSigns(std::uint8_t signs){
    std::string text;
    for(const auto b:{Bottleneck::Loss,Bottleneck::Reordering,Bottleneck::Jitter,Bottleneck::ReceiveScheduling,Bottleneck::DecoderBacklog,Bottleneck::WifiRoute}){
        if(!(signs&bottleneckBit(b)))continue;
        if(!text.empty())text+='+';
        text+=bottleneckName(b);
    }
    return text.empty()?std::string("none"):text;
}
class ConnectionHealth {
public:
    static constexpr HostTime kInterval100ns=50000000; // 5 s
    static constexpr unsigned kRecoverIntervals=2;     // clean intervals per step back up
    // A new session attempt: counters restart from zero, the grade is kept
    // (a reconnect is not evidence that the link got better).
    void begin(HostTime now,const RouteAssessment& route={})noexcept{
        previous_={};start_=now;peak_=0;route_=route;latest_.valid=false;sequence_=0;
    }
    // Call as often as convenient; the highest depth seen is reported per interval.
    void observeIngressDepth(std::size_t depth)noexcept{peak_=(std::max)(peak_,depth);}
    // True when five seconds have passed since begin() or the last snapshot:
    // only then are the counters worth collecting.
    bool due(HostTime now)const noexcept{return now<start_||now-start_>=kInterval100ns;}
    // Returns a snapshot when due. A clock that stepped back restarts the interval.
    std::optional<HealthSnapshot> sample(HostTime now,const HealthCounters& c)noexcept{
        observeIngressDepth(c.ingressDepth);
        if(now<start_){start_=now;return {};}
        if(now-start_<kInterval100ns)return {};
        const auto delta=[](std::uint64_t current,std::uint64_t before)noexcept{return current>=before?current-before:current;};
        HealthSnapshot s;s.valid=true;s.sequence=++sequence_;s.seconds=static_cast<double>(now-start_)/10000000.0;
        s.packetsReceived=delta(c.packetsReceived,previous_.packetsReceived);
        s.packetsLost=delta(c.packetsLost,previous_.packetsLost);
        const auto total=s.packetsReceived+s.packetsLost;
        s.lossPercent=total?100.0*static_cast<double>(s.packetsLost)/static_cast<double>(total):0.0;
        s.frameJitterMs=c.frameJitterMs;s.frameJitterPeakMs=c.frameJitterPeakMs;
        const auto gaps=delta(c.reorderGaps,previous_.reorderGaps);
        s.reorderTimeouts=delta(c.reorderTimeouts,previous_.reorderTimeouts);
        s.reordered=gaps>s.reorderTimeouts?gaps-s.reorderTimeouts:0;
        s.late=delta(c.reorderDropped,previous_.reorderDropped);
        s.lateArrivals=delta(c.reorderLateArrivals,previous_.reorderLateArrivals);
        s.reorderSkipped=delta(c.reorderSkipped,previous_.reorderSkipped);
        s.fecRecovered=delta(c.fecRecovered,previous_.fecRecovered);
        s.fecFailed=delta(c.fecFailed,previous_.fecFailed);
        s.keyframeRequests=delta(c.keyframeRequests,previous_.keyframeRequests);
        s.decoderResets=delta(c.decoderResets,previous_.decoderResets);
        s.networkFrameDrops=delta(c.framesLost,previous_.framesLost)+delta(c.ingressDropped,previous_.ingressDropped);
        s.ingressDepth=c.ingressDepth;s.ingressPeak=peak_;
        s.mailboxOverwrites=delta(c.mailboxOverwrites,previous_.mailboxOverwrites);
        const auto overshoot=delta(c.reorderOvershootUs,previous_.reorderOvershootUs);
        s.reorderOvershootMs=s.reorderTimeouts?static_cast<double>(overshoot)/1000.0/static_cast<double>(s.reorderTimeouts):0.0;
        s.decodeP95Ms=c.decodeP95Ms;
        s.receivedMbps=static_cast<double>(delta(c.bytesReceived,previous_.bytesReceived))*8.0/1000000.0/s.seconds;
        s.receivedFps=static_cast<double>(delta(c.framesReceived,previous_.framesReceived))/s.seconds;
        s.rttMs=c.rttMs;s.console=c.console;s.route=route_;
        s.intervalGrade=gradeInterval(s);
        s.bottleneckSigns=bottleneckSigns(s);s.bottleneck=classifyBottleneck(s.bottleneckSigns);
        if(s.intervalGrade!=HealthGrade::Unknown){
            if(grade_==HealthGrade::Unknown||s.intervalGrade>grade_){grade_=s.intervalGrade;better_=0;}
            else if(s.intervalGrade<grade_){
                if(++better_>=kRecoverIntervals){grade_=static_cast<HealthGrade>(static_cast<std::uint8_t>(grade_)-1u);better_=0;}
            }else better_=0;
        }
        s.grade=grade_;
        previous_=c;start_=now;peak_=c.ingressDepth;latest_=s;
        return s;
    }
    const HealthSnapshot& latest()const noexcept{return latest_;}
    // One interval on its own. Provisional thresholds, see the top of the file.
    static HealthGrade gradeInterval(const HealthSnapshot& s)noexcept{
        if(s.packetsReceived+s.packetsLost==0)return HealthGrade::Unknown;
        if(s.lossPercent>=1.0||s.decoderResets>=2||s.fecFailed>=3||s.networkFrameDrops>=30||s.frameJitterMs>=10.0)return HealthGrade::Unstable;
        if(s.lossPercent>=0.1||s.decoderResets>=1||s.fecFailed>=1||s.networkFrameDrops>=1||s.reorderTimeouts>=1||s.frameJitterMs>=4.0)return HealthGrade::Unsteady;
        return HealthGrade::Stable;
    }
private:
    HealthCounters previous_{};HealthSnapshot latest_{};RouteAssessment route_{};
    HostTime start_=0;std::size_t peak_=0;std::uint64_t sequence_=0;
    HealthGrade grade_=HealthGrade::Unknown;unsigned better_=0;
};
// Where each counter comes from: the patched library (chiaki patch 0004), the
// session inbox, and the decoded-frame slot the engine reads from.
// requestedKbps: the launch's bitrate (the reference for the console's bitrate unit; <= 0 unknown).
// units: the session's learned console units (ConsoleUnits); nullptr = from these values alone.
inline HealthCounters healthCounters(const TransportCounters& t,const SessionInbox::Snapshot& s,std::uint64_t mailboxOverwrites,double requestedKbps=-1,const ConsoleUnits* units=nullptr)noexcept{
    HealthCounters c;
    c.packetsReceived=t.congestionReceived;c.packetsLost=t.congestionLost;
    c.reorderGaps=t.reorderGaps;c.reorderTimeouts=t.reorderTimeouts;c.reorderSkipped=t.reorderSkipped;c.reorderDropped=t.reorderDropped;
    c.fecRecovered=t.fecRecovered;c.fecFailed=t.fecFailed;c.keyframeRequests=t.idrRequests;
    c.decoderResets=s.video.recoveries;c.framesLost=s.framesLost;c.ingressDropped=s.video.dropped;
    c.framesReceived=s.video.accessUnits;c.bytesReceived=s.receivedBytes;
    c.mailboxOverwrites=mailboxOverwrites;c.ingressDepth=s.video.depth;
    c.reorderOvershootUs=t.reorderOvershootUs;c.reorderLateArrivals=t.reorderLateArrivals;c.decodeP95Ms=s.decodeP95Ms;
    c.frameJitterMs=s.frameJitterMs;c.frameJitterPeakMs=s.frameJitterPeakMs;
    if(t.senkushaRttUs)c.rttMs=static_cast<double>(t.senkushaRttUs)/1000.0;
    c.console=interpretConsoleQuality(t,t.libraryNowUs,requestedKbps,units);
    return c;
}
// The text after "[remoteplay-health] " in the log. Fixed keys, no addresses.
inline std::string formatHealthLine(const HealthSnapshot& s){
    char rtt[48];
    if(s.rttMs)std::snprintf(rtt,sizeof(rtt),"%.2fms(at-connect)",*s.rttMs);
    else std::snprintf(rtt,sizeof(rtt),"not-measured");
    char text[640];
    std::snprintf(text,sizeof(text),
        "grade=%s interval=%s seconds=%.1f lossPercent=%.3f lost=%llu received=%llu frameJitterMs=%.2f frameJitterPeakMs=%.2f "
        "reordered=%llu late=%llu reorderTimeouts=%llu skipped=%llu fecRecovered=%llu fecFailed=%llu keyframeRequests=%llu decoderResets=%llu "
        "frameDrops=%llu ingressDepth=%llu ingressPeak=%llu mailboxOverwrites=%llu mbps=%.2f fps=%.1f rtt=%s route=%s link=%s linkMbps=%llu",
        healthGradeName(s.grade),healthGradeName(s.intervalGrade),s.seconds,s.lossPercent,
        static_cast<unsigned long long>(s.packetsLost),static_cast<unsigned long long>(s.packetsReceived),s.frameJitterMs,s.frameJitterPeakMs,
        static_cast<unsigned long long>(s.reordered),static_cast<unsigned long long>(s.late),static_cast<unsigned long long>(s.reorderTimeouts),
        static_cast<unsigned long long>(s.reorderSkipped),static_cast<unsigned long long>(s.fecRecovered),static_cast<unsigned long long>(s.fecFailed),
        static_cast<unsigned long long>(s.keyframeRequests),static_cast<unsigned long long>(s.decoderResets),
        static_cast<unsigned long long>(s.networkFrameDrops),static_cast<unsigned long long>(s.ingressDepth),static_cast<unsigned long long>(s.ingressPeak),
        static_cast<unsigned long long>(s.mailboxOverwrites),s.receivedMbps,s.receivedFps,rtt,s.route.name(),s.route.linkName(),
        static_cast<unsigned long long>(s.route.linkBitsPerSecond/1000000u));
    char decode[32];
    if(s.decodeP95Ms)std::snprintf(decode,sizeof(decode),"%.2f",*s.decodeP95Ms);
    else std::snprintf(decode,sizeof(decode),"not-measured");
    char extra[320];
    // routeClass: wired / wifi / unknown (routeClass() above); "route=" keeps
    // its older meaning (direct-lan, ...), which the kit's parsers read.
    // consoleRttMs / consoleTargetKbps: the console's CONNECTIONQUALITY message (ConsoleQuality.h),
    // -1 unknown; consoleReports / consoleAgeMs say how many arrived and how old the newest is.
    std::snprintf(extra,sizeof(extra)," overshootMs=%.2f decodeP95Ms=%s lateArrivals=%llu routeClass=%s consoleRttMs=%.1f consoleTargetKbps=%.0f consoleMeasuredKbps=%.0f consoleReports=%llu consoleAgeMs=%.0f",
        s.reorderOvershootMs,decode,static_cast<unsigned long long>(s.lateArrivals),routeClassName(s.route),
        s.console.rttMs<0?-1.0:s.console.rttMs,s.console.targetKbps<0?-1.0:s.console.targetKbps,s.console.reports?s.console.measuredKbps:-1.0,
        static_cast<unsigned long long>(s.console.reports),s.console.ageMs<0?-1.0:s.console.ageMs);
    std::string line(text);
    line+=" bottleneck=";line+=bottleneckName(s.bottleneck);
    line+=" signs=";line+=formatBottleneckSigns(s.bottleneckSigns);
    line+=extra;
    return line;
}
} // namespace veyra::remoteplay
