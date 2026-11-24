// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The dominant problem of a PS5 stream in one five-second window, from packet to
// presentation (stream "network", 21 Nov 2026; docs/drafts/NETWORK_AUDIT.md section 8).
// Pure and header-only: evidence in, one cause out, plus every cause whose sign
// was seen and the numbers that decided it. It observes only.
//
// Rules, local and certain causes first (all thresholds provisional: not
// calibrated on a PS5, like the health line's):
//   unknown            the window holds no packet and no frame (an outage, a stall,
//                      a keyframe wait): nothing to judge, never "none"
//   socket-or-adapter  frames were lost, packets never arrived, AND this PC counted
//                      UDP receive errors (system-wide) or adapter discards of the
//                      same order (at least a tenth of the missing packets);
//                      fewer local errors than that are only a sign
//   receive-thread     the reorder waits that ran out lasted >= 2 ms longer than
//                      asked (>= 4 of them): the receive (Takion) thread woke late:
//                      a loaded CPU, or the default system timer granularity of the
//                      library's millisecond wait (arm nd-timer, VEYRA_EXP_RP_TIMER_1MS,
//                      tells the two apart); packets it then gives up look like loss
//   app-drops          our own ingress dropped complete frames although the
//                      library lost nothing (no missing packet, no lost frame)
//   network-loss       frames could not be completed and the missing packets
//                      never arrived (none came late)
//   packet-recovery    frames could not be completed although their packets
//                      arrived late (the reorder wait gave them up), or frames
//                      were lost with no missing packet at all; or frames came
//                      unevenly (the jitter sign) while reorder waits ran out for
//                      packets that never came: the wait held every frame behind
//                      a lost packet and bought nothing (FEC completed them)
//   decoder            decode time P95 >= 0.8 of a frame interval
//   jitter             frame interarrival jitter >= 2 ms or a frame >= 8 ms off
//   frame-generation   (engine) frame generation reports it is over its budget
//   enhancement        (engine) GPU >= 95 % with an upscaler or the neural layer on
//   gpu-processing     >= 2 % of decoded frames were replaced before the engine
//                      took them, or (engine) GPU >= 95 % without enhancement
//   presentation       (engine) fewer real frames presented than decoded (< 95 %)
//                      while the engine took every frame
//   encoder            nothing above, and the console delivered < 60 % of the
//                      requested bitrate: the picture is what the encoder sent
// Never names the network when its counters are clean; never names the GPU,
// the decoder or the encoder while frames are being lost to the network.
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
namespace veyra::remoteplay {
enum class StreamCause : std::uint8_t {
    None,SocketOrAdapter,ReceiveThread,AppDrops,NetworkLoss,PacketRecovery,Decoder,Jitter,FrameGeneration,Enhancement,GpuProcessing,Presentation,Encoder,
    Unknown // no packet and no frame in the window: not a sign, never set in signs
};
inline const char* streamCauseName(StreamCause c)noexcept{
    switch(c){
    case StreamCause::SocketOrAdapter:return "socket-or-adapter";
    case StreamCause::ReceiveThread:return "receive-thread";
    case StreamCause::AppDrops:return "app-drops";
    case StreamCause::NetworkLoss:return "network-loss";
    case StreamCause::PacketRecovery:return "packet-recovery";
    case StreamCause::Decoder:return "decoder";
    case StreamCause::Jitter:return "jitter";
    case StreamCause::FrameGeneration:return "frame-generation";
    case StreamCause::Enhancement:return "enhancement";
    case StreamCause::GpuProcessing:return "gpu-processing";
    case StreamCause::Presentation:return "presentation";
    case StreamCause::Encoder:return "encoder";
    case StreamCause::Unknown:return "unknown";
    case StreamCause::None:break;
    }
    return "none";
}
inline constexpr std::uint16_t streamCauseBit(StreamCause c)noexcept{
    return c==StreamCause::None?std::uint16_t{0}:static_cast<std::uint16_t>(1u<<(static_cast<unsigned>(c)-1u));
}
// What the session owner knows (the [slipstream-net] window). Counts are for the window.
struct NetworkEvidence {
    bool valid=false;
    std::uint64_t packets=0;           // packets the library accounted (received + missing)
    std::uint64_t missingNotLate=0;    // missing packets minus those that came late
    // false: the window's loss count is not usable (the library's loss counter wrapped, ConnectionHealth.h);
    // missingNotLate is then not read, and nothing is claimed from packet counts
    bool countsKnown=true;
    std::uint64_t lateArrivals=0;      // packets that came after the reorder wait gave them up
    std::uint64_t fecFailed=0,framesLost=0;
    std::uint64_t ingressDropped=0;    // complete frames our queue discarded
    std::uint64_t frames=0;            // frames decoded in the window
    std::uint64_t mailboxOverwrites=0; // decoded frames replaced before the engine took them
    std::optional<std::uint64_t> udpInErrors,nicDiscards; // absent = not read
    std::uint64_t reorderTimeouts=0;   // reorder waits that ran out
    double reorderOvershootMs=0;       // mean per such wait: how much longer than asked it lasted
    double jitterMs=0,jitterPeakMs=0;
    double fps=0;                      // frames received per second
    std::optional<double> decodeP95Ms;
    double kbps=0,requestedKbps=0;     // delivered in the window; requested at connect (<= 0 unknown)
    double consoleTargetKbps=-1;       // the console's own target, -1 unknown
};
// What the engine knows (the panel's caller fills it; the owner's log line leaves it invalid).
struct EngineEvidence {
    bool valid=false;
    std::optional<double> gpuUtilizationPct;
    bool fgActive=false,fgBudgetLimited=false,enhancementActive=false;
    std::optional<double> realPresentFps,decodedFps;
};
struct StreamThresholds {
    static constexpr double kOvershootMs=2.0;static constexpr std::uint64_t kMinTimeouts=4;
    static constexpr double kDecodeShare=0.8,kJitterMs=2.0,kJitterPeakMs=8.0;
    static constexpr double kOverwritePercent=2.0,kGpuBusyPercent=95.0,kPresentShare=0.95,kEncoderShare=0.6;
};
struct StreamVerdict {
    StreamCause cause=StreamCause::None;
    std::uint16_t signs=0; // every cause whose sign was seen
    std::string evidence;  // fixed keys and numbers only
};
inline std::uint16_t streamCauseSigns(const NetworkEvidence& n,const EngineEvidence& e)noexcept{
    using T=StreamThresholds;
    std::uint16_t signs=0;
    const auto set=[&](StreamCause c){signs=static_cast<std::uint16_t>(signs|streamCauseBit(c));};
    if(!n.valid)return signs;
    const bool unrecovered=n.fecFailed>0||n.framesLost>0;
    // A count above every packet of the window is a wrapped counter, not loss.
    const bool counts=n.countsKnown&&n.missingNotLate<=n.packets;
    const std::uint64_t missingNotLate=counts?n.missingNotLate:0;
    const bool neverArrived=counts?missingNotLate>0:unrecovered; // unknown counts: lost frames may have missing packets
    const bool localDrops=(n.udpInErrors&&*n.udpInErrors>0)||(n.nicDiscards&&*n.nicDiscards>0);
    if(counts&&missingNotLate>0&&localDrops)set(StreamCause::SocketOrAdapter);
    if(n.reorderTimeouts>=T::kMinTimeouts&&n.reorderOvershootMs>=T::kOvershootMs)set(StreamCause::ReceiveThread);
    if(counts&&n.ingressDropped>0&&!unrecovered&&missingNotLate==0&&n.lateArrivals==0)set(StreamCause::AppDrops);
    if(unrecovered&&n.lateArrivals==0&&neverArrived)set(StreamCause::NetworkLoss);
    if(unrecovered&&(n.lateArrivals>0||(counts&&missingNotLate==0)))set(StreamCause::PacketRecovery);
    const double frameMs=n.fps>1.0?1000.0/n.fps:1000.0/60.0;
    if(n.decodeP95Ms&&*n.decodeP95Ms>=T::kDecodeShare*frameMs)set(StreamCause::Decoder);
    const bool uneven=n.jitterMs>=T::kJitterMs||n.jitterPeakMs>=T::kJitterPeakMs;
    if(uneven)set(StreamCause::Jitter);
    if(uneven&&!unrecovered&&n.reorderTimeouts>0&&n.lateArrivals==0&&counts&&missingNotLate>0)set(StreamCause::PacketRecovery);
    const bool overwrites=n.frames>0&&static_cast<double>(n.mailboxOverwrites)*100.0>=T::kOverwritePercent*static_cast<double>(n.frames);
    const bool gpuBusy=e.valid&&e.gpuUtilizationPct&&*e.gpuUtilizationPct>=T::kGpuBusyPercent;
    if(e.valid&&e.fgActive&&e.fgBudgetLimited)set(StreamCause::FrameGeneration);
    else if(gpuBusy&&e.enhancementActive)set(StreamCause::Enhancement);
    else if(overwrites||gpuBusy)set(StreamCause::GpuProcessing);
    if(e.valid&&!overwrites&&e.realPresentFps&&e.decodedFps&&*e.decodedFps>1.0&&*e.realPresentFps<T::kPresentShare*(*e.decodedFps))set(StreamCause::Presentation);
    const bool networkSigns=!counts||unrecovered||missingNotLate>0||n.lateArrivals>0||(signs&streamCauseBit(StreamCause::Jitter))!=0;
    if(!networkSigns&&n.requestedKbps>0&&n.kbps>0&&n.kbps<T::kEncoderShare*n.requestedKbps)set(StreamCause::Encoder);
    return signs;
}
// Socket / adapter drops lead only when frames were lost and this PC's own errors are of the order of the packets
// that never arrived (at least a tenth): a few errors elsewhere on the PC beside a real network loss are a sign.
inline bool localDropsDominate(const NetworkEvidence& n)noexcept{
    if(!n.valid||!n.countsKnown||n.missingNotLate>n.packets||(n.fecFailed==0&&n.framesLost==0))return false;
    const std::uint64_t local=(n.udpInErrors?*n.udpInErrors:0)+(n.nicDiscards?*n.nicDiscards:0);
    return local>0&&local*10>=n.missingNotLate;
}
inline StreamCause classifyStreamCause(std::uint16_t signs)noexcept{
    for(const auto c:{StreamCause::SocketOrAdapter,StreamCause::ReceiveThread,StreamCause::AppDrops,StreamCause::NetworkLoss,StreamCause::PacketRecovery,StreamCause::Decoder,
            StreamCause::Jitter,StreamCause::FrameGeneration,StreamCause::Enhancement,StreamCause::GpuProcessing,StreamCause::Presentation,StreamCause::Encoder})
        if(signs&streamCauseBit(c))return c;
    return StreamCause::None;
}
// "network-loss+jitter", or "none".
inline std::string formatStreamCauseSigns(std::uint16_t signs){
    std::string text;
    for(unsigned i=1;i<=static_cast<unsigned>(StreamCause::Encoder);++i){
        const auto c=static_cast<StreamCause>(i);
        if(!(signs&streamCauseBit(c)))continue;
        if(!text.empty())text+='+';
        text+=streamCauseName(c);
    }
    return text.empty()?std::string("none"):text;
}
inline StreamVerdict classifyStream(const NetworkEvidence& n,const EngineEvidence& e={}){
    StreamVerdict v;
    v.signs=streamCauseSigns(n,e);
    const auto socketBit=streamCauseBit(StreamCause::SocketOrAdapter);
    v.cause=classifyStreamCause(localDropsDominate(n)?v.signs:static_cast<std::uint16_t>(v.signs&~socketBit));
    if(!n.valid){v.cause=StreamCause::Unknown;v.evidence="no-window";return v;}
    if(n.packets==0&&n.frames==0)v.cause=StreamCause::Unknown; // nothing arrived: an outage or a stall, not "no bottleneck"
    char text[320];
    std::snprintf(text,sizeof(text),"missingNotLate=%llu late=%llu fecFailed=%llu framesLost=%llu ingressDropped=%llu overwrites=%llu/%llu udpInErrors=%lld nicDiscards=%lld jitterMs=%.2f decodeP95Ms=%.2f kbps=%.0f/%.0f",
        static_cast<unsigned long long>(n.missingNotLate),static_cast<unsigned long long>(n.lateArrivals),static_cast<unsigned long long>(n.fecFailed),
        static_cast<unsigned long long>(n.framesLost),static_cast<unsigned long long>(n.ingressDropped),static_cast<unsigned long long>(n.mailboxOverwrites),
        static_cast<unsigned long long>(n.frames),n.udpInErrors?static_cast<long long>(*n.udpInErrors):-1LL,n.nicDiscards?static_cast<long long>(*n.nicDiscards):-1LL,
        n.jitterMs,n.decodeP95Ms?*n.decodeP95Ms:-1.0,n.kbps,n.requestedKbps);
    v.evidence=text;
    if(e.valid){
        char engine[160];
        std::snprintf(engine,sizeof(engine)," gpuPct=%.0f fgLimited=%d enhancement=%d realPresentFps=%.1f decodedFps=%.1f",
            e.gpuUtilizationPct?*e.gpuUtilizationPct:-1.0,e.fgActive&&e.fgBudgetLimited?1:0,e.enhancementActive?1:0,
            e.realPresentFps?*e.realPresentFps:-1.0,e.decodedFps?*e.decodedFps:-1.0);
        v.evidence+=engine;
    }
    return v;
}
} // namespace veyra::remoteplay
