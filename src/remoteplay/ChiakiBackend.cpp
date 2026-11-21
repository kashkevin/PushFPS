// SPDX-License-Identifier: GPL-3.0-only
// Uses chiaki-ng's AGPL-3.0-only + OpenSSL-exception APIs; retain both licenses.
// Windows native initialization and source regression evidence: see execution log.
#include "veyra/remoteplay/ChiakiBackend.h"
#include <chiaki/session.h>
#if !defined(CHIAKI_VEYRA_VIDEO_METADATA_API) || CHIAKI_VEYRA_VIDEO_METADATA_API != 1
#error The reviewed Chiaki video metadata patch is required
#endif
#include <chiaki/takion.h>
#if !defined(CHIAKI_VEYRA_TRANSPORT_API) || CHIAKI_VEYRA_TRANSPORT_API != 1
#error The Chiaki transport tuning patch (0004) is required
#endif
#if !defined(CHIAKI_VEYRA_TRANSPORT_LIVE_REORDER_API) || CHIAKI_VEYRA_TRANSPORT_LIVE_REORDER_API != 1
#error The Chiaki transport tuning patch (0004) with the live reorder wait is required
#endif
#if !defined(CHIAKI_VEYRA_TRANSPORT_FRAME_EVIDENCE_API) || CHIAKI_VEYRA_TRANSPORT_FRAME_EVIDENCE_API != 1
#error The Chiaki transport tuning patch (0004) with the per-frame late-packet evidence is required
#endif
#if !defined(CHIAKI_VEYRA_TRANSPORT_CONSOLE_QUALITY_API) || CHIAKI_VEYRA_TRANSPORT_CONSOLE_QUALITY_API != 1
#error The Chiaki transport tuning patch (0004) with the console quality counters is required
#endif
#if !defined(CHIAKI_VEYRA_TRANSPORT_OPTIONS_API) || CHIAKI_VEYRA_TRANSPORT_OPTIONS_API != 1
#error The Chiaki transport tuning patch (0004) with the transport options is required
#endif
#include <chiaki/thread.h>
#include <chiaki/time.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/controller.h>
#include <chiaki/orientation.h>
#include <cmath>
#include <chiaki/log.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
namespace veyra::remoteplay {
static_assert(CHIAKI_PSN_ACCOUNT_ID_SIZE == 8);
static_assert(CHIAKI_SESSION_AUTH_SIZE == 16);
namespace {
void wipe(void* ptr,std::size_t n) noexcept {
    auto* p=static_cast<volatile unsigned char*>(ptr);
    while(n--) *p++=0;
}
BackendResult result(ChiakiErrorCode code,const char* operation) {
    return {code==CHIAKI_ERR_SUCCESS,static_cast<int>(code),operation};
}
}
// PairingCredentials' move and destructor (they wipe the secrets) are inline in ChiakiBackend.h.
BackendResult initializeChiaki(){
    static std::once_flag flag;
    static ChiakiErrorCode code=CHIAKI_ERR_UNKNOWN;
    std::call_once(flag,[]{code=chiaki_lib_init();});
    return result(code,"chiaki_lib_init");
}
namespace {
// VEYRA_EXP_RP_THREAD_PRIORITY. Chiaki calls its affinity hook at the start of
// each of its threads, on that thread; the two that carry the stream (Takion:
// receive, reorder, FEC, our video callback) and the pad (feedback sender) go
// one step above normal. Installed once and only when the switch was ever on;
// with it off the hook returns before doing anything.
std::atomic<bool> boostThreads{false};
std::atomic<std::uint32_t> threadsRaised{0},threadPriorityFailures{0};
void threadHook(ChiakiThreadName name,void*) noexcept {
    if(!boostThreads.load(std::memory_order_relaxed))return;
    if(name!=CHIAKI_THREAD_NAME_TAKION&&name!=CHIAKI_THREAD_NAME_FEEDBACK)return;
    if(SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL))++threadsRaised;else ++threadPriorityFailures;
}
std::uint32_t stat32(ChiakiVeyraTransportStat stat) noexcept {
    const auto value=chiaki_veyra_transport_stat_get(stat);
    return value>0xffffffffull?0xffffffffu:static_cast<std::uint32_t>(value);
}
}
void configureTransport(const TransportTuning& tuning){
    chiaki_veyra_transport_set_tuning(tuning.reorderTimeoutUs,tuning.receiveBufferBytes); // also clears the options
    // VEYRA_EXP_RP_REORDER_WRAP_FIX / VEYRA_EXP_RP_EARLY_LOSS only: never called without one of them.
    if(tuning.wrapFix||tuning.earlyLoss)
        chiaki_veyra_transport_set_options((tuning.wrapFix?CHIAKI_VEYRA_OPTION_REORDER_WRAP_FIX:0u)|(tuning.earlyLoss?CHIAKI_VEYRA_OPTION_EARLY_LOSS:0u));
    chiaki_veyra_transport_stats_reset();
    threadsRaised=0;threadPriorityFailures=0;
    boostThreads=tuning.threadPriority;
    if(tuning.threadPriority){
        static std::once_flag installed;
        std::call_once(installed,[]{chiaki_thread_set_affinity_cb(threadHook,nullptr);});
    }
}
TransportCounters readTransportCounters() noexcept {
    TransportCounters c;
    // A counter that is a part of another is read before it: the library adds
    // to the whole first (a timeout after its gap, a late arrival after its
    // skip, a frame lost to a late packet after its FEC failure), so a poll
    // between two library updates never sees the part larger than the whole
    // (ReorderWindowController treats that as a fault).
    c.lateFramesFailed=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_LATE_FRAMES_FAILED);
    c.reorderLateArrivals=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_LATE_ARRIVALS);
    c.reorderTimeouts=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_TIMEOUTS);
    c.avPackets=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_AV_PACKETS);
    c.videoPackets=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_VIDEO_PACKETS);
    c.reorderGaps=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_GAPS);
    c.reorderSkipped=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_SKIPPED);
    c.reorderDropped=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_DROPPED);
    c.fecRecovered=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_FRAMES_FEC_RECOVERED);
    c.fecFailed=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_FRAMES_FEC_FAILED);
    c.corruptReports=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CORRUPT_REPORTS);
    c.corruptFrames=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CORRUPT_FRAMES);
    c.idrRequests=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_IDR_REQUESTS);
    c.congestionReports=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CONGESTION_REPORTS);
    c.congestionReceived=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CONGESTION_RECEIVED);
    c.congestionLost=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CONGESTION_LOST);
    c.congestionLostReported=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CONGESTION_LOST_REPORTED);
    c.senkushaRttUs=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_SENKUSHA_RTT_US);
    c.reorderTimeoutUs=stat32(CHIAKI_VEYRA_STAT_REORDER_TIMEOUT_US);
    c.receiveBufferRequested=stat32(CHIAKI_VEYRA_STAT_RCVBUF_REQUESTED);
    c.receiveBufferEffective=stat32(CHIAKI_VEYRA_STAT_RCVBUF_EFFECTIVE);
    c.receiveBufferFallbacks=stat32(CHIAKI_VEYRA_STAT_RCVBUF_FALLBACKS);
    c.threadsRaised=threadsRaised.load();c.threadPriorityFailures=threadPriorityFailures.load();
    c.reorderOvershootUs=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_REORDER_OVERSHOOT_US);
    static_assert(static_cast<int>(kReorderLatenessBuckets)==CHIAKI_VEYRA_REORDER_LATENESS_BUCKETS);
    for(std::size_t i=0;i<kReorderLatenessBuckets;++i)
        c.reorderLateness[i]=chiaki_veyra_transport_stat_get(static_cast<ChiakiVeyraTransportStat>(static_cast<int>(CHIAKI_VEYRA_STAT_REORDER_LATENESS_0)+static_cast<int>(i)));
    c.consoleReports=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CQ_REPORTS);
    c.consoleTargetBitrate=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CQ_TARGET_BITRATE);
    c.consoleUpstreamBitrate=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CQ_UPSTREAM_BITRATE);
    c.consoleRttMilli=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CQ_RTT_MICRO);
    c.consoleUpstreamLossPpm=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CQ_UPSTREAM_LOSS_PPM);
    c.consoleLoss=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CQ_LOSS);
    c.consoleLastUs=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CQ_LAST_US);
    c.consoleMeasuredKbps=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_CQ_MEASURED_KBPS);
    c.libraryNowUs=chiaki_time_now_monotonic_us();
    c.earlyLossCuts=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_EARLY_LOSS_CUTS);
    c.earlyLossWrong=chiaki_veyra_transport_stat_get(CHIAKI_VEYRA_STAT_EARLY_LOSS_WRONG);
    return c;
}
void setLiveReorderTimeout(std::uint32_t microseconds) noexcept {
    chiaki_veyra_transport_set_av_reorder_timeout_live(microseconds);
}
struct ChiakiBackend::Impl {
    explicit Impl(SessionInbox::Token t):token(std::move(t)),owner(std::this_thread::get_id()){}
    SessionInbox::Token token;
    std::thread::id owner;
    ChiakiSession session{};
    ChiakiOpusDecoder opus{};
    ChiakiLog log{};
    VideoProfile profile;
    std::string host;
    bool initialized=false,opusInitialized=false;
    std::atomic<bool> active=false,started=false,connected=false;
    std::atomic<std::uint64_t> warnings=0,errors=0,videoCallbacks=0,audioCallbacks=0;
    std::atomic<int> quitReason=0,apiError=0;
    std::atomic<uint64_t> callbackRejected=0,transportErrors=0,assemblyErrors=0;
    std::atomic<int64_t> serverTargetBitrate=-1;
    std::atomic<uint64_t> qualityReports=0;
    // Opus frames Chiaki's audio receiver concealed (a NULL frame = Opus packet-loss concealment), counted
    // by a forwarding sink in front of the Opus decoder's own (docs/AUDIO.md, round 3). Behaviour unchanged.
    ChiakiAudioSink opusSink{};std::atomic<uint64_t> audioConcealed=0;
    // PS5 手柄控制: set before the session starts, read only by the callbacks.
    bool discardMedia=false;
    // Only the upstream audio/Opus callback thread reads/writes these fields.
    std::uint32_t channels=0,rate=0;
    std::uint64_t sampleIndex=0;
    bool audioDiscontinuity=true;
    ChiakiOrientationTracker orientation{};ChiakiAccelNewZero accelZero{};
    uint64_t lastMotion=0;
    std::mutex feedbackMutex;ControllerFeedback feedback;

    static void logCallback(ChiakiLogLevel level,const char* message,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);
        if(message&&level==CHIAKI_LOG_VERBOSE){
            int target=-1;
            if(std::sscanf(message,"StreamConnection received connection quality: target_bitrate=%d,",&target)==1&&target>=0){self.serverTargetBitrate=target;++self.qualityReports;}
        }
        // Raw upstream log strings/hexdumps may contain keys. Do not forward them.
        if(level==CHIAKI_LOG_ERROR)++self.errors;
        if(level==CHIAKI_LOG_WARNING)++self.warnings;
        // Classify in memory; no upstream strings or hexdumps leave this callback.
        if(message&&(level==CHIAKI_LOG_ERROR||level==CHIAKI_LOG_WARNING)){
            const std::string_view text=message;
            if(text.find("Takion")!=text.npos||text.find("timeout")!=text.npos)++self.transportErrors;
            if(text.find("Video")!=text.npos||text.find("video")!=text.npos||text.find("frame")!=text.npos||text.find("FEC")!=text.npos)++self.assemblyErrors;
        }
    }
    static void eventCallback(ChiakiEvent* event,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);
        if(!event||!self.active.load())return;
        switch(event->type){
        case CHIAKI_EVENT_CONNECTED:self.connected=true;self.token.connected();break;
        case CHIAKI_EVENT_LOGIN_PIN_REQUEST:self.token.loginPinRequired();break;
        case CHIAKI_EVENT_QUIT:
            self.connected=false;self.quitReason=static_cast<int>(event->quit.reason);
            // A remote termination is not a successful decoded EOF.
            self.token.failed(2000+static_cast<int>(event->quit.reason));break;
        case CHIAKI_EVENT_RUMBLE:{std::lock_guard lock(self.feedbackMutex);self.feedback.rumble=true;self.feedback.left=event->rumble.left;self.feedback.right=event->rumble.right;break;}
        case CHIAKI_EVENT_TRIGGER_EFFECTS:{std::lock_guard lock(self.feedbackMutex);auto& f=self.feedback;f.triggers=true;f.leftTrigger[0]=event->trigger_effects.type_left;f.rightTrigger[0]=event->trigger_effects.type_right;std::copy_n(event->trigger_effects.left,10,f.leftTrigger.begin()+1);std::copy_n(event->trigger_effects.right,10,f.rightTrigger.begin()+1);break;}
        case CHIAKI_EVENT_MOTION_RESET:{std::lock_guard lock(self.feedbackMutex);self.feedback.motionReset=true;break;}
        default:break; // No mic, standby or keyboard side effects.

        }
    }
    static void hapticsCallback(uint8_t* data,size_t size,void* user)noexcept{
        auto& self=*static_cast<Impl*>(user);
        if(!self.active||!data||!size||size%4||size>1200)return;
        try{std::lock_guard lock(self.feedbackMutex);auto& pcm=self.feedback.haptics;
            if(pcm.size()+size/2>600)pcm.clear();
            const auto at=pcm.size();pcm.resize(at+size/2);std::memcpy(pcm.data()+at,data,size);
        }catch(...){++self.errors;}
    }
    static bool videoCallback(std::uint8_t* data,std::size_t size,
        const ChiakiVeyraVideoSampleInfo* info,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);
        if(!self.active.load()||!data||!size||!info)return false;
        // Counted and accepted, never rejected: a false return makes Chiaki drop
        // the reference frame, leave the last complete frame behind and send a
        // corrupt-frame report to the console for every following sample.
        if(self.discardMedia){++self.videoCallbacks;return true;}
        try{
            const auto nal=inspectAnnexB({data,size},self.profile.codec);
            if(!nal.valid || (!nal.hasConfig&&!nal.hasPicture)){++self.callbackRejected;return false;}
            VideoSample sample;
            sample.generation=self.token.generation();sample.arrival100ns=monotonic100ns();
            sample.codec=self.profile.codec;
            sample.kind=info->kind==CHIAKI_VEYRA_SAMPLE_FRAME?SampleKind::AccessUnit:SampleKind::CodecConfig;
            if((sample.kind==SampleKind::AccessUnit && !nal.hasPicture) ||
                (sample.kind==SampleKind::CodecConfig && (!nal.hasConfig || nal.hasPicture))){++self.callbackRejected;return false;}
            if(info->frame_index_valid)sample.wireFrameIndex=info->frame_index;
            sample.width=info->width;sample.height=info->height;
            sample.framesLost=info->frames_lost;sample.referenceRecovered=info->reference_recovered;
            sample.payload=PaddedBytes::copy({data,size});++self.videoCallbacks;
            const bool accepted=self.token.video(std::move(sample));if(!accepted)++self.callbackRejected;return accepted;
        }catch(...){self.apiError=-1001;self.token.failed(-1001);return false;}
    }
    // PS5 手柄控制 plays the capture card's audio. A sink is still attached: with
    // no frame callback Chiaki's audio receiver returns before freeing each
    // buffered frame. Nothing is decoded.
    static void discardAudio(std::uint8_t*,std::size_t,void* user) noexcept {++static_cast<Impl*>(user)->audioCallbacks;}
    static void countedAudioHeader(ChiakiAudioHeader* header,void* user) noexcept {auto& self=*static_cast<Impl*>(user);if(self.opusSink.header_cb)self.opusSink.header_cb(header,self.opusSink.user);}
    static void countedAudioFrame(std::uint8_t* buf,std::size_t size,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);if(!buf||!size)++self.audioConcealed;
        if(self.opusSink.frame_cb)self.opusSink.frame_cb(buf,size,self.opusSink.user);
    }
    static void opusSettings(std::uint32_t channels,std::uint32_t rate,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);
        // Sample position stays monotonic across repeated Opus headers within
        // one session; restarting at zero makes AudioIngress reject new PCM.
        self.channels=channels;self.rate=rate;self.audioDiscontinuity=true;
        if((channels!=1&&channels!=2)||rate!=48000){self.channels=0;self.token.failed(-1003);}
    }
    static void opusFrame(std::int16_t* data,std::size_t count,void* user) noexcept {
        auto& self=*static_cast<Impl*>(user);
        if(!self.active.load()||!self.channels||!data)return;
        if(count==0||count>self.rate/5u||count>(std::numeric_limits<std::uint64_t>::max)()-self.sampleIndex){self.token.failed(-1004);return;}
        try{
            PcmBlock block;block.generation=self.token.generation();block.channels=self.channels;block.rate=self.rate;
            block.firstSample=self.sampleIndex;block.arrival100ns=monotonic100ns();block.discontinuity=self.audioDiscontinuity;
            // Opus count is samples PER CHANNEL, not bytes or interleaved elements.
            block.samples.assign(data,data+count*self.channels);self.sampleIndex+=count;++self.audioCallbacks;
            if(self.token.audio(std::move(block))) self.audioDiscontinuity=false;
        }catch(...){self.apiError=-1002;self.token.failed(-1002);}
    }
    BackendResult record(ChiakiErrorCode code,const char* name){apiError=static_cast<int>(code);return result(code,name);}
    bool onOwner()const noexcept{return owner==std::this_thread::get_id();}
};
ChiakiBackend::ChiakiBackend()=default;
ChiakiBackend::~ChiakiBackend(){
    if(!p_)return;
    try{
        const auto stopped=stop();
        if(!stopped.ok && p_){
            // A failed join cannot be repaired by freeing memory still used by callbacks.
            // Quarantine/leak this one session rather than create a use-after-free.
            // The owner must treat stop failure as fatal and not reconnect this object.
            p_->active=false;(void)p_.release();
        }
    }catch(...){if(p_){p_->active=false;(void)p_.release();}}
}
BackendResult ChiakiBackend::start(const NativeConnectRequest& request,SessionInbox::Token token,bool discardMedia){
    if(p_)return {false,-1100,"already_initialized"};
    if(!validHost(request.host)||request.video.validate()||token.generation()==0)return {false,-1101,"validate_connect"};
    auto init=initializeChiaki();if(!init.ok)return init;
    p_=std::make_unique<Impl>(std::move(token));auto& s=*p_;s.host=request.host;s.profile=request.video;
    s.discardMedia=discardMedia;
    // Verbose formatting has per-packet overhead: enable only for explicit diagnostics.
    // Callback above whitelists numeric quality fields and never forwards raw text.
    const bool qualityTrace=std::getenv("VEYRA_TEST_PS5_QUALITY_TRACE")!=nullptr;
    chiaki_log_init(&s.log,CHIAKI_LOG_WARNING|CHIAKI_LOG_ERROR|(qualityTrace?CHIAKI_LOG_VERBOSE:0),Impl::logCallback,&s);
    ChiakiConnectInfo info{};info.ps5=true;info.host=s.host.c_str();
    std::memcpy(info.regist_key,request.credentials.registrationKey.data(),request.credentials.registrationKey.size());
    std::memcpy(info.morning,request.credentials.sessionKey.data(),request.credentials.sessionKey.size());
    std::memcpy(info.psn_account_id,request.credentials.accountId.data(),request.credentials.accountId.size());
    chiaki_connect_video_profile_preset(&info.video_profile,
        request.video.height==720?CHIAKI_VIDEO_RESOLUTION_PRESET_720p:CHIAKI_VIDEO_RESOLUTION_PRESET_1080p,
        request.video.fps==30?CHIAKI_VIDEO_FPS_PRESET_30:CHIAKI_VIDEO_FPS_PRESET_60);
    info.video_profile.codec=request.video.codec==Codec::H264?CHIAKI_CODEC_H264:request.video.codec==Codec::H265Hdr?CHIAKI_CODEC_H265_HDR:CHIAKI_CODEC_H265;
    info.video_profile.bitrate=request.video.bitrateKbps;
    info.video_profile_auto_downgrade=false;info.enable_keyboard=false;info.enable_dualsense=!request.viewOnly||discardMedia;
    info.auto_regist=false;info.packet_loss_max=0.05;info.enable_idr_on_fec_failure=true;
    const auto code=chiaki_session_init(&s.session,&info,&s.log);
    wipe(info.regist_key,sizeof(info.regist_key));wipe(info.morning,sizeof(info.morning));wipe(info.psn_account_id,sizeof(info.psn_account_id));
    if(code!=CHIAKI_ERR_SUCCESS){auto out=s.record(code,"chiaki_session_init");s.token.failed(out.code);p_.reset();return out;}
    s.initialized=true;
    if(discardMedia){ChiakiAudioSink sink{};sink.user=&s;sink.frame_cb=Impl::discardAudio;chiaki_session_set_audio_sink(&s.session,&sink);}
    else{
        chiaki_opus_decoder_init(&s.opus,&s.log);s.opusInitialized=true;
        chiaki_opus_decoder_set_cb(&s.opus,Impl::opusSettings,Impl::opusFrame,&s);
        chiaki_opus_decoder_get_sink(&s.opus,&s.opusSink);
        ChiakiAudioSink sink{};sink.user=&s;sink.header_cb=Impl::countedAudioHeader;sink.frame_cb=Impl::countedAudioFrame;chiaki_session_set_audio_sink(&s.session,&sink);
    }
    ChiakiAudioSink hapticSink{};hapticSink.user=&s;hapticSink.frame_cb=Impl::hapticsCallback;
    chiaki_session_set_haptics_sink(&s.session,&hapticSink);
    chiaki_session_set_event_cb(&s.session,Impl::eventCallback,&s);
    chiaki_session_set_veyra_video_sample_cb(&s.session,Impl::videoCallback,&s);
    s.active=true;
    const auto startCode=chiaki_session_start(&s.session);
    if(startCode!=CHIAKI_ERR_SUCCESS){auto out=s.record(startCode,"chiaki_session_start");s.active=false;s.token.failed(out.code);(void)stop();return out;}
    s.started=true;return s.record(startCode,"chiaki_session_start");
}
BackendResult ChiakiBackend::stop(){
    if(!p_)return {true,0,"already_stopped"};
    auto& s=*p_;
    if(!s.onOwner())return {false,-1102,"wrong_owner_thread"};
    s.active=false;
    ChiakiErrorCode stopResult=CHIAKI_ERR_SUCCESS;
    if(s.started){
        ChiakiControllerState idle;chiaki_controller_state_set_idle(&idle);
        (void)s.record(chiaki_session_set_controller_state(&s.session,&idle),"release_controller");
        const auto stopCode=chiaki_session_stop(&s.session);
        stopResult=stopCode;
        // Join even if stop reported an error. Never fini while a callback is alive.
        const auto joinCode=chiaki_session_join(&s.session);
        if(joinCode!=CHIAKI_ERR_SUCCESS)return s.record(joinCode,"chiaki_session_join");
        s.started=false;s.connected=false;
        if(stopCode!=CHIAKI_ERR_SUCCESS)s.apiError=static_cast<int>(stopCode);
    }
    if(s.opusInitialized){chiaki_opus_decoder_fini(&s.opus);s.opusInitialized=false;}
    if(s.initialized){chiaki_session_fini(&s.session);s.initialized=false;}
    wipe(&s.session,sizeof(s.session));
    p_.reset();return result(stopResult,"stop_join_fini");
}
BackendResult ChiakiBackend::requestIdr(){
    if(!p_||!p_->onOwner()||!p_->started)return {false,-1103,"idr_without_session"};
    return p_->record(chiaki_session_request_idr(&p_->session),"chiaki_session_request_idr");
}
BackendResult ChiakiBackend::submitLoginPin(std::string_view pin){
    if(!p_||!p_->onOwner()||!p_->started)return {false,-1104,"pin_without_session"};
    if(pin.empty()||pin.size()>8||!std::all_of(pin.begin(),pin.end(),[](char c){return c>='0'&&c<='9';}))return {false,-1105,"invalid_login_pin"};
    return p_->record(chiaki_session_set_login_pin(&p_->session,reinterpret_cast<const std::uint8_t*>(pin.data()),pin.size()),"chiaki_session_set_login_pin");
}
BackendResult ChiakiBackend::submitController(const ControllerState& state){
    if(!p_||!p_->onOwner()||!p_->started)return {false,-1106,"controller_without_session"};
    ChiakiControllerState c;chiaki_controller_state_set_idle(&c);
    struct Mapping{std::uint32_t semantic;ChiakiControllerButton target;};
    static constexpr Mapping buttons[]={
        {ControllerState::Cross,CHIAKI_CONTROLLER_BUTTON_CROSS},{ControllerState::Circle,CHIAKI_CONTROLLER_BUTTON_MOON},
        {ControllerState::Square,CHIAKI_CONTROLLER_BUTTON_BOX},{ControllerState::Triangle,CHIAKI_CONTROLLER_BUTTON_PYRAMID},
        {ControllerState::Left,CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT},{ControllerState::Right,CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT},
        {ControllerState::Up,CHIAKI_CONTROLLER_BUTTON_DPAD_UP},{ControllerState::Down,CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN},
        {ControllerState::L1,CHIAKI_CONTROLLER_BUTTON_L1},{ControllerState::R1,CHIAKI_CONTROLLER_BUTTON_R1},
        {ControllerState::L3,CHIAKI_CONTROLLER_BUTTON_L3},{ControllerState::R3,CHIAKI_CONTROLLER_BUTTON_R3},
        {ControllerState::Options,CHIAKI_CONTROLLER_BUTTON_OPTIONS},{ControllerState::Share,CHIAKI_CONTROLLER_BUTTON_SHARE},
        {ControllerState::Touchpad,CHIAKI_CONTROLLER_BUTTON_TOUCHPAD},{ControllerState::PS,CHIAKI_CONTROLLER_BUTTON_PS}};
    for(auto b:buttons)if(state.buttons&b.semantic)c.buttons|=static_cast<std::uint32_t>(b.target);
    for(size_t i=0;i<state.touches.size();++i){c.touches[i].id=state.touches[i].id;c.touches[i].x=state.touches[i].x;c.touches[i].y=state.touches[i].y;}
    const auto applyMotion=[&](const std::array<float,3>& gyro,const std::array<float,3>& accel,uint64_t timestamp){
        if(!timestamp||!std::all_of(gyro.begin(),gyro.end(),[](float v){return std::isfinite(v);})||!std::all_of(accel.begin(),accel.end(),[](float v){return std::isfinite(v);}))return;
        // Repeated state snapshots contain the same batch; never integrate twice.
        if(p_->lastMotion&&timestamp<=p_->lastMotion)return;
        if(!p_->lastMotion||timestamp-p_->lastMotion>100000){chiaki_orientation_tracker_init(&p_->orientation);chiaki_accel_new_zero_set_inactive(&p_->accelZero,false);}
        chiaki_orientation_tracker_update(&p_->orientation,gyro[0],gyro[1],gyro[2],accel[0],accel[1],accel[2],&p_->accelZero,false,uint32_t(timestamp));p_->lastMotion=timestamp;
    };
    if(state.motionValid){
        for(size_t i=0;i<std::min(size_t(state.motionSampleCount),state.motionSamples.size());++i){const auto& sample=state.motionSamples[i];applyMotion(sample.gyro,sample.accel,sample.timestampUs);}
        applyMotion(state.gyro,state.accel,state.motionTimestampUs);
        chiaki_orientation_tracker_apply_to_controller_state(&p_->orientation,&c);
    }else p_->lastMotion=0;
    c.l2_state=state.l2;c.r2_state=state.r2;c.left_x=state.leftX;c.left_y=state.leftY;c.right_x=state.rightX;c.right_y=state.rightY;
    return p_->record(chiaki_session_set_controller_state(&p_->session,&c),"chiaki_session_set_controller_state");
}
ControllerFeedback ChiakiBackend::takeFeedback(){
    if(!p_||!p_->onOwner())return {};
    std::lock_guard lock(p_->feedbackMutex);auto result=std::move(p_->feedback);p_->feedback={};
    if(result.motionReset)p_->lastMotion=0;
    return result;
}
NativeSnapshot ChiakiBackend::snapshot()const{
    // Like start/stop, pointer ownership requires the owner; the atomic counters
    // themselves may be updated by callbacks. Do not call concurrently with reset.
    if(!p_)return {};
    auto& s=*p_;
    NativeSnapshot out{s.started.load(),s.connected.load(),s.warnings.load(),s.errors.load(),s.videoCallbacks.load(),s.audioCallbacks.load(),s.quitReason.load(),s.apiError.load()};
    out.callbackRejected=s.callbackRejected;out.transportErrors=s.transportErrors;out.assemblyErrors=s.assemblyErrors;
    out.serverTargetBitrate=s.serverTargetBitrate;out.qualityReports=s.qualityReports;out.audioConcealed=s.audioConcealed;
    // Upstream packet window may reset itself; do not label this cumulative UDP.
    if(s.initialized)chiaki_packet_stats_get(&s.session.stream_connection.packet_stats,false,&out.packetReceived,&out.packetLost);
    // After a stopped session the PS5 may briefly still report RP_IN_USE.
    // Retrying this request cannot evict it; the console remains authoritative.
    // A manual reconnect may race the console releasing our previous session.
    // Retry RP_IN_USE within the same bounded budget; never force eviction.
    out.automaticRetryAllowed=out.lastQuitReason==CHIAKI_QUIT_REASON_NONE||out.lastQuitReason==CHIAKI_QUIT_REASON_CTRL_UNKNOWN||out.lastQuitReason==CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED||out.lastQuitReason==CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN||out.lastQuitReason==CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE;
    out.startupRetryAllowed=out.lastQuitReason==CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE;
    return out;
}
} // namespace veyra::remoteplay
