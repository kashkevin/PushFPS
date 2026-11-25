// End-to-end engine test of "Lower delay" (Package 15) on CI's Windows machine (software renderer, no NVIDIA runtime).
// docs/drafts/E2E_ENGINE_TEST.md says what it runs, what it proves and what it cannot prove.
//
// It drives the REAL EngineController, EnhanceGraph, presenter and live scheduler: a lossless clip replayed AS a capture
// card (VEYRA_TEST_REPLAY_AS_CARD=1, paced on its PTS at 59.94 Hz, every game picture twice = a 30 FPS game), frame
// generation with frameGenerationBackend == Vfg at 3X, where VFG is the test-only stand-in (VEYRA_TEST_FAKE_VFG=1,
// TestVfgBackend.h). One configuration per process: the engine reads several switches once per process.
//
//   veyra_lowdelay_e2e_tests --make-clip <file.y4m> <width> <height> <seconds>
//   veyra_lowdelay_e2e_tests --run <clip> <outdir> <seconds> [--low-delay] [--multiplier N] [--hud-lock] [--exclusion]
//                            [--smoothness] [--adaptive] [--sync tearing|vsync|auto] [--toggle-at S]
//
// --run writes <outdir>/engine.log (the app's own log, verbose frame lines on) and <outdir>/states.csv (every 50 ms: the
// engine snapshot and the "Lower delay" row state the interface reads, pushLowDelayShared()). The assertions are made
// from those files by tests/integration/lowdelay_e2e.py, which also runs the configurations.
#include "veyra/engine/EngineController.h"
#include "veyra/engine/PushLowDelay.h"
#include "veyra/engine/PushSmoothness.h"
#include "veyra/pipeline/PushPredict.h"
#include "veyra/Log.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
// A 4:2:0 YUV4MPEG2 clip. repeated: 60000/1001 frames a second with every picture written twice (a 30 FPS game inside a
// 59.94 Hz capture, bit-identical repeats). Otherwise 30.00 frames a second, every picture new: the owner's card as his
// log shows it (3840x2160 at 30.00 Hz, dedup=false). Picture k: a vertical bar moving
// 6 pixels per picture over a horizontal gradient, and k in binary as 16 blocks along the top (every picture distinct).
int makeClip(const std::filesystem::path& path,unsigned width,unsigned height,unsigned seconds,bool repeated){
    if(width<64||height<64||width%2||height%2||width>3840||height>2160||seconds<1||seconds>120)return 2;
    std::ofstream out(path,std::ios::binary);if(!out)return 2;
    out<<"YUV4MPEG2 W"<<width<<" H"<<height<<(repeated?" F60000:1001":" F30:1")<<" Ip A1:1 C420jpeg\n";
    std::vector<uint8_t> y(size_t(width)*height),uv(size_t(width/2)*(height/2),128);
    const unsigned pictures=seconds*30;const unsigned block=std::max(4u,width/32);
    for(unsigned k=0;k<pictures;++k){
        const unsigned bar=(k*6)%width;
        for(unsigned row=0;row<height;++row)for(unsigned x=0;x<width;++x){
            uint8_t v=uint8_t(32+x*160/width);
            if(x>=bar&&x<bar+std::max(8u,width/40))v=235;
            if(row<block&&x<16*block){const unsigned bit=x/block;v=uint8_t(((k>>bit)&1u)?200u:16u);}
            y[size_t(row)*width+x]=v;
        }
        for(unsigned repeat=0;repeat<(repeated?2u:1u);++repeat){
            out<<"FRAME\n";
            out.write(reinterpret_cast<const char*>(y.data()),std::streamsize(y.size()));
            out.write(reinterpret_cast<const char*>(uv.data()),std::streamsize(uv.size()));
            out.write(reinterpret_cast<const char*>(uv.data()),std::streamsize(uv.size()));
        }
    }
    return out?0:2;
}
std::string narrow(const std::wstring& w){
    std::string s(w.size()*4+1,'\0');
    const int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,s.data(),int(s.size()),nullptr,nullptr);
    s.resize(n>0?size_t(n-1):0);return s;
}
}

int wmain(int argc,wchar_t** argv){
    using namespace veyra::engine;using Clock=std::chrono::steady_clock;
    // --make-clip <file> <width> <height> <seconds> [30|59.94]
    if((argc==6||argc==7)&&std::wstring(argv[1])==L"--make-clip")
        return makeClip(argv[2],unsigned(_wtoi(argv[3])),unsigned(_wtoi(argv[4])),unsigned(_wtoi(argv[5])),argc==6||std::wstring(argv[6])!=L"30");
    if(argc<5||std::wstring(argv[1])!=L"--run"){std::cout<<"usage: see the file header"<<std::endl;return 2;}
    const std::wstring clip=argv[2];const std::filesystem::path dir=argv[3];const int seconds=std::clamp(_wtoi(argv[4]),3,120);
    bool lowDelay=false,hudLock=false,exclusion=false,adaptive=false,nvLowLatency=false,smoothPacing=false;
    unsigned multiplier=3,quality=0;int smoothTarget=0;bool contentAuto=false;
    DisplaySync sync=DisplaySync::Tearing;double syncAutoAt=-1;std::vector<double> toggles;
    for(int i=5;i<argc;++i){
        const std::wstring a=argv[i];
        if(a==L"--low-delay")lowDelay=true;
        else if(a==L"--hud-lock")hudLock=true;
        else if(a==L"--exclusion")exclusion=true;
        else if(a==L"--smoothness")smoothTarget=90;
        else if(a==L"--smooth-target"&&i+1<argc)smoothTarget=_wtoi(argv[++i]);
        else if(a==L"--adaptive")adaptive=true;
        else if(a==L"--content-auto")contentAuto=true;
        else if(a==L"--nv-low-latency")nvLowLatency=true;
        else if(a==L"--smooth-pacing")smoothPacing=true;
        else if(a==L"--multiplier"&&i+1<argc)multiplier=unsigned(std::clamp(_wtoi(argv[++i]),2,4));
        else if(a==L"--quality"&&i+1<argc)quality=unsigned(std::clamp(_wtoi(argv[++i]),0,5));
        else if(a==L"--toggle-at"&&i+1<argc)toggles.push_back(_wtof(argv[++i]));
        else if(a==L"--sync-auto-at"&&i+1<argc)syncAutoAt=_wtof(argv[++i]);
        else if(a==L"--sync"&&i+1<argc){const std::wstring v=argv[++i];sync=v==L"vsync"?DisplaySync::Vsync:v==L"auto"?DisplaySync::Automatic:DisplaySync::Tearing;}
        else{std::cout<<"unknown argument"<<std::endl;return 2;}
    }
    std::sort(toggles.begin(),toggles.end());
    // The capture-card replay and the fake VFG are the caller's (environment); refuse to run without them, so a run can
    // never silently test something else (a file's own pacing, or no frame generation at all).
    if(GetEnvironmentVariableW(L"VEYRA_TEST_REPLAY_AS_CARD",nullptr,0)==0||GetEnvironmentVariableW(L"VEYRA_TEST_FAKE_VFG",nullptr,0)==0){
        std::cout<<"FAIL VEYRA_TEST_REPLAY_AS_CARD and VEYRA_TEST_FAKE_VFG must be set"<<std::endl;return 2;
    }
    std::filesystem::create_directories(dir);
    SetEnvironmentVariableW(L"VEYRA_VERBOSE_FRAME_LOGS",L"1");
    veyra::Logger::instance().openFile((dir/L"engine.log").wstring());veyra::Logger::instance().setConsoleEnabled(false);
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 2;
    // Windowed, as the owner plays.
    HWND window=CreateWindowExW(0,L"STATIC",L"Lower delay end-to-end test",WS_OVERLAPPEDWINDOW|WS_VISIBLE,40,40,1280,760,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window)return 2;
    int failures=0;auto check=[&](bool value,const char* name){std::cout<<(value?"PASS ":"FAIL ")<<name<<std::endl;failures+=!value;};
    {
        // As the interface does at start (QmlPlayerBridge: pref pushLowDelay -> the engine's flag and EnhancementSettings::lowDelay).
        pushLowDelayShared().wanted.store(lowDelay,std::memory_order_relaxed);
        pushLowDelayShared().vfgRefused.store(false,std::memory_order_relaxed);
        pushLowDelayShared().state.store(uint8_t(lowDelay?PushLowDelayState::Starting:PushLowDelayState::Idle),std::memory_order_relaxed);
        // Smoothness Mode as the interface sets it (pref pushSmoothTarget -> pushSmoothShared().targetFps; the owner: 90).
        pushSmoothShared().targetFps.store(smoothTarget,std::memory_order_relaxed);
        EngineController engine;engine.setVolume(0,true);
        // Low queue always (the owner's lowQueue=1); NVIDIA low latency requested (Reflex does not exist on WARP: the engine
        // reports it unavailable); smooth pacing = Vsync + variable refresh + output cap following the display, as
        // PushQuickSetup.qml setSmoothPacing does; Automatic = the owner after he switched smooth pacing off.
        PresentationSettings p;p.enabled=true;p.mode=PacingMode::LowQueue;p.display=sync;
        if(nvLowLatency)p.nvidiaLowLatency=NvidiaLowLatency::On;
        if(smoothPacing){p.display=DisplaySync::Vsync;p.variableRefresh=true;p.outputRate=OutputRateMode::FollowDisplay;}
        engine.requestPresentation(p);
        EnhancementSettings effects;effects.multiplier=multiplier;effects.frameGenerationBackend=FrameGenerationBackend::Vfg;effects.vfgQuality=quality;
        effects.lowDelay=lowDelay;effects.hudLock=hudLock;effects.fgAdaptive=adaptive;
        if(contentAuto)effects.content=ContentRate::Auto;   // content-rate detection: repeated pictures removed (59.94 Hz with repeats)
        if(exclusion){effects.exclusion.enabled=true;effects.exclusion.regions[0]={0.8f,0.05f,0.95f,0.15f,false};}
        auto options=PlayerOptions::from(effects);options.captureReplayForTest=true;
        engine.open(window,clip,options);
        auto pump=[]{MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}};
        auto wait=[&](auto predicate,int ms){const auto end=Clock::now()+std::chrono::milliseconds(ms);
            while(Clock::now()<end){pump();auto s=engine.snapshot();if(s.failed)return false;if(predicate(s))return true;std::this_thread::sleep_for(std::chrono::milliseconds(2));}return false;};
        check(wait([](auto& s){return s.frames>10;},60000),"first frames");
        std::ofstream states(dir/L"states.csv");
        states<<"wall,frames,generated,fgActive,previewFgMultiplier,softwareLatencyMs,softwareLatencyP95Ms,realPresentFps,generatedPresentFps,captureAgeMs,schedulingWaitP95Ms,previewSkipped,rowWanted,rowState,rowRefusal,rowSuspend,vfgRefused,failed,presentationEnabled,display,smoothTarget,smoothChosen,sourceHdr\n";
        const auto start=Clock::now();size_t nextToggle=0;bool rowOn=lowDelay,syncSwitched=false;auto last=engine.snapshot();const auto first=last;
        while(Clock::now()-start<std::chrono::seconds(seconds)&&!last.failed){
            pump();last=engine.snapshot();
            const double wall=std::chrono::duration<double>(Clock::now()-start).count();
            if(nextToggle<toggles.size()&&wall>=toggles[nextToggle]){
                // The row switched while playing, as applyPushLowDelay does: the flag, a fresh attempt, the graph shape.
                ++nextToggle;rowOn=!rowOn;
                pushLowDelayShared().wanted.store(rowOn,std::memory_order_relaxed);pushLowDelayShared().vfgRefused.store(false,std::memory_order_relaxed);
                pushLowDelayShared().state.store(uint8_t(rowOn?PushLowDelayState::Starting:PushLowDelayState::Idle),std::memory_order_relaxed);
                effects.lowDelay=rowOn;check(engine.requestSettings(effects),"row switch accepted by the engine");
                std::cout<<"toggleAt="<<wall<<" lowDelay="<<rowOn<<std::endl;
                veyra::log::info("e2e-host",std::format("toggle wall={:.3f} lowDelay={}",wall,rowOn));
            }
            if(syncAutoAt>=0&&!syncSwitched&&wall>=syncAutoAt){
                // The owner switched smooth frame pacing off while playing: display sync Automatic, no VRR request, no cap.
                syncSwitched=true;p.display=DisplaySync::Automatic;p.variableRefresh=false;p.outputRate=OutputRateMode::Off;engine.requestPresentation(p);
                veyra::log::info("e2e-host",std::format("smooth pacing off wall={:.3f}",wall));
            }
            const auto& shared=pushLowDelayShared();
            states<<wall<<','<<last.frames<<','<<last.generated<<','<<last.fgActive<<','<<last.previewFgMultiplier<<','<<last.metrics.flow.softwareLatencyMs.value_or(-1.0)<<','<<last.metrics.flow.softwareLatencyP95Ms.value_or(-1.0)<<','<<last.metrics.flow.realPresentFps<<','<<last.metrics.flow.generatedPresentFps<<','
                  <<last.captureAgeMs<<','<<last.schedulingWaitP95Ms<<','<<last.previewSkipped<<','<<shared.wanted.load()<<','
                  <<pushLowDelayStateName(PushLowDelayState(shared.state.load()))<<','<<veyra::pipeline::pushPredictRefusalName(veyra::pipeline::PushPredictRefusal(shared.refusal.load()))<<','
                  <<pushLowDelaySuspendName(PushLowDelaySuspend(shared.suspend.load()))<<','<<shared.vfgRefused.load()<<','<<last.failed<<','
                  <<last.presentationEffective.enabled<<','<<unsigned(last.presentationEffective.display)<<','<<pushSmoothShared().targetFps.load()<<','<<pushSmoothShared().chosen.load()<<','<<last.sourceHdr<<'\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::cout<<"processedDelta="<<last.frames-first.frames<<" generatedDelta="<<last.generated-first.generated<<" multiplier="<<last.previewFgMultiplier
                 <<" presentationStatus="<<narrow(last.presentationStatus)<<std::endl;
        check(!last.failed,"engine did not fail");
        check(last.frames>first.frames+10,"real frames kept coming");
    }
    DestroyWindow(window);
    std::cout<<(failures?"FAILED ":"OK ")<<failures<<std::endl;
    return failures?1:0;
}
