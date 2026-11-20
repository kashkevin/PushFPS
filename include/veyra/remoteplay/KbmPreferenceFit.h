// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental (sprint 5): fitting an input profile's numbers to the player's own hand, from a few
// test motions the Mouse & Keyboard page asks for. Portable and unit-tested
// (tests/remoteplay/KbmInputTests.inc). The game's side (dead zone, fastest turn, response) is the
// game calibration's job (KbmCalibration.h); this file looks at the hand only.
//
// What the player does, each 3 to 10 times, with the stick held neutral meanwhile:
//   turn    "move the mouse as far as you want a quarter turn to take"
//   flick   "your fastest comfortable flick"
//   slow    "your slowest steady movement, as when lining up a far target"
// What comes out (every rule is a formula below; the same input always gives the same result,
// and `explanation` says in words which rule gave which number):
//   counts per full turn   4 x the median quarter-turn distance
//   maximum turn speed     known full-turn time T of the game (from the game calibration):
//                            fullSpeedCounts = countsPer360 / T, sensitivity 1: the wanted distance
//                            turns the camera once around at any speed the stick can follow;
//                          T unknown: fullSpeedCounts = the 90th percentile of the flick speeds
//                            (the fastest comfortable flick just reaches a fully tilted stick)
//   curve                  two straight segments ("precision then speed") whose knee sits at twice
//                          the slowest steady speed (10th percentile), with the precision segment at
//                          60 % of a straight line; a straight line when the slow and the fast speed
//                          are too close for two zones (knee above half of full speed) or no slow
//                          motions were given
// The dead zone, the saturation choice and the vertical / horizontal ratio are kept from `base`.
// Nothing here runs during play, measures the picture or changes a profile by itself: the page
// shows the result and the player applies it.
#include "KbmInputProfile.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
namespace veyra::remoteplay {

enum class KbmFitKind : std::uint8_t {Turn=0,Flick=1,Slow=2};
struct KbmFitMotion {double counts=0;double seconds=0;};   // one test motion: distance in mouse counts, duration
struct KbmFitInput {
    std::vector<double> turnCounts;        // quarter-turn distances, 3..10
    std::vector<KbmFitMotion> flicks;      // fastest comfortable flicks, 3..10
    std::vector<KbmFitMotion> slow;        // slowest steady moves, 3..10 (optional: none keeps a straight curve)
    double gameFullTurnSeconds=0;          // the game's time for 360 degrees at full stick, 0: unknown
};
struct KbmFitResult {
    bool ok=false;
    std::string reason;                    // "" or why not: need-turns, need-flicks, bad-motion
    KbmContextTuning tuning;
    double countsPer360=0;
    double flickP90CountsPerSec=0,slowP10CountsPerSec=0;
    std::string explanation;               // plain English, for the log; the page words its own text from the numbers
};
inline constexpr std::size_t kKbmFitMinMotions=3,kKbmFitMaxMotions=10;
inline constexpr double kKbmFitKneeFactor=2.0,kKbmFitPrecisionShare=0.6,kKbmFitKneeMax=0.5;

namespace kbmfit {
// Nearest-rank percentile of a small list (sorted copy).
inline double percentile(std::vector<double> values,double p){
    if(values.empty())return 0;
    std::sort(values.begin(),values.end());
    const double rank=std::ceil(std::clamp(p,0.0,1.0)*double(values.size()));
    const std::size_t index=rank<1?0:std::min(values.size()-1,static_cast<std::size_t>(rank)-1);
    return values[index];
}
inline double median(std::vector<double> values){
    if(values.empty())return 0;
    std::sort(values.begin(),values.end());
    const std::size_t n=values.size();
    return n%2?values[n/2]:(values[n/2-1]+values[n/2])/2;
}
inline double rounded(double v)noexcept{return std::round(v*10000)/10000;}   // what the text form keeps
inline std::string number(double v,int decimals){char buffer[48];std::snprintf(buffer,sizeof(buffer),"%.*f",decimals,v);return buffer;}
// Speeds of usable motions (finite, a distance and a duration of at least a millisecond).
inline bool speeds(const std::vector<KbmFitMotion>& motions,std::vector<double>& out){
    for(const auto& m:motions){
        if(!std::isfinite(m.counts)||!std::isfinite(m.seconds)||!(m.counts>0)||!(m.seconds>=0.001))return false;
        out.push_back(m.counts/m.seconds);
        if(out.size()>=kKbmFitMaxMotions)break;
    }
    return true;
}
} // namespace kbmfit

inline KbmFitResult kbmFitPreference(const KbmFitInput& given,const KbmContextTuning& base){
    using namespace kbmfit;
    KbmFitResult out;out.tuning=base;
    std::vector<double> turns;
    for(const double c:given.turnCounts){
        if(!std::isfinite(c)||!(c>0)){out.reason="bad-motion";return out;}
        if(turns.size()<kKbmFitMaxMotions)turns.push_back(c);
    }
    std::vector<double> flick,slow;
    if(!speeds(given.flicks,flick)||!speeds(given.slow,slow)){out.reason="bad-motion";return out;}
    if(turns.size()<kKbmFitMinMotions){out.reason="need-turns";return out;}
    if(flick.size()<kKbmFitMinMotions){out.reason="need-flicks";return out;}
    out.countsPer360=4*median(turns);
    out.flickP90CountsPerSec=percentile(flick,0.9);
    out.slowP10CountsPerSec=slow.size()>=kKbmFitMinMotions?percentile(slow,0.1):0;
    auto& t=out.tuning;
    const double ratio=base.sensX>0?base.sensY/base.sensX:1.0;
    const bool timed=std::isfinite(given.gameFullTurnSeconds)&&given.gameFullTurnSeconds>0;
    std::string why;
    if(timed){
        t.fullSpeedCounts=std::clamp(out.countsPer360/given.gameFullTurnSeconds,100.0,1e6);
        why="Full turn: "+number(out.countsPer360,0)+" counts (4 x your median quarter turn). The game needs "+number(given.gameFullTurnSeconds,2)+
            " s for a full turn at full stick, so full stick = "+number(t.fullSpeedCounts,0)+" counts a second; sensitivity 1.";
        if(out.flickP90CountsPerSec>t.fullSpeedCounts)
            why+=" Your fast flicks ("+number(out.flickP90CountsPerSec,0)+" counts a second) are "+number(out.flickP90CountsPerSec/t.fullSpeedCounts,1)+
                 " times faster than the game can turn: the surplus is dropped (or carried, if you chose that). Only the in-game sensitivity raises that limit.";
    }else{
        t.fullSpeedCounts=std::clamp(out.flickP90CountsPerSec,100.0,1e6);
        why="The game's full-turn time is not known (run the game calibration to get it), so full stick = your fast flick speed, "+
            number(t.fullSpeedCounts,0)+" counts a second (90th percentile). The quarter-turn distance you showed ("+number(out.countsPer360/4,0)+
            " counts) cannot be honoured without that time.";
    }
    t.sensX=1.0;t.sensY=std::clamp(ratio,0.01,100.0);
    t.game=false;                       // these numbers are the context's own
    t.curve=KbmCurve{};
    const double kneeIn=out.slowP10CountsPerSec>0?kKbmFitKneeFactor*out.slowP10CountsPerSec/t.fullSpeedCounts:0;
    if(out.slowP10CountsPerSec<=0){
        why+=" Curve: straight (no slow motions given).";
    }else if(kneeIn>kKbmFitKneeMax){
        why+=" Curve: straight (your slow and fast speeds are too close for two zones).";
    }else{
        t.curve.kind=KbmCurveKind::PrecisionSpeed;
        t.curve.kneeIn=std::clamp(kneeIn,0.05,0.95);
        t.curve.kneeOut=std::clamp(t.curve.kneeIn*kKbmFitPrecisionShare,0.01,0.99);
        why+=" Curve: precision zone up to "+number(t.curve.kneeIn*t.fullSpeedCounts,0)+" counts a second (twice your slowest steady speed, "+
             number(out.slowP10CountsPerSec,0)+"), at 60 % of a straight line; above it straight on to full stick.";
    }
    // What the text form (4 decimals) keeps, so that a saved profile reads back as the same numbers.
    t.fullSpeedCounts=rounded(t.fullSpeedCounts);t.sensY=rounded(t.sensY);
    t.curve.kneeIn=rounded(t.curve.kneeIn);t.curve.kneeOut=rounded(t.curve.kneeOut);
    out.tuning=kbmSanitized([&]{KbmInputSettings s;s.kind=KbmInputProfileKind::Custom;s.contexts[0]=t;return s;}()).contexts[0];
    out.explanation=std::move(why);
    out.ok=true;
    return out;
}

// Records one test motion from the raw mouse while the page asks for it. Distance is the straight
// line from the first to the last report with movement; the time runs between those two reports.
class KbmFitRecorder {
public:
    void start(KbmFitKind kind)noexcept{kind_=kind;active_=true;x_=y_=0;firstUs_=lastUs_=0;}
    bool active()const noexcept{return active_;}
    KbmFitKind kind()const noexcept{return kind_;}
    void add(int dx,int dy,std::uint64_t nowUs)noexcept{
        if(!active_||(dx==0&&dy==0))return;
        if(!firstUs_)firstUs_=nowUs;
        lastUs_=nowUs;x_+=dx;y_+=dy;
    }
    // Ends the recording. {0, 0} when nothing moved.
    KbmFitMotion stop()noexcept{
        KbmFitMotion out;
        if(active_&&firstUs_){
            out.counts=std::hypot(double(x_),double(y_));
            out.seconds=std::max(0.001,double(lastUs_-firstUs_)/1e6);
        }
        active_=false;
        return out;
    }
private:
    KbmFitKind kind_=KbmFitKind::Turn;
    bool active_=false;
    std::int64_t x_=0,y_=0;
    std::uint64_t firstUs_=0,lastUs_=0;
};
} // namespace veyra::remoteplay
