#pragma once
#include "veyra/diagnostics/GpuTimer.h"
#include "veyra/gfx/PresentSink.h"
#include <dxgi1_6.h> // DXGI_HDR_METADATA_HDR10 (custom: HDR static metadata)
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/engine/PreviewView.h"
#include "veyra/engine/HdrOutputTuning.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/gfx/ReflexSession.h"
#include "veyra/gfx/NvidiaVrr.h"
#include "veyra/engine/PresentationSettings.h"
#include "veyra/engine/PresentCapture.h"
#include "veyra/engine/DisplayStateService.h"
#include "veyra/engine/PushPresentTiming.h"
#include "veyra/engine/PushCadence.h"
#include <memory>
#include <chrono>
#include <array>
#include <optional>
namespace veyra::gfx { class D3D12DeviceContext; class CommandSlotRing; }
namespace veyra::pipeline { class EnhanceGraph; }
namespace veyra::sink { struct RgbaImage; }
namespace veyra::engine {
class VideoPresenter {
public:
    // Live recording hook; the object must outlive every present() call.
    void setCapture(PresentCapture* capture){capture_=capture;}
    // Shared refresh/VRR states (the engine's); must outlive the presenter.
    void setDisplayStates(DisplayStateService* states){displayStates_=states;}
    ~VideoPresenter(){close();}
    // mediaClockPaced: the owner already waits on a media clock before each
    // source (file playback). Retained for diagnostics; XeLL low latency must
    // stay enabled or the provider refuses generation (result -15).
    bool open(gfx::D3D12DeviceContext&, HWND, pipeline::EnhanceGraph&, bool captureCompatible=false, bool mediaClockPaced=false);
    bool present(gfx::D3D12DeviceContext&,gfx::CommandSlotRing&,pipeline::EnhanceGraph&,unsigned slot,bool generated,bool referencesValid=true,int comparison=0,bool baseReference=false,float split=.5f,pipeline::FrameIdentity identity={},PreviewView view={},int64_t sourcePts100ns=-1,bool retainUnchanged=false);
    void close();
    // Owner-thread graph replacement boundary. Native DXGI remains attached to
    // its HWND; provider-owned presentation must be shut down before its graph.
    bool prepareGraphChange();
    // Explicit lifecycle regression: simulate an overlay retaining the chain.
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChainForTest()const{return sink_.swapChain();}
    // Present-sink providers (XeSS) block inside Present while pacing
    // their generated frames. A helper-thread Present was tried on
    // 2026-09-22 (X3): the provider then estimated its period from the
    // longer hand-off intervals and the source rate fell 51 -> 24/s at 44%
    // GPU. The synchronous call stays; see FG_INDEPENDENT_REPAIR_EXECUTION.
    // stallUs > 0: a live source; a driver low-latency Sleep longer than this,
    // repeatedly, ends NVIDIA Low Latency for the session (ReflexSession).
    bool beginSourceInput(uint64_t stallUs=0);
    bool beginSourceProcessing();
    void sourceProcessed(pipeline::FrameIdentity);
    PresentationSettings configurePresentation(gfx::D3D12DeviceContext&,PresentationSettings,bool fg,std::wstring& status);
    bool presentationReady(){return sink_.presentationReady();}
    gfx::PresentSink::RefreshGrid refreshGrid()const{return sink_.refreshGrid();}
    bool waitPresentationReady(unsigned ms){return sink_.waitPresentationReady(ms);}
    uint64_t beginReflex(){const auto id=reflexInputFrame_;reflexInputFrame_=0;reflex_.renderStart(id);return id;}
    void endReflex(uint64_t id){reflex_.renderEnd(id);}
    void cancelReflexInput(){if(reflexInputFrame_)reflex_.mark(reflexInputFrame_,1);reflexInputFrame_=0;}
    void reflexFrame(uint64_t id){reflexFrame_=id;}
    // True while XeSS owns Present: the low-latency queue and the output
    // cap cannot reach provider-generated frames, so the UI greys them out.
    bool providerOwnedPresentation()const{return providerOwnedPresentation_;}
    // Native presentation has its own command ring and a GPU-side producer
    // fence wait. XeSS shares the graph ring and must not re-enter it.
    bool independentQueue()const{return presentationQueue_!=nullptr;}

    bool reflexActive()const{return reflex_.active();}
    bool reflexDisablePending()const{return reflex_.disablePending();}
    bool reflexFailed()const{return reflex_.failed();}
    bool reflexStalled()const{return reflex_.stalled();}
    // Host time (steady clock, 100 ns) of the last swap-chain Present call, taken
    // just before it as RTSS and PresentMon do: the 1% low counts these, not
    // returns, whose spacing also carries the call's own 0.2-0.6 ms jitter.
    int64_t lastPresentCallHost100ns()const{return lastPresentCallHost100ns_;}
    // Slipstream (VEYRA_EXP_RP_STATS=1): called after a REAL frame's present with the host 100 ns
    // stamps the frame carries (network arrival, decoded, engine pickup); joins the scanout dated by
    // DXGI frame statistics and writes one [slipstream-latency] line per 5 s. No-op without the switch.
    void latencyObserved(int64_t arrival100ns,int64_t decoded100ns,int64_t pickup100ns);
    // VEYRA_EXP_PRESENT_TEARING: whether the last configurePresentation replaced VSync by
    // sync interval 0 + DXGI_PRESENT_ALLOW_TEARING (docs/PRESENTATION_LATENCY.md).
    bool tearingExperimentActive()const{return tearingExperiment_;}
    bool pacingActive()const{return sink_.pacingActive();}
    uint64_t generation()const{return generation_;}
    // Explicit integration-test capture only; never called by playback/export.
    bool readPresentedFrameForTest(gfx::D3D12DeviceContext&,gfx::CommandSlotRing&,sink::RgbaImage&);
    // A visible HDR panel can request sparse GPU statistics. No pixel readback.
    void observeHdrCoverage(bool on){probeObserved_=on;if(!on)liveCoverage_={};}
    const HdrCurveCoverage& liveCurveCoverage() const { return liveCoverage_; }
    // Test-only buffer reference for lossless HDR readback. Drains the private
    // queue; caller drains the shared queue and releases before resize/close.
    Microsoft::WRL::ComPtr<ID3D12Resource> presentedResourceForTest();
    uint64_t submittedCount() const {return sink_.presentCount();}
    // PushFPS stream "delay90" ([slipstream-sizes]): the swap chain's back buffer extent.
    uint32_t backBufferWidth() const {return sink_.bufferWidth();}
    uint32_t backBufferHeight() const {return sink_.bufferHeight();}
    uint64_t consumerFenceValue(uint64_t sharedValue)const{return presentationQueue_?presentationRing_.lastSignaledValue():sharedValue;}
    double cpuWaitMilliseconds(const gfx::CommandSlotRing& shared)const{return presentationQueue_?presentationRing_.cpuWaitMilliseconds():shared.cpuWaitMilliseconds();}
    uint64_t xessGeneratedCount() const {return sink_.xess()?sink_.xess()->generatedCount():0;}
    uint64_t xessPresentedCount() const {return sink_.xess()?sink_.xess()->presentedCount():0;}
    bool xessActive() const {return sink_.xess()!=nullptr;}
    bool xessFailed() const {return xessFailed_;}
    // Sustained under-rate may suppress SDK-owned XeSS-FG generation over a
    // stable interval (xefgSwapChainSetEnabled); re-enabling goes through the
    // per-frame history reset, never a per-frame toggle.
    void setXessGenerationSuppressed(bool v){xessGenerationSuppressed_=v;}
    bool xessGenerationSuppressed() const {return xessGenerationSuppressed_;}
    // Monitor refresh for the live output budget and 跟随显示器, from the shared
    // DisplayStateService. It is read again only on events: a new presentation
    // chain, the window moving to another monitor, a display change or
    // 重新检测, and one VRR confirmation (vrrConfirmAt_); never on a timer.
    // This accessor never waits on QueryDisplayConfig or NVIDIA's driver calls.
    double displayRefreshHz();
    // Variable refresh state of that monitor (NVIDIA only), read with it.
    gfx::VrrState displayVrr()const{return vrr_;}
    // VRR state for the cadence figures and VEYRA_EXP_PRESENT_EVEN_CADENCE: 1 enabled, 0 fixed refresh (read after
    // the first present), -1 unknown (PushCadence.h pushVrrCadenceState).
    int cadenceVrr()const{
        return pushVrrCadenceState(vrr_.known,vrr_.enabled,cadenceConfirm_.readAfterPresent(),vrrConfirmAt_.has_value(),cadenceConfirm_.recentResize(cadenceNowMs()));
    }
    // The 可变刷新 rule: the switch is on, Automatic is selected and VRR is
    // possible on the monitor, so Automatic presents like explicit Vsync.
    // vrrSync() is what the last configurePresentation applied.
    static bool vrrSyncWanted(const PresentationSettings& requested,const gfx::VrrState& vrr){
        return requested.variableRefresh&&requested.display==DisplaySync::Automatic&&gfx::vrrLevel(vrr)>=3;
    }
    bool vrrSync()const{return vrrSync_;}
    diagnostics::GpuSample blitTiming(ID3D12Fence* f,uint64_t revision=0,uint64_t epoch=0){gpuTimer_.collect(presentationFence_?presentationFence_.Get():f);if((revision&&gpuTimer_.last().identity.settingsRevision!=revision)||(epoch&&gpuTimer_.last().identity.epoch!=epoch)){diagnostics::GpuSample pending;pending.state=diagnostics::SampleState::Pending;return pending;}return gpuTimer_.last().gpu[size_t(diagnostics::GpuStage::Blit)];}
    // Application-side frame-generation timing for present-sink backends
    // (XeSS): the copies, barriers and provider prepare work recorded on
    // our list. collect() is idempotent, so this is safe alongside blitTiming.
    diagnostics::GpuSample fgTiming(ID3D12Fence* f,uint64_t revision=0,uint64_t epoch=0){gpuTimer_.collect(f);if((revision&&gpuTimer_.last().identity.settingsRevision!=revision)||(epoch&&gpuTimer_.last().identity.epoch!=epoch)){diagnostics::GpuSample pending;pending.state=diagnostics::SampleState::Pending;return pending;}return gpuTimer_.last().gpu[size_t(diagnostics::GpuStage::FgBatch)];}
std::vector<diagnostics::GpuFrameTiming> takeGpuTimings(ID3D12Fence* fence){gpuTimer_.collect(presentationFence_?presentationFence_.Get():fence);return gpuTimer_.takeCompleted();}
void recordGpuTimings(){gpuTimer_.recordCompleted();}
private:
    // Custom: HDR10 static metadata for the display (MaxCLL / MaxFALL / mastering
    // peak). Sent only when it changes, and cleared when the HDR10 path stops.
    void applyHdrMetadata(pipeline::EnhanceGraph& graph,const HdrMetadataSettings& metadata,const HdrCurveSettings& resolvedCurve,pipeline::FrameIdentity identity);
    bool metadataSent_=false,metadataDirty_=false;
    // The source's mastering/CLL values are absent on many frames; holding the last
    // real value keeps the reported metadata steady instead of flapping to the
    // fallback (which re-sent it on nearly every frame).
    HdrSourceMetadata sourceMetadata_{};
    gfx::PresentSink::DisplayPeakInfo hdrPeakInfo_{};
    std::chrono::steady_clock::time_point metadataAttempt_{};
    DXGI_HDR_METADATA_HDR10 lastMetadata_{};
    gfx::ReflexSession reflex_;
    uint64_t reflexFrame_=0,reflexInputFrame_=0,generation_=0;
    int64_t lastPresentCallHost100ns_=0;
    int64_t lastPresentReturnHost100ns_=0;
    // Slipstream experiments, read once per swap-chain creation (open() without reuse).
    std::unique_ptr<PushPresentLatency> latency_;
    std::unique_ptr<PushCadenceWindow> cadence_; // [slipstream-cadence], with latency_ (VEYRA_EXP_RP_STATS=1)
    // Live readout (always): a 1 s window published to pushLiveCadenceBoard() for the interface's counters.
    std::unique_ptr<PushCadenceWindow> liveCadence_;int64_t nextLiveCadence100ns_=0;
    // Cadence VRR state (pushVrrCadenceState): readings issued after this chain's first present
    // (cadence-confirm, vrr-confirm, slipstream-stats); a resize / full-screen change makes it unknown for 3 s.
    PushVrrConfirmSchedule cadenceConfirm_;
    void resetCadenceVrr(){cadenceConfirm_.reset();}
    static double cadenceNowMs(){return double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())/1e6;}
    bool tearingSwitch_=false,vrrRequestSwitch_=false,tearingExperiment_=false;
    // vrrConfirmLog_: the next display state is logged even when unchanged (vrr-confirm with the VRR switch).
    // statsVrrReread_: STATS asks the display worker for a forced VRR re-read on the next presenter loop.
    bool vrrConfirmLog_=false,statsVrrReread_=false;
    // VEYRA_EXP_PRESENT_WINDOW_FACTS=1 (with STATS): the window facts and the forced VRR re-read run only then,
    // because they query the desktop from the presenting thread (review: keep them out of ordinary STATS runs).
    bool windowFactsSwitch_=false;
    PushWindowFacts windowFacts_{};
    int64_t windowCheckAt_=0;
    void watchWindow(int64_t now100ns);
    const char* tearingReason_=nullptr;
    void openSlipstreamExperiments();
    bool providerOwnedPresentation_=false;
    PresentCapture* capture_=nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> presentationQueue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> presentationFence_;
    HANDLE presentationEvent_=nullptr;
    gfx::CommandSlotRing presentationRing_;
    diagnostics::GpuTimer gpuTimer_;
    gfx::PresentSink sink_;
    pipeline::GraphicsPass pass_;
    pipeline::ComputePass presentMotionPass_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> presentMotion_;
    PreviewView previousXessView_{};
    unsigned previousXessWidth_=0,previousXessHeight_=0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvs_;
    // One bounded GPU-only composition surface, shared by sequential lists on
    // the presentation queue. Recreated only after that queue has drained.
    Microsoft::WRL::ComPtr<ID3D12Resource> compositeTarget_;
    Microsoft::WRL::ComPtr<ID3D12Resource> exclusionUiTarget_,exclusionHudlessTarget_;
    bool exclusionUiEnabled_=false;
    bool copyBeforePresent_=false,copyEligible_=false;
    unsigned inc_=0;
    static constexpr unsigned kProbeWidth=64,kProbeHeight=36,kProbeStatsBytes=5*sizeof(uint32_t);
    pipeline::ComputePass probePass_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> probeCpuViews_;
    Microsoft::WRL::ComPtr<ID3D12Resource> probeStats_,probeReadback_;
    uint64_t probeFenceValue_=0;
    bool probePending_=false,probeObserved_=false,probeFailed_=false;
    HdrCurveSettings probeCurve_{};
    pipeline::FrameIdentity probeIdentity_{};
    std::chrono::steady_clock::time_point probeNext_{};
    HdrCurveCoverage liveCoverage_{};
    bool ensureCoverageProbe(ID3D12Device*);
    void publishLiveCoverage(const HdrCurveSettings&,pipeline::FrameIdentity);
    unsigned lastBuffer_=0;bool hasPresented_=false;
    struct RetainedFrame {
        ID3D12Fence* producer=nullptr;
        uint64_t fence=0;
        unsigned slot=0,width=0,height=0;
        bool generated=false,referencesValid=false,baseReference=false,visible=false;
        int comparison=0;float split=.5f;
        PreviewView view{};
        pipeline::FrameIdentity identity{};
        HMONITOR monitor=nullptr;
        HdrCurveSettings displayCurve{};
        bool operator==(const RetainedFrame&)const=default;
    };
    std::optional<RetainedFrame> retainedFrame_;
    HWND window_=nullptr;
    ID3D12Device* presentationDevice_=nullptr;
    DXGI_FORMAT presentationFormat_=DXGI_FORMAT_UNKNOWN;
    bool captureCompatible_=false;
    std::chrono::steady_clock::time_point lastResize_{};
    unsigned viewWidth_=0,viewHeight_=0;
    HMONITOR bufferMonitor_=nullptr;
    unsigned monitorWidth_=0,monitorHeight_=0;
    std::chrono::steady_clock::time_point nextCostLog_{};
    std::chrono::steady_clock::time_point lastXessFrame_{};
    pipeline::FrameIdentity lastXessIdentity_{};
    int64_t lastXessPts100ns_=-1;
    bool xessWasEnabled_=false;
    bool xessFailed_=false;
    bool xessGenerationSuppressed_=false;
    HMONITOR refreshMonitor_=nullptr;double refreshHz_=0;gfx::VrrState vrr_{};bool refreshQueried_=false,vrrSync_=false;
    DisplayStateService* displayStates_=nullptr;
    uint64_t displayStateSerial_=0,displayGeneration_=0;
    // VRR presentation may start or stop after it is requested or when the
    // window enters/leaves fullscreen: one forced query 3 s after that settles.
    std::optional<std::chrono::steady_clock::time_point> vrrConfirmAt_;
    void startDisplayState();
    void applyDisplayState(const DisplayState&);
    uint32_t xessInputId_=0;
    struct XessWork {pipeline::FrameIdentity identity{};uint32_t id=0;};
    std::array<XessWork,4> xessWork_{};
    size_t xessWorkPosition_=0;
    bool refresh(ID3D12Device*);
};
}
