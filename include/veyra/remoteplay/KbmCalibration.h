// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental (VEYRA_EXP_RP_KBM): a calibration the player runs in front of the game, a few
// minutes per game, that measures how the game answers the right stick and turns that into
// profile numbers (docs/KEYBOARD_MOUSE.md, "Calibrating a game"). Portable and unit-tested; the
// Win32 capture feeds it keys and polls (include/veyra/ui/RemotePlayKeyboardMouse.h).
//
// The console sees an analog stick, never a mouse. All this can do is learn the game's dead zone,
// its fastest turn, the shape of its response and its vertical / horizontal ratio, so that
// MouseAim undoes them as far as these measurements allow.
//
// What it sends: plain right-stick deflections (a slow ramp, a held tilt, short left / right / up /
// down swings) and, in the aiming steps, the aim button held - each only after the player tapped
// to start that step while watching the screen, and only while that step runs. Nothing reads the
// picture; every measurement is the player's own tap. No aim assistance, no recoil compensation,
// no macros, nothing during play.
//
// Keys while the stream window has keyboard and mouse (KbmCalibrationKeys):
//   Ctrl+Alt+Shift+K   start; pressed again: stop without writing anything
//   Space / left mouse  tap: start the step on screen, then mark what the step asks for
//   Enter               skip this step (or, at a profile's first question, the whole profile)
//   Esc                 stop without writing anything
// Every key it does not use goes to the game as before.
//
// Steps per profile (walk; aim with the aim button held, after the player agreed; drive, only if
// the player taps while sitting in a vehicle):
//   dead zone   the stick tilts slowly from the centre: tap when the camera starts to move; then
//               it tilts back from 60 %: tap when the camera stops. The two taps are late by the
//               same reaction and stream delay in opposite directions; their mean cancels it.
//   full turn   the stick is held fully right: tap each time a chosen landmark passes the centre
//               of the screen, three times (two turns, averaged). Delays cancel in differences.
//   response    the same at 25, 50 and 75 % of the tilt above the dead zone, at every full turn or
//               (when a turn would take longer than about 4 s) at every quarter turn.
//   vertical    the camera swings right and back, then up and back, again and again, the up swing
//               growing a little each time: tap when it looks as large as the right swing; then
//               once more with the up swing shrinking. The mean of the two cancels the delay.
// A missed step (no tap in time) or two taps that disagree is asked once more, then the profile's
// own value is kept.
#include "KeyboardMousePad.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace veyra::remoteplay {

inline constexpr double kKbmDefaultCountsPer360=8000;   // 800 DPI and 10 inches for one turn

// --- Keys -----------------------------------------------------------------------------------------
class KbmCalibrationKeys {
public:
    enum class Action : std::uint8_t {
        Pass,     // not ours: the key goes to the game
        Consume,  // ours, nothing to do (a repeat, or the release of a key we took)
        Toggle,   // Ctrl+Alt+Shift+K
        Tap,
        Skip,
        Exit};
    static constexpr std::uint32_t kChordKey='K',kTapKey=0x20,kSkipKey=0x0D,kExitKey=0x1B;
    // A key from the capture. `active`: a calibration is running.
    Action key(std::uint32_t vk,bool down,bool active){
        if(vk==0x10||vk==0xA0||vk==0xA1){shift_=down;return Action::Pass;}
        if(vk==0x11||vk==0xA2||vk==0xA3){ctrl_=down;return Action::Pass;}
        if(vk==0x12||vk==0xA4||vk==0xA5){alt_=down;return Action::Pass;}
        if(!down){
            const auto it=std::find(taken_.begin(),taken_.end(),vk);
            if(it==taken_.end())return Action::Pass;
            taken_.erase(it);return Action::Consume;
        }
        if(std::find(taken_.begin(),taken_.end(),vk)!=taken_.end())return Action::Consume;   // auto-repeat
        Action action=Action::Pass;
        if(vk==kChordKey&&ctrl_&&alt_&&shift_)action=Action::Toggle;
        else if(active&&vk==kTapKey)action=Action::Tap;
        else if(active&&vk==kSkipKey)action=Action::Skip;
        else if(active&&vk==kExitKey)action=Action::Exit;
        if(action!=Action::Pass&&taken_.size()<kMaxTaken)taken_.push_back(vk);
        return action;
    }
    // Mouse button 1 (left) taps while a calibration runs.
    Action mouseButton(int button,bool down,bool active){
        const std::uint32_t code=kMouseButtonBase+std::uint32_t(button>0?button:0);
        if(!down){
            const auto it=std::find(taken_.begin(),taken_.end(),code);
            if(it==taken_.end())return Action::Pass;
            taken_.erase(it);return Action::Consume;
        }
        if(button!=1||!active)return Action::Pass;
        if(std::find(taken_.begin(),taken_.end(),code)!=taken_.end())return Action::Consume;
        if(taken_.size()<kMaxTaken)taken_.push_back(code);
        return Action::Tap;
    }
    // The capture ended: nothing is held any more.
    void reset()noexcept{taken_.clear();shift_=ctrl_=alt_=false;}
private:
    static constexpr std::size_t kMaxTaken=16;
    std::vector<std::uint32_t> taken_;
    bool shift_=false,ctrl_=false,alt_=false;
};

// --- What was measured ------------------------------------------------------------------------------
struct KbmCalibrationMeasured {
    std::optional<double> deadzone;             // tilt at which the camera starts to turn
    std::optional<double> turnSeconds;          // one full turn at full tilt
    std::array<std::optional<double>,3> response;   // turning speed at 25 / 50 / 75 % as a share of the fastest
    std::array<double,3> responseAt{.25,.5,.75};    // the share of the tilt above the dead zone really sent
    std::optional<double> ratio;                // vertical sensitivity / horizontal sensitivity for equal turns
    bool any()const noexcept{return deadzone||turnSeconds||response[0]||response[1]||response[2]||ratio;}
};

// Profile numbers from the measurements, on top of `set`. A profile keeps every number that was
// not measured. Mouse counts per full turn: set.countsPer360, else kKbmDefaultCountsPer360.
//   deadzone   as measured
//   full       counts per turn / seconds per turn, sens_x 1: that many counts turn once around at any
//              hand speed the stick can follow
//   sens_y     the measured ratio (equal turn for equal counts both ways), else the old ratio
//   curve      the power that fits the response points best (least squares on logarithms)
//   curve_25.. the response points themselves, moved to exactly 25 / 50 / 75 % along that power,
//              when all three were measured and rise
//   carry_ms   saturation: a flick faster than the game's fastest turn is played out until the
//              carried part reaches 45 degrees (at most 250 ms); the rest is dropped
inline double kbmCountsPer360(const KbmProfileSet& set)noexcept{
    return set.countsPer360>0?set.countsPer360:kKbmDefaultCountsPer360;
}
inline constexpr double kKbmCalibratedCarryMaxMs=50;
inline KbmProfileSet kbmCalibratedProfiles(KbmProfileSet set,const std::array<KbmCalibrationMeasured,3>& measured){
    const double perTurn=kbmCountsPer360(set);
    for(std::size_t i=0;i<3;++i){
        const auto& m=measured[i];
        MouseAimConfig& aim=set.profiles[i].aim;
        if(m.deadzone)aim.deadzone=std::clamp(*m.deadzone,0.0,0.5);
        const double oldRatio=aim.sensitivityY/aim.sensitivityX;
        if(m.turnSeconds){
            aim.fullSpeedCounts=std::clamp(perTurn / *m.turnSeconds,100.0,1e6);
            aim.sensitivityX=1;
            aim.sensitivityY=std::clamp(oldRatio,0.01,100.0);
            // 45 degrees at full speed, at most 50 ms: a longer carry keeps the stick tilted after the
            // hand stopped (a 2 s turn gave 250 ms, a ghost turn). 50 ms = at most 7.5 degrees then.
            aim.carryMs=std::clamp(*m.turnSeconds*1000*45/360,0.0,kKbmCalibratedCarryMaxMs);
        }
        if(m.ratio)aim.sensitivityY=std::clamp(*m.ratio*aim.sensitivityX,0.01,100.0);
        double sumXY=0,sumXX=0;int points=0;
        for(std::size_t k=0;k<3;++k){
            if(!m.response[k])continue;
            const double u=m.responseAt[k],r=*m.response[k];
            if(!(u>0&&u<1&&r>0&&r<1))continue;
            sumXY+=std::log(u)*std::log(r);sumXX+=std::log(u)*std::log(u);++points;
        }
        if(points>0&&sumXX>0){
            aim.curve=std::clamp(sumXY/sumXX,0.25,4.0);
            aim.curvePoints={0,0,0};
            if(points==3){
                const double knots[3]={.25,.5,.75};
                std::array<double,3> table{};
                for(std::size_t k=0;k<3;++k)table[k]=*m.response[k]*std::pow(knots[k]/m.responseAt[k],aim.curve);
                if(table[0]>0&&table[0]<table[1]&&table[1]<table[2]&&table[2]<1)aim.curvePoints=table;
            }
        }
    }
    return set;
}

// --- The calibration ----------------------------------------------------------------------------------
class KbmCalibration {
public:
    enum class Step : std::uint8_t {Intro,DeadzoneUp,DeadzoneDown,TurnTime,Response,RatioUp,RatioDown};
    struct Settings {
        double rampPerSecond=0.05;      // dead-zone ramp: 12 s from the centre to 60 %
        double rampTop=0.6;
        double deadzoneAgree=0.12;      // the two dead-zone taps may differ by this much tilt
        double intervalAgree=0.2;       // two turn intervals may differ by this share of their mean
        double ratioAgree=1.3;          // the two vertical taps may differ by this factor
        double debounceSeconds=0.25;    // a second tap sooner than this is the same tap
        double turnTimeoutSeconds=30;   // no tap for this long: missed
        double quarterTurnAbove=4;      // a full turn expected (straight response) to take longer: tap every quarter turn
        double swingDegrees=15;         // the right swing of the vertical step
        double ratioFrom=0.4,ratioTo=2.5,ratioGrowth=1.04,swingPauseSeconds=0.4;
        int attempts=2;                 // a measurement is asked at most this often
        double idleTimeoutSeconds=60;   // between steps (the aim button may be held): no tap for this long stops it all
    };
    // What the pad poll sends. `deflect`: the right stick is (x, y) in -1..1, the mouse's movement
    // is dropped; else the right stick is what the player does. `holdAim`: the aim button is held.
    struct Output {bool deflect=false;double x=0,y=0;bool holdAim=false;};
    KbmCalibration():KbmCalibration(Settings{}){}
    explicit KbmCalibration(Settings settings):settings_(settings){}

    bool active()const noexcept{return active_;}
    bool running()const noexcept{return active_&&running_;}
    bool finished()const noexcept{return finished_;}            // ran to its end: measured() holds the result
    Step step()const noexcept{return index_<plan_.size()?plan_[index_].step:Step::Intro;}
    KbmProfileId profile()const noexcept{return index_<plan_.size()?plan_[index_].profile:KbmProfileId::Walk;}
    const std::array<KbmCalibrationMeasured,3>& measured()const noexcept{return measured_;}
    double markDegrees()const noexcept{return markDegrees_;}
    // Lines for the log, oldest first.
    std::vector<std::string> takeMessages(){return std::exchange(messages_,{});}

    // Starts on the profiles in use. False (and a message) when there is nothing to calibrate.
    bool start(const KbmProfileSet& set){
        *this=KbmCalibration(settings_);
        if(set[KbmProfileId::Walk].aim.mode==MouseAimConfig::Mode::Gyro){
            say("calibration: not available in gyro mode (it measures the right stick)");return false;
        }
        for(const auto id:{KbmProfileId::Walk,KbmProfileId::Aim,KbmProfileId::Drive}){
            if(set[id].mouse!=KbmProfile::Mouse::Camera)continue;
            if(id==KbmProfileId::Aim&&set.aimWhile==PadAction::None)continue;
            if(id!=KbmProfileId::Walk)plan_.push_back({Step::Intro,id,0});
            for(const Step s:{Step::DeadzoneUp,Step::DeadzoneDown,Step::TurnTime})plan_.push_back({s,id,0});
            for(std::uint8_t k=0;k<3;++k)plan_.push_back({Step::Response,id,k});
            for(const Step s:{Step::RatioUp,Step::RatioDown})plan_.push_back({s,id,0});
        }
        if(plan_.empty()){say("calibration: no profile uses the mouse as the camera");return false;}
        for(std::size_t i=0;i<3;++i)deadzoneUsed_[i]=set.profiles[i].aim.deadzone;
        stickBits_=set[KbmProfileId::Walk].aim.stickBits;
        aimName_=actionLabel(set.aimWhile);
        active_=true;
        say("calibration started: the console sees an analog stick, not a mouse; this measures how the game answers it. "
            "Stand somewhere open with a landmark ahead, in-game stick sensitivity as you will play it, aim assist and "
            "acceleration off if the game offers it. Space or left mouse: tap. Enter: skip. Esc or Ctrl+Alt+Shift+K: stop without saving.");
        announce();
        return true;
    }
    // Stops; nothing measured is kept.
    void stop(const char* why){
        if(!active_)return;
        active_=false;running_=false;aimHeld_=false;measured_={};
        say(std::string("calibration stopped (")+why+"); nothing was written");
    }
    void tap(std::uint64_t nowUs){
        if(!active_)return;
        idleSinceUs_=0;
        const Item& item=plan_[index_];
        if(!running_){
            if(item.step==Step::Intro){
                say(std::string("calibration: ")+kbmProfileName(item.profile)+" profile agreed");
                aimHeld_=item.profile==KbmProfileId::Aim;
                next();return;
            }
            running_=true;idleSinceUs_=0;startUs_=lastTapUs_=nowUs;taps_.clear();
            say(std::string("calibration: ")+stepName(item)+" running");
            return;
        }
        if(!taps_.empty()&&double(nowUs-taps_.back())<settings_.debounceSeconds*1e6)return;
        if(taps_.empty()&&double(nowUs-startUs_)<settings_.debounceSeconds*1e6&&item.step!=Step::DeadzoneUp)return;
        if(nowUs<startUs_)return;
        lastTapUs_=nowUs;
        const double t=double(nowUs-startUs_)/1e6;
        switch(item.step){
        case Step::DeadzoneUp:deadzoneUp_=tiltAt(item,t);finishStep();break;
        case Step::DeadzoneDown:
            if(t<0.5)return;           // the camera is still turning fast: not the stop
            deadzoneDown_=tiltAt(item,t);finishStep();break;
        case Step::TurnTime:case Step::Response:
            taps_.push_back(nowUs);
            if(taps_.size()==3)finishStep();
            break;
        case Step::RatioUp:ratioUp_=ratioAt(item,t);finishStep();break;
        case Step::RatioDown:ratioDown_=ratioAt(item,t);finishStep();break;
        case Step::Intro:break;
        }
    }
    void skip(std::uint64_t){
        if(!active_)return;
        idleSinceUs_=0;
        const Item item=plan_[index_];
        running_=false;attempt_=0;
        if(item.step==Step::Intro){
            say(std::string("calibration: ")+kbmProfileName(item.profile)+" profile skipped");
            while(index_<plan_.size()&&plan_[index_].profile==item.profile)++index_;
            aimHeld_=false;
            settle();return;
        }
        say(std::string("calibration: ")+stepName(item)+" skipped; the profile's value stays");
        skipGroup(item);
    }
    Output poll(std::uint64_t nowUs){
        Output out;
        if(!active_)return out;
        out.holdAim=aimHeld_;
        if(!running_){
            // Waiting for the player between steps, perhaps with the aim button held for the game:
            // never for ever. Nothing tapped for a minute stops the calibration, releases everything
            // and writes nothing.
            if(idleSinceUs_==0||nowUs<idleSinceUs_)idleSinceUs_=nowUs;
            else if(double(nowUs-idleSinceUs_)/1e6>settings_.idleTimeoutSeconds){
                aimHeld_=false;
                stop("no tap for a minute between steps; every button released");
                return Output{};
            }
            return out;
        }
        const Item& item=plan_[index_];
        const double t=nowUs>startUs_?double(nowUs-startUs_)/1e6:0.0;
        // Missed: nothing tapped in time.
        bool missed=false;
        switch(item.step){
        case Step::DeadzoneUp:case Step::DeadzoneDown:missed=t>settings_.rampTop/settings_.rampPerSecond+3;break;
        case Step::TurnTime:case Step::Response:missed=nowUs>lastTapUs_&&double(nowUs-lastTapUs_)/1e6>tapTimeout(item);break;
        case Step::RatioUp:case Step::RatioDown:missed=t>sweepSeconds(item)+2;break;
        case Step::Intro:break;
        }
        if(missed){
            say(std::string("calibration: ")+stepName(item)+": no tap in time");
            failGroup(item);
            out.holdAim=aimHeld_;
            return out;
        }
        out.deflect=true;
        switch(item.step){
        case Step::DeadzoneUp:case Step::DeadzoneDown:case Step::TurnTime:case Step::Response:out.x=tiltAt(item,t);break;
        case Step::RatioUp:case Step::RatioDown:swing(item,t,out);break;
        case Step::Intro:out.deflect=false;break;
        }
        return out;
    }
    // A tilt as the console will read it: whole steps of the profile's stick resolution.
    double quantized(double tilt)const noexcept{
        const double levels=double((1<<(std::clamp(stickBits_,8,16)-1))-1);
        return std::clamp(std::round(tilt*levels)/levels,-1.0,1.0);
    }
    // The pad value for a tilt (the same whole steps).
    std::int16_t stickValue(double tilt)const noexcept{
        const double q=quantized(tilt);
        if(q>=1)return 32767;
        if(q<=-1)return -32767;
        const int bits=std::clamp(stickBits_,8,16);
        const long levels=(1L<<(bits-1))-1;
        return std::int16_t(std::lround(q*double(levels))*(1L<<(16-bits)));
    }
private:
    struct Item {Step step;KbmProfileId profile;std::uint8_t point;};
    static const char* actionLabel(PadAction a)noexcept{
        switch(a){
        case PadAction::L2:return "L2";case PadAction::R2:return "R2";case PadAction::L1:return "L1";case PadAction::R1:return "R1";
        case PadAction::L3:return "L3";case PadAction::R3:return "R3";case PadAction::Cross:return "Cross";case PadAction::Circle:return "Circle";
        case PadAction::Square:return "Square";case PadAction::Triangle:return "Triangle";default:return "none";
        }
    }
    static std::string number(double v,int decimals){
        // Plain decimal without exponent, for messages and the profile file.
        const double scale=std::pow(10.0,decimals);
        const long long whole=std::llround(v*scale);
        const bool negative=whole<0;
        const unsigned long long magnitude=negative?0ull-static_cast<unsigned long long>(whole):static_cast<unsigned long long>(whole);
        std::string digits=std::to_string(magnitude);
        if(decimals>0){
            if(digits.size()<=std::size_t(decimals))digits.insert(0,std::size_t(decimals)+1-digits.size(),'0');
            digits.insert(digits.size()-std::size_t(decimals),1,'.');
            while(digits.back()=='0')digits.pop_back();
            if(digits.back()=='.')digits.pop_back();
        }
        return negative&&digits!="0"?"-"+digits:digits;
    }
public:
    static std::string formatNumber(double v,int decimals){return number(v,decimals);}
private:
    std::string stepName(const Item& item)const{
        std::string name=std::string(kbmProfileName(item.profile))+" ";
        switch(item.step){
        case Step::Intro:return name+"start";
        case Step::DeadzoneUp:return name+"dead zone (rising)";
        case Step::DeadzoneDown:return name+"dead zone (falling)";
        case Step::TurnTime:return name+"full turn";
        case Step::Response:return name+"response at "+std::to_string(25*(item.point+1))+" %";
        case Step::RatioUp:return name+"vertical (growing)";
        case Step::RatioDown:return name+"vertical (shrinking)";
        }
        return name;
    }
    void say(std::string text){if(messages_.size()<64)messages_.push_back(std::move(text));}
    std::size_t at(KbmProfileId id)const noexcept{return std::size_t(id);}
    double maxDegreesPerSecond(KbmProfileId id)const noexcept{
        const auto& t=measured_[at(id)].turnSeconds;
        return t?360.0 / *t:200.0;
    }
    double tiltAt(const Item& item,double t)const noexcept{
        switch(item.step){
        case Step::DeadzoneUp:return quantized(std::min(settings_.rampTop,settings_.rampPerSecond*t));
        case Step::DeadzoneDown:return quantized(std::max(0.0,settings_.rampTop-settings_.rampPerSecond*t));
        case Step::TurnTime:return 1.0;
        case Step::Response:{
            const double dz=deadzoneUsed_[at(item.profile)];
            return quantized(dz+(1-dz)*0.25*(item.point+1));
        }
        default:return 0.0;
        }
    }
    double swingSeconds(KbmProfileId id)const noexcept{return std::clamp(settings_.swingDegrees/maxDegreesPerSecond(id),0.04,0.4);}
    double cycleRatio(const Item& item,int k)const noexcept{
        const double grown=std::pow(settings_.ratioGrowth,double(k));
        return item.step==Step::RatioUp?settings_.ratioFrom*grown:settings_.ratioTo/grown;
    }
    int cycles()const noexcept{
        return 1+int(std::ceil(std::log(settings_.ratioTo/settings_.ratioFrom)/std::log(settings_.ratioGrowth)));
    }
    double cycleSeconds(const Item& item,int k)const noexcept{
        const double h=swingSeconds(item.profile);
        return 2*h+2*h*cycleRatio(item,k)+settings_.swingPauseSeconds;
    }
    double sweepSeconds(const Item& item)const noexcept{
        double total=0;for(int k=0;k<cycles();++k)total+=cycleSeconds(item,k);return total;
    }
    // The ratio of the swing on screen at `t`: the cycle that last finished its up swing.
    double ratioAt(const Item& item,double t)const noexcept{
        double begin=0;
        for(int k=0;k<cycles();++k){
            const double length=cycleSeconds(item,k);
            if(t<begin+length)return cycleRatio(item,k);
            begin+=length;
        }
        return cycleRatio(item,cycles()-1);
    }
    void swing(const Item& item,double t,Output& out)const noexcept{
        const double h=swingSeconds(item.profile);
        double begin=0;
        for(int k=0;k<cycles();++k){
            const double v=h*cycleRatio(item,k),length=cycleSeconds(item,k),p=t-begin;
            if(p<length){
                if(p<h)out.x=1;
                else if(p<2*h)out.x=-1;
                else if(p<2*h+v)out.y=-1;            // up
                else if(p<2*h+2*v)out.y=1;
                return;
            }
            begin+=length;
        }
    }
    double expectedInterval(const Item& item)const noexcept{
        const double turn=360.0/maxDegreesPerSecond(item.profile);
        if(item.step==Step::TurnTime)return turn;
        return turn/(0.25*(item.point+1))*(markDegrees_/360.0);
    }
    double tapTimeout(const Item& item)const noexcept{
        if(item.step==Step::TurnTime)return settings_.turnTimeoutSeconds;
        return std::clamp(expectedInterval(item)*8,20.0,120.0);   // a curved response is slower than expected
    }
    void announce(){
        if(index_>=plan_.size())return;
        const Item& item=plan_[index_];
        std::string text="calibration, "+stepName(item)+": ";
        switch(item.step){
        case Step::Intro:
            if(item.profile==KbmProfileId::Aim)text+="the same steps while aiming. The calibration will hold "+aimName_+" for you until they end. Tap to agree, Enter to skip aiming.";
            else text+="the same steps in a vehicle (camera only). Get in, stand still, tap to agree; Enter to skip.";
            break;
        case Step::DeadzoneUp:text+="tap to start. The right stick tilts slowly from the centre; tap the moment the camera starts to turn.";break;
        case Step::DeadzoneDown:text+="tap to start. The stick jumps to 60 % and slowly returns; tap the moment the camera stops.";break;
        case Step::TurnTime:text+="pick a landmark, tap to start. The stick is held fully right; tap each time the landmark crosses the centre (3 taps).";break;
        case Step::Response:{
            const double turn=360.0/maxDegreesPerSecond(item.profile);
            markDegrees_=turn/(0.25*(item.point+1))>settings_.quarterTurnAbove?90.0:360.0;
            text+="tap to start. The stick is held part way; tap each time ";
            text+=markDegrees_<360?"the camera has turned another quarter turn (90 degrees: four landmarks, or the compass)":"the landmark crosses the centre";
            text+=" (3 taps).";
            break;
        }
        case Step::RatioUp:text+="level the camera on the horizon with a mark ahead, tap to start. It swings right and back, then up and back; the up swing grows. Tap when up looks as large as right.";break;
        case Step::RatioDown:text+="tap to start. The same, the up swing now shrinks. Tap when up looks as large as right.";break;
        }
        say(std::move(text));
    }
    void next(){++index_;running_=false;settle();}
    // After the index moved: the end, or the next question.
    void settle(){
        running_=false;
        if(index_>=plan_.size()){
            active_=false;finished_=true;aimHeld_=false;
            say(measured_[0].any()||measured_[1].any()||measured_[2].any()?"calibration finished":"calibration finished without a measurement; nothing is written");
            return;
        }
        if(plan_[index_].profile!=KbmProfileId::Aim)aimHeld_=false;
        announce();
    }
    std::size_t groupStart(const Item& item)const noexcept{
        std::size_t i=index_;
        if(item.step==Step::DeadzoneDown||item.step==Step::RatioDown)--i;
        return i;
    }
    std::size_t groupEnd(const Item& item)const noexcept{
        std::size_t i=index_+1;
        if(item.step==Step::DeadzoneUp||item.step==Step::RatioUp)++i;
        return i;
    }
    // The full turn decides the response steps: without it they cannot be read.
    void dropResponses(KbmProfileId id,std::size_t from){
        std::size_t to=from;
        while(to<plan_.size()&&plan_[to].profile==id&&plan_[to].step==Step::Response)++to;
        if(to>from){
            plan_.erase(plan_.begin()+std::ptrdiff_t(from),plan_.begin()+std::ptrdiff_t(to));
            say(std::string("calibration: ")+kbmProfileName(id)+" response steps left out (they need the full turn)");
        }
    }
    void skipGroup(const Item& item){
        index_=groupEnd(item);
        if(item.step==Step::TurnTime)dropResponses(item.profile,index_);
        settle();
    }
    void failGroup(const Item& item){
        running_=false;
        if(++attempt_<settings_.attempts){
            say(std::string("calibration: ")+stepName(item)+": once more");
            index_=groupStart(item);settle();return;
        }
        attempt_=0;
        say(std::string("calibration: ")+stepName(item)+": no clear result; the profile's value stays");
        skipGroup(item);
    }
    void succeed(){attempt_=0;next();}
    void finishStep(){
        const Item item=plan_[index_];
        auto& m=measured_[at(item.profile)];
        switch(item.step){
        case Step::DeadzoneUp:case Step::RatioUp:running_=false;++index_;announce();return;
        case Step::DeadzoneDown:{
            const double a=deadzoneUp_,b=deadzoneDown_;
            if(std::abs(a-b)>settings_.deadzoneAgree||(a+b)/2>0.5){
                say("calibration: dead zone taps disagree ("+number(a,3)+" rising, "+number(b,3)+" falling)");failGroup(item);return;
            }
            m.deadzone=(a+b)/2;deadzoneUsed_[at(item.profile)]=*m.deadzone;
            say(std::string("calibration: ")+kbmProfileName(item.profile)+" dead zone "+number(*m.deadzone,3));
            succeed();return;
        }
        case Step::TurnTime:case Step::Response:{
            const double i1=double(taps_[1]-taps_[0])/1e6,i2=double(taps_[2]-taps_[1])/1e6,mean=(i1+i2)/2;
            if(std::abs(i1-i2)>settings_.intervalAgree*mean){
                say("calibration: the two intervals disagree ("+number(i1,2)+" s and "+number(i2,2)+" s)");failGroup(item);return;
            }
            if(item.step==Step::TurnTime){
                if(mean<0.2||mean>settings_.turnTimeoutSeconds){say("calibration: a full turn of "+number(mean,2)+" s is not plausible");failGroup(item);return;}
                m.turnSeconds=mean;
                say(std::string("calibration: ")+kbmProfileName(item.profile)+" full turn "+number(mean,3)+" s ("+number(360/mean,1)+" degrees per second)");
                succeed();return;
            }
            const double share=markDegrees_/mean/maxDegreesPerSecond(item.profile);
            if(!(share>0&&share<1.05)){say("calibration: a response of "+number(share,3)+" of the fastest turn is not plausible");failGroup(item);return;}
            const double dz=deadzoneUsed_[at(item.profile)];
            m.response[item.point]=std::min(share,0.995);
            m.responseAt[item.point]=std::clamp((tiltAt(item,0)-dz)/(1-dz),0.01,0.99);
            say(std::string("calibration: ")+kbmProfileName(item.profile)+" response at "+number(m.responseAt[item.point]*100,1)+" %: "+number(share,3)+" of the fastest turn");
            succeed();return;
        }
        case Step::RatioDown:{
            const double a=ratioUp_,b=ratioDown_;
            if(std::max(a,b)/std::min(a,b)>settings_.ratioAgree){
                say("calibration: vertical taps disagree ("+number(a,2)+" growing, "+number(b,2)+" shrinking)");failGroup(item);return;
            }
            m.ratio=std::sqrt(a*b);
            say(std::string("calibration: ")+kbmProfileName(item.profile)+" vertical / horizontal sensitivity "+number(*m.ratio,3));
            succeed();return;
        }
        case Step::Intro:return;
        }
    }

    Settings settings_;
    std::vector<Item> plan_;
    std::size_t index_=0;
    bool active_=false,running_=false,finished_=false,aimHeld_=false;
    int attempt_=0,stickBits_=8;
    std::uint64_t startUs_=0,lastTapUs_=0,idleSinceUs_=0;
    std::vector<std::uint64_t> taps_;
    double deadzoneUp_=0,deadzoneDown_=0,ratioUp_=1,ratioDown_=1,markDegrees_=360;
    std::array<double,3> deadzoneUsed_{.12,.12,.12};
    std::array<KbmCalibrationMeasured,3> measured_{};
    std::string aimName_="L2";
    std::vector<std::string> messages_;
};
} // namespace veyra::remoteplay
