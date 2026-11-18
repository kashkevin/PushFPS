// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental (VEYRA_EXP_RP_KBM): keyboard and mouse played as a DualSense on a PS5 stream.
// Portable and free of Windows, Qt and Chiaki: the Win32 capture feeds key(), mouseButton(),
// mouseMove() and scroll(), and the pad poll calls apply() to lay the result over the state of
// the physical pad (which may be absent).
//
// A console game reads a stick as a turning speed, a mouse reports a distance. The translation
// follows the published practice of GIMX (blog.gimx.fr, "Subposition precision, 1:1 translation")
// and PCSX2 (pull request 7885):
//   speed   = mouse counts per second over a short window
//   tilt    = deadzone + (1 - deadzone) * (speed / fullSpeed) ^ (1 / curve)
// The dead zone term skips the range in which the game ignores the stick, so the smallest mouse
// movement already turns the view. `curve` undoes a game whose turning speed grows with
// tilt^curve. Movement faster than a fully tilted stick can turn is carried into the next
// polls instead of being lost, up to `carryMs` of full-speed turning.
// None of these values can be measured from here: dead zone, curve and full speed differ per
// game and have to be tuned on the console.
//
// Gyro mode sends the movement as angular velocity of the pad instead and leaves the right
// stick alone. A game with motion aiming then turns in proportion to the mouse, with no dead
// zone, curve or speed cap of a stick in the way.
#include "Types.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
namespace veyra::remoteplay {
struct MouseAimConfig {
    enum class Mode {Stick,Gyro};
    Mode mode=Mode::Stick;
    double sensitivity=1.0;        // scales the mouse counts
    double fullSpeedCounts=4000;   // counts per second that tilt the stick fully
    double deadzone=0.12;          // the game's stick dead zone to skip, 0..0.5
    double curve=1.0;              // the game's turning speed ~ tilt^curve
    double yScale=1.0;
    bool invertX=false,invertY=false;
    double smoothingMs=8;          // window the speed is taken over; 0: one poll
    double carryMs=40;             // faster-than-full movement kept for later polls
    double gyroDegPerCount=0.03;   // Gyro mode: degrees of turn per mouse count
};
class MouseAim {
public:
    struct Output {std::int16_t x=0,y=0;float yawRate=0,pitchRate=0;bool moving=false;};
    explicit MouseAim(MouseAimConfig config={}):config_(sanitize(config)){}
    const MouseAimConfig& config()const noexcept{return config_;}
    void add(int dx,int dy)noexcept{pendingX_+=dx*config_.sensitivity*(config_.invertX?-1:1);pendingY_+=dy*config_.sensitivity*config_.yScale*(config_.invertY?-1:1);}
    void reset()noexcept{pendingX_=pendingY_=0;window_.clear();}
    // One pad poll, `elapsedUs` after the previous one.
    Output step(std::uint64_t elapsedUs){
        const double dt=std::clamp(double(elapsedUs)/1e6,0.0005,0.05);
        Output out;
        if(config_.mode==MouseAimConfig::Mode::Gyro){
            // The whole movement of this poll, as the rotation a hand would have made in it.
            const double radians=config_.gyroDegPerCount*3.14159265358979323846/180.0;
            out.yawRate=float(-pendingX_*radians/dt);out.pitchRate=float(-pendingY_*radians/dt);
            out.moving=pendingX_!=0||pendingY_!=0;pendingX_=pendingY_=0;
            return out;
        }
        // No more than a fully tilted stick turns in this poll; the rest waits.
        const double budget=config_.fullSpeedCounts*dt,length=std::hypot(pendingX_,pendingY_);
        double takeX=pendingX_,takeY=pendingY_;
        if(length>budget&&length>0){
            takeX*=budget/length;takeY*=budget/length;
            pendingX_-=takeX;pendingY_-=takeY;
            const double carry=config_.fullSpeedCounts*config_.carryMs/1000.0,left=std::hypot(pendingX_,pendingY_);
            if(left>carry){pendingX_*=carry/left;pendingY_*=carry/left;}
        }else pendingX_=pendingY_=0;
        window_.push_back({dt,takeX,takeY});
        double time=0,sumX=0,sumY=0;
        // Newest first until the window is covered; older polls leave.
        std::size_t keep=0;
        for(auto it=window_.rbegin();it!=window_.rend();++it){
            if(keep>0&&time+it->dt>config_.smoothingMs/1000.0+1e-9)break;
            time+=it->dt;sumX+=it->x;sumY+=it->y;++keep;
        }
        while(window_.size()>keep)window_.pop_front();
        const double speed=std::hypot(sumX,sumY)/time;
        if(speed<=1e-9)return out;
        const double tilt=config_.deadzone+(1-config_.deadzone)*std::pow(std::min(1.0,speed/config_.fullSpeedCounts),1.0/config_.curve);
        const double scale=tilt*32767.0/std::hypot(sumX,sumY);
        out.x=std::int16_t(std::clamp(std::lround(sumX*scale),-32767L,32767L));
        out.y=std::int16_t(std::clamp(std::lround(sumY*scale),-32767L,32767L));
        out.moving=true;
        return out;
    }
private:
    static MouseAimConfig sanitize(MouseAimConfig c){
        const auto finite=[](double v,double fallback){return std::isfinite(v)?v:fallback;};
        c.sensitivity=std::clamp(finite(c.sensitivity,1),0.01,100.0);
        c.fullSpeedCounts=std::clamp(finite(c.fullSpeedCounts,4000),100.0,1e6);
        c.deadzone=std::clamp(finite(c.deadzone,.12),0.0,0.5);
        c.curve=std::clamp(finite(c.curve,1),0.25,4.0);
        c.yScale=std::clamp(finite(c.yScale,1),0.05,20.0);
        c.smoothingMs=std::clamp(finite(c.smoothingMs,8),0.0,50.0);
        c.carryMs=std::clamp(finite(c.carryMs,40),0.0,250.0);
        c.gyroDegPerCount=std::clamp(finite(c.gyroDegPerCount,.03),0.0005,1.0);
        return c;
    }
    struct Poll {double dt,x,y;};
    MouseAimConfig config_;
    double pendingX_=0,pendingY_=0;
    std::deque<Poll> window_;
};

// What a key or mouse button does on the pad.
enum class PadAction : std::uint8_t {None,Cross,Circle,Square,Triangle,L1,R1,L2,R2,L3,R3,Options,Share,Touchpad,PS,
    DUp,DDown,DLeft,DRight,LUp,LDown,LLeft,LRight,Walk};
// Keys are Win32 virtual-key codes; mouse buttons and wheel notches use the codes below.
inline constexpr std::uint32_t kMouseButtonBase=0x10000,kWheelUp=0x10010,kWheelDown=0x10011;
class KeyboardPadBindings {
public:
    KeyboardPadBindings(){
        // A first-person layout: WASD moves, the mouse aims, left / right button fire and aim.
        const std::pair<std::uint32_t,PadAction> defaults[]={
            {'W',PadAction::LUp},{'S',PadAction::LDown},{'A',PadAction::LLeft},{'D',PadAction::LRight},
            {0x20,PadAction::Cross},{'C',PadAction::Circle},{0x11,PadAction::Circle},{'R',PadAction::Square},{'F',PadAction::Triangle},
            {'Q',PadAction::L1},{'E',PadAction::R1},{0x10,PadAction::L3},{'V',PadAction::R3},{0x12,PadAction::Walk},
            {0x26,PadAction::DUp},{0x28,PadAction::DDown},{0x25,PadAction::DLeft},{0x27,PadAction::DRight},
            {0x09,PadAction::Touchpad},{0x1B,PadAction::Options},{0x08,PadAction::Share},{0x24,PadAction::PS},
            {kMouseButtonBase+1,PadAction::R2},{kMouseButtonBase+3,PadAction::L2},{kMouseButtonBase+2,PadAction::R3},
            {kMouseButtonBase+4,PadAction::L1},{kMouseButtonBase+5,PadAction::R1},
            {kWheelUp,PadAction::Triangle},{kWheelDown,PadAction::Triangle}};
        for(const auto& [code,action]:defaults)map_[code]=action;
    }
    PadAction action(std::uint32_t code)const{const auto it=map_.find(code);return it==map_.end()?PadAction::None:it->second;}
    void bind(std::uint32_t code,PadAction action){if(action==PadAction::None)map_.erase(code);else map_[code]=action;}
    // "F=Square;Mouse2=None;Shift=L3": overrides on top of the defaults. Returns the number
    // of entries it could not read; those are skipped.
    unsigned parse(std::string_view text){
        unsigned bad=0;
        while(!text.empty()){
            const auto end=text.find(';');auto item=text.substr(0,end);
            text=end==std::string_view::npos?std::string_view{}:text.substr(end+1);
            item=trim(item);if(item.empty())continue;
            const auto equals=item.find('=');
            if(equals==std::string_view::npos){++bad;continue;}
            const auto code=keyCode(trim(item.substr(0,equals)));
            PadAction action=PadAction::None;
            if(!code||!actionName(trim(item.substr(equals+1)),action)){++bad;continue;}
            bind(code,action);
        }
        return bad;
    }
    static std::uint32_t keyCode(std::string_view name){
        const auto lower=lowered(name);
        if(lower.size()==1){
            const char c=lower[0];
            if(c>='a'&&c<='z')return std::uint32_t(c-'a'+'A');
            if(c>='0'&&c<='9')return std::uint32_t(c);
        }
        static const std::pair<const char*,std::uint32_t> names[]={{"space",0x20},{"shift",0x10},{"ctrl",0x11},{"alt",0x12},
            {"tab",0x09},{"esc",0x1B},{"enter",0x0D},{"back",0x08},{"home",0x24},{"end",0x23},{"insert",0x2D},{"delete",0x2E},
            {"up",0x26},{"down",0x28},{"left",0x25},{"right",0x27},{"caps",0x14},
            {"mouse1",kMouseButtonBase+1},{"mouse2",kMouseButtonBase+2},{"mouse3",kMouseButtonBase+3},
            {"mouse4",kMouseButtonBase+4},{"mouse5",kMouseButtonBase+5},{"wheelup",kWheelUp},{"wheeldown",kWheelDown}};
        for(const auto& [text,code]:names)if(lower==text)return code;
        if(lower.size()>=2&&lower.size()<=3&&lower[0]=='f'){
            int n=0;for(std::size_t i=1;i<lower.size();++i){if(lower[i]<'0'||lower[i]>'9')return 0;n=n*10+(lower[i]-'0');}
            if(n>=1&&n<=12)return std::uint32_t(0x70+n-1);
        }
        return 0;
    }
    static bool actionName(std::string_view name,PadAction& action){
        static const std::pair<const char*,PadAction> names[]={{"none",PadAction::None},{"cross",PadAction::Cross},{"circle",PadAction::Circle},
            {"square",PadAction::Square},{"triangle",PadAction::Triangle},{"l1",PadAction::L1},{"r1",PadAction::R1},{"l2",PadAction::L2},
            {"r2",PadAction::R2},{"l3",PadAction::L3},{"r3",PadAction::R3},{"options",PadAction::Options},{"share",PadAction::Share},
            {"touchpad",PadAction::Touchpad},{"ps",PadAction::PS},{"dup",PadAction::DUp},{"ddown",PadAction::DDown},{"dleft",PadAction::DLeft},
            {"dright",PadAction::DRight},{"lup",PadAction::LUp},{"ldown",PadAction::LDown},{"lleft",PadAction::LLeft},{"lright",PadAction::LRight},
            {"walk",PadAction::Walk}};
        const auto lower=lowered(name);
        for(const auto& [text,value]:names)if(lower==text){action=value;return true;}
        return false;
    }
private:
    static std::string_view trim(std::string_view s){
        while(!s.empty()&&(s.front()==' '||s.front()=='\t'))s.remove_prefix(1);
        while(!s.empty()&&(s.back()==' '||s.back()=='\t'))s.remove_suffix(1);
        return s;
    }
    static std::string lowered(std::string_view s){std::string out(s);for(char& c:out)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return out;}
    std::unordered_map<std::uint32_t,PadAction> map_;
};

class KeyboardMousePad {
public:
    static constexpr std::uint64_t kWheelPulseUs=60000; // a wheel notch holds its button this long
    explicit KeyboardMousePad(MouseAimConfig aim={},KeyboardPadBindings bindings={}):aim_(aim),bindings_(std::move(bindings)){}
    MouseAim& aim()noexcept{return aim_;}
    void key(std::uint32_t virtualKey,bool down){set(virtualKey,down);}
    void mouseButton(int button,bool down){if(button>=1&&button<=5)set(kMouseButtonBase+std::uint32_t(button),down);}
    void mouseMove(int dx,int dy){aim_.add(dx,dy);}
    void scroll(int delta){if(delta!=0)wheel_=delta>0?kWheelUp:kWheelDown;}
    // Capture ended or focus lost: nothing stays held.
    void releaseAll(){held_.clear();wheel_=0;pulseAction_=PadAction::None;aim_.reset();}
    // Lays keyboard and mouse over the physical pad's state for this poll. A pressed key adds its
    // button; held movement keys replace the left stick; a moving mouse replaces the right stick
    // (or the motion sensor in Gyro mode).
    ControllerState apply(ControllerState pad,std::uint64_t nowUs){
        const std::uint64_t elapsed=lastUs_&&nowUs>lastUs_?nowUs-lastUs_:4000;lastUs_=nowUs;
        pad.inputActive=true;
        if(wheel_){pulseAction_=bindings_.action(wheel_);pulseUntilUs_=nowUs+kWheelPulseUs;wheel_=0;}
        int moveX=0,moveY=0;bool walk=false;
        const auto press=[&](PadAction action){
            switch(action){
            case PadAction::Cross:pad.buttons|=ControllerState::Cross;break;
            case PadAction::Circle:pad.buttons|=ControllerState::Circle;break;
            case PadAction::Square:pad.buttons|=ControllerState::Square;break;
            case PadAction::Triangle:pad.buttons|=ControllerState::Triangle;break;
            case PadAction::L1:pad.buttons|=ControllerState::L1;break;
            case PadAction::R1:pad.buttons|=ControllerState::R1;break;
            case PadAction::L3:pad.buttons|=ControllerState::L3;break;
            case PadAction::R3:pad.buttons|=ControllerState::R3;break;
            case PadAction::Options:pad.buttons|=ControllerState::Options;break;
            case PadAction::Share:pad.buttons|=ControllerState::Share;break;
            case PadAction::Touchpad:pad.buttons|=ControllerState::Touchpad;break;
            case PadAction::PS:pad.buttons|=ControllerState::PS;break;
            case PadAction::DUp:pad.buttons|=ControllerState::Up;break;
            case PadAction::DDown:pad.buttons|=ControllerState::Down;break;
            case PadAction::DLeft:pad.buttons|=ControllerState::Left;break;
            case PadAction::DRight:pad.buttons|=ControllerState::Right;break;
            case PadAction::L2:pad.l2=255;break;
            case PadAction::R2:pad.r2=255;break;
            case PadAction::LUp:--moveY;break;
            case PadAction::LDown:++moveY;break;
            case PadAction::LLeft:--moveX;break;
            case PadAction::LRight:++moveX;break;
            case PadAction::Walk:walk=true;break;
            case PadAction::None:break;
            }
        };
        bool movementHeld=false;
        for(const auto& [code,down]:held_){
            if(!down)continue;
            const auto action=bindings_.action(code);
            movementHeld|=action==PadAction::LUp||action==PadAction::LDown||action==PadAction::LLeft||action==PadAction::LRight;
            press(action);
        }
        if(pulseAction_!=PadAction::None){if(nowUs<pulseUntilUs_)press(pulseAction_);else pulseAction_=PadAction::None;}
        if(movementHeld){
            // Opposite keys cancel; a diagonal stays on the unit circle like a real stick.
            const double length=std::hypot(double(moveX),double(moveY)),tilt=walk?0.5:1.0;
            pad.leftX=length>0?std::int16_t(std::lround(moveX/length*tilt*32767)):0;
            pad.leftY=length>0?std::int16_t(std::lround(moveY/length*tilt*32767)):0;
        }
        const auto aim=aim_.step(elapsed);
        if(aim_.config().mode==MouseAimConfig::Mode::Gyro){
            // The sensor of a pad lying still, turned by the mouse. One sample per poll.
            pad.gyro={aim.pitchRate,aim.yawRate,0.f};pad.accel={0.f,1.f,0.f};
            motionUs_+=elapsed;pad.motionTimestampUs=motionUs_;pad.motionValid=true;
            pad.motionSamples[0]={pad.gyro,pad.accel,motionUs_};pad.motionSampleCount=1;
        }else if(aim.moving){pad.rightX=aim.x;pad.rightY=aim.y;}
        return pad;
    }
private:
    void set(std::uint32_t code,bool down){held_[code]=down;}
    MouseAim aim_;
    KeyboardPadBindings bindings_;
    std::unordered_map<std::uint32_t,bool> held_;
    std::uint32_t wheel_=0;
    PadAction pulseAction_=PadAction::None;
    std::uint64_t pulseUntilUs_=0,lastUs_=0,motionUs_=0;
};
} // namespace veyra::remoteplay
