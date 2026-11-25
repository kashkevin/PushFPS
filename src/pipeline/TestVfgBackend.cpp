// TEST ONLY: see include/veyra/pipeline/TestVfgBackend.h. Nothing here runs unless VEYRA_TEST_FAKE_VFG=1.
#include "veyra/pipeline/TestVfgBackend.h"
#include "veyra/pipeline/FrameBatch.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/pipeline/PushGpuPath.h"
#include "veyra/core/SceneCadenceAnalyzer.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/Log.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace veyra::pipeline {
namespace {
constexpr unsigned kOutputs=2*(FrameBatch::Capacity-1);   // as VfgBackend
constexpr unsigned kShotChangeConstants=12;                 // VfgShotChange.hlsl cbuffer
constexpr UINT64 kShotChangeBytes=256;
constexpr unsigned kBlendConstants=6;                       // TestVfgBlend.hlsl cbuffer
constexpr unsigned kLists=kOutputs+2;                       // command lists of the fake queue, reused in turn
void transition(D3D12_RESOURCE_BARRIER& barrier,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
    barrier={};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};
}
bool hr(HRESULT value,const char* operation){
    if(SUCCEEDED(value))return true;
    log::error("vfg",std::format("TEST-ONLY fake VFG: {} HRESULT=0x{:08X}",operation,unsigned(value)));return false;
}
bool waitFence(ID3D12Fence* fence,uint64_t value,HANDLE event,DWORD ms){
    if(fence->GetCompletedValue()>=value)return true;
    if(FAILED(fence->SetEventOnCompletion(value,event)))return false;
    return WaitForSingleObject(event,ms)==WAIT_OBJECT_0||fence->GetCompletedValue()>=value;
}
}

struct TestVfgBackend::Impl {
    gfx::D3D12DeviceContext* context=nullptr;
    unsigned width=0,height=0,maxMultiplier=2;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t bytes=0;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> inputs;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,kOutputs> outputs;
    // producer: signalled by the graph's queue when this pair's inputs are copied (as VfgBackend's);
    // completion: signalled by the fake queue when the generated frame is written (the graph's queue waits for it).
    Microsoft::WRL::ComPtr<ID3D12Fence> producer,completion,listFence;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>,kLists> allocators;
    std::array<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>,kLists> lists;
    std::array<uint64_t,kLists> listUse{};
    uint64_t listCounter=0,listFenceValue=0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> blendRoot,cutRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> blendPso,cutPso;
    Microsoft::WRL::ComPtr<ID3D12Resource> cutResults;
    D3D12_RESOURCE_STATES cutState=D3D12_RESOURCE_STATE_COMMON;
    bool gpuShotChange=true,ready=false,asynchronous=true,rejectAlone=false,backToBack=false;
    // Artificial cost of a request by its order within the pair (the owner's log, VFG at 4K HDR on his card: the first
    // generated frame about 2.0 ms, the second about 2.2 ms more): VEYRA_TEST_FAKE_VFG_COST_MS="first,second" (or one value
    // for both). An index-2-alone request costs aloneCostMs (VEYRA_TEST_FAKE_VFG_ALONE_COST_MS; default: the first cost;
    // first + second models a VFG that does index 1's work first).
    double costFirstMs=2.0,costLaterMs=2.2,aloneCostMs=-1;
    uint64_t lastJobRevision=0;unsigned orderInPair=0;
    uint64_t signalValue=0,inputRevision=0,pairProducerValue=0,pairProducerRevision=0;
    std::chrono::steady_clock::time_point lastEnd{},nextStatLog{};
    double statProducerMs=0,statCostMs=0;uint64_t statJobs=0;
    struct Job {unsigned parity,sub,multiplier;bool reset;uint64_t value,wait;bool alone;double costMs;std::chrono::steady_clock::time_point queued;};
    std::array<std::atomic<VfgBackend::SubmissionState>,kOutputs> states{};
    std::atomic<bool> failed=false,predictRefused=false;
    std::atomic<uint64_t> runs=0,refusals=0,aloneRuns=0;
    std::mutex jobsMutex;std::condition_variable jobsChanged;std::deque<Job> jobs;
    std::thread worker;bool stopping=false;unsigned outstanding=0;
    HANDLE event=nullptr,timer=nullptr;
    // The artificial cost must be what it says: std::this_thread::sleep_until rounds to the system timer (about 15.6 ms
    // on CI's runner, build 45511031793: fg1 P50 10-18 ms for a 2 ms cost). A high-resolution waitable timer to 0.5 ms
    // before the end, then a yielding spin.
    void waitUntil(std::chrono::steady_clock::time_point end){
        for(;;){
            const auto left=end-std::chrono::steady_clock::now();
            if(left<=std::chrono::steady_clock::duration::zero())return;
            if(timer&&left>std::chrono::microseconds(700)){
                LARGE_INTEGER due;due.QuadPart=-std::max<LONGLONG>(1,LONGLONG(std::chrono::duration_cast<std::chrono::microseconds>(left).count()-500)*10);
                if(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE)){WaitForSingleObject(timer,100);continue;}
            }
            std::this_thread::yield();
        }
    }

    bool rootSignature(unsigned constants,Microsoft::WRL::ComPtr<ID3D12RootSignature>& root){
        D3D12_ROOT_PARAMETER1 parameters[4]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.Num32BitValues=constants;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[1].Descriptor.ShaderRegister=0;
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[2].Descriptor.ShaderRegister=1;
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[3].Descriptor.ShaderRegister=0;
        for(auto& parameter:parameters)parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};desc.Version=D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters=4;desc.Desc_1_1.pParameters=parameters;
        Microsoft::WRL::ComPtr<ID3DBlob> blob,error;
        return hr(D3D12SerializeVersionedRootSignature(&desc,&blob,&error),"serialize root signature")&&
            hr(context->device()->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"CreateRootSignature");
    }
    bool pipeline(const char* shader,ID3D12RootSignature* root,Microsoft::WRL::ComPtr<ID3D12PipelineState>& pso){
        std::vector<uint8_t> cs;if(!loadShaderBytes(shader,cs))return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root;desc.CS={cs.data(),cs.size()};
        return hr(context->device()->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pso)),"CreateComputePipelineState");
    }
    bool buffer(Microsoft::WRL::ComPtr<ID3D12Resource>& resource,UINT64 size,bool uav){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=size;desc.Height=1;
        desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(uav)desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return hr(context->device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&resource)),"Create buffer");
    }
    // As VfgBackend::Impl::recordShotChangeTest: the same shader, constants and states.
    void recordShotChangeTest(ID3D12GraphicsCommandList* list,unsigned parity,bool compare){
        auto* current=inputs[parity].Get();auto* previous=inputs[1-parity].Get();
        std::array<D3D12_RESOURCE_BARRIER,3> barriers{};
        transition(barriers[0],current,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(barriers[1],previous,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(barriers[2],cutResults.Get(),cutState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(UINT(barriers.size()),barriers.data());
        const core::SceneCadenceAnalyzer::Config scene{};
        const std::array<float,6> thresholds={float(scene.sceneCutSadThreshold),float(scene.sceneCutHistogramThreshold),
            float(scene.replacementSadThreshold),float(scene.replacementHistogramThreshold),float(scene.flashSadThreshold),float(scene.flashHistogramThreshold)};
        std::array<uint32_t,kShotChangeConstants> constants={width,height,footprint.Footprint.RowPitch,format==DXGI_FORMAT_R10G10B10A2_UNORM?1u:0u,parity,compare?1u:0u};
        std::memcpy(constants.data()+6,thresholds.data(),sizeof(thresholds));
        list->SetComputeRootSignature(cutRoot.Get());list->SetPipelineState(cutPso.Get());
        list->SetComputeRoot32BitConstants(0,kShotChangeConstants,constants.data(),0);
        list->SetComputeRootShaderResourceView(1,previous->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2,current->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(3,cutResults->GetGPUVirtualAddress());
        list->Dispatch(1,1,1);
        transition(barriers[0],current,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        transition(barriers[1],previous,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        transition(barriers[2],cutResults.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PREDICATION);
        list->ResourceBarrier(UINT(barriers.size()),barriers.data());cutState=D3D12_RESOURCE_STATE_PREDICATION;
    }
    // The fake "Run": the blend on the fake queue, then completion. A refused request writes nothing and only signals.
    bool runJob(const Job& job,bool refuse){
        const unsigned index=unsigned(listCounter%kLists);
        if(listUse[index]&&!waitFence(listFence.Get(),listUse[index],event,5000)){log::error("vfg","TEST-ONLY fake VFG: command list still busy after 5 s");return false;}
        auto* list=lists[index].Get();
        if(!hr(allocators[index]->Reset(),"allocator Reset")||!hr(list->Reset(allocators[index].Get(),nullptr),"list Reset"))return false;
        if(!refuse){
            // Buffers promote from COMMON implicitly and decay back after the list: no explicit barriers (as CUDA's use).
            const std::array<uint32_t,kBlendConstants> constants={width,height,footprint.Footprint.RowPitch,format==DXGI_FORMAT_R10G10B10A2_UNORM?1u:0u,job.sub,job.multiplier};
            auto* a=inputs[job.reset?job.parity:1-job.parity].Get();auto* b=inputs[job.parity].Get();
            list->SetComputeRootSignature(blendRoot.Get());list->SetPipelineState(blendPso.Get());
            list->SetComputeRoot32BitConstants(0,kBlendConstants,constants.data(),0);
            list->SetComputeRootShaderResourceView(1,a->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2,b->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(3,outputs[job.parity+2*(job.sub-1)]->GetGPUVirtualAddress());
            list->Dispatch((width+7)/8,(height+7)/8,1);
        }
        if(!hr(list->Close(),"list Close"))return false;
        ID3D12CommandList* submitted[]={list};queue->ExecuteCommandLists(1,submitted);
        listUse[index]=++listFenceValue;++listCounter;
        return hr(queue->Signal(listFence.Get(),listFenceValue),"signal list fence");
    }
    // One job, in order, like the CUDA stream: starts when its producer value is reached and the previous job ended,
    // then takes costMs, then the blend and the completion signal on the fake queue.
    bool process(const Job& job){
        if(!waitFence(producer.Get(),job.wait,event,5000)){log::error("vfg",std::format("TEST-ONLY fake VFG: producer value {} not reached in 5 s",job.wait));return false;}
        const auto now=std::chrono::steady_clock::now();
        const auto start=std::max(now,lastEnd);
        statProducerMs+=std::chrono::duration<double,std::milli>(now-job.queued).count();
        const auto end=start+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double,std::milli>(job.costMs));
        waitUntil(end);
        lastEnd=end;
        statCostMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();++statJobs;
        if(std::chrono::steady_clock::now()>=nextStatLog&&statJobs){
            // Where the fake's time goes on this machine (CI: WARP): queued -> producer value reached (the graph's own GPU
            // work before it, on the software renderer), and the artificial cost as actually waited.
            log::info("vfg",std::format("TEST-ONLY fake VFG timing jobs={} queuedToProducerMeanMs={:.3f} costWaitedMeanMs={:.3f} (2 s window)",
                statJobs,statProducerMs/double(statJobs),statCostMs/double(statJobs)));
            statJobs=0;statProducerMs=statCostMs=0;nextStatLog=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        }
        const bool refuse=job.alone&&rejectAlone;
        if(refuse)log::warn("vfg","injected rejection of a FrameIndex-2-only request (VEYRA_TEST_VFG_REJECT_ALONE, TEST-ONLY fake VFG)");
        const bool ok=runJob(job,refuse);
        // The completion is always signalled (as the real backend releases a rejected job's wait).
        hr(queue->Signal(completion.Get(),job.value),"signal completion");
        ++runs;if(job.alone)++aloneRuns;
        if(refuse){++refusals;predictRefused.store(true);return false;}
        if(!ok){failed.store(true);completion->Signal(job.value);}
        return ok;
    }
    void work(){
        for(;;){
            Job job{};
            {std::unique_lock lock(jobsMutex);jobsChanged.wait(lock,[&]{return stopping||!jobs.empty();});
                if(jobs.empty()){return;}
                job=jobs.front();jobs.pop_front();}
            bool ok=false;
            try{ok=process(job);}catch(...){
                log::error("vfg","TEST-ONLY fake VFG: worker exception");
                if(!job.alone){failed.store(true);}
                completion->Signal(job.value);
            }
            if(asynchronous)states[job.parity+2*(job.sub-1)].store(ok?VfgBackend::SubmissionState::Succeeded:VfgBackend::SubmissionState::Failed);
            {std::lock_guard lock(jobsMutex);--outstanding;}
        }
    }
    uint64_t producerFor(unsigned sub,bool reset,uint64_t value){
        const uint64_t chained=pushVfgChainedWait(backToBack,sub,reset,pairProducerValue,pairProducerRevision,inputRevision);
        if(chained)return chained;
        if(!hr(context->directQueue()->Signal(producer.Get(),value),"signal producer"))return 0;
        pairProducerValue=sub==1?value:0;pairProducerRevision=inputRevision;
        return value;
    }
    bool handoff(unsigned parity,unsigned sub,unsigned multiplier,bool reset,bool alone){
        const unsigned slot=parity+2*(sub-1);
        std::lock_guard lock(jobsMutex);
        if(stopping||failed.load()||outstanding>=kOutputs){log::error("vfg","TEST-ONLY fake VFG: bounded submission rejected; no GPU wait queued");return false;}
        const uint64_t value=++signalValue;
        const uint64_t waitValue=producerFor(sub,reset,value);
        if(!waitValue)return false;
        orderInPair=inputRevision!=lastJobRevision?0u:orderInPair+1;lastJobRevision=inputRevision;
        const double cost=alone?(aloneCostMs>=0?aloneCostMs:costFirstMs):orderInPair==0?costFirstMs:costLaterMs;
        jobs.push_back({parity,sub,multiplier,reset,value,waitValue,alone,cost,std::chrono::steady_clock::now()});
        if(!hr(context->directQueue()->Wait(completion.Get(),value),"queue wait completion")){jobs.pop_back();return false;}
        // The real synchronous path knows the verdict when generate() returns; the fake's refusal is decided by the switch.
        states[slot].store(asynchronous?VfgBackend::SubmissionState::Pending:
            (alone&&rejectAlone?VfgBackend::SubmissionState::Failed:VfgBackend::SubmissionState::Succeeded));
        if(!asynchronous&&alone&&rejectAlone)predictRefused.store(true);
        ++outstanding;jobsChanged.notify_one();return true;
    }
    void stop(){
        {std::lock_guard lock(jobsMutex);stopping=true;}
        jobsChanged.notify_one();if(worker.joinable())worker.join();
    }
};

bool TestVfgBackend::requested(){
    wchar_t value[4]{};
    return GetEnvironmentVariableW(L"VEYRA_TEST_FAKE_VFG",value,4)==1&&value[0]==L'1';
}
TestVfgBackend::TestVfgBackend():impl_(std::make_unique<Impl>()){}
TestVfgBackend::~TestVfgBackend(){shutdown();}

bool TestVfgBackend::initialize(gfx::D3D12DeviceContext& context,unsigned width,unsigned height,DXGI_FORMAT format,unsigned multiplier,unsigned quality,bool asynchronousSubmission){
    shutdown();
    auto& p=*impl_;
    if(!width||!height||multiplier<2||multiplier>FrameBatch::Capacity||(format!=DXGI_FORMAT_R8G8B8A8_UNORM&&format!=DXGI_FORMAT_R10G10B10A2_UNORM)){
        log::error("vfg","TEST-ONLY fake VFG: unsupported dimensions/multiplier/encoding");return false;
    }
    p.context=&context;p.width=width;p.height=height;p.format=format;p.maxMultiplier=multiplier;p.asynchronous=asynchronousSubmission;
    p.gpuShotChange=GetEnvironmentVariableW(L"VEYRA_TEST_VFG_AUTO_SHOT_CHANGE",nullptr,0)==0;
    p.rejectAlone=GetEnvironmentVariableW(L"VEYRA_TEST_VFG_REJECT_ALONE",nullptr,0)>0;
    {wchar_t chain[4]{};p.backToBack=GetEnvironmentVariableW(L"VEYRA_EXP_RP_VFG_BACK_TO_BACK",chain,4)==1&&chain[0]==L'1';}
    {wchar_t cost[32]{};const DWORD n=GetEnvironmentVariableW(L"VEYRA_TEST_FAKE_VFG_COST_MS",cost,32);
     if(n&&n<32){const std::wstring text(cost,n);const auto comma=text.find(L',');
        p.costFirstMs=std::clamp(_wtof(text.substr(0,comma).c_str()),0.0,40.0);
        p.costLaterMs=comma==std::wstring::npos?p.costFirstMs:std::clamp(_wtof(text.substr(comma+1).c_str()),0.0,40.0);}}
    {wchar_t cost[16]{};const DWORD n=GetEnvironmentVariableW(L"VEYRA_TEST_FAKE_VFG_ALONE_COST_MS",cost,16);if(n&&n<16)p.aloneCostMs=std::clamp(_wtof(cost),0.0,40.0);}
    p.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!p.event)return false;
    p.timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);   // null: the spin alone
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=width;texture.Height=height;texture.DepthOrArraySize=1;texture.MipLevels=1;texture.Format=format;texture.SampleDesc.Count=1;
    UINT rows=0;uint64_t rowBytes=0,total=0;context.device()->GetCopyableFootprints(&texture,0,1,0,&p.footprint,&rows,&rowBytes,&total);
    p.bytes=uint64_t(p.footprint.Footprint.RowPitch)*height;
    for(auto& input:p.inputs)if(!p.buffer(input,p.bytes,false))return false;
    for(unsigned i=0;i<2*(multiplier-1);++i)if(!p.buffer(p.outputs[i],p.bytes,true))return false;
    auto* device=context.device();
    D3D12_COMMAND_QUEUE_DESC queueDesc{};queueDesc.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if(!hr(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&p.queue)),"CreateCommandQueue")||
       !hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&p.producer)),"CreateFence producer")||
       !hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&p.completion)),"CreateFence completion")||
       !hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&p.listFence)),"CreateFence lists"))return false;
    p.queue->SetName(L"TEST-ONLY fake VFG queue");
    for(unsigned i=0;i<kLists;++i){
        if(!hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,IID_PPV_ARGS(&p.allocators[i])),"CreateCommandAllocator")||
           !hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COMPUTE,p.allocators[i].Get(),nullptr,IID_PPV_ARGS(&p.lists[i])),"CreateCommandList")||
           !hr(p.lists[i]->Close(),"initial Close"))return false;
    }
    if(!p.rootSignature(kBlendConstants,p.blendRoot)||!p.pipeline("TestVfgBlend.dxil",p.blendRoot.Get(),p.blendPso))return false;
    if(p.gpuShotChange){
        if(!p.rootSignature(kShotChangeConstants,p.cutRoot)||!p.pipeline("VfgShotChange.dxil",p.cutRoot.Get(),p.cutPso)||
           !p.buffer(p.cutResults,kShotChangeBytes,true))return false;
        p.cutState=D3D12_RESOURCE_STATE_COMMON;
    }
    try{p.worker=std::thread([&p]{p.work();});}
    catch(...){log::error("vfg","TEST-ONLY fake VFG: worker creation failed");return false;}
    p.ready=true;
    log::warn("vfg",std::format("TEST-ONLY fake VFG (VEYRA_TEST_FAKE_VFG=1): no NVIDIA model, no CUDA; generated frames are linear blends of A and B "
        "after costMs={:.2f}/{:.2f} (first/later request of a pair; alone {:.2f}) on its own D3D12 queue; size={}x{} format={} multiplier={} quality={} shotChange={} submission={} rejectAlone={} backToBack={}",
        p.costFirstMs,p.costLaterMs,p.aloneCostMs>=0?p.aloneCostMs:p.costFirstMs,width,height,unsigned(format),multiplier,quality,p.gpuShotChange?"gpu-pair-test":"none",p.asynchronous?"bounded worker":"graph owner",p.rejectAlone,p.backToBack));
    return true;
}
bool TestVfgBackend::created()const{return impl_->ready;}
bool TestVfgBackend::capture(ID3D12GraphicsCommandList* list,ID3D12Resource* texture,unsigned parity,bool comparePrevious){
    auto& p=*impl_;if(!p.ready||p.failed.load()||!list||!texture||parity>1)return false;
    const auto desc=texture->GetDesc();if(desc.Width!=p.width||desc.Height!=p.height||desc.Format!=p.format)return false;
    auto* buffer=p.inputs[parity].Get();
    D3D12_RESOURCE_BARRIER barrier{};transition(barrier,buffer,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=texture;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=buffer;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=p.footprint;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    if(p.gpuShotChange)p.recordShotChangeTest(list,parity,comparePrevious);
    else{std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);}
    ++p.inputRevision;return true;
}
bool TestVfgBackend::generate(unsigned parity,unsigned sub,unsigned multiplier,bool reset,bool fast,bool alone){
    (void)fast;   // no second model: fastReady() is false, the graph never asks for it
    auto& p=*impl_;if(!p.ready||p.failed.load()||parity>1||multiplier<2||multiplier>p.maxMultiplier||sub<1||sub>=multiplier)return false;
    return p.handoff(parity,sub,multiplier,reset,alone);
}
bool TestVfgBackend::takePredictRefused(){return impl_->predictRefused.exchange(false);}
VfgBackend::SubmissionState TestVfgBackend::submissionState(unsigned slot)const{
    return slot<kOutputs?impl_->states[slot].load():VfgBackend::SubmissionState::Failed;
}
// Identical to VfgBackend::copyOutput (the same predicated choice on the shot-change result).
bool TestVfgBackend::copyOutput(ID3D12GraphicsCommandList* list,ID3D12Resource* texture,unsigned slot){
    auto& p=*impl_;if(!p.ready||!list||!texture||slot>=2*(p.maxMultiplier-1))return false;
    const auto desc=texture->GetDesc();if(desc.Width!=p.width||desc.Height!=p.height||desc.Format!=p.format)return false;
    auto* buffer=p.outputs[slot].Get();
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=buffer;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=p.footprint;
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=texture;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    if(!p.gpuShotChange){
        D3D12_RESOURCE_BARRIER barrier{};transition(barrier,buffer,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1,&barrier);
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);return true;
    }
    auto* current=p.inputs[slot%2].Get();
    std::array<D3D12_RESOURCE_BARRIER,3> barriers{};UINT count=2;
    transition(barriers[0],buffer,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(barriers[1],current,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    if(p.cutState!=D3D12_RESOURCE_STATE_PREDICATION){
        transition(barriers[2],p.cutResults.Get(),p.cutState,D3D12_RESOURCE_STATE_PREDICATION);count=3;p.cutState=D3D12_RESOURCE_STATE_PREDICATION;
    }
    list->ResourceBarrier(count,barriers.data());
    const UINT64 flag=(slot%2)*8;
    list->SetPredication(p.cutResults.Get(),flag,D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    src.pResource=current;
    list->SetPredication(p.cutResults.Get(),flag,D3D12_PREDICATION_OP_EQUAL_ZERO);
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    list->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
    transition(barriers[0],buffer,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    transition(barriers[1],current,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(2,barriers.data());return true;
}
// Identical to VfgBackend::copyInput.
bool TestVfgBackend::copyInput(ID3D12GraphicsCommandList* list,ID3D12Resource* texture,unsigned parity,bool onlyOnCut){
    auto& p=*impl_;if(!p.ready||!list||!texture||parity>=2)return false;
    const auto desc=texture->GetDesc();if(desc.Width!=p.width||desc.Height!=p.height||desc.Format!=p.format)return false;
    if(onlyOnCut&&!p.gpuShotChange)return true;
    auto* current=p.inputs[parity].Get();
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=current;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=p.footprint;
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=texture;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    if(onlyOnCut&&p.cutState!=D3D12_RESOURCE_STATE_PREDICATION){
        D3D12_RESOURCE_BARRIER barrier{};transition(barrier,p.cutResults.Get(),p.cutState,D3D12_RESOURCE_STATE_PREDICATION);
        list->ResourceBarrier(1,&barrier);p.cutState=D3D12_RESOURCE_STATE_PREDICATION;
    }
    if(onlyOnCut)list->SetPredication(p.cutResults.Get(),UINT64(parity)*8,D3D12_PREDICATION_OP_EQUAL_ZERO);
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    if(onlyOnCut)list->SetPredication(nullptr,0,D3D12_PREDICATION_OP_EQUAL_ZERO);
    return true;
}
ID3D12Resource* TestVfgBackend::shotChangeResults()const{return impl_->gpuShotChange?impl_->cutResults.Get():nullptr;}
ID3D12Resource* TestVfgBackend::inputBuffer(unsigned parity)const{return impl_->ready&&parity<2?impl_->inputs[parity].Get():nullptr;}
uint32_t TestVfgBackend::inputRowPitch()const{return impl_->ready?impl_->footprint.Footprint.RowPitch:0;}
void TestVfgBackend::shutdown(){
    auto& p=*impl_;
    const bool wasReady=p.ready;p.ready=false;p.stop();
    // Teardown only: let the fake queue finish before its buffers go (the graph drains its own queue first).
    if(p.queue&&p.listFence){
        const uint64_t last=++p.listFenceValue;
        if(SUCCEEDED(p.queue->Signal(p.listFence.Get(),last))&&p.event)waitFence(p.listFence.Get(),last,p.event,5000);
    }
    if(wasReady)log::info("vfg",std::format("TEST-ONLY fake VFG closed: runs={} aloneRuns={} refusedAlone={}",p.runs.load(),p.aloneRuns.load(),p.refusals.load()));
    if(p.event)CloseHandle(p.event);
    if(p.timer)CloseHandle(p.timer);
    impl_=std::make_unique<Impl>();
}
}
