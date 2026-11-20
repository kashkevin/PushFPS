#pragma once
// SPDX-License-Identifier: GPL-3.0-only
// Slipstream presentation latency instrument (sprint 4, contract line [slipstream-latency]).
// Portable and allocation-free: fixed arrays only, so the present path never allocates or locks.
// One owner thread (the presentation owner) calls presented() / scanout(); take() every 5 s.
// Times are host 100 ns ticks on one steady clock (EngineController host100ns, VideoPresenter
// steady_clock, PresentSink QPC converted to 100 ns); 0 means "unknown".
// The scanout time comes from DXGI frame statistics, which name a present by its present id
// one or more presents later; presented() keeps the last kPending frames so the join can happen
// when the statistics arrive. A frame whose scanout is never dated counts in `frames` only.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace veyra::engine {

struct PushPresentTimes {
    int64_t arrival=0;       // network arrival of the frame's data (takion callback)
    int64_t decoded=0;       // decoder output ready
    int64_t pickup=0;        // engine took the frame from the mailbox
    int64_t presentCall=0;   // just before IDXGISwapChain::Present
    int64_t presentReturn=0; // Present returned
};

class PushPresentLatency {
public:
    static constexpr std::size_t kCapacity=1024;   // samples per metric and window (5 s at 200 fps)
    static constexpr std::size_t kPending=32;      // presents waiting for their scanout
    static constexpr int64_t kWindow100ns=50000000; // 5 s
    enum Metric : unsigned { ArrivalToDecoded, DecodedToPickup, PickupToPresentCall, PresentCall,
        PresentToScanout, ArrivalToScanout, kMetrics };
    struct Percentiles { double p50=-1,p95=-1,p99=-1,max=-1; uint32_t samples=0; };
    struct Summary {
        uint32_t frames=0,scanoutKnown=0;
        std::array<Percentiles,kMetrics> metric{};
        const Percentiles& operator[](Metric m)const{return metric[m];}
    };

    // A real frame was presented. presentId: DXGI present id of that Present (GetLastPresentCount
    // right after it), 0 when the scanout can never be dated (tearing, provider swap chain).
    void presented(uint32_t presentId,const PushPresentTimes& t){
        ++frames_;
        add(ArrivalToDecoded,t.arrival,t.decoded);
        add(DecodedToPickup,t.decoded,t.pickup);
        add(PickupToPresentCall,t.pickup,t.presentCall);
        add(PresentCall,t.presentCall,t.presentReturn);
        if(!presentId)return;
        auto& slot=pending_[pendingNext_++%kPending];
        slot.id=presentId;slot.times=t;slot.used=true;
    }
    // The statistics dated present `presentId` at scanout100ns (the vblank that showed it).
    // Unknown or already joined ids are ignored, so feeding the same record twice is harmless.
    void scanout(uint32_t presentId,int64_t scanout100ns){
        if(!presentId||scanout100ns<=0)return;
        for(auto& slot:pending_){
            if(!slot.used||slot.id!=presentId)continue;
            slot.used=false;
            const bool a=add(PresentToScanout,slot.times.presentCall,scanout100ns);
            const bool b=add(ArrivalToScanout,slot.times.arrival,scanout100ns);
            if(a||b)++scanoutKnown_;
            return;
        }
    }
    bool due(int64_t now100ns){
        if(!windowStart_){windowStart_=now100ns;return false;}
        return now100ns-windowStart_>=kWindow100ns;
    }
    // Summarises and clears the window. Pending presents stay: their scanout may come next window.
    Summary take(int64_t now100ns){
        Summary s;s.frames=frames_;s.scanoutKnown=scanoutKnown_;
        for(unsigned m=0;m<kMetrics;++m){
            auto& values=series_[m];const std::size_t n=counts_[m];
            auto& out=s.metric[m];out.samples=uint32_t(n);
            if(!n)continue;
            std::sort(values.begin(),values.begin()+std::ptrdiff_t(n));
            out.p50=rank(values,n,50);out.p95=rank(values,n,95);out.p99=rank(values,n,99);out.max=values[n-1];
        }
        counts_.fill(0);frames_=scanoutKnown_=0;windowStart_=now100ns;
        return s;
    }
    void reset(){for(auto& slot:pending_)slot.used=false;counts_.fill(0);frames_=scanoutKnown_=0;windowStart_=0;}

private:
    struct Pending { uint32_t id=0; PushPresentTimes times{}; bool used=false; };
    // Nearest rank, as PresentSink::takeDisplayLatency: value at ceil(n*q/100)-1.
    static double rank(const std::array<float,kCapacity>& values,std::size_t n,std::size_t q){
        const std::size_t index=(n*q+99)/100;
        return double(values[std::min(n-1,index?index-1:0)]);
    }
    // ms between two stamps; both known, not reversed, under 10 s. Returns whether it counted.
    bool add(Metric m,int64_t from,int64_t to){
        if(from<=0||to<=0||to<from||to-from>100000000)return false;
        auto& n=counts_[m];
        if(n>=kCapacity)return false;
        series_[m][n++]=float(double(to-from)/10000.0);
        return true;
    }
    std::array<Pending,kPending> pending_{};
    std::size_t pendingNext_=0;
    std::array<std::array<float,kCapacity>,kMetrics> series_{};
    std::array<std::size_t,kMetrics> counts_{};
    uint32_t frames_=0,scanoutKnown_=0;
    int64_t windowStart_=0;
};

// DXGI_FRAME_PRESENTATION_MODE from IDXGISwapChainMedia::GetFrameStatisticsMedia: 0 composed,
// 1 overlay (hardware plane, no DWM copy), 2 none (the chain is not composed: independent flip),
// 3 composition failure; -1 query unavailable.
inline const char* pushFlipName(int compositionMode){
    return compositionMode==0?"composed":compositionMode==1||compositionMode==2?"independent":"unknown";
}

struct PushPresentContract {
    int syncInterval=-1;
    bool tearing=false;
    int queueLatency=-1; // SetMaximumFrameLatency in force, -1 unknown
    bool waitable=false;
    int compositionMode=-1;
};

inline std::string formatSlipstreamLatency(const PushPresentLatency::Summary& s,const PushPresentContract& c){
    using L=PushPresentLatency;
    const auto v=[](double x){return x<0?-1.0:x;};
    char line[1024];
    const auto& ad=s[L::ArrivalToDecoded];const auto& dp=s[L::DecodedToPickup];const auto& pc=s[L::PickupToPresentCall];
    const auto& pr=s[L::PresentCall];const auto& ps=s[L::PresentToScanout];const auto& as=s[L::ArrivalToScanout];
    std::snprintf(line,sizeof(line),
        "frames=%u arrivalToDecodedP50=%.3f arrivalToDecodedP95=%.3f decodedToPickupP50=%.3f decodedToPickupP95=%.3f "
        "pickupToPresentCallP50=%.3f pickupToPresentCallP95=%.3f presentCallP50=%.3f presentCallP95=%.3f "
        "presentToScanoutP50=%.3f presentToScanoutP95=%.3f arrivalToScanoutP50=%.3f arrivalToScanoutP95=%.3f "
        "arrivalToScanoutP99=%.3f arrivalToScanoutMax=%.3f scanoutKnown=%u syncInterval=%d tearing=%d queueLatency=%d "
        "waitable=%d flip=%s",
        s.frames,v(ad.p50),v(ad.p95),v(dp.p50),v(dp.p95),v(pc.p50),v(pc.p95),v(pr.p50),v(pr.p95),v(ps.p50),v(ps.p95),
        v(as.p50),v(as.p95),v(as.p99),v(as.max),s.scanoutKnown,c.syncInterval,c.tearing?1:0,c.queueLatency,c.waitable?1:0,
        pushFlipName(c.compositionMode));
    return line;
}

struct PushSwapchainFacts {
    bool flipModel=false;bool flipKnown=false;
    unsigned buffers=0;unsigned format=0;bool waitable=false;bool tearingSupported=false;
    int syncInterval=-1;unsigned flags=0;bool fullscreen=false;bool windowCoversMonitor=false;
    int compositionMode=-1;
};
// The swap chain has its own swap effect (flip or not); whether DWM composes it is a property of each
// present, which only GetFrameStatisticsMedia or PresentMon can tell: flip= says that, when known.
inline std::string formatSlipstreamSwapchain(const PushSwapchainFacts& f){
    char line[512];
    std::snprintf(line,sizeof(line),
        "flip=%s buffers=%u format=%u waitable=%d tearingSupported=%d syncInterval=%d flags=0x%X fullscreen=%d windowCoversMonitor=%d",
        !f.flipModel?"no":f.compositionMode>=0?pushFlipName(f.compositionMode):"unknown",
        f.buffers,f.format,f.waitable?1:0,f.tearingSupported?1:0,f.syncInterval,f.flags,f.fullscreen?1:0,f.windowCoversMonitor?1:0);
    return line;
}

// Rectangles in screen pixels; true when `inner` covers all of `outer` (a window covering its monitor).
inline bool pushCoversRect(long innerLeft,long innerTop,long innerRight,long innerBottom,
                           long outerLeft,long outerTop,long outerRight,long outerBottom){
    return outerRight>outerLeft&&outerBottom>outerTop&&innerLeft<=outerLeft&&innerTop<=outerTop&&
        innerRight>=outerRight&&innerBottom>=outerBottom;
}

// VEYRA_EXP_PRESENT_TEARING decision (portable so the rule is tested): sync interval 0 with
// DXGI_PRESENT_ALLOW_TEARING replaces a VSync present only when the switch is on, the sink
// would otherwise use explicit VSync, the factory and the chain support tearing, the driver
// reports the display in VRR mode (level 4), no frame generation runs and no output cap or
// provider (XeSS) owns the cadence.
struct PushTearingInputs {
    bool switchOn=false,vsync=false,tearingSupported=false,frameGeneration=false,outputCap=false,provider=false;
    unsigned vrrLevel=0;
};
inline const char* pushTearingRefusal(const PushTearingInputs& in){
    if(!in.switchOn)return "off";
    if(!in.vsync)return "not-vsync";
    if(in.provider)return "provider";
    if(in.frameGeneration)return "frame-generation";
    if(in.outputCap)return "output-cap";
    if(!in.tearingSupported)return "no-tearing-support";
    if(in.vrrLevel<4)return "vrr-not-active";
    return nullptr;
}
inline bool pushTearingExperiment(const PushTearingInputs& in){return pushTearingRefusal(in)==nullptr;}

} // namespace veyra::engine
