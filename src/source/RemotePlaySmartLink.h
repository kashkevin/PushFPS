// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The session owner's SmartLink glue (included by RemotePlaySessionSource.cpp
// only). Created only when VEYRA_EXP_RP_SMARTLINK, VEYRA_EXP_RP_SMARTLINK_TRACE
// or VEYRA_EXP_RP_WIFI_STREAMING_MODE is set: with them unset the session holds
// an empty std::optional and calls nothing here. Owner thread only.
//
//   - Runner (SmartLinkRunner.h): feed -> engine every 200 ms; publishShared();
//     `[slipstream-smartlink] ...` at most every 5 s; apply mode: the reorder wait.
//   - Trace (VEYRA_EXP_RP_SMARTLINK_TRACE=1, also without SmartLink):
//     smartlink-trace.csv beside the app log, one SmartLinkTrace.h line per
//     window, at most 9000 lines (30 min) per stream; at each stream start the
//     previous file becomes .1 and .1 becomes .2 (3 kept). Written through a
//     64 KB stdio buffer (the frame-path thread reaches the disk about once a
//     minute, not every few seconds), flushed at the line limit and at stream
//     end. If a rename fails the previous trace is kept and this stream records none.
//   - Wi-Fi (SmartLinkWifi.h): the RSSI on a Wi-Fi route; the media streaming
//     mode only with VEYRA_EXP_RP_WIFI_STREAMING_MODE=1 (SmartLink never asks) and
//     only when the route to the console is Wi-Fi (else wifiStreamingMode=skipped-not-wifi).
// Unverified on Windows and on a PS5 (blind code).
#include "veyra/Log.h"
#include "veyra/RuntimePaths.h"
#include "veyra/remoteplay/ChiakiBackend.h"
#include "veyra/remoteplay/SmartLinkRunner.h"
#include "veyra/remoteplay/SmartLinkShared.h"
#include "veyra/remoteplay/SmartLinkTrace.h"
#include "veyra/remoteplay/SmartLinkWifi.h"
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
namespace veyra::source {
namespace smartlink=remoteplay::smartlink;
struct SmartLinkSwitches {
    smartlink::Mode mode=smartlink::Mode::Off;
    smartlink::ModelKind model=smartlink::ModelKind::Statistics;
    bool trace=false,streamingMode=false;
    bool any()const noexcept{return mode!=smartlink::Mode::Off||trace||streamingMode;}
};
class RemotePlaySmartLink {
public:
    static constexpr std::size_t kTraceLines=9000,kTraceBuffer=64*1024;
    RemotePlaySmartLink(const SmartLinkSwitches& s,std::uint32_t configuredReorderUs,bool explicitReorder,double requestedKbps,double fps)
        :switches_(s),requestedKbps_(requestedKbps),fps_(fps),
         runner_(s.mode==smartlink::Mode::Off?smartlink::Mode::Observe:s.mode,s.model,configuredReorderUs,explicitReorder){
        smartlink::clearShared();
        if(switches_.trace)openTrace();
    }
    ~RemotePlaySmartLink(){
        // The library's live reorder wait is process-wide and is applied to every later connection until
        // it is cleared: a stream that applied SmartLink's wait must not leave it for the next one (which
        // may run with SmartLink off). 0 = no live value: connections use their configured wait again.
        if(appliesReorder())remoteplay::setLiveReorderTimeout(0);
        smartlink::clearShared();
        if(trace_)std::fclose(trace_); // flushes the buffer: stream end
        if(wifi_.stop())log::info("slipstream-smartlink","wifiStreamingMode=released");
    }
    RemotePlaySmartLink(const RemotePlaySmartLink&)=delete;RemotePlaySmartLink& operator=(const RemotePlaySmartLink&)=delete;
    bool engineOn()const noexcept{return switches_.mode!=smartlink::Mode::Off;}
    bool appliesReorder()const noexcept{return switches_.mode==smartlink::Mode::Apply&&runner_.appliesReorder();}
    // The session's console units (ConsoleQuality.h), shared with the health line.
    void shareUnits(remoteplay::ConsoleUnits* units)noexcept{runner_.shareUnits(units);}
    // Each connection attempt, once the route is known.
    void connectionStarted(const remoteplay::RouteAssessment& route){
        route_=remoteplay::routeClass(route)==remoteplay::RouteClass::Wired?smartlink::Route::Wired
              :remoteplay::routeClass(route)==remoteplay::RouteClass::Wifi?smartlink::Route::Wifi:smartlink::Route::Unknown;
        if(!wifiStarted_){
            wifiStarted_=true;
            const bool poll=route_==smartlink::Route::Wifi&&(engineOn()||switches_.trace);
            const bool streamingMode=switches_.streamingMode&&route_==smartlink::Route::Wifi; // a wired route: never on another adapter
            wifi_.start(poll,streamingMode);
            if(streamingMode)log::info("slipstream-smartlink",std::format("wifiStreamingMode={}",smartlink::streamingModeName(wifi_.streamingMode())));
            else if(switches_.streamingMode)log::info("slipstream-smartlink","wifiStreamingMode=skipped-not-wifi");
        }
        const auto us=runner_.newConnection();
        if(us&&appliesReorder())remoteplay::setLiveReorderTimeout(us);
    }
    void frameArrived(std::int64_t arrival100ns)noexcept{if(arrival100ns>0)runner_.frameArrived(arrival100ns/10000);}
    // Every owner loop pass; does nothing until a 200 ms window is due.
    void tick(std::int64_t now100ns,const remoteplay::SessionInbox::Snapshot& snapshot){
        if(!engineOn()&&!switches_.trace)return;
        const std::int64_t nowMs=now100ns/10000;
        if(lastPollMs_>=0&&nowMs>=lastPollMs_&&nowMs-lastPollMs_<smartlink::SampleFeed::kWindowMs)return;
        lastPollMs_=nowMs;
        smartlink::FeedInput in;
        in.nowMs=nowMs;in.transport=remoteplay::readTransportCounters();
        in.framesComplete=snapshot.video.accessUnits;in.receivedBytes=snapshot.receivedBytes;
        in.videoQueueDepth=static_cast<std::int64_t>(snapshot.video.depth);
        in.requestedKbps=requestedKbps_;in.nominalFps=fps_;in.route=route_;
        const auto audio=smartlink::latestAudioFeed();
        if(audio.sequence!=0){in.audioBufferedMs=audio.bufferedMs;in.audioUnderrunsTotal=audio.underruns;}
        in.wifi=wifi_.latest();
        const auto step=runner_.step(in);
        if(step.sample&&trace_&&traceLines_<kTraceLines){
            char line[smartlink::trace::kMaxLine];
            const auto size=smartlink::trace::formatTraceLine(*step.sample,line,sizeof(line));
            if(size)std::fwrite(line,1,size,trace_);
            if(++traceLines_==kTraceLines)std::fflush(trace_);
        }
        if(!step.decided||!engineOn())return;
        smartlink::publishShared(step.decision,switches_.mode,nowMs);
        if(step.explicitWins)log::info("slipstream-smartlink","VEYRA_EXP_RP_REORDER_US is set: the explicit reorder wait wins, SmartLink does not change it");
        if(step.setReorderUs&&appliesReorder()){
            remoteplay::setLiveReorderTimeout(step.setReorderUs);
            log::info("slipstream-smartlink",std::format("reorder wait -> {} us",step.setReorderUs));
        }
        if(step.log)log::info("slipstream-smartlink",std::string(smartlink::toLogLine(step.decision,switches_.mode).view()).substr(23));
    }
private:
    void openTrace(){
        try{
            const auto dir=runtime::logsDirectory();
            const auto file=dir/"smartlink-trace.csv",one=dir/"smartlink-trace.1.csv",two=dir/"smartlink-trace.2.csv";
            std::error_code ec;
            // A failed step keeps what is there: "wb" must never overwrite an earlier trace.
            bool rotated=true;
            std::filesystem::remove(two,ec);
            if(ec||std::filesystem::exists(two,ec))rotated=false;
            if(rotated&&std::filesystem::exists(one,ec)){std::filesystem::rename(one,two,ec);if(ec)rotated=false;}
            if(rotated&&std::filesystem::exists(file,ec)){std::filesystem::rename(file,one,ec);if(ec)rotated=false;}
            if(!rotated||std::filesystem::exists(file,ec)){log::warn("slipstream-smartlink","trace files could not be rotated; no trace for this stream (earlier traces kept)");return;}
            if(_wfopen_s(&trace_,file.wstring().c_str(),L"wb")!=0)trace_=nullptr;
            if(!trace_){log::warn("slipstream-smartlink","trace file could not be opened; no trace");return;}
            traceBuffer_=std::make_unique<char[]>(kTraceBuffer);
            if(std::setvbuf(trace_,traceBuffer_.get(),_IOFBF,kTraceBuffer)!=0)traceBuffer_.reset(); // the runtime's own buffer then
            char header[smartlink::trace::kHeader.size()+2];
            const auto size=smartlink::trace::writeTraceHeader(header,sizeof(header));
            if(size)std::fwrite(header,1,size,trace_);
            log::info("slipstream-smartlink","trace=smartlink-trace.csv (numbers only, at most 30 minutes)");
        }catch(...){trace_=nullptr;}
    }
    SmartLinkSwitches switches_;
    double requestedKbps_,fps_;
    smartlink::Runner runner_;
    smartlink::WifiMonitor wifi_;
    smartlink::Route route_=smartlink::Route::Unknown;
    std::unique_ptr<char[]> traceBuffer_; // declared before trace_ is closed in the destructor body: outlives it
    std::FILE* trace_=nullptr;
    std::size_t traceLines_=0;
    std::int64_t lastPollMs_=-1;
    bool wifiStarted_=false;
};
} // namespace veyra::source
