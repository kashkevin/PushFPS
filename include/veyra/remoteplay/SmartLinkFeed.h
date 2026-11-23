// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// SmartLink: turns what the session owner already has (the patched library's
// cumulative counters, the session inbox, the decoded frames' arrival times, the
// audio sink's feed and the Wi-Fi reading) into one smartlink::Sample per
// observation window of 200 ms (SmartLinkTypes.h). Portable, header-only, owner
// thread only, no allocation. Unknown stays negative (the Wi-Fi RSSI: >= 0).
//
// What each field is made from, and its limits:
//   packetsReceived/Lost   the library's congestion accounting (200 ms windows of
//                          its own, not aligned with ours: a window can hold 0 or 2)
//   packetsReordered       reorder gaps that closed in time (gaps - timeouts)
//   packetsLate            packets that arrived after their wait gave them up
//   packetGapMeanMs        window / packets received (a mean, not measured per packet)
//   packetGapMaxMs         unknown (-1): the library keeps no per-packet times
//   framesArrived          complete frames the inbox received
//   framesIncomplete       frames FEC could not complete; framesFecRecovered likewise
//   frameGapMaxMs/Excess   from the decoded frames' arrival stamps (observeFrameArrival),
//                          including the gap still open at the end of the window
//   lateRescueMs           0 when no frame was lost to a late packet in the window;
//                          else the upper edge of the latest lateness bucket that came
//                          after the wait, minus the wait (bucket resolution: 1-4 ms;
//                          a late packet beyond 16 ms gives 16 - wait). Which late
//                          packet belonged to which lost frame is not known per bucket.
//                          A late packet counted in the previous window still counts
//                          (the packet and its lost frame can land in different windows).
//   reorderWaitMs          the wait the library reports using
//   rttMs, consoleTargetKbps  the console's CONNECTIONQUALITY (ConsoleQuality.h), the
//                          newest message repeated; unknown until its unit is decided
//                          (the session's ConsoleUnits, shared with the health line:
//                          shareUnits())
//   measuredKbps           bytes of complete frames received x 8 / window
//   requestedKbps          the launch request
//   videoQueueDepth        compressed frames waiting for the decoder
//   audio*                 latestAudioFeed() (SmartLinkShared.h); unknown until the sink reports
//   wifi*                  WifiReading (SmartLinkWifi.h on Windows); unknown on a wired route
#include "ConsoleQuality.h"
#include "SmartLinkTypes.h"
#include "TransportTuning.h"
#include <algorithm>
#include <cstdint>
#include <optional>
namespace veyra::remoteplay::smartlink {

// The Wi-Fi adapter carrying the route to the console (numbers only).
struct WifiReading {
    double signalPercent=-1; // 0..100 (WLAN_ASSOCIATION_ATTRIBUTES::wlanSignalQuality)
    double rssiDbm=0;        // negative dBm; >= 0 unknown
    double rxMbps=-1,txMbps=-1;
    double bandGhz=-1;       // 2.4, 5 or 6
};

// What the owner has at the end of a window. Cumulative values restart with each
// connection attempt (call SampleFeed::newConnection()).
struct FeedInput {
    std::int64_t nowMs=-1;              // monotonic host ms
    TransportCounters transport{};      // readTransportCounters()
    std::uint64_t framesComplete=0;     // SessionInbox::Snapshot::video.accessUnits
    std::uint64_t receivedBytes=0;      // SessionInbox::Snapshot::receivedBytes
    std::int64_t videoQueueDepth=-1;    // SessionInbox::Snapshot::video.depth
    double requestedKbps=-1,nominalFps=-1;
    Route route=Route::Unknown;
    double audioBufferedMs=-1;          // latestAudioFeed()
    std::int64_t audioUnderrunsTotal=-1;
    WifiReading wifi{};
};

class SampleFeed {
public:
    static constexpr std::int64_t kWindowMs=200;
    // The decoded frame's network arrival (FramePacket::arrivalHost100ns / 10000), in order.
    void observeFrameArrival(std::int64_t arrivalMs)noexcept{
        if(arrivalMs<0)return;
        if(lastArrivalMs_>=0&&arrivalMs>=lastArrivalMs_)gapMaxMs_=(std::max)(gapMaxMs_,static_cast<double>(arrivalMs-lastArrivalMs_));
        if(lastArrivalMs_<0||arrivalMs>=lastArrivalMs_)lastArrivalMs_=arrivalMs;
    }
    // The library's and the inbox's counters restart: the next poll takes them as
    // its new base and starts a new window (no sample across a reconnect).
    void newConnection()noexcept{based_=false;lastArrivalMs_=-1;gapMaxMs_=-1;carriedLateEdgeMs_=-1;}
    // The session's console units (one object shared with the health line); nullptr =
    // the feed's own. The object must outlive the feed's use of it.
    void shareUnits(ConsoleUnits* units)noexcept{shared_=units;}
    // A Sample when a window of at least kWindowMs has ended, else nothing. A clock
    // that went back restarts the window without a sample.
    std::optional<Sample> poll(const FeedInput& in)noexcept{
        if(in.nowMs<0)return {};
        if(!based_){based_=true;base(in);return {};}
        if(in.nowMs<startMs_){base(in);carriedLateEdgeMs_=-1;return {};}
        if(in.nowMs-startMs_<kWindowMs)return {};
        Sample s;
        s.endMs=in.nowMs;s.windowMs=static_cast<double>(in.nowMs-startMs_);s.route=in.route;
        const auto& t=in.transport;const auto& p=previous_.transport;
        s.packetsReceived=delta(t.congestionReceived,p.congestionReceived);
        s.packetsLost=delta(t.congestionLost,p.congestionLost);
        const std::int64_t gaps=delta(t.reorderGaps,p.reorderGaps),timeouts=delta(t.reorderTimeouts,p.reorderTimeouts);
        s.packetsReordered=gaps>timeouts?gaps-timeouts:0;
        s.packetsLate=delta(t.reorderLateArrivals,p.reorderLateArrivals);
        if(s.packetsReceived>0)s.packetGapMeanMs=s.windowMs/static_cast<double>(s.packetsReceived);
        s.framesArrived=delta(in.framesComplete,previous_.framesComplete);
        s.framesIncomplete=delta(t.fecFailed,p.fecFailed);
        s.framesFecRecovered=delta(t.fecRecovered,p.fecRecovered);
        double gap=gapMaxMs_;
        if(lastArrivalMs_>=0&&in.nowMs>=lastArrivalMs_)gap=(std::max)(gap,static_cast<double>(in.nowMs-lastArrivalMs_)); // still open
        if(gap>=0){
            s.frameGapMaxMs=gap;
            if(in.nominalFps>0)s.frameGapExcessMs=(std::max)(0.0,gap-1000.0/in.nominalFps);
        }
        if(t.reorderTimeoutUs!=0)s.reorderWaitMs=static_cast<double>(t.reorderTimeoutUs)/1000.0;
        s.lateRescueMs=lateRescueMs(t,p,s.reorderWaitMs,carriedLateEdgeMs_);
        ConsoleUnits& units=shared_?*shared_:units_;
        units.learn(t,in.requestedKbps);
        const auto console=interpretConsoleQuality(t,t.libraryNowUs,in.requestedKbps,&units);
        s.rttMs=console.rttMs;s.consoleTargetKbps=console.targetKbps;
        if(s.windowMs>0)s.measuredKbps=static_cast<double>(delta(in.receivedBytes,previous_.receivedBytes))*8.0/s.windowMs;
        if(in.requestedKbps>0)s.requestedKbps=in.requestedKbps;
        s.videoQueueDepth=in.videoQueueDepth<0?-1:in.videoQueueDepth;
        if(in.audioBufferedMs>=0)s.audioBufferedMs=in.audioBufferedMs;
        if(in.audioUnderrunsTotal>=0&&previous_.audioUnderrunsTotal>=0)
            s.audioUnderruns=in.audioUnderrunsTotal>=previous_.audioUnderrunsTotal?in.audioUnderrunsTotal-previous_.audioUnderrunsTotal:in.audioUnderrunsTotal;
        if(in.route==Route::Wifi){
            s.wifiSignalPercent=in.wifi.signalPercent;s.wifiRssiDbm=in.wifi.rssiDbm;
            s.wifiLinkMbps=in.wifi.rxMbps;s.wifiBandGhz=in.wifi.bandGhz;
        }
        base(in);
        return s;
    }
private:
    // Counters restart from zero at a reconnect: a smaller value is counted from zero.
    static std::int64_t delta(std::uint64_t now,std::uint64_t before)noexcept{
        const std::uint64_t d=now>=before?now-before:now;
        return d>0x7fffffffffffffffull?0x7fffffffffffffffll:static_cast<std::int64_t>(d);
    }
    // carried: in, the latest bucket edge seen in the previous window (-1 none); out,
    // this window's, for the next one (one window of carry-over, then it is spent).
    static double lateRescueMs(const TransportCounters& t,const TransportCounters& p,double waitMs,double& carried)noexcept{
        double latest=-1;
        for(std::size_t b=0;b<kReorderLatenessBuckets;++b){
            if(delta(t.reorderLateness[b],p.reorderLateness[b])==0)continue;
            const double upper=b<kReorderLatenessEdgesUs.size()?static_cast<double>(kReorderLatenessEdgesUs[b])/1000.0:16.0;
            latest=(std::max)(latest,upper);
        }
        const double previous=carried;carried=latest;
        const std::int64_t rescuable=delta(t.lateFramesFailed,p.lateFramesFailed);
        if(rescuable==0)return 0;
        if(waitMs<0)return -1;
        double edge=latest>waitMs?latest:-1;
        if(edge<0&&previous>waitMs)edge=previous;
        return edge<0?-1:edge-waitMs;
    }
    void base(const FeedInput& in)noexcept{
        previous_=in;startMs_=in.nowMs;gapMaxMs_=-1;
    }
    FeedInput previous_{};
    ConsoleUnits units_{}; // used when no shared object is given; kept across reconnects
    ConsoleUnits* shared_=nullptr;
    double carriedLateEdgeMs_=-1;
    std::int64_t startMs_=0,lastArrivalMs_=-1;
    double gapMaxMs_=-1;
    bool based_=false;
};
} // namespace veyra::remoteplay::smartlink
