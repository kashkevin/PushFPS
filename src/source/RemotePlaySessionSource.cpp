#include "veyra/source/RemotePlaySessionSource.h"
#include "veyra/Log.h"
#include "veyra/remoteplay/Discovery.h"
#include "veyra/remoteplay/ReorderWindow.h"
#include "veyra/remoteplay/StreamRecovery.h"
#include "veyra/remoteplay/TransportTuning.h"
#include <timeapi.h>
#include <cmath>
#include <format>
#pragma comment(lib,"winmm.lib")
extern "C" {
#include <libavutil/frame.h>
}
namespace veyra::source {
namespace {
// A/B switches for the latency work of 2026-11-18: any value restores the
// original timing of that part, VEYRA_TEST_RP_LEGACY_TIMING of all of them.
bool legacyTiming(const wchar_t* part){
    return GetEnvironmentVariableW(L"VEYRA_TEST_RP_LEGACY_TIMING",nullptr,0)>0||GetEnvironmentVariableW(part,nullptr,0)>0;
}
// A VEYRA_EXP_RP_* value as ASCII for remoteplay::resolveLanExperiments; no
// value when the variable is unset or empty. One too long to be a number
// reads as "?": set, but not a number.
std::optional<std::string> experimentSwitch(const wchar_t* name){
    wchar_t value[32]{};
    const DWORD length=GetEnvironmentVariableW(name,value,32);
    if(length==0)return std::nullopt;
    if(length>=32)return std::string("?");
    std::string text;
    for(DWORD i=0;i<length;++i)text.push_back(value[i]>0&&value[i]<128?static_cast<char>(value[i]):'?');
    return text;
}
remoteplay::LanExperiments lanExperiments(){
    remoteplay::TransportSwitches s;
    s.lanProfile=experimentSwitch(L"VEYRA_EXP_RP_LAN_PROFILE");
    s.reorderUs=experimentSwitch(L"VEYRA_EXP_RP_REORDER_US");
    s.receiveBuffer=experimentSwitch(L"VEYRA_EXP_RP_RCVBUF");
    s.threadPriority=experimentSwitch(L"VEYRA_EXP_RP_THREAD_PRIORITY");
    s.timer1ms=experimentSwitch(L"VEYRA_EXP_RP_TIMER_1MS");
    s.backlog=experimentSwitch(L"VEYRA_EXP_RP_BACKLOG");
    s.idrRetryMs=experimentSwitch(L"VEYRA_EXP_RP_IDR_RETRY_MS");
    s.health=experimentSwitch(L"VEYRA_EXP_RP_HEALTH");
    s.reorderAdaptive=experimentSwitch(L"VEYRA_EXP_RP_REORDER_ADAPTIVE");
    s.reconnectPatient=experimentSwitch(L"VEYRA_EXP_RP_RECONNECT_PATIENT");
    return remoteplay::resolveLanExperiments(s);
}
// Two independent experiments on the owner thread, each for as long as the
// owner loop runs: VEYRA_EXP_RP_THREAD_PRIORITY (the owner's half: one step
// above normal) and VEYRA_EXP_RP_TIMER_1MS (1 ms system timer resolution).
// Made and destroyed on the owner thread; each is undone on every way out.
class OwnerBoost {
public:
    OwnerBoost(bool priority,bool timer){
        if(timer)timer_=timeBeginPeriod(1)==TIMERR_NOERROR;
        if(!priority)return;
        previous_=GetThreadPriority(GetCurrentThread());
        raised_=previous_!=THREAD_PRIORITY_ERROR_RETURN&&SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL)!=0;
    }
    ~OwnerBoost(){
        if(raised_)SetThreadPriority(GetCurrentThread(),previous_);
        if(timer_)timeEndPeriod(1);
    }
    OwnerBoost(const OwnerBoost&)=delete;
    OwnerBoost& operator=(const OwnerBoost&)=delete;
    bool timer()const noexcept{return timer_;}
    bool raised()const noexcept{return raised_;}
private:
    bool timer_=false,raised_=false;
    int previous_=THREAD_PRIORITY_NORMAL;
};
}
RemotePlaySessionSource::RemotePlaySessionSource()
    :controllers_(legacyTiming(L"VEYRA_TEST_RP_LEGACY_INPUT")?remoteplay::ControllerQueue::Policy::Tick:remoteplay::ControllerQueue::Policy::Immediate),
     wake_(std::make_shared<remoteplay::WakeSignal>()),
     frameEvent_(CreateEventW(nullptr,FALSE,FALSE,nullptr)),
     ownerWake_(!legacyTiming(L"VEYRA_TEST_RP_LEGACY_OWNER_WAIT")),
     engineWake_(!legacyTiming(L"VEYRA_TEST_RP_LEGACY_ENGINE_WAKE")) {}
RemotePlaySessionSource::~RemotePlaySessionSource() {
    close();
    if(frameEvent_){CloseHandle(static_cast<HANDLE>(frameEvent_));frameEvent_=nullptr;}
}
bool RemotePlaySessionSource::connect(RemotePlayConnectDesc desc) {
    close();
    { std::lock_guard lock(mutex_); initialized_=started_=failed_=false; skipped_=publishedSequence_=0; recovery_={}; rates_={}; feedback_={}; controllers_.clearPending(); snapshot_={}; health_={}; }
    owner_=std::jthread([this, request=std::move(desc)](std::stop_token stop) mutable { run(stop,std::move(request)); });
    std::unique_lock lock(mutex_);
    ready_.wait(lock,[&]{return initialized_;});
    info_=publishedInfo_;
    return started_;
}
void RemotePlaySessionSource::run(std::stop_token stop, RemotePlayConnectDesc desc) {
    alive_.store(true);
    RemotePlaySource source;
    // Video samples and session state changes wake this thread (see WakeSignal).
    if(ownerWake_)source.wakeOn(wake_);
    const bool immediateInput=controllers_.policy()==remoteplay::ControllerQueue::Policy::Immediate;
    log::info("remoteplay-timing",std::format("ownerWait={} input={} engineWake={} (event-driven unless VEYRA_TEST_RP_LEGACY_* asks for the original polling)",ownerWake_?"event":"poll-1ms",immediateInput?"immediate":"tick-4ms",engineWake_&&frameEvent_?"event":"poll"));
    // Experimental same-LAN transport profile (TransportTuning.h). Without a
    // VEYRA_EXP_RP_* switch nothing below is logged, called or changed.
    const auto experiments=lanExperiments();
    const OwnerBoost ownerBoost(experiments.transport.threadPriority,experiments.timer1ms);
    experiments.applyTo(desc.queueLimits);
    bool transportLogged=!experiments.any();
    if(experiments.any())log::info("remoteplay-lan",std::format("experiment requested: {} (off by default; unverified on a PS5)",experiments.describe()));
    // Once per session, when the console has connected: what the library and
    // the system actually took.
    const auto logTransport=[&]{
        transportLogged=true;
        const auto c=remoteplay::readTransportCounters();
        log::info("remoteplay-lan",std::format("effective reorderTimeoutUs={} receiveBufferRequested={} receiveBufferEffective={} receiveBufferFallbacks={} chiakiThreadsRaised={} chiakiThreadPriorityFailures={} ownerThread={} timer1ms={} senkushaRttUs={} (receive window advertised to the console unchanged; 0 = not reported)",
            c.reorderTimeoutUs,c.receiveBufferRequested,c.receiveBufferEffective,c.receiveBufferFallbacks,c.threadsRaised,c.threadPriorityFailures,ownerBoost.raised()?"above-normal":"unchanged",ownerBoost.timer(),c.senkushaRttUs));
    };
    // VEYRA_EXP_RP_HEALTH: observation only. The grade lives for the whole
    // session; the counters restart with each connection attempt.
    remoteplay::ConnectionHealth health;
    bool wifiWarned=false;
    uint64_t skippedBase=0;
    const auto sampleHealth=[&](const remoteplay::SessionInbox::Snapshot& state,uint64_t mailboxOverwrites){
        health.observeIngressDepth(state.video.depth);
        const auto at=remoteplay::monotonic100ns();
        if(!health.due(at))return;
        const auto line=health.sample(at,remoteplay::healthCounters(remoteplay::readTransportCounters(),state,mailboxOverwrites));
        if(!line)return;
        log::info("remoteplay-health",remoteplay::formatHealthLine(*line));
        std::lock_guard lock(mutex_);health_=*line;
    };
    // VEYRA_EXP_RP_REORDER_ADAPTIVE: the reorder wait follows what the link
    // shows (ReorderWindow.h). Owner thread only; the library takes a changed
    // value through one atomic. The window lives for the whole session, the
    // library's counters restart with each connection attempt.
    remoteplay::ReorderWindowController reorderWindow(experiments.transport.effectiveReorderUs());
    auto nextReorderLook=std::chrono::steady_clock::now();
    const auto adaptReorder=[&](std::chrono::steady_clock::time_point at){
        if(at<nextReorderLook)return;
        nextReorderLook=at+std::chrono::milliseconds(250);
        const auto counters=remoteplay::readTransportCounters();
        const auto before=reorderWindow.windowUs();
        const auto decision=reorderWindow.update(remoteplay::monotonic100ns(),remoteplay::reorderEvidence(counters));
        if(decision.changed)remoteplay::setLiveReorderTimeout(decision.windowUs);
        if(!decision.changed&&decision.reason==remoteplay::ReorderWindowController::Reason::None)return;
        const auto reason=decision.reason==remoteplay::ReorderWindowController::Reason::Widen?"late packets arrived after the wait"
            :decision.reason==remoteplay::ReorderWindowController::Reason::Narrow?"the shorter wait would have been enough"
            :decision.reason==remoteplay::ReorderWindowController::Reason::Rollback?"the longer wait did not save frames"
            :reorderWindow.latched()?"inconsistent counters for the third time: library default for the rest of the session"
            :"inconsistent counters: library default for a minute";
        log::info("remoteplay-lan",std::format("reorder wait {} -> {} us ({}); needUs={} gaps={} timeouts={} skipped={} lateArrivals={} fecFailed={} widenings={} narrowings={} rollbacks={} faults={}",
            before,decision.windowUs,reason,reorderWindow.neededUs().value_or(0u),counters.reorderGaps,counters.reorderTimeouts,counters.reorderSkipped,counters.reorderLateArrivals,counters.fecFailed,
            reorderWindow.widenings(),reorderWindow.narrowings(),reorderWindow.rollbacks(),reorderWindow.faults()));
    };
    remoteplay::StreamRecovery recovery;
    recovery.setKeyframeRetryMs(experiments.idrRetryMs); // 1000, the original, unless VEYRA_EXP_RP_IDR_RETRY_MS
    if(experiments.reconnectPatient)recovery.setMaxEpisodeRetries(remoteplay::StreamRecovery::kPatientRetries); // 3 otherwise
    bool repeatedTestDisconnect=false;
    const auto milliseconds=[] {return remoteplay::monotonic100ns()/10000;};
    // Keep the existing in-memory credentials only for this user-started run.
    // Their move-only destructor wipes them; never reread another saved profile.
    for(;;){
    recovery.beginAttempt(milliseconds());
    bool retry=false;
    // RP_IN_USE needs its own, much longer backoff: the console keeps the old
    // session slot for roughly twenty seconds after it ends.
    bool retryWasRpInUse=false;
    uint64_t receivedBase=0,droppedBase=0;
    {std::lock_guard lock(mutex_);receivedBase=rates_.received;droppedBase=rates_.ingressDropped;skippedBase=skipped_;}
    std::jthread feeder;
    std::jthread monitor;
    try {
        if(experiments.usesTransport())remoteplay::configureTransport(experiments.transport);
        if(experiments.reorderAdaptive){
            // The connection starts with the window reached so far, not with the
            // configured wait again: a reconnect is not evidence of a better link.
            reorderWindow.newConnection();
            remoteplay::setLiveReorderTimeout(reorderWindow.windowUs());
        }
        if(experiments.health){
            const auto route=remoteplay::assessRoute(desc.request.host,remoteplay::localInterfaces());
            health.begin(remoteplay::monotonic100ns(),route);
            log::info("remoteplay-health",std::format("route={} link={} linkMbps={} (this PC's side only; the console's own link is not visible from here)",route.name(),route.linkName(),route.linkBitsPerSecond/1000000u));
            if(route.wifi()&&!wifiWarned){
                wifiWarned=true;
                log::warn("remoteplay-health","this PC reaches the console over Wi-Fi: expect loss bursts and jitter that a cable does not have; the same-LAN experiments assume Ethernet");
            }
        }
        const bool ok=source.connect(desc);
        if(!ok){retryWasRpInUse=source.nativeSnapshot().startupRetryAllowed;retry=recovery.poll(milliseconds(),false,true)==remoteplay::StreamRecovery::Action::Reconnect;}
        {std::lock_guard lock(mutex_);publishedInfo_=source.info();if(!initialized_)started_=ok;failed_=!ok&&!retry;initialized_=true;telemetryInbox_=source.telemetryInbox();
            if(failed_)recovery_.message=desc.controlOnly?L"PS5 手柄控制连接失败，请检查主机及网络；已保存的配对无需重输。":L"PS5 串流连接失败，请检查主机及网络；已保存的配对无需重输。";}
        ready_.notify_all();
        if(ok&&desc.controlOnly) {
            // PS5 手柄控制: the capture card is the picture. No decoder, no WASAPI,
            // no mailbox: the backend counts and drops the console's keepalive
            // video, and that count is the progress StreamRecovery watches.
            log::info("remoteplay-control",std::format("controller-only session attempt={} keepalive={}x{}@{} {} kbps (dropped on receipt; the console still sends it)",recovery.reconnects(),desc.request.video.width,desc.request.video.height,desc.request.video.fps,desc.request.video.bitrateKbps));
            // Sleep(1) without a raised timer resolution can take 15.6 ms; the
            // controller is sent every 4 ms, so wait on a high-resolution timer.
            const auto timer=std::unique_ptr<void,decltype(&CloseHandle)>(CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS),&CloseHandle);
            auto nextController=std::chrono::steady_clock::now();
            auto nextLog=nextController,rateStart=nextController;
            uint64_t previousReceived=receivedBase,lastVideo=0;
            bool connectedLogged=false;
            while(!stop.stop_requested()) {
                std::string pin;
                {std::lock_guard lock(mutex_);pin.swap(pin_);}
                if(!pin.empty()) {
                    const auto r=source.submitLoginPin(pin);SecureZeroMemory(pin.data(),pin.size());
                    if(!r.ok)log::warn("remoteplay",std::format("{} code={}",r.operation,r.code));
                }
                auto now=std::chrono::steady_clock::now();
                if(now>=nextController) {
                    remoteplay::ControllerState controller;
                    {std::lock_guard lock(mutex_);controller=takeControllerLocked(remoteplay::monotonic100ns());}
                    const auto r=source.submitController(controller);
                    if(!r.ok)log::warn("remoteplay",std::format("{} code={}",r.operation,r.code));
                    nextController=now+std::chrono::milliseconds(4);
                }
                const auto native=source.nativeSnapshot();
                const auto snapshot=source.sessionSnapshot();
                if(now>=nextLog){nextLog=now+std::chrono::seconds(1);
                    log::info("remoteplay-transport",std::format("attempt={} connected={} videoCallbacks={} audioCallbacks={} packetWindowReceived={} packetWindowLost={} upstreamWarnings={} upstreamErrors={} transportErrors={} quitReason={} apiError={} (controller-only; media dropped on receipt)",recovery.reconnects(),native.connected,native.videoCallbacks,native.audioCallbacks,native.packetReceived,native.packetLost,native.warnings,native.errors,native.transportErrors,native.lastQuitReason,native.lastApiError));}
                if(native.connected&&!connectedLogged){connectedLogged=true;log::info("remoteplay-control","console connected; controller channel open");}
                if(native.connected&&!transportLogged)logTransport();
                if(experiments.health)sampleHealth(snapshot,0);
                {std::lock_guard lock(mutex_);snapshot_=snapshot;feedback_.merge(source.takeFeedback());
                    rates_.received=receivedBase+native.videoCallbacks;
                    const double elapsed=std::chrono::duration<double>(now-rateStart).count();
                    if(elapsed>=1){rates_.receivedFps=double(rates_.received-previousReceived)/elapsed;rates_.ready=true;previousReceived=rates_.received;rateStart=now;}
                }
                if(native.videoCallbacks>lastVideo) {
                    lastVideo=native.videoCallbacks;
                    if(recovery.frame(milliseconds()))log::info("remoteplay-recovery",std::format("retry allowance renewed after 30s continuous keepalive; lifetimeAttempts={}",recovery.reconnects()));
                    std::lock_guard lock(mutex_);recovery_.active=false;recovery_.message.clear();
                } else {
                    // Keyframe actions are ignored: there is no decoder to repair.
                    const auto action=recovery.poll(milliseconds(),snapshot.state==remoteplay::SessionState::LoginPinRequired,snapshot.state==remoteplay::SessionState::Failed,native.automaticRetryAllowed,native.startupRetryAllowed);
                    if(action==remoteplay::StreamRecovery::Action::Reconnect){retryWasRpInUse=native.startupRetryAllowed;retry=true;break;}
                    if(action==remoteplay::StreamRecovery::Action::Fail){
                        log::error("remoteplay-recovery",std::format("controller-only recovery stopped attempts={} quitReason={} error={}",recovery.reconnects(),native.lastQuitReason,snapshot.errorCode));
                        std::lock_guard lock(mutex_);failed_=true;recovery_.active=false;
                        recovery_.message=snapshot.state==remoteplay::SessionState::LoginPinRequired?L"等待 PS5 登录 PIN 超时，请重新连接。":native.startupRetryAllowed?L"PS5 仍被串流会话占用，请结束其他串流或稍后重试；无需重新配对。":!native.automaticRetryAllowed?L"PS5 已结束或拒绝串流，请检查主机状态后重新连接。":L"PS5 手柄控制没有恢复，请检查主机及网络后重新打开；无需重新配对。";
                        recovery_.message+=std::format(L"（终止码 {} / 错误 {}）",native.lastQuitReason,snapshot.errorCode);break;
                    }
                }
                now=std::chrono::steady_clock::now();
                const auto wait=std::clamp<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(nextController-now).count(),100,4000);
                if(timer){LARGE_INTEGER due;due.QuadPart=-wait*10;if(SetWaitableTimer(timer.get(),&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer.get(),20);else Sleep(1);}
                else Sleep(1);
            }
        } else if(ok) {
            monitor=std::jthread([inbox=source.telemetryInbox()](std::stop_token cancel){
                while(!cancel.stop_requested()){
                    const auto s=inbox->snapshot();const auto now=remoteplay::monotonic100ns();
                    const auto age=[&](auto stamp){return stamp>0?double(now-stamp)/10000:-1.0;};
                    log::info("remoteplay-progress",std::format("state={} received={} decoded={} inputFps={:.1f} decodeFps={:.1f} videoMbps={:.3f} receiveAgeMs={:.1f} decodeAgeMs={:.1f} decodeBusyMs={:.1f} queue={} waitingIdr={} idrRequests={} errors={} (complete-video callback; not network latency)",int(s.state),s.video.accessUnits,s.decodedFrames,s.receivedFps,s.decodedFps,s.videoMbps,age(s.lastVideo100ns),age(s.lastDecoded100ns),age(s.decodeStarted100ns),s.video.depth,s.video.waitingForIdr,s.video.idrRequests,s.errorCode));
                    for(int i=0;i<10&&!cancel.stop_requested();++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
            WAVEFORMATEX format{}; format.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;format.nChannels=2;
            format.nSamplesPerSec=48000;format.wBitsPerSample=32;format.nBlockAlign=8;format.nAvgBytesPerSec=384000;
            // Audio experiments (off unless set; "0" is off): playout recovery and endpoint period.
            audio_.setRemotePlayExperiments(remoteplay::switchOn(experimentSwitch(L"VEYRA_EXP_RP_AUDIO_FAST_RECOVER")),
                remoteplay::switchOn(experimentSwitch(L"VEYRA_EXP_RP_AUDIO_LOW_BUFFER")));
            if(audio_.configure(format)&&audio_.start()) {
                // Separate from both decoder and GPU owners. Source is closed
                // only after this reader and the WASAPI owner have joined.
                feeder=std::jthread([this,&source](std::stop_token cancel){
                    try {
                        float pcm[960]; double next=-1;
                        while(!cancel.stop_requested()) {
                            double pts=0;const auto count=source.pullAudio(pcm,480,&pts);
                            if(count) {
                                const bool gap=next>=0&&std::abs(pts-next)>0.5;
                                if(!audio_.push(pcm,count*2*sizeof(float),pts,gap))
                                    log::warn("remoteplay-audio","PCM queue rejected block");
                                next=pts+1000.0*double(count)/48000;
                            } else std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                    } catch(const std::exception& e) {log::error("remoteplay-audio",e.what());}
                });
            } else log::error("remoteplay-audio","output initialization failed; video remains available");
            auto lastFrame=std::chrono::steady_clock::now();
            auto nextController=lastFrame;
            auto rateStart=lastFrame;uint64_t previousReceived=receivedBase,previousDecoded=0;
            {std::lock_guard lock(mutex_);previousDecoded=rates_.decoded;}
            auto nextNativeLog=lastFrame;
            remoteplay::NativeSnapshot native;
            bool firstFrame=true;
            wchar_t disconnectAfter[24]{};uint64_t testDisconnectFrames=0;bool testDisconnected=false;
            if(!recovery.reconnects()&&GetEnvironmentVariableW(L"VEYRA_TEST_REMOTEPLAY_DISCONNECT_AFTER_FRAMES",disconnectAfter,24))testDisconnectFrames=std::clamp<uint64_t>(_wtoi64(disconnectAfter),60,3600);
            while(!stop.stop_requested()) {
                std::string pin;
                {std::lock_guard lock(mutex_);pin.swap(pin_);}
                if(!pin.empty()) {
                    const auto r=source.submitLoginPin(pin);SecureZeroMemory(pin.data(),pin.size());
                    if(!r.ok)log::warn("remoteplay",std::format("{} code={}",r.operation,r.code));
                }
                const auto now=std::chrono::steady_clock::now();
                if(!desc.request.viewOnly) {
                    // Original: one queued state per 4 ms tick. Immediate: everything
                    // pending goes out now, in order (Chiaki sends each button edge as
                    // its own history event), and the tick only repeats the current
                    // state so that a pad that stopped being polled fails neutral.
                    bool due=now>=nextController;
                    for(unsigned sent=0;sent<remoteplay::ControllerQueue::kCapacity+1;++sent) {
                        remoteplay::ControllerState controller;bool more=false;
                        {std::lock_guard lock(mutex_);
                            if(!due&&!(immediateInput&&controllers_.pending()))break;
                            controller=takeControllerLocked(remoteplay::monotonic100ns());more=controllers_.pending();}
                        const auto r=source.submitController(controller);
                        if(!r.ok)log::warn("remoteplay",std::format("{} code={}",r.operation,r.code));
                        due=false;nextController=now+std::chrono::milliseconds(4);
                        if(!immediateInput||!more)break;
                    }
                }
                pipeline::FramePacket packet; const AVFrame* frame=nullptr;
                const auto result=source.read(packet,&frame);
                auto feedback=source.takeFeedback();
                const auto snapshot=source.sessionSnapshot();
                if(now>=nextNativeLog||result==SourceReadStatus::Error){native=source.nativeSnapshot();nextNativeLog=now+std::chrono::seconds(1);
                    log::info("remoteplay-transport",std::format("attempt={} connected={} videoCallbacks={} audioCallbacks={} callbackRejected={} packetWindowReceived={} packetWindowLost={} upstreamWarnings={} upstreamErrors={} transportErrors={} assemblyErrors={} quitReason={} apiError={} (upstream resetting packet window; not cumulative UDP)",recovery.reconnects(),native.connected,native.videoCallbacks,native.audioCallbacks,native.callbackRejected,native.packetReceived,native.packetLost,native.warnings,native.errors,native.transportErrors,native.assemblyErrors,native.lastQuitReason,native.lastApiError));
                    if(native.connected&&!transportLogged)logTransport();}
                uint64_t skippedNow=0;
                {std::lock_guard lock(mutex_);snapshot_=snapshot;feedback_.merge(std::move(feedback));skippedNow=skipped_;
                    rates_.received=receivedBase+snapshot.video.accessUnits;rates_.ingressDropped=droppedBase+snapshot.video.dropped;
                    const double elapsed=std::chrono::duration<double>(now-rateStart).count();
                    if(elapsed>=1){rates_.receivedFps=double(rates_.received-previousReceived)/elapsed;
                        rates_.decodedFps=double(rates_.decoded-previousDecoded)/elapsed;rates_.ready=true;
                        previousReceived=rates_.received;previousDecoded=rates_.decoded;rateStart=now;}
                }
                if(experiments.health)sampleHealth(snapshot,skippedNow-skippedBase);
                if(experiments.reorderAdaptive)adaptReorder(now);
                if(result==SourceReadStatus::Frame && frame) {
                    packet.sourceEpoch+=uint64_t(recovery.reconnects())<<32;
                    if(firstFrame&&recovery.reconnects())packet.flags|=static_cast<pipeline::FrameFlags>(pipeline::FrameFlagBits::Discontinuity);
                    if(firstFrame&&recovery.reconnects())log::info("remoteplay-recovery",std::format("reconnected attempt={} first decoded frame; reset video history and audio clock",recovery.reconnects()));
                    firstFrame=false;
                    publishDecoded(frame,packet,source.info());
                    lastFrame=now;
                    const bool renewed=recovery.frame(milliseconds());
                    if(renewed){
                        log::info("remoteplay-recovery",std::format("retry allowance renewed after 30s continuous decoded progress; lifetimeAttempts={}",recovery.reconnects()));
                        if(!repeatedTestDisconnect&&GetEnvironmentVariableW(L"VEYRA_TEST_REMOTEPLAY_REPEAT_OUTAGE",nullptr,0)){
                            repeatedTestDisconnect=true;const auto r=source.disconnectTransportForTest();
                            log::warn("remoteplay-recovery-test",std::format("second owned transport stop after renewal ok={} code={}",r.ok,r.code));
                        }
                    }
                    {std::lock_guard lock(mutex_);recovery_.active=false;recovery_.message.clear();}
                    if(testDisconnectFrames&&!testDisconnected&&snapshot.decodedFrames>=testDisconnectFrames){
                        testDisconnected=true;const auto r=source.disconnectTransportForTest();
                        log::warn("remoteplay-recovery-test",std::format("injected owned transport stop ok={} code={} (no console power command)",r.ok,r.code));
                    }
                } else {
                    const auto action=recovery.poll(milliseconds(),snapshot.state==remoteplay::SessionState::LoginPinRequired,result==SourceReadStatus::Error,native.automaticRetryAllowed,native.startupRetryAllowed);
                    if(action==remoteplay::StreamRecovery::Action::Keyframe){
                        source.recoverVideo();log::warn("remoteplay-recovery",std::format("request keyframe; received={} decoded={} videoCallbacks={} rejected={} packetReceived={} packetLost={}",snapshot.video.accessUnits,snapshot.decodedFrames,native.videoCallbacks,native.callbackRejected,native.packetReceived,native.packetLost));
                        std::lock_guard lock(mutex_);recovery_.active=true;recovery_.message=L"画面中断，正在请求关键帧恢复…";
                    }else if(action==remoteplay::StreamRecovery::Action::Reconnect){retryWasRpInUse=native.startupRetryAllowed;retry=true;break;}
                    else if(action==remoteplay::StreamRecovery::Action::Fail){
                        log::error("remoteplay-recovery",std::format("recovery stopped attempts={} quitReason={} error={}",recovery.reconnects(),native.lastQuitReason,snapshot.errorCode));
                        std::lock_guard lock(mutex_);failed_=true;recovery_.active=false;
                        recovery_.message=snapshot.state==remoteplay::SessionState::LoginPinRequired?L"等待 PS5 登录 PIN 超时，请重新连接。":native.startupRetryAllowed?L"PS5 仍被串流会话占用，请结束其他串流或稍后重试；无需重新配对。":!native.automaticRetryAllowed?L"PS5 已结束或拒绝串流，请检查主机状态后重新连接。":L"PS5 串流未恢复，请检查主机及网络后点击连接；无需重新配对。";
                        recovery_.message+=std::format(L"（终止码 {} / 错误 {}）",native.lastQuitReason,snapshot.errorCode);break;
                    }
                    // Nothing to decode. Original: sleep 1 ms and poll again, which on
                    // Windows is 1-2 ms with a raised timer resolution and up to 15.6 ms
                    // without one. Now: wait for a sample or a pad state, at most until
                    // the next controller tick (view-only has no tick: 4 ms, for the
                    // recovery poll and a login PIN).
                    if(ownerWake_){
                        auto left=std::chrono::microseconds(4000);
                        if(!desc.request.viewOnly)left=std::clamp(std::chrono::duration_cast<std::chrono::microseconds>(nextController-std::chrono::steady_clock::now()),std::chrono::microseconds(100),std::chrono::microseconds(4000));
                        wake_->waitFor(left);
                    }else std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
    } catch(const std::exception& e) {
        log::error("remoteplay",e.what());
        retry=false;
        {std::lock_guard lock(mutex_);failed_=true;initialized_=true;recovery_.message=L"PS5 串流处理异常，已停止自动恢复，请查看日志。";}
        ready_.notify_all();
    }
    if(retry&&!stop.stop_requested()){
        std::lock_guard lock(mutex_);
        // The message reads n/3; the extra attempts of VEYRA_EXP_RP_RECONNECT_PATIENT read 3/3.
        const unsigned shown=(std::min)(recovery.episodeRetries(),remoteplay::StreamRecovery::kRetries);
        recovery_={true,shown,retryWasRpInUse
            ?std::format(L"PS5 正在释放上一个串流会话，等待后自动重试（{}/3）…",shown)
            :desc.controlOnly?std::format(L"PS5 手柄控制中断，正在重新连接（{}/3）…",shown)
            :std::format(L"串流中断，正在重新连接（{}/3）…",shown)};
        latest_.reset();controllers_.clearPending();feedback_={};
    }
    if(experiments.any()){
        // For comparing runs with a switch on and off: what the queue did in this attempt.
        const auto video=source.sessionSnapshot().video;
        log::info("remoteplay-lan",std::format("attempt={} ingress accepted={} dropped={} recoveries={} keyframeRequests={} earlyKeyframeRequests={} highWaterDepth={} backlogEpisodes={} backlogFrames={}",recovery.reconnects(),video.accepted,video.dropped,video.recoveries,video.idrRequests,video.idrEarlyRequests,video.highWaterDepth,video.backlogEpisodes,video.backlogFrames));
    }
    if(experiments.reorderAdaptive)
        log::info("remoteplay-lan",std::format("attempt={} reorder wait at the end={} us widenings={} narrowings={} rollbacks={} faults={} latched={}",recovery.reconnects(),reorderWindow.windowUs(),reorderWindow.widenings(),reorderWindow.narrowings(),reorderWindow.rollbacks(),reorderWindow.faults(),reorderWindow.latched()));
    log::info("remoteplay-recovery",std::format("teardown begin attempt={} retry={} cancelled={}",recovery.reconnects(),retry,stop.stop_requested()));
    if(monitor.joinable()){monitor.request_stop();monitor.join();}
    if(feeder.joinable()){feeder.request_stop();feeder.join();}
    audio_.stop();
    source.close();
    log::info("remoteplay-recovery",std::format("teardown complete attempt={}",recovery.reconnects()));
    if(!retry||stop.stop_requested())break;
    log::warn("remoteplay-recovery",std::format("old session joined; reconnect={} using existing in-memory pairing",recovery.reconnects()));
    // Interruptible backoff; manual Stop prevents the next start. The console
    // keeps the previous session slot for ~20 s after it ends, so an RP_IN_USE
    // refusal gets 10/20/40 s instead of the generic 1/2/4 s: the short budget
    // spent all three retries inside that window (field log 2026-09-17: three
    // refusals in 11 s, while a 25 s wait reconnected fine).
    const unsigned backoffSeconds=remoteplay::StreamRecovery::backoffSeconds(recovery.episodeRetries(),retryWasRpInUse);
    if(retryWasRpInUse)log::warn("remoteplay-recovery",std::format("PS5 still holds the previous session; waiting {}s before retry {}/{}",backoffSeconds,recovery.episodeRetries(),recovery.maxEpisodeRetries()));
    for(unsigned i=0;i<backoffSeconds*20&&!stop.stop_requested();++i)std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if(stop.stop_requested())break;
    }
    {std::lock_guard lock(mutex_);recovery_.active=false;}
    // run() has no early return: the owner loop and its retries are over.
    alive_.store(false);
}
void RemotePlaySessionSource::publishDecoded(const AVFrame* frame,pipeline::FramePacket packet,const SourceInfo& info){
    auto* cloned=av_frame_clone(frame);if(!cloned)throw std::bad_alloc();
    Frame item{std::shared_ptr<AVFrame>(cloned,[](AVFrame* p){av_frame_free(&p);}),packet,info};
    std::lock_guard lock(mutex_);
    item.packet.sequence=++publishedSequence_;
    ++rates_.decoded;
    if(latest_){++skipped_;item.packet.flags|=latest_->packet.flags;
        item.packet.flags|=static_cast<pipeline::FrameFlags>(pipeline::FrameFlagBits::Drop);}
    latest_=std::move(item);
    // The engine loop waits on this (see frameEvent()); set under the lock so a
    // reader that wakes always finds the frame.
    if(frameEvent_)SetEvent(static_cast<HANDLE>(frameEvent_));
}
SourceReadStatus RemotePlaySessionSource::read(pipeline::FramePacket& packet,const AVFrame** frame) {
    if(frame)*frame=nullptr;
    std::lock_guard lock(mutex_);
    if(failed_)return SourceReadStatus::Error;
    if(!latest_)return started_?SourceReadStatus::Waiting:SourceReadStatus::Error;
    packet=latest_->packet;info_=latest_->info;view_=std::move(latest_->frame);latest_.reset();
    if(frame)*frame=view_.get();return SourceReadStatus::Frame;
}
void RemotePlaySessionSource::close() noexcept {
    if(owner_.joinable()){owner_.request_stop();if(wake_)wake_->notify();owner_.join();}
    std::lock_guard lock(mutex_);latest_.reset();view_.reset();info_={};started_=false;
    SecureZeroMemory(pin_.data(),pin_.size());pin_.clear();
}
void RemotePlaySessionSource::controller(remoteplay::ControllerState state){
    bool wake=false;
    {std::lock_guard lock(mutex_);
        const auto overflows=controllers_.overflows();
        wake=controllers_.push(remoteplay::monotonic100ns(),std::move(state));
        if(controllers_.overflows()!=overflows)log::warn("remoteplay-input","Controller queue overflow; releasing before latest state");
    }
    // Immediate policy only: the tick policy sends on its own 4 ms schedule.
    if(wake&&ownerWake_&&controllers_.policy()==remoteplay::ControllerQueue::Policy::Immediate)wake_->notify();
}
remoteplay::ControllerState RemotePlaySessionSource::takeControllerLocked(remoteplay::HostTime now){
    return controllers_.take(now);
}
void RemotePlaySessionSource::loginPin(std::string pin){
    {std::lock_guard lock(mutex_);SecureZeroMemory(pin_.data(),pin_.size());pin_=std::move(pin);}
    if(ownerWake_)wake_->notify();
}
remoteplay::SessionInbox::Snapshot RemotePlaySessionSource::sessionSnapshot()const{
    std::shared_ptr<const remoteplay::SessionInbox> inbox;{std::lock_guard lock(mutex_);inbox=telemetryInbox_;if(!inbox)return snapshot_;}
    return inbox->snapshot();
}
RemotePlaySessionSource::Rates RemotePlaySessionSource::rates()const{std::lock_guard lock(mutex_);return rates_;}
remoteplay::ControllerFeedback RemotePlaySessionSource::takeFeedback(){std::lock_guard lock(mutex_);auto result=std::move(feedback_);feedback_={};return result;}
uint64_t RemotePlaySessionSource::skipped()const{std::lock_guard lock(mutex_);return skipped_;}
}
