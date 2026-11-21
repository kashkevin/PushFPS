// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental same-LAN transport profile for a PS5 stream (all off unless a
// VEYRA_EXP_RP_* switch is set). Portable: the switches arrive as strings, the
// Windows code only reads the environment. Unverified on Windows and on a PS5.
//
//   VEYRA_EXP_RP_LAN_PROFILE=1      reorder wait 2000 us, socket receive buffer 1 MiB
//     VEYRA_EXP_RP_REORDER_US=N     with the profile: wait of N us, 500..16000
//     VEYRA_EXP_RP_RCVBUF=N         with the profile: buffer of N bytes, 102400..8388608
//   VEYRA_EXP_RP_REORDER_ADAPTIVE=1 the reorder wait follows the link within 2000..16000 us (ReorderWindow.h)
//   VEYRA_EXP_RP_THREAD_PRIORITY=1  Takion, feedback and owner threads above normal
//   VEYRA_EXP_RP_TIMER_1MS=1        1 ms system timer resolution while the session owner runs
//   VEYRA_EXP_RP_BACKLOG=1          decode through a burst of up to 16 frames (see VideoIngress.h)
//   VEYRA_EXP_RP_IDR_RETRY_MS=N     first keyframe request of an outage after N ms, 250..1000 (default 1000)
//   VEYRA_EXP_RP_HEALTH=1           connection health line every 5 s (see ConnectionHealth.h)
//   VEYRA_EXP_RP_RECONNECT_PATIENT=1 up to 8 reconnection attempts per outage instead of 3 (StreamRecovery.h)
//   VEYRA_EXP_RP_EXPIRY_FIX=1       a frame that arrived after the owner read its clock is never called expired (VideoIngress.h)
//   VEYRA_EXP_RP_REORDER_WRAP_FIX=1 the library's reorder queue truncates its 16-bit index sum (packet 0 no longer dropped at the wrap)
//   VEYRA_EXP_RP_EARLY_LOSS=1       a gap larger than its frame's FEC units is given up at once while nothing arrives out of order
//
// A switch set to an empty value or to 0 is off. No switch implies another:
// the LAN profile sets neither the thread priority nor the timer. Nothing here
// changes the receive window advertised to the console.
#include "VideoIngress.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
namespace veyra::remoteplay {
// What the patched Chiaki library is asked to do. Zero means "library default",
// which is exactly the unpatched behaviour.
struct TransportTuning {
    static constexpr std::uint32_t kLibraryReorderUs=16000,kLibraryReceiveBuffer=0x19000;
    static constexpr std::uint32_t kLanReorderUs=2000,kReorderMinUs=500,kReorderMaxUs=16000;
    static constexpr std::uint32_t kLanReceiveBuffer=1024u*1024u,kReceiveBufferMin=0x19000,kReceiveBufferMax=8u*1024u*1024u;
    std::uint32_t reorderTimeoutUs=0;
    std::uint32_t receiveBufferBytes=0;
    bool threadPriority=false;
    bool reorderExplicit=false; // VEYRA_EXP_RP_REORDER_US was set and taken: an adaptive rule must not change it
    // Chiaki patch 0004 options (CHIAKI_VEYRA_OPTION_*): VEYRA_EXP_RP_REORDER_WRAP_FIX, VEYRA_EXP_RP_EARLY_LOSS.
    bool wrapFix=false,earlyLoss=false;
    std::uint32_t effectiveReorderUs()const noexcept{return reorderTimeoutUs?reorderTimeoutUs:kLibraryReorderUs;}
    std::uint32_t effectiveReceiveBuffer()const noexcept{return receiveBufferBytes?receiveBufferBytes:kLibraryReceiveBuffer;}
    bool operator==(const TransportTuning&)const=default;
};
// How long after its wait began an out-of-order video packet arrived (chiaki
// patch 0004): counters for <= each edge, and one more for anything later.
inline constexpr std::array<std::uint32_t,8> kReorderLatenessEdgesUs{1000,2000,3000,4000,6000,8000,12000,16000};
inline constexpr std::size_t kReorderLatenessBuckets=kReorderLatenessEdgesUs.size()+1;
// Counters kept by the patched library (chiaki patch 0004), cumulative since
// the transport was last configured. Process-wide in the library: one session
// at a time.
struct TransportCounters {
    std::uint64_t avPackets=0,videoPackets=0;
    std::uint64_t reorderGaps=0,reorderTimeouts=0,reorderSkipped=0,reorderDropped=0;
    std::uint64_t fecRecovered=0,fecFailed=0,corruptReports=0,corruptFrames=0,idrRequests=0;
    // Sums over the library's 200 ms congestion windows. "Lost" is what was
    // measured; "lostReported" is what the console was told (capped at 5 %).
    std::uint64_t congestionReports=0,congestionReceived=0,congestionLost=0,congestionLostReported=0;
    std::uint64_t senkushaRttUs=0; // 0 = the library did not measure it
    std::uint32_t reorderTimeoutUs=0,receiveBufferRequested=0,receiveBufferEffective=0; // 0 = no connection yet
    // Connections on which the system refused the tuned receive buffer and the
    // library went on with its own size (the connection is never failed for it).
    std::uint32_t receiveBufferFallbacks=0;
    std::uint32_t threadsRaised=0,threadPriorityFailures=0;
    // Of reorderDropped: packets that arrived after their wait had run out
    // (late, not lost). reorderOvershootUs: summed over reorderTimeouts, how
    // much longer than asked those waits lasted on the Takion thread.
    std::uint64_t reorderLateArrivals=0,reorderOvershootUs=0;
    std::array<std::uint64_t,kReorderLatenessBuckets> reorderLateness{};
    // Of fecFailed: frames one of whose given-up packets arrived afterwards and
    // of which the library saw enough packets to have completed them (a longer
    // wait would have saved them). Per frame, on the Takion thread (patch 0004,
    // CHIAKI_VEYRA_STAT_LATE_FRAMES_FAILED; model LateFrameEvidenceModel).
    std::uint64_t lateFramesFailed=0;
    // The console's CONNECTIONQUALITY message as patch 0004 keeps it (newest
    // values, units as sent; ConsoleQuality.h interprets them). consoleLastUs:
    // the library's monotonic clock at the newest message; 0 = none.
    std::uint64_t consoleReports=0,consoleTargetBitrate=0,consoleUpstreamBitrate=0,consoleRttMilli=0;
    std::uint64_t consoleUpstreamLossPpm=0,consoleLoss=0,consoleLastUs=0,consoleMeasuredKbps=0;
    std::uint64_t libraryNowUs=0; // the library's monotonic clock when the counters were read
    // VEYRA_EXP_RP_EARLY_LOSS: gaps given up at once (also counted in reorderTimeouts), and
    // packets of such a gap that came afterwards (the decision was wrong for them).
    std::uint64_t earlyLossCuts=0,earlyLossWrong=0;
};
// Environment values as read; nullopt = variable not set.
struct TransportSwitches {
    std::optional<std::string> lanProfile,reorderUs,receiveBuffer,threadPriority,timer1ms,backlog,idrRetryMs,health;
    std::optional<std::string> reorderAdaptive,reconnectPatient;
    std::optional<std::string> expiryFix,wrapFix,earlyLoss;
};
struct LanExperiments {
    static constexpr std::size_t kBacklogFrames=16;
    static constexpr int kIdrRetryDefaultMs=1000,kIdrRetryFloorMs=250;
    TransportTuning transport;
    bool lanProfile=false,backlog=false,health=false;
    // The reorder wait starts at transport.effectiveReorderUs() and is then
    // moved by ReorderWindowController. Its own switch: it sets no buffer size.
    bool reorderAdaptive=false;
    // More reconnection attempts per outage (StreamRecovery::kPatientRetries),
    // the later ones at most 8 s apart; nothing else about recovery changes.
    bool reconnectPatient=false;
    // The ingress age rule leaves a frame that arrived after the owner's clock
    // read alone (QueueLimits::expiryFix); nothing else about the queue changes.
    bool expiryFix=false;
    // timeBeginPeriod(1) for the owner loop. Its own switch, apart from the
    // thread priority, so that a comparison can tell the two effects apart.
    bool timer1ms=false;
    int idrRetryMs=kIdrRetryDefaultMs;
    // An override that was set but not usable (no profile, not a number, or out of range).
    bool reorderOverrideIgnored=false,receiveBufferOverrideIgnored=false,idrRetryIgnored=false;
    bool any()const noexcept{return lanProfile||transport.threadPriority||timer1ms||backlog||health||reorderAdaptive||reconnectPatient||expiryFix||transport.wrapFix||transport.earlyLoss||idrRetryMs!=kIdrRetryDefaultMs;}
    // The library is only called (tuning, counters, thread hook) when this is true.
    bool usesTransport()const noexcept{return lanProfile||transport.threadPriority||health||reorderAdaptive||transport.wrapFix||transport.earlyLoss;}
    // Queue limits for this session: untouched unless a switch asks.
    void applyTo(QueueLimits& limits)const noexcept{
        if(backlog&&limits.maxFrames<kBacklogFrames)limits.backlogFrames=kBacklogFrames;
        // Below the queue's 500 ms request spacing the first repeat of an outage
        // must be let through early or it would wait until 500 ms; nothing else
        // about the spacing changes (QueueLimits::idrFirstRetry100ns).
        if(idrRetryMs<500)limits.idrFirstRetry100ns=static_cast<HostTime>(idrRetryMs)*10000;
        if(expiryFix)limits.expiryFix=true;
    }
    std::string describe()const{
        std::string text="lanProfile=";text+=lanProfile?"on":"off";
        text+=" reorderUs="+std::to_string(transport.effectiveReorderUs());
        text+=" receiveBuffer="+std::to_string(transport.effectiveReceiveBuffer());
        text+=" threadPriority=";text+=transport.threadPriority?"above-normal":"unchanged";
        text+=" timer1ms=";text+=timer1ms?"on":"off";
        text+=" backlogFrames="+std::to_string(backlog?kBacklogFrames:std::size_t{0});
        text+=" idrRetryMs="+std::to_string(idrRetryMs);
        text+=" health=";text+=health?"on":"off";
        text+=" reorderAdaptive=";text+=reorderAdaptive?"on":"off";
        text+=" reconnectPatient=";text+=reconnectPatient?"on":"off";
        text+=" expiryFix=";text+=expiryFix?"on":"off";
        text+=" wrapFix=";text+=transport.wrapFix?"on":"off";
        text+=" earlyLoss=";text+=transport.earlyLoss?"on":"off";
        if(reorderOverrideIgnored)text+=" (VEYRA_EXP_RP_REORDER_US ignored: needs the LAN profile and 500..16000)";
        if(receiveBufferOverrideIgnored)text+=" (VEYRA_EXP_RP_RCVBUF ignored: needs the LAN profile and 102400..8388608)";
        if(idrRetryIgnored)text+=" (VEYRA_EXP_RP_IDR_RETRY_MS ignored: not a number)";
        return text;
    }
};
inline bool switchOn(const std::optional<std::string>& value)noexcept{return value&&!value->empty()&&*value!="0";}
// Decimal digits only; no sign, spaces or suffix. Values past 10 digits are refused.
inline std::optional<std::uint64_t> switchNumber(const std::optional<std::string>& value)noexcept{
    if(!value||value->empty()||value->size()>10)return {};
    std::uint64_t number=0;
    for(const char c:*value){if(c<'0'||c>'9')return {};number=number*10u+static_cast<std::uint64_t>(c-'0');}
    return number;
}
inline LanExperiments resolveLanExperiments(const TransportSwitches& s)noexcept{
    LanExperiments e;
    e.lanProfile=switchOn(s.lanProfile);
    if(e.lanProfile){
        e.transport.reorderTimeoutUs=TransportTuning::kLanReorderUs;
        e.transport.receiveBufferBytes=TransportTuning::kLanReceiveBuffer;
    }
    if(s.reorderUs){
        const auto n=switchNumber(s.reorderUs);
        if(e.lanProfile&&n&&*n>=TransportTuning::kReorderMinUs&&*n<=TransportTuning::kReorderMaxUs){e.transport.reorderTimeoutUs=static_cast<std::uint32_t>(*n);e.transport.reorderExplicit=true;}
        else e.reorderOverrideIgnored=true;
    }
    if(s.receiveBuffer){
        const auto n=switchNumber(s.receiveBuffer);
        if(e.lanProfile&&n&&*n>=TransportTuning::kReceiveBufferMin&&*n<=TransportTuning::kReceiveBufferMax)e.transport.receiveBufferBytes=static_cast<std::uint32_t>(*n);
        else e.receiveBufferOverrideIgnored=true;
    }
    e.transport.threadPriority=switchOn(s.threadPriority);
    e.timer1ms=switchOn(s.timer1ms);
    e.backlog=switchOn(s.backlog);
    e.health=switchOn(s.health);
    e.reorderAdaptive=switchOn(s.reorderAdaptive);
    e.reconnectPatient=switchOn(s.reconnectPatient);
    e.expiryFix=switchOn(s.expiryFix);
    e.transport.wrapFix=switchOn(s.wrapFix);
    e.transport.earlyLoss=switchOn(s.earlyLoss);
    if(switchOn(s.idrRetryMs)){
        const auto n=switchNumber(s.idrRetryMs);
        // Never later than today's 1000 ms, never earlier than the floor.
        if(!n)e.idrRetryIgnored=true;
        else if(*n<static_cast<std::uint64_t>(LanExperiments::kIdrRetryFloorMs))e.idrRetryMs=LanExperiments::kIdrRetryFloorMs;
        else if(*n<static_cast<std::uint64_t>(LanExperiments::kIdrRetryDefaultMs))e.idrRetryMs=static_cast<int>(*n);
    }
    return e;
}
} // namespace veyra::remoteplay
