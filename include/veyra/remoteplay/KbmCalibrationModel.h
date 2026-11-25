// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// A LINUX MODEL, not part of the application: a simulated game and a simulated player for the
// keyboard and mouse calibration (KbmCalibration.h). Used by tests/remoteplay/KbmCalibrationTests.inc
// and `tools/remoteplay_latency_bench/input_sim --mode calibrate`. Everything here is an
// assumption: how a real game reads the stick, shapes its response and limits its turn, and how
// a real player sees and taps. It shows what the calibration can recover under these assumptions
// and nothing about any real game.
#include "KbmCalibration.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>
namespace veyra::remoteplay::kbmmodel {

// A game's right stick: radial dead zone, response, fastest turn, vertical turn slower or faster
// by `vertical`; it reads the stick in `bits` (values cut towards zero).
struct Game {
    enum class Shape : std::uint8_t {Power,TwoZone};
    double deadzone=.2,maxDegPerSecond=240,vertical=.6,curve=1.8;
    Shape shape=Shape::Power;
    int bits=8;
    double response(double u)const noexcept{
        u=std::clamp(u,0.0,1.0);
        if(shape==Shape::Power)return std::pow(u,curve);
        // Slow inner zone, fast outer zone: a common console layout, not a power.
        return u<0.7?0.3*u/0.7:0.3+0.7*(u-0.7)/0.3;
    }
    double axis(std::int16_t v)const noexcept{
        const int shift=16-bits,levels=(1<<(bits-1))-1;
        const int step=std::abs(int(v))>=32767?(v<0?-levels:levels):int(v)/(1<<shift);
        return double(step)/levels;
    }
    // Degrees per second (yaw, pitch; pitch up is negative) for a stick state.
    void rates(std::int16_t x,std::int16_t y,double& yaw,double& pitch)const noexcept{
        const double ax=axis(x),ay=axis(y),tilt=std::hypot(ax,ay);
        yaw=pitch=0;
        if(tilt<=deadzone)return;
        const double speed=maxDegPerSecond*response((std::min(1.0,tilt)-deadzone)/(1-deadzone));
        yaw=speed*ax/tilt;pitch=speed*ay/tilt*vertical;
    }
};

// The player: sees the camera with `lagSeconds` of reaction and stream delay, taps with an
// independent error of up to +/- `errorSeconds`, notices movement above `visibleDegPerSecond`.
struct Player {double lagSeconds=.25,errorSeconds=.08,visibleDegPerSecond=1.0;bool calibrateAim=true;};

struct Run {std::array<KbmCalibrationMeasured,3> measured{};bool finished=false;double seconds=0;unsigned taps=0;};

// The whole calibration in virtual time: 4 ms polls, the game reading every poll.
inline Run calibrate(const KbmProfileSet& set,const std::array<Game,3>& games,const Player& player,std::uint32_t seed,
                     KbmCalibration::Settings settings={}){
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> error(-player.errorSeconds,player.errorSeconds),unit(0,1);
    KbmCalibration cal(settings);
    Run run;
    if(!cal.start(set))return run;
    const std::uint64_t pollUs=4000;
    std::uint64_t now=1000000;
    std::vector<std::uint64_t> taps;            // scheduled tap times
    std::uint64_t skipAt=0;
    // What the player watches.
    double yaw=0,pitch=0,landmark=0;
    bool wasRunning=false,seen=false;
    KbmCalibration::Step lastStep=KbmCalibration::Step::Intro;KbmProfileId lastProfile=KbmProfileId::Walk;
    std::uint64_t runStart=0;
    double rightGain=0,upGain=0;int phase=0;    // ratio: 0 right, 1 left, 2 up, 3 down/pause
    bool ratioTapped=false;
    const auto schedule=[&](std::uint64_t at){taps.push_back(at+std::uint64_t(std::max(0.0,player.lagSeconds+error(rng))*1e6));};
    for(int guard=0;guard<2000000&&cal.active();++guard){
        // Taps due now.
        std::sort(taps.begin(),taps.end());
        while(!taps.empty()&&taps.front()<=now){cal.tap(taps.front());taps.erase(taps.begin());++run.taps;}
        if(skipAt&&now>=skipAt){cal.skip(now);skipAt=0;}
        if(!cal.active())break;
        const auto step=cal.step();const auto profile=cal.profile();
        const bool running=cal.running();
        const bool changed=running!=wasRunning||step!=lastStep||profile!=lastProfile||guard==0;
        if(!running&&changed){
            // A new question: answer it a second later.
            taps.clear();skipAt=0;
            if(step==KbmCalibration::Step::Intro&&(profile==KbmProfileId::Drive||(profile==KbmProfileId::Aim&&!player.calibrateAim)))skipAt=now+1000000;
            else taps.push_back(now+1000000);
        }
        if(running&&changed){runStart=now;seen=false;landmark=yaw+unit(rng)*cal.markDegrees();rightGain=upGain=0;phase=0;ratioTapped=false;taps.clear();}
        wasRunning=running;lastStep=step;lastProfile=profile;

        const auto out=cal.poll(now);
        const Game& game=games[std::size_t(out.holdAim?KbmProfileId::Aim:profile)];
        std::int16_t sx=0,sy=0;
        if(out.deflect){sx=cal.stickValue(out.x);sy=cal.stickValue(out.y);}
        double yawRate=0,pitchRate=0;game.rates(sx,sy,yawRate,pitchRate);
        const double dt=double(pollUs)/1e6;
        yaw+=yawRate*dt;pitch+=pitchRate*dt;
        if(running){
            const double t=double(now-runStart)/1e6;
            const double speed=std::hypot(yawRate,pitchRate);
            switch(step){
            case KbmCalibration::Step::DeadzoneUp:
                if(!seen&&speed>player.visibleDegPerSecond){seen=true;schedule(now);}
                break;
            case KbmCalibration::Step::DeadzoneDown:
                if(!seen&&t>1.0&&speed<player.visibleDegPerSecond){seen=true;schedule(now);}
                break;
            case KbmCalibration::Step::TurnTime:case KbmCalibration::Step::Response:{
                const double mark=step==KbmCalibration::Step::TurnTime?360.0:cal.markDegrees();
                if(yaw>=landmark+mark){landmark+=mark;schedule(now);}
                break;
            }
            case KbmCalibration::Step::RatioUp:case KbmCalibration::Step::RatioDown:{
                // Phases from what the camera does.
                if(yawRate>0){if(phase>=2){rightGain=upGain=0;}phase=0;rightGain+=yawRate*dt;}
                else if(yawRate<0)phase=1;
                else if(pitchRate<0){phase=2;upGain+=-pitchRate*dt;}
                else if(pitchRate>0&&phase==2){
                    phase=3;
                    const bool equal=step==KbmCalibration::Step::RatioUp?upGain>=rightGain:upGain<=rightGain;
                    if(equal&&!ratioTapped&&rightGain>0){ratioTapped=true;schedule(now);}
                }
                break;
            }
            case KbmCalibration::Step::Intro:break;
            }
        }
        now+=pollUs;
    }
    run.finished=cal.finished();
    run.measured=cal.measured();
    run.seconds=double(now-1000000)/1e6;
    return run;
}

// How linear the turn is in the mouse speed for one profile: steady mouse speeds from 2 % to 90 %
// of the profile's full-tilt speed, 1.2 s each through MouseAim (4 ms polls), the game's turn per
// count. `shape`: RMS of (turn per count / median - 1), in per cent. `scale`: the median turn per
// count against `degreesPerCount`, in per cent (0: exact). `fast`: the shape error over 15 to 90 %
// only (the slowest speeds are the most sensitive to a dead zone that is off by a stick step).
struct Linearity {double shape=0,scale=0,fast=0;};
inline Linearity linearity(const MouseAimConfig& config,const Game& game,double degreesPerCount){
    static constexpr double fractions[]={.02,.05,.1,.15,.2,.3,.4,.5,.6,.7,.8,.9};
    std::vector<double> gains;
    for(const double f:fractions){
        MouseAim aim(config);
        const double countsPerSecond=f*config.fullSpeedCounts/config.sensitivityX;
        double owed=0,turned=0,counted=0;
        for(int i=0;i<300;++i){
            owed+=countsPerSecond*0.004;
            const int whole=int(owed);owed-=whole;
            aim.add(whole,0);
            const auto o=aim.step(4000);
            double yaw=0,pitch=0;game.rates(o.x,o.y,yaw,pitch);
            if(i>=75){turned+=yaw*0.004;counted+=whole;}
        }
        gains.push_back(counted>0?turned/counted:0);
    }
    std::vector<double> sorted=gains;std::sort(sorted.begin(),sorted.end());
    const double median=sorted[sorted.size()/2];
    Linearity out;
    if(!(median>0))return {100,100,100};
    double squared=0,fastSquared=0;std::size_t fastCount=0;
    for(std::size_t i=0;i<gains.size();++i){
        const double e=(gains[i]/median-1)*(gains[i]/median-1);
        squared+=e;
        if(fractions[i]>=.15){fastSquared+=e;++fastCount;}
    }
    out.shape=std::sqrt(squared/double(gains.size()))*100;
    out.fast=std::sqrt(fastSquared/double(fastCount))*100;
    out.scale=(median/degreesPerCount-1)*100;
    return out;
}
} // namespace veyra::remoteplay::kbmmodel
