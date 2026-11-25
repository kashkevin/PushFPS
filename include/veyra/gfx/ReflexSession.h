#pragma once
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <intrin.h>
#include "veyra/Log.h"
namespace veyra::gfx {
// Renamed public NVAPI ABI subset: NVIDIA/nvapi
// 87dca625e83fd89a983e19b904e5f3a580da90d2 (MIT), see THIRD_PARTY_NOTICES.
// Owner-thread only. One Sleep per SOURCE input, never per generated Present.
class ReflexSession {
public:
    struct SleepParams {uint32_t version;uint8_t low,boost;uint32_t interval;uint8_t markers,minQueue,reserved[30];};
    struct MarkerParams {uint32_t version;uint64_t frame;uint32_t type;uint64_t reserved0;uint8_t reserved[56];};
    static_assert(sizeof(SleepParams)==44&&sizeof(MarkerParams)==88);
    // NV_GET_SLEEP_STATUS_PARAMS_V1 (nvapi.h, NVIDIA/nvapi main, MIT): what the driver reports about low latency
    // (stream "present" R2, [present-path]; read on a change check, never per frame).
    struct SleepStatusParams {uint32_t version;uint8_t lowLatency,fsVrr,cplVsyncOn;uint32_t sleepIntervalUs;uint8_t useGameSleep,fullscreenIFlip,fgMultiplier,dfgControl;uint32_t dfgTargetUs;uint8_t reserved[114];};
    static_assert(sizeof(SleepStatusParams)==136);
    using GetStatus=int(__cdecl*)(IUnknown*,SleepStatusParams*);
    struct DriverStatus {bool known=false;int result=-1;bool lowLatency=false,fsVrr=false,cplVsyncOn=false,useGameSleep=false,fullscreenIFlip=false;uint32_t sleepIntervalUs=0;};
    DriverStatus driverStatus()const{
        DriverStatus s;
        if(!status_||!device_)return s;
        SleepStatusParams p{};p.version=sizeof(p)|(1u<<16);
        s.result=status_(device_,&p);s.known=s.result==0;
        if(s.known){s.lowLatency=p.lowLatency!=0;s.fsVrr=p.fsVrr!=0;s.cplVsyncOn=p.cplVsyncOn!=0;s.useGameSleep=p.useGameSleep!=0;s.fullscreenIFlip=p.fullscreenIFlip!=0;s.sleepIntervalUs=p.sleepIntervalUs;}
        return s;
    }
    // Host times (steady clock, 100 ns) of the last Sleep that returned (0 before the first).
    int64_t lastSleepStart100ns()const{return lastSleepStart100ns_;}
    int64_t lastSleepEnd100ns()const{return lastSleepEnd100ns_;}
    // Stream "present" R11 (engine/PushFgHealth.h PushReflexOverrunGuard): on a capture card with frame generation the
    // Sleep kept running past the arrival of the sample read after it. Low latency ends (SetSleepMode off, as the stall
    // guard does); not counted as a stall: the interface shows its own text (VideoPresenter::reflexOverrunEnded).
    void endForOverrun(unsigned window,double lastOverrunMs){
        if(!active_)return;
        log::warn("reflex",std::format("sleep overrun: in two consecutive blocks of the last {} measured sleeps at least 4 per block ended more than 0.5 ms after the capture sample had arrived (last {:.3f} ms; up to that much is added to that real frame); low latency off for this source session (VEYRA_TEST_RP_LEGACY_REFLEX_SLEEP=1 keeps it)",
            window,lastOverrunMs));
        fail("sleep-overrun",0);
    }
    using Set=int(__cdecl*)(IUnknown*,SleepParams*);
    using Sleep=int(__cdecl*)(IUnknown*);
    using Marker=int(__cdecl*)(IUnknown*,MarkerParams*);
    using Notify=int(__cdecl*)(ID3D12CommandQueue*,uint32_t);
    // Explicit injection for lifecycle/failed-driver unit tests; production uses System32 NVAPI.
    struct DriverApi {Set set=nullptr;Sleep sleep=nullptr;Marker marker=nullptr;Notify notify=nullptr;};
    ReflexSession()=default;
    explicit ReflexSession(DriverApi api):api_(api),initialized_(true){}
    ReflexSession(const ReflexSession&)=delete;
    ReflexSession& operator=(const ReflexSession&)=delete;
    ~ReflexSession(){close();}
    bool active()const{return active_;}
    bool disablePending()const{return driverEnabled_&&!active_;}
    bool failed()const{return failed_;}
    uint64_t sleepCount()const{return sleeps_;}
    uint64_t generatedPresents()const{return generatedPresents_;}
    void close(){
        if(!disable())log::warn("reflex","device close while driver disable failed; device teardown must clear state");
        device_=nullptr;queue_=nullptr;driverEnabled_=false;
    }
    bool disable(){
        active_=false;
        if(driverEnabled_&&api_.set){
            SleepParams p{};p.version=sizeof(p)|(1u<<16);const int r=api_.set(device_,&p);
            log::info("reflex",std::format("disable status={} successfulSleeps={} sleepMeanMs={:.3f} sleepMaxMs={:.3f} successfulMarkers={} oobMarkers={} realPresents={} generatedPresents={}",
                r,sleeps_,sleeps_?sleepUs_/1000.0/double(sleeps_):0.0,maxSleepUs_/1000.0,markers_,oobMarkers_,realPresents_,generatedPresents_));
            if(r){failed_=true;return false;}driverEnabled_=false;
        }
        return true;
    }
    bool enable(ID3D12Device* device,bool generated=false,ID3D12CommandQueue* queue=nullptr){
        if(active_&&device_==device&&generated_==generated&&queue_==queue)return true;
        if(!disable())return false;
        device_=device;queue_=queue;failed_=false;stalled_=false;stallHistory_=0;queueMarkers_=false;generated_=generated;
        if(!load()||!api_.set||!api_.sleep||!api_.marker){log::warn("reflex","required system-driver low-latency interfaces unavailable");return false;}
        if(generated&&(!api_.notify||!queue)){
            log::warn("reflex","frame generation needs out-of-band notification and an owned presentation queue");return false;
        }
        if(queue&&api_.notify){
            const int r=api_.notify(queue,3); // OUT_OF_BAND_RENDER_PRESENT
            log::info("reflex",std::format("present queue out-of-band status={}",r));
            // Only generated frames need the out-of-band queue. Source Sleep and
            // in-band markers ran without it before frame generation support.
            if(r&&generated)return false;
            queueMarkers_=r==0;
        }
        SleepParams p{};p.version=sizeof(p)|(1u<<16);p.low=1;p.boost=0;p.markers=1; // Boost is deliberately unavailable; keep the NVAPI ABI field zero.
        const int r=api_.set(device_,&p);
        log::info("reflex",std::format("enable status={} intervalUs=0 boost={} frameGeneration={} queueMarkers={}",r,false,generated,queueMarkers_));
        sleeps_=sleepUs_=maxSleepUs_=markers_=oobMarkers_=realPresents_=generatedPresents_=0;firstFrame_=next_+1;
        return active_=driverEnabled_=r==0;
    }
    void mark(uint64_t frame,uint32_t type){
        if(!eligible(frame))return;
        MarkerParams p{};p.version=sizeof(p)|(1u<<16);p.frame=frame;p.type=type;
        const int r=api_.marker(device_,&p);
        if(r)fail("marker",r);else{++markers_;if(frame<firstFrame_+2)log::info("reflex-marker",std::format("source={} type={} status=0",frame,type));}
    }
    // stallUs: the caller already holds the source read until the GPU finished
    // the previous one, so a Sleep longer than this cannot be queue control.
    // Field log 2026-11-12 (RTX 4070 Ti SUPER, 160 Hz, fullscreen DLSS/VFG):
    // the driver slept 26 ms before every 59.94 fps source while the GPU work
    // took about 1.4 ms, and 30 frames a second reached the screen. Ten such
    // sleeps out of the last sixteen end low latency for the session; the
    // caller reports it and keeps the low queue. 0 disables the check.
    static constexpr unsigned kStallWindow=16,kStallSleeps=10;
    uint64_t begin(uint64_t stallUs=0){
        if(!active_)return 0;
        // How long the driver really holds the source read, for the disable log.
        const auto start=std::chrono::steady_clock::now();
        const int r=api_.sleep(device_);if(r){fail("sleep",r);return 0;}
        const auto end=std::chrono::steady_clock::now();
        const auto us=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(end-start).count());
        lastSleepStart100ns_=std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count()/100;
        lastSleepEnd100ns_=std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count()/100;
        ++sleeps_;sleepUs_+=us;maxSleepUs_=std::max(maxSleepUs_,us);
        if(stallUs){
            stallHistory_=((stallHistory_<<1)|uint32_t(us>stallUs))&((1u<<kStallWindow)-1);
            if(unsigned(__popcnt(stallHistory_))>=kStallSleeps){
                log::warn("reflex",std::format("sleep stall: {} of the last {} sleeps exceeded {} us (last {} us, mean {:.3f} ms); low latency disabled for this session",
                    __popcnt(stallHistory_),kStallWindow,stallUs,us,sleepUs_/1000.0/double(sleeps_)));
                stalled_=true;fail("sleep-stall",0);return 0;
            }
        }
        const auto frame=++next_;mark(frame,0);return active_?frame:0;
    }
    bool stalled()const{return stalled_;}
    void renderStart(uint64_t frame){mark(frame,1);mark(frame,2);}
    void renderEnd(uint64_t frame){if(queueMarkers_)mark(frame,3);}
    void blitStart(uint64_t frame){if(eligible(frame))++presentId_;if(queueMarkers_)queueMark(frame,9);}
    void blitEnd(uint64_t frame){if(queueMarkers_)queueMark(frame,10);}
    void presentStart(uint64_t frame,bool generated){
        if(!eligible(frame))return;
        if(generated)queueMark(frame,11);
        else{if(!queueMarkers_)mark(frame,3);mark(frame,4);}
    }
    void presentEnd(uint64_t frame,bool generated){
        if(!eligible(frame))return;
        if(generated){queueMark(frame,12);if(active_)++generatedPresents_;}
        else{mark(frame,5);if(active_)++realPresents_;}
    }
private:
    bool eligible(uint64_t frame)const{return active_&&frame>=firstFrame_&&frame<=next_;}
    void fail(const char* op,int result){log::warn("reflex",std::format("{} status={}; low latency disabled",op,result));failed_=true;disable();}
    void queueMark(uint64_t frame,uint32_t type){
        if(!eligible(frame)||!queueMarkers_)return;
        // Matches NVIDIA Streamline sl.chi/d3d12.cpp::setLatencyMarker:
        // the public IUnknown target is the out-of-band command queue.
        MarkerParams p{};p.version=sizeof(p)|(1u<<16);p.frame=frame;p.type=type;
        const int r=api_.marker(queue_,&p);if(r)fail("queue-marker",r);else{++oobMarkers_;if(frame<firstFrame_+2)log::info("reflex-oob",std::format("source={} present={} type={} status=0",frame,presentId_,type));}
    }
    bool load(){
        if(initialized_)return true;
        // Keep NVAPI initialized/pinned through D3D12 device teardown. Unloading
        // it before the device has caused driver callbacks into unloaded code.
        wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
        const std::wstring path=std::wstring(system)+L"\\nvapi64.dll";
        if(!module_)module_=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE pinned=nullptr;
        if(module_)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(module_),&pinned);
        using Query=void*(__cdecl*)(uint32_t);using Init=int(__cdecl*)();
        auto query=module_?reinterpret_cast<Query>(GetProcAddress(module_,"nvapi_QueryInterface")):nullptr;if(!query)return false;
        auto init=reinterpret_cast<Init>(query(0x0150E828));
        api_={reinterpret_cast<Set>(query(0xac1ca9e0)),reinterpret_cast<Sleep>(query(0x852cd1d2)),reinterpret_cast<Marker>(query(0xd9984c05)),reinterpret_cast<Notify>(query(0x03d6e8cb))};
        status_=reinterpret_cast<GetStatus>(query(0xaef96ca1));   // NvAPI_D3D_GetSleepStatus (nvapi_interface.h); null on an old driver
        if(!init)return false;const int r=init();initialized_=r==0;if(r)log::warn("reflex",std::format("initialize status={}",r));return initialized_;
    }
    DriverApi api_{};GetStatus status_=nullptr;int64_t lastSleepStart100ns_=0,lastSleepEnd100ns_=0;HMODULE module_=nullptr;ID3D12Device* device_=nullptr;ID3D12CommandQueue* queue_=nullptr;
    bool initialized_=false,active_=false,driverEnabled_=false,generated_=false,queueMarkers_=false,failed_=false,stalled_=false;
    uint32_t stallHistory_=0;
    uint64_t next_=0,firstFrame_=1,presentId_=0,sleeps_=0,sleepUs_=0,maxSleepUs_=0,markers_=0,oobMarkers_=0,realPresents_=0,generatedPresents_=0;
};
}
