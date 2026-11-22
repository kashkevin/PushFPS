// SPDX-License-Identifier: GPL-3.0-only
// Input performance profiles against the legacy path (the Package 7 behaviour) on the same
// generated mouse traces, through the real translation code (KeyboardMousePad.h, KbmInputProfile.h,
// ControllerQueue.h). A LINUX MODEL IN VIRTUAL TIME: no Windows timer, no USB stack, no console, no
// game. It compares the profiles with each other under the assumptions printed below and says
// nothing about feel or about any real game.
//
//   g++ -std=c++20 -O2 -Iinclude tools/remoteplay_latency_bench/kbm_profile_bench.cpp -o kbm_profile_bench
//   ./kbm_profile_bench            # the tables of docs/KEYBOARD_MOUSE.md, "Input profiles against legacy"
#include "veyra/remoteplay/ControllerQueue.h"
#include "veyra/remoteplay/KbmCalibrationRate.h"
#include "veyra/remoteplay/KbmInputProfile.h"
#include "veyra/remoteplay/KbmProfile.h"
#include "veyra/remoteplay/KeyboardMousePad.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>
using namespace veyra::remoteplay;
namespace {
// --- Assumptions ---------------------------------------------------------------------------------
constexpr double kCountsPer360=8000;            // 800 DPI, 10 inches per turn
constexpr double kCountsPerDegree=kCountsPer360/360;
constexpr double kGameDeadzone=0.20,kGamePower=1.8,kGameFullTurnSeconds=1.5;   // the simulated game's right stick
constexpr double kGameMaxDegPerSecond=360/kGameFullTurnSeconds;
constexpr int kPollUs=4000,kTimerLateUs=300;    // pad poll and how late the timer may fire
constexpr int kTickUs=125;                      // base step: one report of an 8 kHz mouse
constexpr double kReactionMs=150,kToleranceDeg=0.5;

struct Profile {const char* name;KbmInputProfileKind kind;double smoothingMs,carryMs;bool aimCurve;};
const Profile kProfiles[]={{"legacy (8 ms window, carry 50 ms)",KbmInputProfileKind::Legacy,0,0,false},
    {"ultra-low-latency",KbmInputProfileKind::UltraLowLatency,0,0,false},
    {"adaptive-precision, its aiming curve",KbmInputProfileKind::AdaptivePrecision,0,0,true},
    {"custom, carry 50 ms",KbmInputProfileKind::Custom,0,50,false},
    {"custom, 8 ms window",KbmInputProfileKind::Custom,8,0,false}};

// The profile file a perfect game calibration would have written (the calibration's own error is a
// separate table in the guide): dead zone, full-turn speed and response of the simulated game.
// Adaptive precision on foot is the same transform as ultra-low-latency, so its column shows the
// one context where it differs by default: the aiming curve (ADS factor 1 here, to compare curves only).
KbmProfileSet calibrated(const Profile& profile){
    char text[256];
    std::snprintf(text,sizeof(text),"all.deadzone=%.3f\nall.full=%.3f\nall.curve=%.3f\nall.carry_ms=50\nall.sens_x=1\nall.sens_y=1\n",
        kGameDeadzone,kCountsPer360/kGameFullTurnSeconds,kGamePower);
    auto set=parseKbmProfiles(text).set;
    set.input=kbmInputPreset(profile.kind);
    set.input.adsFactor=1;set.input.smoothingMs=profile.smoothingMs;
    if(profile.aimCurve)set.input.override_=KbmContextOverride::Aim;
    if(profile.carryMs>0)for(auto& c:set.input.contexts){c.saturation=KbmSaturation::Carry;c.carryMs=profile.carryMs;}
    set.input=kbmSanitized(set.input);
    return set;
}
// The simulated game: 8-bit stick, radial dead zone, turning speed ~ (tilt above the dead zone)^power.
double gameDegPerSecond(std::int16_t stick){
    const int step=int(std::lround(double(stick)/32767.0*127.0));
    const double tilt=std::abs(step)/127.0;
    if(tilt<=kGameDeadzone)return 0;
    return std::copysign(kGameMaxDegPerSecond*std::pow((tilt-kGameDeadzone)/(1-kGameDeadzone),kGamePower),double(step));
}
struct Result {
    double cameraDeg=0,idealDeg=0,rmsDeg=0,centroidMs=0,statesPerSecond=0,saturatedPct=0,queueAgeMaxMs=0,settleMs=-1;
    std::vector<double> camera;   // per base tick
};
// Runs `seconds` of a hand whose wanted camera speed is `wanted(t)` deg/s (t in seconds; the hand
// moves kCountsPerDegree counts per wanted degree). `gameHz`: how often the game reads the newest state.
Result run(const Profile& profile,int mouseHz,double gameHz,double seconds,const std::function<double(double,double)>& wanted,unsigned seed=1){
    KeyboardMousePad pad(calibrated(profile));
    pad.immediateAim().setHoldUs(kbmHoldUsFor(mouseHz,kPollUs/1000));
    pad.aim().setWindowFloorMs(kbmAccumulationFor(mouseHz,kPollUs/1000,pad.aim().config().smoothingMs).windowFloorMs);
    ControllerQueue queue;
    std::mt19937 rng(seed);
    Result out;
    const std::int64_t ticks=std::int64_t(seconds*1e6/kTickUs);
    const int reportEvery=8000/mouseHz;
    std::int64_t nextPollUs=kPollUs+std::int64_t(rng()%kTimerLateUs),nextGameUs=std::int64_t(rng()%2000);
    double carry=0,rate=0,sumIn=0,sumInT=0,sumOut=0,sumOutT=0,squares=0;
    std::int16_t sent=0;std::uint64_t states=0;
    (void)pad.apply({},1);
    for(std::int64_t tick=0;tick<ticks;++tick){
        const std::int64_t nowUs=tick*kTickUs+kTickUs;
        const double t=double(nowUs)/1e6;
        const double speed=wanted(t,out.cameraDeg);
        out.idealDeg+=speed*kTickUs/1e6;
        carry+=speed*kCountsPerDegree*kTickUs/1e6;
        if(tick%reportEvery==reportEvery-1){
            const int counts=int(carry);carry-=counts;
            if(counts){pad.mouseMove(counts,0);sumIn+=counts;sumInT+=counts*t;}
        }
        if(nowUs>=nextPollUs){
            const auto state=pad.apply({},std::uint64_t(nowUs));
            // The session owner wakes at once in this model: queue age is the push-to-take time, 50 us.
            if(queue.push(HostTime(nowUs)*10,state)){++states;out.queueAgeMaxMs=std::max(out.queueAgeMaxMs,0.05);}
            sent=queue.take(HostTime(nowUs)*10+500).rightX;
            nextPollUs+=kPollUs;
            nextPollUs=std::max(nextPollUs,nowUs)+std::int64_t(rng()%kTimerLateUs)-kTimerLateUs/2;
        }
        if(nowUs>=nextGameUs){rate=gameDegPerSecond(sent);nextGameUs+=std::int64_t(1e6/gameHz);}
        const double turned=rate*kTickUs/1e6;
        out.cameraDeg+=turned;sumOut+=std::abs(turned);sumOutT+=std::abs(turned)*t;
        squares+=(out.cameraDeg-out.idealDeg)*(out.cameraDeg-out.idealDeg);
        out.camera.push_back(out.cameraDeg);
    }
    out.rmsDeg=std::sqrt(squares/double(ticks));
    out.centroidMs=sumIn>0&&sumOut>0?(sumOutT/sumOut-sumInT/sumIn)*1000:0;
    out.statesPerSecond=double(states)/seconds;
    out.saturatedPct=pad.movingPolls()?100.0*double(pad.saturatedPolls())/double(pad.movingPolls()):0;
    return out;
}
// A smooth point-to-point hand movement (minimum jerk): `degrees` in `ms`, starting at `startMs`.
double stroke(double tMs,double startMs,double ms,double degrees){
    const double x=(tMs-startMs)/ms;
    if(x<=0||x>=1)return 0;
    return degrees/(ms/1000)*30*x*x*(1-x)*(1-x);
}
double strokeMs(double degrees){return std::clamp(100+1.2*std::abs(degrees),100.0,300.0);}
// The model player: turns to a target `degrees` away with one stroke, looks at what is left
// kReactionMs after the stroke ended, corrects with another stroke, and so on (at most 8) until the
// camera is within kToleranceDeg. Returns the time the camera last entered that band, in ms.
struct Aim {double timeMs=-1;int strokes=0;double firstErrorDeg=0;};
Aim aimAt(const Profile& profile,int mouseHz,double gameHz,double degrees){
    struct Plan {double startMs,ms,degrees;};
    std::vector<Plan> plans{{20,strokeMs(degrees),degrees}};
    Aim aim;
    for(int round=0;round<8;++round){
        const double endMs=plans.back().startMs+plans.back().ms+kReactionMs+100;
        const auto result=run(profile,mouseHz,gameHz,endMs/1000+0.2,[&](double t,double){
            double v=0;for(const auto& p:plans)v+=stroke(t*1000,p.startMs,p.ms,p.degrees);return v;});
        const double seenAt=plans.back().startMs+plans.back().ms+kReactionMs;
        const double seen=result.camera[std::min(result.camera.size()-1,std::size_t(seenAt*1000/kTickUs))];
        const double left=degrees-seen;
        if(round==0)aim.firstErrorDeg=-left;
        aim.strokes=round+1;
        if(std::abs(left)<=kToleranceDeg){
            // When the camera last entered the band.
            std::size_t at=result.camera.size();
            while(at>0&&std::abs(result.camera[at-1]-degrees)<=kToleranceDeg)--at;
            aim.timeMs=double(at)*kTickUs/1000;
            return aim;
        }
        plans.push_back({seenAt,strokeMs(left),left});
    }
    return aim;
}
} // namespace

int main(){
    std::printf("LINUX MODEL (virtual time). Assumptions: %.0f counts per turn; pad poll %d ms, timer +/- %.2f ms; simulated game: 8-bit stick,\n"
        "dead zone %.2f, turning speed ~ tilt^%.1f, full turn %.1f s (%.0f deg/s cap); profiles hold a perfect calibration of it;\n"
        "the session owner sends a queued state 50 us after the poll; model player: minimum-jerk strokes (100 ms + 1.2 ms per degree,\n"
        "at most 300 ms), looks %.0f ms after each stroke, corrects until within %.1f degrees. Nothing here is a measurement on a PC or a PS5.\n\n",
        kCountsPer360,kPollUs/1000,kTimerLateUs/2000.0,kGameDeadzone,kGamePower,kGameFullTurnSeconds,kGameMaxDegPerSecond,kReactionMs,kToleranceDeg);
    std::printf("Table 1: one small stroke (2 degrees in 100 ms) and a slow pursuit (+/- 30 deg/s, 0.5 Hz, 4 s); the game reads every state.\n");
    std::printf("| Mouse | Profile | Delay of the turn (centroid), ms | Against legacy, ms | Turned of 2 degrees | Pursuit error RMS, deg | States sent /s | Queue age max, ms |\n|---|---|---|---|---|---|---|---|\n");
    for(const int hz:{125,1000,8000}){
        double legacy=0;
        for(const auto& profile:kProfiles){
            const auto small=run(profile,hz,250,0.5,[](double t,double){return stroke(t*1000,20,100,2);});
            const auto pursuit=run(profile,hz,250,4,[](double t,double){return 30*std::sin(2*3.14159265358979*0.5*t);});
            if(profile.kind==KbmInputProfileKind::Legacy)legacy=small.centroidMs;
            std::printf("| %d Hz | %s | %.1f | %+.1f | %.2f | %.2f | %.0f | %.2f |\n",hz,profile.name,small.centroidMs,small.centroidMs-legacy,small.cameraDeg,
                pursuit.rmsDeg,pursuit.statesPerSecond,pursuit.queueAgeMaxMs);
        }
    }
    std::printf("\nTable 2: the model player turning to a target, 1000 Hz mouse. Time until the camera is on target for good (ms), strokes needed,\n"
        "and how far the first stroke missed (+ too far, - short).\n");
    std::printf("| Target | Game reads | Profile | Time to target, ms | Strokes | First stroke off by, deg | Saturated polls |\n|---|---|---|---|---|---|---|\n");
    for(const double degrees:{2.0,20.0,90.0,180.0})for(const double gameHz:{250.0,30.0})for(const auto& profile:kProfiles){
        const auto aim=aimAt(profile,1000,gameHz,degrees);
        const auto once=run(profile,1000,gameHz,1.0,[&](double t,double){return stroke(t*1000,20,strokeMs(degrees),degrees);});
        char time[32];if(aim.timeMs<0)std::snprintf(time,sizeof(time),"not in 8 strokes");else std::snprintf(time,sizeof(time),"%.0f",aim.timeMs);
        char saturated[16];if(profile.kind==KbmInputProfileKind::Legacy)std::snprintf(saturated,sizeof(saturated),"not counted");else std::snprintf(saturated,sizeof(saturated),"%.0f %%",once.saturatedPct);
        std::printf("| %.0f deg | %s | %s | %s | %d | %+.2f | %s |\n",degrees,gameHz>100?"every state":"30 fps",profile.name,time,aim.strokes,aim.firstErrorDeg,saturated);
    }
    return 0;
}
