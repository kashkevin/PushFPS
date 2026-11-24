// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental (sprint 5, third directive "Zero-Unnecessary-Delay Adaptive Input"): input
// performance profiles for keyboard and mouse on a PS5 stream, and the immediate mouse-to-stick
// transform they use. Portable, free of Windows and Qt, unit-tested (tests/remoteplay/KbmInputTests.inc).
//
//   legacy              exactly the behaviour before this file: MouseAim (KeyboardMousePad.h) with the
//                       profile file's smoothing window (8 ms built in). The default for everyone who
//                       does not choose.
//   ultra-low-latency   immediate transform, no smoothing, surplus motion beyond full stick dropped,
//                       the proven transmission schedule (4 ms pad poll).
//   adaptive-precision  immediate transform with calibrated sensitivity, dead-zone compensation and a
//                       per-context "precision then speed" curve; no temporal buffering.
//   custom              everything editable, including an optional smoothing window (0 = off) and a
//                       leading-edge transmission choice.
//
// Immediate means: the stick value of a poll is a function of the mouse counts that arrived since
// the previous poll (the current sample) and nothing older, except in two bounded, stated cases:
//   - a mouse that reports more slowly than the poll (125 / 250 Hz against a 4 ms poll): a poll that
//     falls between two reports repeats the last value for at most one measured report interval
//     (`holdUs`), and the next report's counts are divided by the time since the previous report.
//     Nothing waits; the stop is seen at most one report interval late (Linux model:
//     tests kbm_input_slow_mouse_*). With 500 Hz and faster mice at a 4 ms poll there is no hold.
//   - Carry saturation (a user choice): motion faster than a fully tilted stick is kept for at most
//     `carryMs` of full-speed turning. Drop (the default) discards it.
// The game's own stick response (dead zone, fastest turn, response curve, vertical ratio) is what the
// calibration measured and the profile file holds per way of playing (walk / aim / drive). A context
// with `game` on places the mouse speed on the inverse of that response, as the legacy path does,
// but from this poll's counts alone:  tilt = deadzone + (1 - deadzone) * inverse(curve(u)).
// With `game` off the context's own deadzone and fullSpeedCounts are used and the response is
// taken as straight. `maxTilt` caps the tilt (an idea from xcontrol's MouseStick.cs, MIT, one
// `min`; re-implemented): a game may add its own time-ramped turn in the outer ring of the stick.
// Nothing here aims, tracks, compensates recoil or times an action: every value transforms the
// player's own motion of this poll.
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
namespace veyra::remoteplay {

enum class KbmInputProfileKind : std::uint8_t {Legacy=0,UltraLowLatency=1,AdaptivePrecision=2,Custom=3};
enum class KbmContext : std::uint8_t {Walk=0,Aim=1,Sprint=2,Drive=3};
enum class KbmContextOverride : std::uint8_t {Auto=0,Walk=1,Aim=2,Sprint=3,Drive=4};
enum class KbmCurveKind : std::uint8_t {Linear=0,Power=1,PrecisionSpeed=2};
enum class KbmSaturation : std::uint8_t {Drop=0,Carry=1};
enum class KbmTransmit : std::uint8_t {Proven=0,LeadingEdge=1};
// How a poll's mouse speed is timed (stream "kbm", docs/drafts/KBM_AUDIT.md D1 / D4; both zero delay):
//   Poll    the counts of the poll divided by the poll interval (as before). A poll holds 4R/1000 +-1 reports
//           of an R Hz mouse, so a steady hand gives a stick value that jitters from poll to poll.
//   Report  the counts divided by the time between the last mouse report read before the previous poll and
//           the last one read before this poll (the capture time-stamps each read, stamp()). A poll without a
//           report repeats the last value only until the next report was due (one report interval and a
//           margin), at every polling rate; after that the stick is at rest.
enum class KbmTiming : std::uint8_t {Poll=0,Report=1};
inline constexpr std::size_t kKbmContexts=4;

inline const char* kbmInputProfileId(KbmInputProfileKind k)noexcept{
    switch(k){case KbmInputProfileKind::UltraLowLatency:return "ultra-low-latency";case KbmInputProfileKind::AdaptivePrecision:return "adaptive-precision";
    case KbmInputProfileKind::Custom:return "custom";default:return "legacy";}
}
inline std::optional<KbmInputProfileKind> kbmInputProfileFromId(std::string_view id)noexcept{
    for(const auto k:{KbmInputProfileKind::Legacy,KbmInputProfileKind::UltraLowLatency,KbmInputProfileKind::AdaptivePrecision,KbmInputProfileKind::Custom})
        if(id==kbmInputProfileId(k))return k;
    return std::nullopt;
}
inline const char* kbmContextId(KbmContext c)noexcept{
    switch(c){case KbmContext::Aim:return "aim";case KbmContext::Sprint:return "sprint";case KbmContext::Drive:return "drive";default:return "walk";}
}
inline const char* kbmContextOverrideId(KbmContextOverride o)noexcept{
    return o==KbmContextOverride::Auto?"auto":kbmContextId(KbmContext(std::uint8_t(o)-1));
}
inline std::optional<KbmContextOverride> kbmContextOverrideFromId(std::string_view id)noexcept{
    for(const auto o:{KbmContextOverride::Auto,KbmContextOverride::Walk,KbmContextOverride::Aim,KbmContextOverride::Sprint,KbmContextOverride::Drive})
        if(id==kbmContextOverrideId(o))return o;
    return std::nullopt;
}
inline const char* kbmCurveId(KbmCurveKind k)noexcept{return k==KbmCurveKind::Power?"power":k==KbmCurveKind::PrecisionSpeed?"precision-speed":"linear";}

// Maps the share u of full mouse speed (0..1) to a share of the stick travel above the dead zone.
struct KbmCurve {
    KbmCurveKind kind=KbmCurveKind::Linear;
    double exponent=1.0;   // Power: u^exponent, 0.25..4
    double kneeIn=0.25;    // PrecisionSpeed: up to kneeIn the line rises to kneeOut (fine aim),
    double kneeOut=0.15;   // then straight on to (1, 1) (fast turns)
    bool operator==(const KbmCurve&)const=default;
};
inline double kbmCurve(const KbmCurve& c,double u)noexcept{
    if(!(u>0))return 0;
    if(u>=1)return 1;
    switch(c.kind){
    case KbmCurveKind::Power:return std::pow(u,c.exponent);
    case KbmCurveKind::PrecisionSpeed:
        if(u<=c.kneeIn)return u*c.kneeOut/c.kneeIn;
        return c.kneeOut+(u-c.kneeIn)*(1-c.kneeOut)/(1-c.kneeIn);
    default:return u;
    }
}

struct KbmContextTuning {
    double sensX=1.0,sensY=1.0;        // 0.01..100, independent
    KbmCurve curve;                    // the player's own shaping of hand speed (Power: u^exponent; to cancel a game that turns with tilt^c by hand, the exponent is 1/c)
    double deadzone=0.12;              // inverse dead zone: the smallest motion starts here, 0..0.5 (`game` off)
    double fullSpeedCounts=4000;       // maximum turn speed: counts per second at full stick, 100..1e6 (`game` off)
    KbmSaturation saturation=KbmSaturation::Drop;
    double carryMs=0;                  // Carry only, 0..50
    double maxTilt=1.0;                // the stick is never tilted further than this, 0.1..1 (1: no cap)
    bool game=false;                   // dead zone, full speed, response and vertical ratio from the profile's calibration instead
    bool operator==(const KbmContextTuning&)const=default;
};
// The game's measured stick response, as the profile in use holds it (MouseAimConfig: deadzone,
// fullSpeedCounts, curve or curvePoints, sensitivityY / sensitivityX).
struct KbmGameResponse {
    double deadzone=0.12;
    double fullSpeedCounts=4000;
    double curve=1.0;                      // the game's turning speed ~ tilt^curve ...
    std::array<double,3> points{0,0,0};    // ... or its measured speed at 25 / 50 / 75 % of the tilt (all 0: the power)
    double yRatio=1.0;                     // vertical counts are scaled by this: equal counts, equal turn
};
// The share of the tilt above the dead zone that makes the game turn at the share `s` of its
// fastest turn. The same inverse as MouseAim::inverseResponse (tested against it).
inline double kbmGameInverse(const KbmGameResponse& g,double s)noexcept{
    s=std::clamp(s,0.0,1.0);
    if(!(g.points[0]>0))return std::pow(s,1.0/g.curve);
    const double knots[5]={0,g.points[0],g.points[1],g.points[2],1};
    std::size_t i=0;
    while(i<3&&s>knots[i+1])++i;
    return (double(i)+(s-knots[i])/(knots[i+1]-knots[i]))/4;
}
struct KbmInputSettings {
    KbmInputProfileKind kind=KbmInputProfileKind::Legacy;
    std::array<KbmContextTuning,kKbmContexts> contexts{};
    double adsFactor=1.0;              // the Aim context's sensitivity factor, 0.05..4
    double smoothingMs=0;              // Custom only: averaging window, 0..50, 0 = off
    KbmContextOverride override_=KbmContextOverride::Auto;
    KbmTransmit transmit=KbmTransmit::Proven;   // Custom only
    KbmTiming timing=KbmTiming::Poll;          // immediate profiles only; Poll = as before
    int stickSteps=16;                         // 8: whole 8-bit stick steps, dead zone rounded up to a step; 16 = as before
    KbmContextTuning& operator[](KbmContext c)noexcept{return contexts[std::size_t(c)];}
    const KbmContextTuning& operator[](KbmContext c)const noexcept{return contexts[std::size_t(c)];}
    bool immediate()const noexcept{return kind!=KbmInputProfileKind::Legacy;}
    bool operator==(const KbmInputSettings&)const=default;
};

inline KbmInputSettings kbmSanitized(KbmInputSettings s){
    const auto within=[](double v,double low,double high,double fallback){return std::isfinite(v)?std::clamp(v,low,high):fallback;};
    if(std::uint8_t(s.kind)>3)s.kind=KbmInputProfileKind::Legacy;
    if(std::uint8_t(s.override_)>4)s.override_=KbmContextOverride::Auto;
    if(std::uint8_t(s.transmit)>1)s.transmit=KbmTransmit::Proven;
    if(std::uint8_t(s.timing)>1)s.timing=KbmTiming::Poll;
    if(s.stickSteps!=8)s.stickSteps=16;
    for(auto& t:s.contexts){
        t.sensX=within(t.sensX,0.01,100,1);t.sensY=within(t.sensY,0.01,100,1);
        if(std::uint8_t(t.curve.kind)>2)t.curve.kind=KbmCurveKind::Linear;
        t.curve.exponent=within(t.curve.exponent,0.25,4,1);
        t.curve.kneeIn=within(t.curve.kneeIn,0.05,0.95,0.25);
        t.curve.kneeOut=within(t.curve.kneeOut,0.01,0.99,0.15);
        t.deadzone=within(t.deadzone,0,0.5,0.12);
        t.fullSpeedCounts=within(t.fullSpeedCounts,100,1e6,4000);
        if(std::uint8_t(t.saturation)>1)t.saturation=KbmSaturation::Drop;
        t.carryMs=within(t.carryMs,0,50,0);
        t.maxTilt=within(t.maxTilt,0.1,1,1);
    }
    s.adsFactor=within(s.adsFactor,0.05,4,1);
    s.smoothingMs=within(s.smoothingMs,0,50,0);
    // Only Custom may smooth or change the schedule; the two named profiles never do.
    if(s.kind!=KbmInputProfileKind::Custom){s.smoothingMs=0;s.transmit=KbmTransmit::Proven;}
    if(s.kind==KbmInputProfileKind::UltraLowLatency)for(auto& t:s.contexts){t.saturation=KbmSaturation::Drop;t.carryMs=0;}
    return s;
}

// The built-in numbers of a profile. Untuned starting points: no number was tried on a PS5.
inline KbmInputSettings kbmInputPreset(KbmInputProfileKind kind){
    KbmInputSettings s;s.kind=kind;
    // Every new profile reads the game's calibration (zero delay: numbers, not a filter); with an
    // uncalibrated profile file those are the same built-in dead zone and full speed as here.
    if(kind!=KbmInputProfileKind::Legacy)for(auto& t:s.contexts)t.game=true;
    if(kind==KbmInputProfileKind::AdaptivePrecision){
        // On foot and sprinting the hand's distance is kept (straight: with the game's inverse a count
        // turns the same angle at any speed). While aiming, slow movement counts for less (fine aim)
        // and fast movement catches up; in a car the camera is calmer around the centre. A curve that
        // is not straight gives up "equal distance, equal turn": see the model's numbers in the guide.
        // Mild on purpose: untuned guesses, and the Linux model's player (who expects equal distance,
        // equal turn) needs more strokes with any curve that is not straight.
        s[KbmContext::Aim].curve={KbmCurveKind::PrecisionSpeed,1.0,0.25,0.20};
        s[KbmContext::Drive].curve={KbmCurveKind::Power,1.25,0.25,0.15};
        s.adsFactor=0.6;
    }else if(kind==KbmInputProfileKind::UltraLowLatency||kind==KbmInputProfileKind::Custom){
        s.adsFactor=0.6;
    }
    return kbmSanitized(s);
}

// The simpler path the self-check falls back to (KbmInputStats.h): ultra-low-latency with the same
// sensitivities, dead zone, full speed, cap and game calibration; the player's curve straight, no
// carry, no smoothing, the proven schedule.
inline KbmInputSettings kbmFallbackSettings(KbmInputSettings s){
    s.kind=KbmInputProfileKind::UltraLowLatency;s.smoothingMs=0;s.transmit=KbmTransmit::Proven;
    for(auto& t:s.contexts){t.curve=KbmCurve{};t.saturation=KbmSaturation::Drop;t.carryMs=0;}
    return kbmSanitized(s);
}

// Leading-edge transmission (transmit = LeadingEdge, an experiment): at most one early pad state
// between two polls, so the states handed to the sender can never exceed two per poll interval.
// With times: an early state is built only on a fresh poll state, one at most two poll intervals
// old (a stalled poll, a latency test that took the pad, or an experiment switched off and on
// leaves a state that no longer describes the pad); reset() forgets the poll altogether.
class KbmEarlyGate {
public:
    bool take()noexcept{if(used_)return false;used_=true;return true;}
    void polled()noexcept{used_=false;}
    void polled(std::uint64_t nowUs,std::uint64_t pollUs)noexcept{used_=false;lastPollUs_=nowUs;pollUs_=pollUs;}
    bool fresh(std::uint64_t nowUs)const noexcept{
        return lastPollUs_>0&&nowUs>=lastPollUs_&&nowUs-lastPollUs_<=2*std::max<std::uint64_t>(pollUs_,1000);
    }
    bool take(std::uint64_t nowUs)noexcept{return fresh(nowUs)&&take();}
    void reset()noexcept{used_=false;lastPollUs_=0;}
private:
    bool used_=false;
    std::uint64_t lastPollUs_=0,pollUs_=4000;
};

// --- Text form: "key=value;key=value" (preference kbmInputCustom, profile file input.* lines) ---------
struct KbmInputParse {KbmInputSettings settings;unsigned applied=0,refused=0;std::string firstProblem;};
namespace kbminput {
inline std::string_view trim(std::string_view s)noexcept{
    while(!s.empty()&&(s.front()==' '||s.front()=='\t'||s.front()=='\r'||s.front()=='\n'))s.remove_prefix(1);
    while(!s.empty()&&(s.back()==' '||s.back()=='\t'||s.back()=='\r'||s.back()=='\n'))s.remove_suffix(1);
    return s;
}
inline bool number(std::string_view text,double low,double high,double& out)noexcept{
    text=trim(text);
    if(text.empty())return false;
    double v=0;
    const auto r=std::from_chars(text.data(),text.data()+text.size(),v);
    if(r.ec!=std::errc{}||r.ptr!=text.data()+text.size()||!std::isfinite(v)||v<low||v>high)return false;
    out=v;return true;
}
inline std::string format(double v){
    char buffer[32];
    const auto r=std::to_chars(buffer,buffer+sizeof(buffer),std::round(v*10000)/10000);
    return r.ec==std::errc{}?std::string(buffer,r.ptr):std::string("0");
}
// One key on the contexts it names (walk / aim / sprint / drive / all).
inline bool applyTuning(KbmInputSettings& s,std::string_view ctx,std::string_view field,std::string_view value){
    std::array<bool,kKbmContexts> on{};
    if(ctx=="all")on.fill(true);
    else{
        bool found=false;
        for(std::size_t i=0;i<kKbmContexts;++i)if(ctx==kbmContextId(KbmContext(std::uint8_t(i)))){on[i]=true;found=true;}
        if(!found)return false;
    }
    double v=0;
    KbmContextTuning probe;
    if(field=="sensX"||field=="sensY"){if(!number(value,0.01,100,v))return false;}
    else if(field=="exponent"){if(!number(value,0.25,4,v))return false;}
    else if(field=="kneeIn"){if(!number(value,0.05,0.95,v))return false;}
    else if(field=="kneeOut"){if(!number(value,0.01,0.99,v))return false;}
    else if(field=="deadzone"){if(!number(value,0,0.5,v))return false;}
    else if(field=="fullSpeed"){if(!number(value,100,1e6,v))return false;}
    else if(field=="carryMs"){if(!number(value,0,50,v))return false;}
    else if(field=="maxTilt"){if(!number(value,0.1,1,v))return false;}
    else if(field=="game"){if(!number(value,0,1,v)||(v!=0&&v!=1))return false;}
    else if(field=="curve"){
        const auto t=trim(value);
        if(t=="linear")probe.curve.kind=KbmCurveKind::Linear;else if(t=="power")probe.curve.kind=KbmCurveKind::Power;
        else if(t=="precision-speed")probe.curve.kind=KbmCurveKind::PrecisionSpeed;else return false;
    }else if(field=="saturation"){
        const auto t=trim(value);
        if(t=="drop")probe.saturation=KbmSaturation::Drop;else if(t=="carry")probe.saturation=KbmSaturation::Carry;else return false;
    }else return false;
    for(std::size_t i=0;i<kKbmContexts;++i){
        if(!on[i])continue;
        auto& t=s.contexts[i];
        if(field=="sensX")t.sensX=v;else if(field=="sensY")t.sensY=v;else if(field=="exponent")t.curve.exponent=v;
        else if(field=="kneeIn")t.curve.kneeIn=v;else if(field=="kneeOut")t.curve.kneeOut=v;else if(field=="deadzone")t.deadzone=v;
        else if(field=="fullSpeed")t.fullSpeedCounts=v;else if(field=="carryMs")t.carryMs=v;
        else if(field=="maxTilt")t.maxTilt=v;else if(field=="game")t.game=v!=0;
        else if(field=="curve")t.curve.kind=probe.curve.kind;else if(field=="saturation")t.saturation=probe.saturation;
    }
    return true;
}
} // namespace kbminput

inline constexpr std::size_t kKbmInputTextMaxBytes=2048;

// One "key=value". True when it was understood and in range; out of range is refused, not clamped.
inline bool kbmApplyInputSetting(KbmInputSettings& s,std::string_view key,std::string_view value){
    key=kbminput::trim(key);value=kbminput::trim(value);
    double v=0;
    if(key=="profile"){const auto k=kbmInputProfileFromId(value);if(!k)return false;s.kind=*k;return true;}
    if(key=="ads"){if(!kbminput::number(value,0.05,4,v))return false;s.adsFactor=v;return true;}
    if(key=="smoothingMs"){if(!kbminput::number(value,0,50,v))return false;s.smoothingMs=v;return true;}
    if(key=="context"){const auto o=kbmContextOverrideFromId(value);if(!o)return false;s.override_=*o;return true;}
    if(key=="transmit"){
        if(value=="proven")s.transmit=KbmTransmit::Proven;else if(value=="leading-edge")s.transmit=KbmTransmit::LeadingEdge;else return false;
        return true;
    }
    if(key=="timing"){
        if(value=="poll")s.timing=KbmTiming::Poll;else if(value=="report")s.timing=KbmTiming::Report;else return false;
        return true;
    }
    if(key=="stickSteps"){
        if(value=="8")s.stickSteps=8;else if(value=="16")s.stickSteps=16;else return false;
        return true;
    }
    const auto dot=key.find('.');
    if(dot==std::string_view::npos)return false;
    return kbminput::applyTuning(s,key.substr(0,dot),key.substr(dot+1),value);
}
// `base` supplies every value the text does not name. The result is sanitized (the named profiles
// cannot smooth even when the text says so).
inline KbmInputParse parseKbmInputSettings(std::string_view text,KbmInputSettings base){
    KbmInputParse out;out.settings=base;
    if(text.size()>kKbmInputTextMaxBytes){out.refused=1;out.firstProblem="longer than 2048 bytes";return out;}
    while(!text.empty()){
        const auto end=text.find(';');
        const auto item=kbminput::trim(text.substr(0,end));
        text=end==std::string_view::npos?std::string_view{}:text.substr(end+1);
        if(item.empty())continue;
        const auto eq=item.find('=');
        if(eq==std::string_view::npos||!kbmApplyInputSetting(out.settings,item.substr(0,eq),item.substr(eq+1))){
            ++out.refused;
            if(out.firstProblem.empty())out.firstProblem=std::string(item.substr(0,64));
            continue;
        }
        ++out.applied;
    }
    out.settings=kbmSanitized(out.settings);
    return out;
}
inline std::string kbmInputSettingsText(const KbmInputSettings& s){
    using kbminput::format;
    std::string out="profile=";out+=kbmInputProfileId(s.kind);
    out+=";ads="+format(s.adsFactor)+";smoothingMs="+format(s.smoothingMs)+";context="+kbmContextOverrideId(s.override_);
    out+=std::string(";transmit=")+(s.transmit==KbmTransmit::LeadingEdge?"leading-edge":"proven");
    // Written only when not the default, so that settings from before them read and write exactly as before.
    if(s.timing==KbmTiming::Report)out+=";timing=report";
    if(s.stickSteps==8)out+=";stickSteps=8";
    for(std::size_t i=0;i<kKbmContexts;++i){
        const auto& t=s.contexts[i];
        const std::string c=kbmContextId(KbmContext(std::uint8_t(i)));
        out+=";"+c+".sensX="+format(t.sensX)+";"+c+".sensY="+format(t.sensY)+";"+c+".curve="+kbmCurveId(t.curve.kind);
        out+=";"+c+".exponent="+format(t.curve.exponent)+";"+c+".kneeIn="+format(t.curve.kneeIn)+";"+c+".kneeOut="+format(t.curve.kneeOut);
        out+=";"+c+".deadzone="+format(t.deadzone)+";"+c+".fullSpeed="+format(t.fullSpeedCounts);
        out+=";"+c+".saturation="+(t.saturation==KbmSaturation::Carry?"carry":"drop")+";"+c+".carryMs="+format(t.carryMs);
        out+=";"+c+".maxTilt="+format(t.maxTilt)+";"+c+".game="+(t.game?"1":"0");
    }
    return out;
}

// --- The immediate transform -------------------------------------------------------------------
// Raw mouse counts in, one right-stick value per pad poll out. Sensitivity is applied at the poll,
// with the context of that poll, so a context change acts on the very next sample.
class KbmImmediateAim {
public:
    struct Output {std::int16_t x=0,y=0;bool moving=false,received=false,saturated=false,held=false;};
    static constexpr std::size_t kWindow=32;   // Custom smoothing: at most this many polls
    static constexpr double kSteps=127.0;      // stickSteps=8: whole steps of an 8-bit stick, each side of the centre
    void add(int dx,int dy)noexcept{rawX_+=dx;rawY_+=dy;}
    // When the counts just added were read (a steady clock in microseconds). Only timing=report uses it.
    void stamp(std::uint64_t us)noexcept{if(readFirstUs_==0)readFirstUs_=us;readLastUs_=std::max(readLastUs_,us);}
    bool hasInput()const noexcept{return rawX_!=0||rawY_!=0;}
    // Drops everything on its way (focus loss, release): nothing turns after this.
    void reset()noexcept{rawX_=rawY_=0;carryX_=carryY_=0;last_={};lastMoveUs_=0;held_=false;count_=0;head_=0;
        readFirstUs_=readLastUs_=prevReportUs_=lastReportUs_=0;stepRes_=0;stepDirX_=stepDirY_=0;stepAtUs_=0;}
    // One report interval of a mouse slower than the poll, else 0 (KbmMouseRate decides). timing=poll only.
    void setHoldUs(std::uint64_t us)noexcept{holdUs_=std::min<std::uint64_t>(us,50000);}
    std::uint64_t holdUs()const noexcept{return holdUs_;}
    // timing=report: the mouse's report interval (KbmMouseRate's class; 0 = not known yet) and the pad poll's.
    void setReportIntervalUs(std::uint64_t us)noexcept{reportUs_=std::min<std::uint64_t>(us,50000);}
    void setPollUs(std::uint64_t us)noexcept{pollUs_=std::clamp<std::uint64_t>(us,500,50000);}
    std::uint64_t reportIntervalUs()const noexcept{return reportUs_;}
    // timing=report: how long after the last report a poll without one still repeats the last value. Only for
    // a mouse faster than the poll (else 0), and never a whole poll interval: the next report was due by then.
    std::uint64_t reportHoldUs()const noexcept{
        if(reportUs_==0||reportUs_>=pollUs_)return 0;
        return std::min(reportUs_+std::min<std::uint64_t>(250,reportUs_/2),pollUs_-1);
    }
    // `scale`: everything that multiplies both axes this poll (the ADS factor, the fine key).
    // `game`: the calibrated response of the profile in use, for a context with `game` on (else null).
    // `timing`, `stickSteps`: KbmInputSettings' (Poll and 16 = the behaviour before they existed).
    Output step(const KbmContextTuning& t,double scale,bool invertX,bool invertY,double smoothingMs,
                std::uint64_t elapsedUs,std::uint64_t nowUs,const KbmGameResponse* game=nullptr,
                KbmTiming timing=KbmTiming::Poll,int stickSteps=16)noexcept{
        const double dtUs=std::clamp(double(elapsedUs),500.0,50000.0);
        const bool steps=stickSteps==8,reportTiming=timing==KbmTiming::Report;
        const double deadzone=game?game->deadzone:t.deadzone,fullSpeed=game?game->fullSpeedCounts:t.fullSpeedCounts;
        double cx=double(rawX_)*t.sensX*scale*(invertX?-1:1)+carryX_;
        double cy=double(rawY_)*t.sensY*(game?game->yRatio:1.0)*scale*(invertY?-1:1)+carryY_;
        const bool received=rawX_!=0||rawY_!=0;
        const std::uint64_t readFirst=readFirstUs_,readLast=readLastUs_;
        rawX_=rawY_=0;carryX_=carryY_=0;readFirstUs_=readLastUs_=0;
        Output out;out.received=received;
        if(cx==0&&cy==0){
            if(reportTiming){
                // Until the next report of a mouse faster than the poll was due: repeat. Else at rest at once,
                // whatever the mouse's rate: nothing is shown after one poll interval past the last report.
                const std::uint64_t hold=reportHoldUs();
                if(last_.moving&&!received&&hold>0&&lastReportUs_>0&&nowUs>=lastReportUs_&&nowUs-lastReportUs_<hold){
                    held_=true;out=last_;out.received=false;out.held=true;out.saturated=false;return out;
                }
                held_=false;last_={};   // stickSteps=8: its remainder waits (stepOutput decides whether it still counts)
                prevReportUs_=received?readLast:0;   // counts that cancelled out still time the next poll
                if(smoothingMs<=0){count_=0;return out;}
            }else{
                // A poll between two reports of a slow mouse: repeat, for at most one report interval.
                if(last_.moving&&holdUs_>0&&nowUs>=lastMoveUs_&&nowUs-lastMoveUs_<holdUs_){
                    held_=true;out=last_;out.received=false;out.held=true;out.saturated=false;return out;
                }
                held_=false;last_={};
                if(smoothingMs<=0){count_=0;return out;}
            }
        }
        // The time these counts cover: one poll, or since the previous report when the poll
        // before this one only repeated.
        double spanUs=dtUs;
        if(reportTiming){
            // The counts of the reports read since the previous poll's last report cover the time between
            // that report and this poll's last one (a sensor adds up its motion between two reports): when the
            // reports came one after the other (the first of this poll within one report interval and a margin of
            // the previous poll's last), that time divides them, bounded by the same slack either way of the poll
            // interval. Otherwise (the motion starts, or reports sparser than the mouse's rate: a slow hand) the
            // poll interval, as timing=poll: the stick rests between such reports (the stop rule), so the polls
            // that do carry counts must show them over one poll for the distance to be the hand's.
            if(received&&readLast>0&&reportUs_>0&&reportUs_<pollUs_){
                const double slack=double(reportHoldUs());
                const bool dense=prevReportUs_>0&&readFirst>=prevReportUs_&&double(readFirst-prevReportUs_)<=slack;
                if(dense)spanUs=std::clamp(double(readLast-prevReportUs_),std::max(250.0,dtUs-slack),std::min(50000.0,dtUs+slack));
            }
            if(received){held_=false;prevReportUs_=readLast;lastReportUs_=readLast>0?readLast:nowUs;}
        }else{
            if(received&&held_&&lastMoveUs_>0&&nowUs>lastMoveUs_)spanUs=std::clamp(double(nowUs-lastMoveUs_),dtUs,double(holdUs_)+dtUs);
            if(received){held_=false;lastMoveUs_=nowUs;}
        }
        const double budget=fullSpeed*spanUs/1e6;   // counts a fully tilted stick turns in this span
        // Tilt for a share u of full speed: the player's curve, then (with `game`) the inverse of the game's response.
        const auto tiltAt=[&](double share)noexcept{
            const double shaped=kbmCurve(t.curve,share);
            return std::min(1.0,deadzone+(1-deadzone)*(game?kbmGameInverse(*game,shaped):shaped));
        };
        // The cap never sits inside the dead zone (that would be a stick that cannot turn the camera).
        const double cap=std::max(t.maxTilt,std::min(1.0,deadzone+0.05));
        // The share of full speed at which the tilt reaches the cap (1 without a cap): what the stick can turn at most.
        const auto capShare=[&]()noexcept{
            if(cap>=1)return 1.0;
            double low=0,high=1;
            for(int i=0;i<24;++i){const double mid=(low+high)/2;if(tiltAt(mid)<cap)low=mid;else high=mid;}
            return high;
        };
        // More than the stick may show: the surplus is dropped, or (Carry) kept for at most carryMs of
        // turning at the stick's limit. `turned`: the counts the stick turns in this span at that limit.
        const auto keepSurplus=[&](double amount,double turned,double share)noexcept{
            if(t.saturation!=KbmSaturation::Carry||!(t.carryMs>0))return;
            const double keep=std::clamp(amount-turned,0.0,share*fullSpeed*t.carryMs/1000.0);
            carryX_=cx/amount*keep;carryY_=cy/amount*keep;
        };
        // Optional averaging (Custom only), in the legacy path's order: first the stick's limit (so
        // that carried counts are never counted twice), then the mean of the polls inside the window.
        if(smoothingMs>0){
            const double asked=std::hypot(cx,cy);
            if(asked>0){
                const double share=capShare(),limit=share*budget;
                if(asked>limit){out.saturated=true;keepSurplus(asked,limit,share);cx*=limit/asked;cy*=limit/asked;}
            }
            window_[head_]={spanUs,cx,cy};head_=(head_+1)%kWindow;count_=std::min(kWindow,count_+1);
            double time=0,sx=0,sy=0;
            for(std::size_t i=0;i<count_;++i){
                const auto& e=window_[(head_+kWindow-1-i)%kWindow];
                if(i>0&&time+e.us>smoothingMs*1000+1e-6)break;
                time+=e.us;sx+=e.x;sy+=e.y;
            }
            // The output covers spanUs of the averaged speed.
            cx=sx/time*spanUs;cy=sy/time*spanUs;
            if(cx==0&&cy==0){last_={};return out;}
        }
        const double length=std::hypot(cx,cy);
        const double u=std::min(1.0,length/budget);
        double tilt=tiltAt(u);
        if(length>budget||tilt>cap){
            out.saturated=true;
            if(!(smoothingMs>0)){const double share=capShare();keepSurplus(length,share*budget,share);}
            tilt=std::min(tilt,cap);
        }
        if(steps)stepOutput(cx/length*tilt,cy/length*tilt,deadzone,nowUs,out);
        else{
            out.x=stick(cx/length*tilt);out.y=stick(cy/length*tilt);
            // The smallest motion must leave the console's dead zone: never round a moving stick to 0.
            if(out.x==0&&out.y==0){if(std::abs(cx)>=std::abs(cy))out.x=cx>0?1:-1;else out.y=cy>0?1:-1;}
        }
        out.moving=true;
        last_=out;
        return out;
    }
private:
    static std::int16_t stick(double v)noexcept{return std::int16_t(std::clamp(std::lround(v*32767.0),-32767L,32767L));}
    // Step `s` of 127 as the 16-bit value sent: a whole multiple of 256, which stays the same step whether the
    // console cuts, rounds or shifts it down (the legacy MouseAim::stickValue rule).
    static std::int16_t stepValue(long s)noexcept{
        if(s>=127)return 32767;
        if(s<=-127)return -32767;
        return std::int16_t(s*256);
    }
    // stickSteps=8: the tilt (x, y in -1..1) as whole 8-bit steps, so that a console which reads the stick in 8 bits
    // sees exactly what was meant, the same to the left as to the right. A moving stick is always past the dead zone
    // (its first whole step beyond it); a motion too slow even for that step this poll waits for the next (the stick
    // rests), so that slow aim turns the distance the hand moved instead of jumping to the dead zone's edge every poll.
    // The part above the dead zone that falls between two steps (at most one step) is used only by a later poll that
    // carries counts, never by a poll without: nothing turns while the hand is still. It is dropped when the motion
    // turns by more than 90 degrees (exactly 90 keeps it), after kRestUs without counts (a pause, not the gap between two reports of a slow
    // hand, which keeps slow aim's distance) and at a release (reset). A step never points against this poll's motion.
    static constexpr std::uint64_t kRestUs=50000;
    void stepOutput(double x,double y,double deadzone,std::uint64_t nowUs,Output& out)noexcept{
        const double tilt=std::hypot(x,y),dz=deadzone*kSteps;
        if(stepRes_!=0&&(nowUs<stepAtUs_||nowUs-stepAtUs_>kRestUs||x*stepDirX_+y*stepDirY_<0))stepRes_=0;   // a right angle keeps it (a slow diagonal alternates axes)
        if(tilt>0){stepDirX_=x/tilt;stepDirY_=y/tilt;stepAtUs_=nowUs;}
        const double first=std::floor(dz+1e-9)+1;            // the first whole step beyond the dead zone
        const double want=std::max(0.0,tilt*kSteps-dz)+stepRes_;
        long sx=0,sy=0;double given=0;
        if(tilt>0&&want>=0.5*(first-dz)){
            const double size=std::clamp(std::max(first,std::round(want+dz)),1.0,kSteps);
            sx=std::lround(x/tilt*size);sy=std::lround(y/tilt*size);
            if((sx>0&&!(x>0))||(sx<0&&!(x<0)))sx=0;
            if((sy>0&&!(y>0))||(sy<0&&!(y<0)))sy=0;
            // Rounding each axis of a diagonal can land on the dead zone: one step outward on the larger axis.
            for(int i=0;i<4&&std::hypot(double(sx),double(sy))<=dz+1e-9;++i){
                if(std::abs(x)>=std::abs(y))sx+=x>0?1:-1;else sy+=y>0?1:-1;
            }
            sx=std::clamp(sx,-127L,127L);sy=std::clamp(sy,-127L,127L);
            given=std::max(0.0,std::hypot(double(sx),double(sy))-dz);
        }
        stepRes_=std::clamp(want-given,-1.0,1.0);
        out.x=stepValue(sx);out.y=stepValue(sy);
    }
    struct Entry {double us=0,x=0,y=0;};
    std::int64_t rawX_=0,rawY_=0;
    double carryX_=0,carryY_=0;
    std::uint64_t holdUs_=0,lastMoveUs_=0;
    bool held_=false;
    Output last_{};
    std::array<Entry,kWindow> window_{};
    std::size_t count_=0,head_=0;
    // timing=report and stickSteps=8.
    std::uint64_t readFirstUs_=0,readLastUs_=0,prevReportUs_=0,lastReportUs_=0,reportUs_=0,pollUs_=4000;
    double stepRes_=0;   // steps above the dead zone not yet shown (stickSteps=8)
    double stepDirX_=0,stepDirY_=0;   // the direction of the last motion that used it, and when
    std::uint64_t stepAtUs_=0;
};

// The hold a mouse of `rateHz` needs at a `pollMs` poll: one report interval when reports come more
// slowly than polls, else none.
inline std::uint64_t kbmHoldUsFor(int rateHz,int pollMs)noexcept{
    if(rateHz<=0||pollMs<=0)return 0;
    const std::uint64_t interval=std::uint64_t(1000000/rateHz);
    return interval>std::uint64_t(pollMs)*1000?interval:0;
}
} // namespace veyra::remoteplay
