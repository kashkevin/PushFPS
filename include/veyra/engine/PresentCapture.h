#pragma once
// Live recording (LiveRecorder) reads the presented frame inside the
// presentation command list, after the presenter's own reads. The consumer
// fence the graph waits on before reusing the texture then covers the
// recording too, and no second queue ever touches the graph's textures.
// Engine thread only. Kept free of the presenter's headers: EngineController.h
// is included after Qt, whose `slots` macro breaks diagnostics/GpuTimer.h.
#include <d3d12.h>
#include <cstdint>

namespace veyra::engine {

class PresentCapture {
public:
    struct Frame {
        ID3D12Resource* texture=nullptr; // graph output; COMMON on entry, must be left in COMMON
        DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
        uint32_t width=0,height=0;
        uint32_t hdrMode=0;              // 0 SDR, 1 scRGB (linear BT.709), 2 HDR10 (PQ BT.2020)
        float ditherStep=0;              // graph output dither step (RgbToNv12 reserved.y)
        bool generated=false;
        // PushFPS comparison clips: the raw console picture of a real frame (the graph's
        // untouched linear BT.709 copy of the input, float RGBA at the source size; COMMON on
        // entry, must be left in COMMON), null when not kept. rawSdrInHdr: an SDR source shown
        // in HDR (RTX Video HDR), as the presenter's comparison view scales it.
        ID3D12Resource* raw=nullptr;
        bool rawSdrInHdr=false;
        // PushFPS quality test (PushQualityProbe.h): the frame's presentation timestamp (100 ns, the
        // source clock; generated frames carry their in-between time), 0 when unknown.
        int64_t pts100ns=0;
        // Stream "clarity": the swap chain's back buffer after the presenter's blit, before Present (PRESENT = COMMON
        // on entry, must be left in COMMON); what the window shows of this frame. Null when not available.
        ID3D12Resource* screen=nullptr;
        // How the presenter scaled this frame (EnhanceGraph::highQualityPresentation): 1 FineSample, 0 one bilinear tap, -1 unknown.
        int presentFine=-1;
    };
    // Whether the next frames should carry `raw` (the graph keeps it only when asked).
    virtual bool wantsRawFrames() const { return false; }
    // Stream "present" R3 (review S9): whether this consumer may record the next presented frames. The blit-ahead
    // experiment is refused while any consumer says yes, so recordings, streams and probes see exactly what they see with it
    // off. Conservative default: yes.
    virtual bool capturing() const { return true; }
    virtual ~PresentCapture()=default;
    // true: commands were recorded into `list`; submitted() or discarded() follows.
    virtual bool record(ID3D12GraphicsCommandList* list,const Frame& frame)=0;
    // The list was submitted; this fence point covers the recorded commands.
    virtual void submitted(ID3D12Fence* fence,uint64_t value)=0;
    // The list was not submitted (failed present); nothing will execute.
    virtual void discarded()=0;
};

// The presenter has one capture; a recording and the instant replay buffer
// both take the presented frame through this.
class PresentCaptureFanout final : public PresentCapture {
public:
    PresentCaptureFanout(PresentCapture& first,PresentCapture& second):first_(first),second_(second){}
    bool record(ID3D12GraphicsCommandList* list,const Frame& frame) override {
        const bool a=first_.record(list,frame);
        const bool b=second_.record(list,frame);
        return a||b;
    }
    // Each one acts only on what it recorded itself.
    void submitted(ID3D12Fence* fence,uint64_t value) override {first_.submitted(fence,value);second_.submitted(fence,value);}
    void discarded() override {first_.discarded();second_.discarded();}
    bool wantsRawFrames() const override {return first_.wantsRawFrames()||second_.wantsRawFrames();}
    bool capturing() const override {return first_.capturing()||second_.capturing();}
private:
    PresentCapture& first_;
    PresentCapture& second_;
};

} // namespace veyra::engine
