// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// SmartLink: the adaptive networking engine of Slipstream (experiment
// VEYRA_EXP_RP_SMARTLINK, off by default). This file holds only the plain
// types every part of SmartLink shares, so that the Windows wiring (samples in,
// decisions out) can be written against them:
//
//   Sample      one observation window of the stream (nominally 200 ms, the
//               library's congestion-report period); filled by the session owner
//   Prediction  what SmartLinkPredictor.h makes of the recent samples
//   Profile     Wired / Strong Wi-Fi / Unstable Wi-Fi (SmartLinkProfile.h)
//   Advice      plain-language advice ids; the interface turns them into
//               translated sentences. Advice never changes a system setting.
//   Decision    what SmartLinkController.h asks for; 0 = restore the configured value
//   Mode        off / observe / apply, from VEYRA_EXP_RP_SMARTLINK or
//               slipstream.ini `smartlink=`
//
// Conventions:
//   - An unknown input is NEGATIVE (-1), never guessed. The one exception is
//     the Wi-Fi RSSI, which is negative by nature: there "unknown" is >= 0.
//   - Times are milliseconds on one monotonic host clock; rates are kbit/s.
//   - Everything is trivially copyable and allocation-free; no names, no
//     addresses, no network names anywhere (numbers only).
// Portable C++20, header-only. Unverified on Windows and on a PS5.
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
namespace veyra::remoteplay::smartlink {

// How the PC reaches the console's network, from the default route's adapter.
enum class Route : std::uint8_t {Unknown, Wired, Wifi};
// What the interface shows: "Automatic" chooses one of these by itself.
enum class Profile : std::uint8_t {Unknown, Wired, StrongWifi, UnstableWifi};
// What kind of trouble the recent windows show (SmartLinkPredictor.h).
enum class Regime : std::uint8_t {
    Unknown,        // not enough samples yet
    Stable,         // nothing beyond normal arrival noise
    PeriodicSpikes, // frame-gap spikes that repeat at a steady period (typical of Wi-Fi background scans)
    Congested,      // round trip or packet gaps rising together with the console lowering its rate, or loss
    WeakSignal,     // a Wi-Fi signal that is low or falling, with trouble in the stream
    Interrupted,    // no frames for most of a window
};
enum class Mode : std::uint8_t {Off, Observe, Apply};
enum class ModelKind : std::uint8_t {Statistics, Learned};

// Plain-language advice; at most two are given at a time (SmartLinkProfile.h).
enum class Advice : std::uint8_t {
    None,
    LinkIsFine,               // "Your connection is fine."
    UseCable,                 // "A network cable would make the stream steadier."
    MoveCloserOrFasterBand,   // "Move closer to the router, or use its 5 GHz network."
    LowerPictureRateNextTime, // "Next time, start with a lower picture quality (bitrate)."
    RaiseRateNextTime,        // "Your connection can carry a higher picture quality next time."
    BackgroundActivity,       // "Something on this PC or network pauses Wi-Fi every few seconds."
};

// Reason bits in Decision::reasons: why each output is what it is ("what
// would change" in observe mode). Stable values: they are logged as a number.
namespace reason {
inline constexpr std::uint32_t kLateFramesRescuable=1u<<0;  // frames were given up that a longer wait would have completed
inline constexpr std::uint32_t kLongClean=1u<<1;            // a long clean period: the wait narrowed or returned to "leave"
inline constexpr std::uint32_t kPredictedStall=1u<<2;       // the predictor expects a stall soon
inline constexpr std::uint32_t kAudioUnderrun=1u<<3;        // audio ran dry in the recent past
inline constexpr std::uint32_t kAudioDecay=1u<<4;           // the audio target is coming back down
inline constexpr std::uint32_t kLinkStalls=1u<<5;           // frame gaps far above the interval while the mean jitter is small
inline constexpr std::uint32_t kHeadroom=1u<<6;             // the link carried the requested rate with margin for minutes
inline constexpr std::uint32_t kRateBelowRequest=1u<<7;     // the console sends clearly less than was requested
inline constexpr std::uint32_t kLoss=1u<<8;                 // packets lost on the link
inline constexpr std::uint32_t kInterrupted=1u<<9;          // the stream stopped for most of a window
inline constexpr std::uint32_t kInputRejected=1u<<10;       // a sample was unusable (time backwards, NaN, empty window)
inline constexpr std::uint32_t kChangeBudget=1u<<11;        // a change was wanted but the per-minute limit held it
inline constexpr std::uint32_t kWeakSignal=1u<<12;          // Wi-Fi signal low or falling
inline constexpr std::uint32_t kPeriodic=1u<<13;            // periodic spikes: risk raised before the next one
inline constexpr std::uint32_t kWiredClean=1u<<14;          // wired and clean: everything stays as configured
inline constexpr std::uint32_t kLearnedModel=1u<<15;        // the stall risk came from the learned comparison model
// Appended later (values are only ever appended):
inline constexpr std::uint32_t kReorderHeld=1u<<16;         // the reorder wait is still off its configured value from earlier evidence
inline constexpr std::uint32_t kAudioHeld=1u<<17;           // the audio target is still raised from an earlier underrun or risk
inline constexpr std::uint32_t kFrequentReordering=1u<<18;  // many packets out of order: the reorder wait is left as configured
}

// One observation window. The session owner fills what it knows and leaves
// the rest at -1. Counts are for THIS window only (not cumulative).
struct Sample {
    std::int64_t endMs=-1;       // host time at the end of the window (monotonic ms); -1 unknown
    double windowMs=-1;          // how long the window was (nominally 200)
    Route route=Route::Unknown;

    // Packets (video and audio as the library counts them in its 200 ms congestion windows).
    std::int64_t packetsReceived=-1,packetsLost=-1;
    std::int64_t packetsReordered=-1; // arrived out of order but within the reorder wait
    std::int64_t packetsLate=-1;      // arrived after the reorder wait had given them up
    double packetGapMeanMs=-1,packetGapMaxMs=-1; // time between consecutive packet arrivals

    // Frames.
    std::int64_t framesArrived=-1;      // complete frames handed to the decoder
    std::int64_t framesIncomplete=-1;   // given up (FEC could not complete them)
    std::int64_t framesFecRecovered=-1; // completed only thanks to FEC
    double frameGapMaxMs=-1;            // largest time between two complete frames in the window
    double frameGapExcessMs=-1;         // frameGapMaxMs minus the nominal frame interval (>= 0)
    // The smallest extra reorder wait (ms, on top of the wait in effect) that
    // would have completed every frame given up in this window: 0 = none was
    // given up, or none would have been saved; negative = unknown.
    double lateRescueMs=-1;
    double reorderWaitMs=-1;            // the reorder wait in effect during the window

    // Rates and the console's own view (takion CONNECTIONQUALITY; it arrives
    // only every so often: repeat the last value or leave -1).
    double rttMs=-1;               // console-reported round trip
    double consoleTargetKbps=-1;   // the console's own bitrate target
    double measuredKbps=-1;        // video bits received in the window / windowMs
    double requestedKbps=-1;       // what was asked for at launch

    // Local pipeline.
    std::int64_t videoQueueDepth=-1; // frames waiting for the decoder at the end of the window
    double audioBufferedMs=-1;       // PCM queued for playout at the end of the window
    std::int64_t audioUnderruns=-1;  // playout gaps in the window

    // Wi-Fi (only on a Wi-Fi route; never a network name or address).
    double wifiSignalPercent=-1; // 0..100 as Windows reports it
    double wifiRssiDbm=0;        // negative dBm; >= 0 = unknown
    double wifiLinkMbps=-1;      // the adapter's current link rate
    double wifiBandGhz=-1;       // 2.4, 5 or 6

    // Appended after the first commit (fields are only ever appended).
    // Mean over the complete frames of the window of (arrival - nominal
    // cadence) in ms, on any fixed origin (e.g. arrival minus frameIndex x
    // interval, minus the session's first value). Only its CHANGE is used: a
    // rising value means a queue is building on the path (the GCC trendline
    // idea, docs/SMARTLINK_RESEARCH.md E). -1 = unknown (negative values are
    // not allowed: subtract a fixed origin that keeps it >= 0, or add 10000).
    double frameLateMeanMs=-1;

    bool operator==(const Sample&)const=default;
};

// A value from a Sample is usable only when finite and not negative.
inline bool known(double v)noexcept{return std::isfinite(v)&&v>=0;}
inline bool known(std::int64_t v)noexcept{return v>=0;}
inline bool rssiKnown(double v)noexcept{return std::isfinite(v)&&v<=-10&&v>-130;} // real RSSI is -10 .. -130 dBm

// The features both prediction models see (SmartLinkPredictor.h computes
// them; the learned comparison model in SmartLinkModel.h weighs the same ones).
inline constexpr std::size_t kFeatureCount=10;
using Features=std::array<double,kFeatureCount>;

// What the predictor says after a sample.
struct Prediction {
    // Chance (0..1) that a frame gap exceeding the nominal interval by more
    // than SmartLinkPredictor::kStallExcessMs occurs within the next
    // SmartLinkPredictor::kHorizonMs. Calibrated in the simulator only
    // (docs/SMARTLINK.md says what 0.5 means there).
    double stallRisk=0;
    Regime regime=Regime::Unknown;
    double gapExcessP95Ms=-1;   // streaming 95th percentile of frameGapExcessMs (about the last minute)
    double gapExcessMaxMs=-1;   // largest excess in about the last 5 s
    double jitterMeanMs=-1;     // mean frame-gap excess (about the last 10 s): what "mean jitter" means here
    double rttMeanMs=-1,rttMaxMs=-1; // round trip: smoothed mean, and the largest in about the last 5 s
    double spikePeriodMs=-1;    // the period of periodic spikes, when one was found
    double msToNextSpike=-1;    // expected time to the next periodic spike
    double signalTrendPerMin=0; // Wi-Fi signal percent per minute (negative = falling); 0 unknown
    Features features{};
    bool usable=false;          // the sample was accepted
};

// What SmartLink asks for. 0 / false everywhere = "the configured value":
// that is the answer on a clean link. In apply mode a 0 means RESTORE the
// configured reorder wait / audio target (it follows a raised value), never
// "keep whatever is set now". reset() the Engine at every stream start.
struct Decision {
    std::uint32_t reorderWindowMs=0;    // 0 = restore the configured wait, else 2, 4, 8 or 16
    std::uint32_t audioTargetMs=0;      // 0 = restore the sink's own target, else the audio reserve to keep (ms)
    bool networkLimited=false;          // stutter comes from the link: lowering graphics would not help
    bool linkHeadroom=false;            // the link carried the requested rate with margin: no need to reduce quality for it
    std::uint32_t suggestedStartKbps=0; // for the NEXT session; 0 = no suggestion
    std::uint32_t reasons=0;            // reason:: bits
    bool changed=false;                 // reorderWindowMs or audioTargetMs differs from the previous decision
    Profile profile=Profile::Unknown;
    Regime regime=Regime::Unknown;
    double stallRisk=0;
    // Summary of about the last 5 s, for the log line (-1 = unknown).
    double rttMs=-1,rttMaxMs=-1,targetKbps=-1,measuredKbps=-1,gapP95Ms=-1,gapMaxMs=-1,signalPercent=-1;
    std::int64_t late=0,lost=0; // packets, summed over about the last 5 s
    // Appended later (fields are only ever appended). The one piece of advice to
    // show (the interface translates it): None until there is enough evidence;
    // changes at most once a minute (SmartLinkController.h). Not in the log line
    // (its 400-byte buffer has no room for it at every field's maximum width).
    Advice advice=Advice::None;
};

static_assert(std::is_trivially_copyable_v<Sample>);
static_assert(std::is_trivially_copyable_v<Prediction>);
static_assert(std::is_trivially_copyable_v<Decision>);

inline constexpr const char* profileName(Profile p)noexcept{
    switch(p){case Profile::Wired:return "wired";case Profile::StrongWifi:return "strong-wifi";case Profile::UnstableWifi:return "unstable-wifi";default:return "unknown";}
}
inline constexpr const char* regimeName(Regime r)noexcept{
    switch(r){case Regime::Stable:return "stable";case Regime::PeriodicSpikes:return "periodic-spikes";case Regime::Congested:return "congested";
        case Regime::WeakSignal:return "weak-signal";case Regime::Interrupted:return "interrupted";default:return "unknown";}
}
inline constexpr const char* modeName(Mode m)noexcept{
    switch(m){case Mode::Observe:return "observe";case Mode::Apply:return "apply";default:return "off";}
}

// VEYRA_EXP_RP_SMARTLINK / slipstream.ini `smartlink=`: unset, empty, "0" or
// "off" = Off; "apply" (any case) = Apply; anything else that is set ("1",
// "observe", a typo) = Observe, which changes nothing.
inline Mode parseMode(std::optional<std::string_view> value)noexcept{
    if(!value)return Mode::Off;
    const std::string_view v=*value;
    const auto equalsNoCase=[&](std::string_view word){
        if(v.size()!=word.size())return false;
        for(std::size_t i=0;i<v.size();++i){char c=v[i];if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');if(c!=word[i])return false;}
        return true;
    };
    if(v.empty()||v=="0"||equalsNoCase("off"))return Mode::Off;
    if(equalsNoCase("apply"))return Mode::Apply;
    return Mode::Observe;
}
// VEYRA_EXP_RP_SMARTLINK_MODEL: exactly "learned" selects the learned
// comparison model; anything else (unset, "stat", a typo) the statistics.
inline ModelKind parseModel(std::optional<std::string_view> value)noexcept{
    return value&&*value=="learned"?ModelKind::Learned:ModelKind::Statistics;
}

// The log line, built without allocation:
// [slipstream-smartlink] profile= regime= risk= rttMs= rttMaxMs= targetKbps= measuredKbps= gapP95Ms= gapMaxMs= late= lost=
// reorderMs= audioTargetMs= netLimited= headroom= signal= mode= reasons=
// Unknown numbers are printed as -1. Locale independent (std::to_chars).
struct LogLine {
    std::array<char,400> text{};
    std::size_t size=0;
    std::string_view view()const noexcept{return {text.data(),size};}
};
namespace detail {
inline void put(LogLine& l,std::string_view s)noexcept{
    for(char c:s){if(l.size+1>=l.text.size())return;l.text[l.size++]=c;}
}
inline void putInt(LogLine& l,std::int64_t v)noexcept{
    std::array<char,24> b{};auto r=std::to_chars(b.data(),b.data()+b.size(),v);put(l,{b.data(),static_cast<std::size_t>(r.ptr-b.data())});
}
inline void putNum(LogLine& l,double v,int precision)noexcept{
    if(!std::isfinite(v)||v<0){put(l,"-1");return;}
    if(v>1e9)v=1e9;
    std::array<char,40> b{};auto r=std::to_chars(b.data(),b.data()+b.size(),v,std::chars_format::fixed,precision);
    if(r.ec!=std::errc{}){put(l,"-1");return;}
    put(l,{b.data(),static_cast<std::size_t>(r.ptr-b.data())});
}
}
inline LogLine toLogLine(const Decision& d,Mode mode)noexcept{
    LogLine l;using namespace detail;
    put(l,"[slipstream-smartlink] profile=");put(l,profileName(d.profile));
    put(l," regime=");put(l,regimeName(d.regime));
    put(l," risk=");putNum(l,d.stallRisk,2);
    put(l," rttMs=");putNum(l,d.rttMs,1);
    put(l," rttMaxMs=");putNum(l,d.rttMaxMs,1);
    put(l," targetKbps=");putNum(l,d.targetKbps,0);
    put(l," measuredKbps=");putNum(l,d.measuredKbps,0);
    put(l," gapP95Ms=");putNum(l,d.gapP95Ms,1);
    put(l," gapMaxMs=");putNum(l,d.gapMaxMs,1);
    put(l," late=");putInt(l,d.late<0?-1:d.late);
    put(l," lost=");putInt(l,d.lost<0?-1:d.lost);
    put(l," reorderMs=");putInt(l,d.reorderWindowMs);
    put(l," audioTargetMs=");putInt(l,d.audioTargetMs);
    put(l," netLimited=");put(l,d.networkLimited?"1":"0");
    put(l," headroom=");put(l,d.linkHeadroom?"1":"0");
    put(l," signal=");putNum(l,d.signalPercent,0);
    put(l," mode=");put(l,mode==Mode::Apply?"apply":"observe");
    put(l," reasons=");putInt(l,d.reasons);
    return l;
}
} // namespace veyra::remoteplay::smartlink
