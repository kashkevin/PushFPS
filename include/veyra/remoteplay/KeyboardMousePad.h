// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental (VEYRA_EXP_RP_KBM): keyboard and mouse played as a DualSense on a PS5 stream.
// Portable and free of Windows, Qt and Chiaki: the Win32 capture feeds key(), mouseButton(),
// mouseMove() and scroll(), and the pad poll calls apply() to lay the result over the state of
// the physical pad (which may be absent).
//
// This is input translation and nothing else: every pad state is what the player's hands are
// doing at that moment. There is no aim assistance, no recoil compensation, no macro, no
// repeated or timed input and nothing that reads the picture.
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
// A game reads the stick in 8 bits; with `stickBits` = 8 the output uses whole 8-bit steps only
// and the part of the movement that falls between two steps is carried into the next poll, so
// slow movement adds up instead of being cut off.
// None of these values can be measured from here: dead zone, curve and full speed differ per
// game and have to be tuned on the console.
//
// Gyro mode sends the movement as angular velocity of the pad instead and leaves the right
// stick alone. A game with motion aiming then turns in proportion to the mouse, with no dead
// zone, curve or speed cap of a stick in the way.
//
// Three profiles (KbmProfileSet): walking, aiming while the aim button is held, and driving,
// switched by the player's key. Each has its own mouse numbers and keys; the driving one can
// ramp keys onto the triggers and the left stick and use the mouse as a steering wheel. The
// text file they are read from is KbmProfile.h. A physical controller stays usable next to all
// of it (StickHandover).
#include "Types.h"
#include "KbmInputProfile.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
namespace veyra::remoteplay {
struct MouseAimConfig {
    enum class Mode {Stick,Gyro};
    Mode mode=Mode::Stick;
    double sensitivityX=1.0;       // scales the horizontal mouse counts
    double sensitivityY=1.0;       // and the vertical ones
    double fullSpeedCounts=4000;   // counts per second that tilt the stick fully
    double deadzone=0.12;          // the game's stick dead zone to skip, 0..0.5
    double curve=1.0;              // the game's turning speed ~ tilt^curve
    double accel=0.0;              // 0: linear. Else the counts of a poll are scaled by 1 + accel * speed / fullSpeed ...
    double accelCap=2.0;           // ... but by no more than this
    bool invertX=false,invertY=false;
    double smoothingMs=8;          // window the speed is taken over; 0: one poll. See mouseSmoothingDelay()
    double carryMs=40;             // faster-than-full movement kept for later polls
    double fineScale=0.5;          // sensitivity factor while the fine-aim key is held
    int stickBits=16;              // 8..15: whole steps of that many bits, the rounding error carried; 16: plain rounding
    double gyroDegPerCount=0.03;   // Gyro mode: degrees of turn per mouse count
    // The game's measured response (KbmCalibration.h): its turning speed at 25, 50 and 75 % of the
    // tilt above the dead zone, as a share of its fastest turn. All 0: the power `curve` instead.
    // Used only when 0 < p25 < p50 < p75 < 1; between the points (and 0 and 1 at the ends) the
    // response is taken as straight, and the mouse speed is placed on its inverse.
    std::array<double,3> curvePoints{0,0,0};
    bool operator==(const MouseAimConfig&)const=default;
};
// What the smoothing window costs. A count is spread evenly over the polls the window holds, so
// its turn reaches the console (polls - 1) / 2 poll intervals later on average and its last part
// polls - 1 intervals later. A window no longer than the poll interval adds nothing.
struct SmoothingDelay {double meanMs=0,lastMs=0;};
inline SmoothingDelay mouseSmoothingDelay(double smoothingMs,double pollMs)noexcept{
    if(!(pollMs>0)||!(smoothingMs>0)||!std::isfinite(smoothingMs)||!std::isfinite(pollMs))return {};
    const double polls=std::max(1.0,std::floor((smoothingMs+1e-6)/pollMs));
    return {(polls-1)*pollMs/2,(polls-1)*pollMs};
}
class MouseAim {
public:
    // `moving`: the stick is deflected. `received`: mouse movement arrived since the last poll.
    struct Output {std::int16_t x=0,y=0;float yawRate=0,pitchRate=0;bool moving=false,received=false;};
    explicit MouseAim(MouseAimConfig config={}):config_(sanitize(config)){}
    const MouseAimConfig& config()const noexcept{return config_;}
    // Another profile's numbers. Movement that is on its way stays.
    void setConfig(const MouseAimConfig& config){config_=sanitize(config);}
    void setFine(bool held)noexcept{fine_=held;}
    void add(int dx,int dy)noexcept{
        const double fine=fine_?config_.fineScale:1.0;
        incomingX_+=dx*config_.sensitivityX*fine*(config_.invertX?-1:1);
        incomingY_+=dy*config_.sensitivityY*fine*(config_.invertY?-1:1);
    }
    void reset()noexcept{incomingX_=incomingY_=pendingX_=pendingY_=residualX_=residualY_=0;window_.clear();}
    // The shortest smoothing window whatever the profile says: two report intervals of the mouse
    // in use (KbmCalibrationRate.h), so that a poll between two reports of a slow mouse does not
    // read as a stop. 0: none. Kept across profile changes.
    void setWindowFloorMs(double ms)noexcept{floorMs_=std::isfinite(ms)?std::clamp(ms,0.0,50.0):0.0;}
    double windowMs()const noexcept{return std::max(config_.smoothingMs,floorMs_);}
    // The game's response to a share `u` of the tilt above the dead zone, as a share of its
    // fastest turn, and the inverse. The power `curve` unless curvePoints are in use.
    static double response(const MouseAimConfig& c,double u)noexcept{
        u=std::clamp(u,0.0,1.0);
        if(!table(c))return std::pow(u,c.curve);
        const double knots[5]={0,c.curvePoints[0],c.curvePoints[1],c.curvePoints[2],1};
        const std::size_t i=std::min<std::size_t>(3,std::size_t(u*4));
        return knots[i]+(knots[i+1]-knots[i])*(u*4-double(i));
    }
    static double inverseResponse(const MouseAimConfig& c,double s)noexcept{
        s=std::clamp(s,0.0,1.0);
        if(!table(c))return std::pow(s,1.0/c.curve);
        const double knots[5]={0,c.curvePoints[0],c.curvePoints[1],c.curvePoints[2],1};
        std::size_t i=0;
        while(i<3&&s>knots[i+1])++i;
        return (double(i)+(s-knots[i])/(knots[i+1]-knots[i]))/4;
    }
    static bool table(const MouseAimConfig& c)noexcept{return c.curvePoints[0]>0;}
    // One pad poll, `elapsedUs` after the previous one.
    Output step(std::uint64_t elapsedUs){
        const double dt=std::clamp(double(elapsedUs)/1e6,0.0005,0.05);
        Output out;
        out.received=incomingX_!=0||incomingY_!=0;
        // Acceleration control. Off, the turn depends on the distance the hand moved and on
        // nothing else; on, a fast movement counts for more than the same distance moved slowly.
        double gain=1;
        if(config_.accel>0)gain=std::min(config_.accelCap,1+config_.accel*std::hypot(incomingX_,incomingY_)/dt/config_.fullSpeedCounts);
        pendingX_+=incomingX_*gain;pendingY_+=incomingY_*gain;incomingX_=incomingY_=0;
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
        // Straight after a reset there is no history. Polls without movement stand in for it, so
        // that the first movement is spread over the whole window like any other instead of over
        // the part filled so far (which turned it too far).
        const double windowSeconds=windowMs()/1000.0;
        if(window_.empty())for(double filled=2*dt;filled<=windowSeconds+1e-9;filled+=dt)window_.push_back({dt,0,0});
        window_.push_back({dt,takeX,takeY});
        double time=0,sumX=0,sumY=0;
        // Newest first until the window is covered; older polls leave.
        std::size_t keep=0;
        for(auto it=window_.rbegin();it!=window_.rend();++it){
            if(keep>0&&time+it->dt>windowSeconds+1e-9)break;
            time+=it->dt;sumX+=it->x;sumY+=it->y;++keep;
        }
        while(window_.size()>keep)window_.pop_front();
        if(config_.stickBits>=16){
            const double speed=std::hypot(sumX,sumY)/time;
            if(speed<=1e-9)return out;
            const double tilt=config_.deadzone+(1-config_.deadzone)*inverseResponse(config_,speed/config_.fullSpeedCounts);
            const double scale=tilt*32767.0/std::hypot(sumX,sumY);
            out.x=std::int16_t(std::clamp(std::lround(sumX*scale),-32767L,32767L));
            out.y=std::int16_t(std::clamp(std::lround(sumY*scale),-32767L,32767L));
            out.moving=out.x!=0||out.y!=0;
            return out;
        }
        // Whole stick steps only. `want` is what this poll should turn, in counts, plus what
        // earlier polls could not express. The step at or below it is sent (never more than was
        // asked for, so a remainder can not push the stick the other way) and the difference is
        // kept. Once the window holds no movement the stick is at rest: the remainder, a fraction
        // of a step, waits for the next movement instead of trickling out as drift.
        if(sumX==0&&sumY==0)return out;
        const int levels=(1<<(config_.stickBits-1))-1;
        const double wantX=sumX/time*dt+residualX_,wantY=sumY/time*dt+residualY_,want=std::hypot(wantX,wantY);
        int stepX=0,stepY=0;double turned=0;
        if(want>1e-12){
            const double tilt=config_.deadzone+(1-config_.deadzone)*inverseResponse(config_,want/budget);
            stepX=int(wantX/want*tilt*levels);stepY=int(wantY/want*tilt*levels);
            const double reached=std::hypot(double(stepX),double(stepY))/levels;
            if(reached>config_.deadzone)turned=response(config_,(reached-config_.deadzone)/(1-config_.deadzone))*budget;
            else stepX=stepY=0;
        }
        residualX_=wantX;residualY_=wantY;
        if(turned>0){
            const double steps=std::hypot(double(stepX),double(stepY));
            residualX_-=stepX/steps*turned;residualY_-=stepY/steps*turned;
        }
        // A remainder is a fraction of a step; the bound only keeps a diagonal full tilt, whose
        // corner the steps cannot reach, from piling it up.
        const double remainder=std::hypot(residualX_,residualY_);
        if(remainder>budget){residualX_*=budget/remainder;residualY_*=budget/remainder;}
        out.x=stickValue(stepX,levels);out.y=stickValue(stepY,levels);
        out.moving=out.x!=0||out.y!=0;
        return out;
    }
private:
    // Step `step` of `levels` as the 16-bit value sent to the console: a whole multiple of the
    // step size, which stays the same step whether the console cuts, rounds or shifts it down.
    std::int16_t stickValue(int step,int levels)const noexcept{
        if(step>=levels)return 32767;
        if(step<=-levels)return -32767;
        return std::int16_t(step*(1<<(16-config_.stickBits)));
    }
    static MouseAimConfig sanitize(MouseAimConfig c){
        const auto finite=[](double v,double fallback){return std::isfinite(v)?v:fallback;};
        c.sensitivityX=std::clamp(finite(c.sensitivityX,1),0.01,100.0);
        c.sensitivityY=std::clamp(finite(c.sensitivityY,1),0.01,100.0);
        c.fullSpeedCounts=std::clamp(finite(c.fullSpeedCounts,4000),100.0,1e6);
        c.deadzone=std::clamp(finite(c.deadzone,.12),0.0,0.5);
        c.curve=std::clamp(finite(c.curve,1),0.25,4.0);
        c.accel=std::clamp(finite(c.accel,0),0.0,4.0);
        c.accelCap=std::clamp(finite(c.accelCap,2),1.0,8.0);
        c.smoothingMs=std::clamp(finite(c.smoothingMs,8),0.0,50.0);
        c.carryMs=std::clamp(finite(c.carryMs,40),0.0,250.0);
        c.fineScale=std::clamp(finite(c.fineScale,.5),0.05,1.0);
        c.stickBits=std::clamp(c.stickBits,8,16);
        c.gyroDegPerCount=std::clamp(finite(c.gyroDegPerCount,.03),0.0005,1.0);
        // A response table only when it rises from 0 to 1 through its three points; else none.
        const auto& p=c.curvePoints;
        const bool rising=std::isfinite(p[0])&&std::isfinite(p[1])&&std::isfinite(p[2])&&p[0]>0&&p[0]<p[1]&&p[1]<p[2]&&p[2]<1;
        if(!rising)c.curvePoints={0,0,0};
        return c;
    }
    struct Poll {double dt,x,y;};
    MouseAimConfig config_;
    bool fine_=false;
    double floorMs_=0;
    double incomingX_=0,incomingY_=0;   // since the last poll, sensitivity applied
    double pendingX_=0,pendingY_=0;     // waiting for a poll that can turn it
    double residualX_=0,residualY_=0;   // between two stick steps
    std::deque<Poll> window_;
};

// What a key or mouse button does on the pad.
enum class PadAction : std::uint8_t {None,Cross,Circle,Square,Triangle,L1,R1,L2,R2,L3,R3,Options,Share,Touchpad,PS,
    DUp,DDown,DLeft,DRight,LUp,LDown,LLeft,LRight,Walk,
    Fine,   // slower mouse while held (MouseAimConfig::fineScale); not a pad button
    Drive,  // a press changes between the walking and the driving profile; not a pad button
    Menu};  // Options while held, and a press changes into or out of the menu keys (KbmProfileSet::menu)
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
    void clear(){map_.clear();}   // no key does anything: the start of a layout that is not the first-person one above
    std::size_t size()const noexcept{return map_.size();}
    bool operator==(const KeyboardPadBindings&)const=default;
    void bind(std::uint32_t code,PadAction action){if(action==PadAction::None)map_.erase(code);else map_[code]=action;}
    // Every bound key with its action, sorted by key code (for a settings page and for writing a profile file).
    std::vector<std::pair<std::uint32_t,PadAction>> entries()const{
        std::vector<std::pair<std::uint32_t,PadAction>> out(map_.begin(),map_.end());
        std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.first<b.first;});
        return out;
    }
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
        for(const auto& [text,code]:kKeyNames)if(lower==lowered(text))return code;
        if(lower=="capslock")return 0x14;
        if(lower.size()==4&&lower.compare(0,3,"num")==0&&lower[3]>='0'&&lower[3]<='9')return std::uint32_t(0x60+(lower[3]-'0')); // numeric pad
        if(lower.size()>=2&&lower.size()<=3&&lower[0]=='f'){
            int n=0;for(std::size_t i=1;i<lower.size();++i){if(lower[i]<'0'||lower[i]>'9')return 0;n=n*10+(lower[i]-'0');}
            if(n>=1&&n<=12)return std::uint32_t(0x70+n-1);
        }
        return 0;
    }
    static bool actionName(std::string_view name,PadAction& action){
        const auto lower=lowered(name);
        for(const auto& [text,value]:kActionNames)if(lower==text){action=value;return true;}
        return false;
    }
    // The name tables, also read the other way (kbmKeyName, kbmActionName). Key names as the
    // profile file and the guide write them; they are matched without regard to case.
    static constexpr std::pair<std::string_view,std::uint32_t> kKeyNames[]={{"Space",0x20},{"Shift",0x10},{"Ctrl",0x11},{"Alt",0x12},
        {"Tab",0x09},{"Esc",0x1B},{"Enter",0x0D},{"Back",0x08},{"Home",0x24},{"End",0x23},{"Insert",0x2D},{"Delete",0x2E},
        {"Up",0x26},{"Down",0x28},{"Left",0x25},{"Right",0x27},{"Caps",0x14},
        {"PageUp",0x21},{"PageDown",0x22},{"Grave",0xC0},{"Minus",0xBD},{"Plus",0xBB},{"Comma",0xBC},{"Period",0xBE},
        {"Slash",0xBF},{"Semicolon",0xBA},{"Quote",0xDE},{"LBracket",0xDB},{"RBracket",0xDD},{"Backslash",0xDC},
        {"Mouse1",kMouseButtonBase+1},{"Mouse2",kMouseButtonBase+2},{"Mouse3",kMouseButtonBase+3},
        {"Mouse4",kMouseButtonBase+4},{"Mouse5",kMouseButtonBase+5},{"WheelUp",kWheelUp},{"WheelDown",kWheelDown}};
    static constexpr std::pair<std::string_view,PadAction> kActionNames[]={{"none",PadAction::None},{"cross",PadAction::Cross},{"circle",PadAction::Circle},
        {"square",PadAction::Square},{"triangle",PadAction::Triangle},{"l1",PadAction::L1},{"r1",PadAction::R1},{"l2",PadAction::L2},
        {"r2",PadAction::R2},{"l3",PadAction::L3},{"r3",PadAction::R3},{"options",PadAction::Options},{"share",PadAction::Share},
        {"touchpad",PadAction::Touchpad},{"ps",PadAction::PS},{"dup",PadAction::DUp},{"ddown",PadAction::DDown},{"dleft",PadAction::DLeft},
        {"dright",PadAction::DRight},{"lup",PadAction::LUp},{"ldown",PadAction::LDown},{"lleft",PadAction::LLeft},{"lright",PadAction::LRight},
        {"walk",PadAction::Walk},{"fine",PadAction::Fine},{"drive",PadAction::Drive},{"menu",PadAction::Menu}};
private:
    static std::string_view trim(std::string_view s){
        while(!s.empty()&&(s.front()==' '||s.front()=='\t'))s.remove_prefix(1);
        while(!s.empty()&&(s.back()==' '||s.back()=='\t'))s.remove_suffix(1);
        return s;
    }
    static std::string lowered(std::string_view s){std::string out(s);for(char& c:out)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return out;}
    std::unordered_map<std::uint32_t,PadAction> map_;
};

// The name the profile file uses for a key code ("W", "Space", "Mouse1", "WheelUp", "F5", "Num3"); empty when it has none.
inline std::string kbmKeyName(std::uint32_t code){
    if((code>=std::uint32_t('A')&&code<=std::uint32_t('Z'))||(code>=std::uint32_t('0')&&code<=std::uint32_t('9')))return std::string(1,char(code));
    for(const auto& [text,value]:KeyboardPadBindings::kKeyNames)if(value==code)return std::string(text);
    if(code>=0x60&&code<=0x69)return "Num"+std::to_string(code-0x60);
    if(code>=0x70&&code<=0x7B)return "F"+std::to_string(code-0x70+1);
    return {};
}
// "cross", "l2", "lup", "walk", ... ("none" for None and for a value outside the enum).
inline std::string_view kbmActionName(PadAction action)noexcept{
    for(const auto& [text,value]:KeyboardPadBindings::kActionNames)if(value==action)return text;
    return "none";
}

// Who moves a stick when a physical controller and keyboard / mouse are both in use: the device
// that acted last. "Acting" is an event, not a level, so a held key does not fight a held stick:
//   keyboard / mouse: a movement key goes down, or the mouse starts to move;
//   controller: the stick is pushed out of its rest zone (past 30 %, back in under 20 %, so
//               drift and a wobbling worn stick do nothing), or it is moved by more than a
//               quarter of its travel after losing the stick.
// When the device that has the stick lets go and the other still holds it, the other takes over;
// when neither holds it the physical stick passes through unchanged. A mouse is silent between
// reports, so it keeps the stick for `kbmHoldUs` after its last movement (the camera stands still
// in that time instead of snapping to a controller stick that is still held).
class StickHandover {
public:
    enum class Owner : std::uint8_t {Pad,Kbm};
    static constexpr double kPadOn=0.30,kPadOff=0.20,kPadMove=0.25;
    explicit StickHandover(std::uint64_t kbmHoldUs=0)noexcept:holdUs_(kbmHoldUs){}
    Owner owner()const noexcept{return owner_;}
    void reset()noexcept{owner_=Owner::Pad;padActive_=kbmWas_=false;kbmUntilUs_=0;refX_=refY_=0;}
    // padX / padY: the physical stick, -1..1. kbmActive: keyboard / mouse deflect the stick in
    // this poll. kbmEdge: they were given a new command in this poll (another key went down).
    Owner update(double padX,double padY,bool kbmActive,bool kbmEdge,std::uint64_t nowUs)noexcept{
        if(!std::isfinite(padX)||!std::isfinite(padY))padX=padY=0;
        const double tilt=std::hypot(padX,padY);
        const bool padWas=padActive_;
        padActive_=tilt>=(padActive_?kPadOff:kPadOn);
        bool padEdge=padActive_&&!padWas;
        if(owner_==Owner::Kbm&&padActive_&&std::hypot(padX-refX_,padY-refY_)>kPadMove)padEdge=true;
        if(kbmActive)kbmUntilUs_=nowUs+holdUs_;
        const bool kbmLive=kbmActive||nowUs<kbmUntilUs_;
        const bool kbmNew=(kbmEdge&&kbmLive)||(kbmLive&&!kbmWas_);
        kbmWas_=kbmLive;
        Owner next=owner_;
        if(kbmNew&&!padEdge)next=Owner::Kbm;
        else if(padEdge&&!kbmNew)next=Owner::Pad;      // both in one poll: whoever had it keeps it
        if(next==Owner::Kbm&&!kbmLive)next=Owner::Pad;
        else if(next==Owner::Pad&&!padActive_&&kbmLive)next=Owner::Kbm;
        if(next==Owner::Kbm&&owner_!=Owner::Kbm){refX_=padX;refY_=padY;}   // where the stick was when it lost
        owner_=next;
        return owner_;
    }
private:
    std::uint64_t holdUs_,kbmUntilUs_=0;
    Owner owner_=Owner::Pad;
    bool padActive_=false,kbmWas_=false;
    double refX_=0,refY_=0;
};

// A table in which no key does anything (KeyboardPadBindings starts with a first-person layout).
inline KeyboardPadBindings kbmNoBindings(){KeyboardPadBindings none;none.clear();return none;}

// One way of playing: how the mouse aims, what the keys do, and how keys become analog values.
struct KbmProfile {
    enum class Mouse : std::uint8_t {
        Camera,   // right stick (or the motion sensor in Gyro mode): look and aim
        Steer,    // left stick, left / right, as a wheel position: the mouse position is the steering angle
        None};    // ignored
    MouseAimConfig aim;
    KeyboardPadBindings bindings;
    Mouse mouse=Mouse::Camera;
    // Keys on the left stick (LUp, LDown, LLeft, LRight).
    double moveDeadzone=0;      // the game's left-stick dead zone: output starts above it
    double moveRiseMs=0;        // centre to full; 0: at once
    double moveFallMs=0;        // full to centre; 0: at once
    double walkTilt=0.5;        // tilt while the Walk key is held
    // Keys on the triggers (L2, R2).
    double triggerRiseMs=0;     // released to fully pressed; 0: at once
    double triggerFallMs=0;     // fully pressed to released; 0: at once
    double triggerStart=0;      // a press starts here (0..0.9): past the part of the travel the game ignores
    int triggerStep=1;          // values are multiples of this (1..64): every new value is a packet to the console
    // Mouse::Steer.
    double steerRangeCounts=800;// counts from centre to full lock
    double steerReturnMs=0;     // full lock back to centre while the mouse rests; 0: the wheel stays where it is
    double steerCurve=1;        // the game's steering ~ tilt^curve
    bool operator==(const KbmProfile&)const=default;
};
enum class KbmProfileId : std::uint8_t {Walk,Aim,Drive};
inline const char* kbmProfileName(KbmProfileId id)noexcept{return id==KbmProfileId::Walk?"walk":id==KbmProfileId::Aim?"aim":"drive";}
// Walk and Drive are chosen by the player with a key bound to PadAction::Drive. Aim replaces Walk
// for as long as the pad action `aimWhile` is held, on the keyboard / mouse or on the physical
// controller.
struct KbmProfileSet {
    std::string name;
    std::string game;            // the player's own name for the game (profile file `game=`); never detected
    double countsPer360=0;       // calibration target: mouse counts for one full turn; 0: not set (KbmCalibration.h)
    std::array<KbmProfile,3> profiles;
    PadAction aimWhile=PadAction::None;
    PadAction sprintWhile=PadAction::None;  // held together with a movement key: the sprint context (input profiles only)
    KbmInputSettings input;                 // input performance profile (KbmInputProfile.h); Legacy: exactly the behaviour before it
    // The menu keys (stream "kbm"): a key bound to PadAction::Menu changes from the profile in use to these and
    // back (pause menu, phone, map lists). Chosen by the player's own key, never by reading the game. While they are
    // in use the mouse does not turn the camera. Empty (the default, and every file without menu.bind lines): no
    // menu keys, and a Menu key only presses Options.
    KeyboardPadBindings menu=kbmNoBindings();
    // In the driving profile, a key whose action is this also changes back to walking when it goes down (its button is
    // still pressed): the player's own exit key (GTA: Triangle). None (the default, every older set): only the Drive key.
    PadAction leaveDriveWith=PadAction::None;
    KbmProfile& operator[](KbmProfileId id)noexcept{return profiles[std::size_t(id)];}
    const KbmProfile& operator[](KbmProfileId id)const noexcept{return profiles[std::size_t(id)];}
    bool operator==(const KbmProfileSet&)const=default;
};

class KeyboardMousePad {
public:
    static constexpr std::uint64_t kWheelPulseUs=60000; // a wheel notch holds its button this long
    static constexpr std::uint64_t kMouseHoldUs=250000; // the mouse keeps the right stick this long after it last moved
    static constexpr std::uint8_t kTriggerHeld=64;      // a physical trigger this deep is "held" for aimWhile
    static constexpr std::size_t kMaxHeld=64;           // more keys than a keyboard can report
    // One profile for everything: no switching.
    explicit KeyboardMousePad(MouseAimConfig aim={},KeyboardPadBindings bindings={}):KeyboardMousePad(single(aim,std::move(bindings))){}
    explicit KeyboardMousePad(KbmProfileSet set):set_(sanitized(std::move(set))),aim_(set_.profiles[0].aim){}
    MouseAim& aim()noexcept{return aim_;}
    const KbmProfileSet& profiles()const noexcept{return set_;}
    KbmProfileId profile()const noexcept{return active_;}      // what the last poll used
    bool driving()const noexcept{return manual_==KbmProfileId::Drive;}
    bool inMenu()const noexcept{return menu_;}                 // the menu keys are in use (the last poll's choice)
    void key(std::uint32_t virtualKey,bool down){set(virtualKey,down);}
    void mouseButton(int button,bool down){if(button>=1&&button<=5)set(kMouseButtonBase+std::uint32_t(button),down);}
    void mouseMove(int dx,int dy){
        if(menu_)return;                                       // the menu keys: the camera stands still
        const KbmProfile& p=set_[active_];
        if(p.mouse==KbmProfile::Mouse::Camera){aim_.add(dx,dy);fast_.add(dx,dy);}
        else if(p.mouse==KbmProfile::Mouse::Steer)steerIn_+=dx*p.aim.sensitivityX*(fine_?p.aim.fineScale:1.0)*(p.aim.invertX?-1:1);
    }
    // When the counts of the last mouseMove were read (steady clock, microseconds): timing=report times the
    // speed of a poll from these (KbmInputProfile.h). Without it a poll is timed by its interval, as before.
    void mouseStamp(std::uint64_t us)noexcept{fast_.stamp(us);}
    void scroll(int delta){if(delta!=0)wheel_=delta>0?kWheelUp:kWheelDown;}
    // Capture ended, focus lost or a device unplugged: nothing stays held, no movement is still
    // on its way, no ramp is half-way, and the next poll is the physical pad alone. The profile
    // stays: the car is still there when the window comes back.
    void releaseAll(){clearInputs();blocked_.clear();menu_=false;}   // a release also leaves the menu keys
    // The session ended: also back to the walking profile.
    void restart(){releaseAll();manual_=active_=KbmProfileId::Walk;menu_=menuToggle_=false;aim_.setConfig(set_[active_].aim);}
    // Other numbers and keys (a calibration just finished): everything held is released, the
    // walking / driving choice stays.
    void setProfiles(KbmProfileSet set){
        set_=sanitized(std::move(set));
        releaseAll();
        menu_=menuToggle_=false;
        active_=manual_;
        aim_.setConfig(set_[active_].aim);
    }
    // Another input performance profile, or other numbers in it: from the next poll on. Nothing
    // held is released, no movement on its way is lost (both transforms receive every count, see
    // mouseMove), no ramp restarts.
    void setInputSettings(const KbmInputSettings& input){set_.input=kbmSanitized(input);}
    const KbmInputSettings& inputSettings()const noexcept{return set_.input;}
    void setContextOverride(KbmContextOverride o)noexcept{set_.input.override_=o;}
    KbmContext context()const noexcept{return context_;}
    KbmImmediateAim& immediateAim()noexcept{return fast_;}
    // Polls in which the mouse moved the camera, and those of them in which it asked for more
    // than a fully tilted stick (input profiles only), and context changes.
    std::uint64_t movingPolls()const noexcept{return movingPolls_;}
    std::uint64_t saturatedPolls()const noexcept{return saturatedPolls_;}
    std::uint64_t contextSwitches()const noexcept{return contextSwitches_;}
    // Keys, mouse buttons and key-driven stick or trigger values held right now (for the
    // stuck-input counters: what a release had to let go of).
    std::size_t heldCount()const noexcept{return held_.size()+(l2_>0?1:0)+(r2_>0?1:0)+((moveX_!=0||moveY_!=0||steer_!=0)?1:0);}
    // The camera transform the next poll would run, on a copy: nothing of the pad changes. `simple`:
    // with the fallback's tuning (straight curve, drop, no smoothing) instead of the one in use.
    // For the self-check, which times both on the same counts (KbmInputStats.h).
    KbmImmediateAim::Output shadowStep(bool simple,std::uint64_t elapsedUs,std::uint64_t nowUs)const noexcept{
        return shadowStep(fast_,simple,elapsedUs,nowUs);
    }
    // The same on counts saved before a poll (`from`: a copy of immediateAim() taken then), so that
    // the timing can run after that poll's state has been handed off.
    KbmImmediateAim::Output shadowStep(KbmImmediateAim copy,bool simple,std::uint64_t elapsedUs,std::uint64_t nowUs)const noexcept{
        const KbmProfile& p=set_[active_];
        KbmContextTuning tuning=set_.input[context_];
        if(simple){tuning.curve=KbmCurve{};tuning.saturation=KbmSaturation::Drop;tuning.carryMs=0;}
        const KbmGameResponse response=gameResponse(p);
        const double aimScale=(context_==KbmContext::Aim?set_.input.adsFactor:1.0)*(fine_?p.aim.fineScale:1.0);
        return copy.step(tuning,aimScale,p.aim.invertX,p.aim.invertY,simple?0.0:set_.input.smoothingMs,elapsedUs,nowUs,tuning.game?&response:nullptr,set_.input.timing,set_.input.stickSteps);
    }
    // Leading-edge transmission (an experiment, off by default): the pad state that may be sent at
    // once when the key or mouse button `code` has just gone down, instead of with the next poll.
    // It is `last` (the state of the last poll) with that one button pressed, and only for a plain
    // button or a trigger without a ramp: sticks, ramps and the mouse always wait for the poll.
    // Nothing when the press changes nothing, was not accepted (blocked, a repeat), or could mean
    // something else by the next poll (an on-foot / car switch is pending, or the key differs
    // between the walking and the aiming layout). It changes nothing in the pad: the next poll
    // builds its state from the held keys as always.
    std::optional<ControllerState> earlyPress(std::uint32_t code,const ControllerState& last)const{
        if(toggle_||menuToggle_||std::find(held_.begin(),held_.end(),code)==held_.end())return std::nullopt;
        const PadAction action=keys().action(code);
        if(manual_==KbmProfileId::Walk&&set_.aimWhile!=PadAction::None&&
           set_[KbmProfileId::Walk].bindings.action(code)!=set_[KbmProfileId::Aim].bindings.action(code))return std::nullopt;
        // A trigger goes early only at full depth, so only when no ramp could follow: neither in the
        // profile in use nor, on foot with an aim key, in the one the next poll may use (walk / aim).
        const auto ramps=[this]{
            if(set_[active_].triggerRiseMs>0)return true;
            return manual_==KbmProfileId::Walk&&set_.aimWhile!=PadAction::None&&
                (set_[KbmProfileId::Walk].triggerRiseMs>0||set_[KbmProfileId::Aim].triggerRiseMs>0);
        };
        ControllerState out=last;
        switch(action){
        case PadAction::Cross:out.buttons|=ControllerState::Cross;break;
        case PadAction::Circle:out.buttons|=ControllerState::Circle;break;
        case PadAction::Square:out.buttons|=ControllerState::Square;break;
        case PadAction::Triangle:out.buttons|=ControllerState::Triangle;break;
        case PadAction::L1:out.buttons|=ControllerState::L1;break;
        case PadAction::R1:out.buttons|=ControllerState::R1;break;
        case PadAction::L3:out.buttons|=ControllerState::L3;break;
        case PadAction::R3:out.buttons|=ControllerState::R3;break;
        case PadAction::Options:out.buttons|=ControllerState::Options;break;
        case PadAction::Share:out.buttons|=ControllerState::Share;break;
        case PadAction::Touchpad:out.buttons|=ControllerState::Touchpad;break;
        case PadAction::PS:out.buttons|=ControllerState::PS;break;
        case PadAction::DUp:out.buttons|=ControllerState::Up;break;
        case PadAction::DDown:out.buttons|=ControllerState::Down;break;
        case PadAction::DLeft:out.buttons|=ControllerState::Left;break;
        case PadAction::DRight:out.buttons|=ControllerState::Right;break;
        case PadAction::L2:if(ramps())return std::nullopt;out.l2=255;break;
        case PadAction::R2:if(ramps())return std::nullopt;out.r2=255;break;
        default:return std::nullopt;
        }
        out.inputActive=true;
        if(out==last)return std::nullopt;
        return out;
    }
    // Lays keyboard and mouse over the physical pad's state for this poll. Buttons add up and a
    // trigger takes the deeper of the two. The sticks go to the device that acted last (see
    // StickHandover): movement keys against the left stick, the mouse against the right one
    // (in Gyro mode the mouse is the motion sensor and leaves the stick to the controller).
    ControllerState apply(ControllerState pad,std::uint64_t nowUs){
        const std::uint64_t elapsed=lastUs_&&nowUs>lastUs_?nowUs-lastUs_:4000;lastUs_=nowUs;
        const double dtMs=std::clamp(double(elapsed)/1000.0,0.5,50.0);
        pad.inputActive=true;
        if(toggle_){toggle_=false;if(!menu_)switchTo(manual_==KbmProfileId::Drive?KbmProfileId::Walk:KbmProfileId::Drive);}   // not from the menu keys
        if(menuToggle_){menuToggle_=false;switchMenu(!menu_);}
        select(pad);
        const KbmProfile& p=set_[active_];
        const KeyboardPadBindings& bindings=keys();
        if(wheel_){pulseAction_=bindings.action(wheel_);pulseUntilUs_=nowUs+kWheelPulseUs;wheel_=0;}
        int moveX=0,moveY=0;bool walk=false,fine=false,l2=false,r2=false,movementHeld=false;
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
            case PadAction::L2:l2=true;break;
            case PadAction::R2:r2=true;break;
            case PadAction::LUp:--moveY;movementHeld=true;break;
            case PadAction::LDown:++moveY;movementHeld=true;break;
            case PadAction::LLeft:--moveX;movementHeld=true;break;
            case PadAction::LRight:++moveX;movementHeld=true;break;
            case PadAction::Walk:walk=true;break;
            case PadAction::Fine:fine=true;break;
            case PadAction::Drive:break;      // acted on when the key went down
            case PadAction::Menu:pad.buttons|=ControllerState::Options;break;   // the change of keys: when it went down
            case PadAction::None:break;
            }
        };
        for(const auto code:held_)press(bindings.action(code));
        if(pulseAction_!=PadAction::None){if(nowUs<pulseUntilUs_)press(pulseAction_);else pulseAction_=PadAction::None;}
        // Input profiles: a key pressed and let go between two polls (a late poll: a busy interface
        // thread) was kept for this one poll so that the press reaches the console; now it is released.
        // A tap that a foot / car switch in this same poll moved to blocked_ is let go too: its release
        // has already come, so nothing else would ever unblock it and its next press would be lost.
        if(!taps_.empty()){for(const auto code:taps_){std::erase(held_,code);std::erase(blocked_,code);}taps_.clear();}
        fresh_.clear();

        // Triggers: a key is a ramp, the physical trigger a depth; the deeper one counts.
        l2_=ramped(l2_,l2,p,dtMs);r2_=ramped(r2_,r2,p,dtMs);
        pad.l2=std::max(pad.l2,triggerValue(l2_,p.triggerStep));pad.r2=std::max(pad.r2,triggerValue(r2_,p.triggerStep));

        // Left stick: the keys' direction (opposite keys cancel; a diagonal stays on the unit
        // circle like a real stick), approached at the profile's ramp, plus the steering wheel.
        const double length=std::hypot(double(moveX),double(moveY)),tilt=walk?p.walkTilt:1.0;
        const double targetX=length>0?moveX/length*tilt:0.0,targetY=length>0?moveY/length*tilt:0.0;
        const double gapX=targetX-moveX_,gapY=targetY-moveY_,gap=std::hypot(gapX,gapY);
        const double rampMs=std::hypot(targetX,targetY)>=std::hypot(moveX_,moveY_)-1e-9?p.moveRiseMs:p.moveFallMs;
        if(rampMs<=0||gap<=dtMs/rampMs+1e-12){moveX_=targetX;moveY_=targetY;}
        else{moveX_+=gapX/gap*dtMs/rampMs;moveY_+=gapY/gap*dtMs/rampMs;}
        bool steered=false;
        if(p.mouse==KbmProfile::Mouse::Steer){
            if(steerIn_!=0){steer_=std::clamp(steer_+steerIn_/p.steerRangeCounts,-1.0,1.0);steered=true;}
            else if(p.steerReturnMs>0)steer_=steer_>0?std::max(0.0,steer_-dtMs/p.steerReturnMs):std::min(0.0,steer_+dtMs/p.steerReturnMs);
            if(std::abs(steer_)<1e-9)steer_=0;
        }else steer_=0;
        steerIn_=0;
        const bool moveEdge=moveEdge_||steered;moveEdge_=false;
        const bool leftActive=movementHeld||moveX_!=0||moveY_!=0||steer_!=0;
        if(left_.update(pad.leftX/32767.0,pad.leftY/32767.0,leftActive,moveEdge,nowUs)==StickHandover::Owner::Kbm){
            double x=moveX_+(steer_==0?0.0:std::copysign(std::pow(std::abs(steer_),1.0/p.steerCurve),steer_)),y=moveY_;
            x=std::clamp(x,-1.0,1.0);
            double size=std::hypot(x,y);
            if(size>1){x/=size;y/=size;size=1;}
            // Past the game's dead zone from the first step of a ramp on.
            const double scale=size>0?(p.moveDeadzone+(1-p.moveDeadzone)*size)/size:0.0;
            pad.leftX=std::int16_t(std::lround(x*scale*32767));
            pad.leftY=std::int16_t(std::lround(y*scale*32767));
        }

        // Which tuning the camera uses this poll (input profiles; Legacy ignores it).
        const KbmContext nextContext=contextFor(movementHeld);
        if(nextContext!=context_){context_=nextContext;++contextSwitches_;}
        // Mouse on the camera.
        fine_=fine;aim_.setFine(fine);
        if(p.mouse!=KbmProfile::Mouse::Camera||menu_){fast_.reset();return pad;}
        MouseAim::Output aim;
        if(set_.input.immediate()&&aim_.config().mode==MouseAimConfig::Mode::Stick){
            // Immediate transform: this poll's counts only. MouseAim's copy of them is dropped.
            aim_.reset();
            const auto& tuning=set_.input[context_];
            const double aimScale=(context_==KbmContext::Aim?set_.input.adsFactor:1.0)*(fine?p.aim.fineScale:1.0);
            const KbmGameResponse response=gameResponse(p);
            const auto fast=fast_.step(tuning,aimScale,p.aim.invertX,p.aim.invertY,set_.input.smoothingMs,elapsed,nowUs,tuning.game?&response:nullptr,set_.input.timing,set_.input.stickSteps);
            aim.x=fast.x;aim.y=fast.y;aim.moving=fast.moving;aim.received=fast.received;
            if(fast.moving){++movingPolls_;if(fast.saturated)++saturatedPolls_;}
        }else{
            fast_.reset();
            aim=aim_.step(elapsed);
        }
        if(aim_.config().mode==MouseAimConfig::Mode::Gyro){
            // The sensor of a pad lying still, turned by the mouse. One sample per poll.
            pad.gyro={aim.pitchRate,aim.yawRate,0.f};pad.accel={0.f,1.f,0.f};
            motionUs_+=elapsed;pad.motionTimestampUs=motionUs_;pad.motionValid=true;
            pad.motionSamples[0]={pad.gyro,pad.accel,motionUs_};pad.motionSampleCount=1;
        }else if(right_.update(pad.rightX/32767.0,pad.rightY/32767.0,aim.moving||aim.received,false,nowUs)==StickHandover::Owner::Kbm){
            pad.rightX=aim.x;pad.rightY=aim.y; // at rest between two reports of a slow mouse
        }
        return pad;
    }
private:
    // The game's response as a profile holds it (the calibration's numbers). Only the vertical /
    // horizontal ratio is taken from its sensitivities; the sensitivity itself is the context's.
    static KbmGameResponse gameResponse(const KbmProfile& p)noexcept{
        return {p.aim.deadzone,p.aim.fullSpeedCounts,p.aim.curve,p.aim.curvePoints,p.aim.sensitivityY/p.aim.sensitivityX};
    }
    static KbmProfileSet single(const MouseAimConfig& aim,KeyboardPadBindings bindings){
        KbmProfileSet set;
        for(auto& profile:set.profiles){profile.aim=aim;profile.bindings=bindings;}
        return set;
    }
    // Every number inside its limits, whatever the caller built.
    static KbmProfileSet sanitized(KbmProfileSet set){
        const auto within=[](double v,double low,double high,double fallback){return std::isfinite(v)?std::clamp(v,low,high):fallback;};
        set.input=kbmSanitized(set.input);
        for(auto& p:set.profiles){
            p.aim=MouseAim(p.aim).config();
            p.moveDeadzone=within(p.moveDeadzone,0,.5,0);p.moveRiseMs=within(p.moveRiseMs,0,2000,0);p.moveFallMs=within(p.moveFallMs,0,2000,0);
            p.walkTilt=within(p.walkTilt,.1,1,.5);
            p.triggerRiseMs=within(p.triggerRiseMs,0,2000,0);p.triggerFallMs=within(p.triggerFallMs,0,2000,0);
            p.triggerStart=within(p.triggerStart,0,.9,0);p.triggerStep=std::clamp(p.triggerStep,1,64);
            p.steerRangeCounts=within(p.steerRangeCounts,10,100000,800);p.steerReturnMs=within(p.steerReturnMs,0,5000,0);
            p.steerCurve=within(p.steerCurve,.25,4,1);
        }
        return set;
    }
    static bool moves(PadAction action)noexcept{return action==PadAction::LUp||action==PadAction::LDown||action==PadAction::LLeft||action==PadAction::LRight;}
    // A key held as a trigger: jumps to the start value, then rises; falls when released.
    static double ramped(double value,bool pressed,const KbmProfile& p,double dtMs)noexcept{
        if(pressed){
            if(p.triggerRiseMs<=0)return 1.0;
            return value<p.triggerStart?p.triggerStart:std::min(1.0,value+dtMs/p.triggerRiseMs);
        }
        if(p.triggerFallMs<=0)return 0.0;
        value-=dtMs/p.triggerFallMs;
        return value<1e-9?0.0:value;
    }
    // 0..1 as the trigger byte in whole steps. A trigger that is pressed at all is never 0: the
    // press itself reaches the console with the first poll.
    static std::uint8_t triggerValue(double value,int step)noexcept{
        if(value<=0)return 0;
        if(value>=1)return 255;
        const long steps=std::max(1L,std::lround(value*255/step));
        return std::uint8_t(std::min(255L,steps*step));
    }
    static bool shows(const ControllerState& pad,PadAction action)noexcept{
        switch(action){
        case PadAction::L2:return pad.l2>=kTriggerHeld;
        case PadAction::R2:return pad.r2>=kTriggerHeld;
        case PadAction::L1:return (pad.buttons&ControllerState::L1)!=0;
        case PadAction::R1:return (pad.buttons&ControllerState::R1)!=0;
        case PadAction::L3:return (pad.buttons&ControllerState::L3)!=0;
        case PadAction::R3:return (pad.buttons&ControllerState::R3)!=0;
        case PadAction::Cross:return (pad.buttons&ControllerState::Cross)!=0;
        case PadAction::Circle:return (pad.buttons&ControllerState::Circle)!=0;
        case PadAction::Square:return (pad.buttons&ControllerState::Square)!=0;
        case PadAction::Triangle:return (pad.buttons&ControllerState::Triangle)!=0;
        default:return false;
        }
    }
    // Which profile this poll uses. `pad` is still the physical controller alone here.
    void select(const ControllerState& pad){
        KbmProfileId next=manual_;
        if(!menu_&&manual_==KbmProfileId::Walk&&set_.aimWhile!=PadAction::None){
            bool aiming=shows(pad,set_.aimWhile);
            // Looked up in the walking layout, so that the aiming layout cannot unbind its own switch.
            for(const auto code:held_)aiming=aiming||set_[KbmProfileId::Walk].bindings.action(code)==set_.aimWhile;
            if(aiming)next=KbmProfileId::Aim;
        }
        if(next==active_)return;
        // Keys stay as they are: the pad state is built from the held keys on every poll, so
        // nothing can stick. Mouse movement on its way is kept and finishes under the new numbers.
        active_=next;aim_.setConfig(set_[active_].aim);
    }
    // The player's own switch between foot and car. A key held through it whose meaning changes
    // (forward becomes the throttle, fire becomes the horn) is released and stays ignored until it
    // has been let go and pressed again, and what it was doing stops at once: the left stick if it
    // moved it, its trigger if it pressed one. A key that means the same in both keeps working,
    // and mouse movement on its way keeps turning the camera when the mouse is the camera in both
    // (the new numbers apply from the next poll): nothing that keeps its meaning jumps or lets go.
    void switchTo(KbmProfileId profile){
        const KbmProfile& from=set_[active_];
        const KbmProfile& to=set_[profile];
        std::vector<std::uint32_t> keep;
        bool moveDropped=false,l2Dropped=false,r2Dropped=false;
        for(const auto code:held_){
            const auto was=from.bindings.action(code);
            if(was==to.bindings.action(code)){keep.push_back(code);continue;}
            blocked_.push_back(code);
            moveDropped=moveDropped||moves(was);
            l2Dropped=l2Dropped||was==PadAction::L2;
            r2Dropped=r2Dropped||was==PadAction::R2;
        }
        held_=std::move(keep);
        wheel_=0;pulseAction_=PadAction::None;moveEdge_=false;
        if(moveDropped){moveX_=moveY_=0;}
        if(l2Dropped)l2_=0;
        if(r2Dropped)r2_=0;
        if(from.mouse!=KbmProfile::Mouse::Camera||to.mouse!=KbmProfile::Mouse::Camera){aim_.reset();fast_.reset();right_.reset();}
        if(to.mouse!=KbmProfile::Mouse::Steer){steer_=steerIn_=0;}
        manual_=profile;
    }
    // Walk / aim / drive follow the profile in use; sprint is a held sprintWhile key with a movement
    // key on foot. A manual override wins over all of it (it changes the camera tuning only, never
    // the bindings or the held keys).
    KbmContext contextFor(bool movementHeld)const{
        switch(set_.input.override_){
        case KbmContextOverride::Walk:return KbmContext::Walk;
        case KbmContextOverride::Aim:return KbmContext::Aim;
        case KbmContextOverride::Sprint:return KbmContext::Sprint;
        case KbmContextOverride::Drive:return KbmContext::Drive;
        default:break;
        }
        if(active_==KbmProfileId::Drive)return KbmContext::Drive;
        if(active_==KbmProfileId::Aim)return KbmContext::Aim;
        if(movementHeld&&set_.sprintWhile!=PadAction::None)
            for(const auto code:held_)if(set_[KbmProfileId::Walk].bindings.action(code)==set_.sprintWhile)return KbmContext::Sprint;
        return KbmContext::Walk;
    }
    // The keys in use: the menu keys while they are on, else the profile's.
    const KeyboardPadBindings& keys()const noexcept{return menu_?set_.menu:set_[active_].bindings;}
    // Into or out of the menu keys. As at the foot / car switch: a key held through it whose meaning changes is
    // released and ignored until it is let go and pressed again, what it was doing stops at once, and a key that
    // means the same in both (the Menu key itself) keeps working. Mouse movement on its way is dropped: the
    // camera never turns because of motion made in a menu. Without menu keys nothing changes but Options.
    void switchMenu(bool on){
        if(on&&set_.menu.size()==0)return;
        const KeyboardPadBindings& from=keys();
        const KeyboardPadBindings& to=on?set_.menu:set_[active_].bindings;
        std::vector<std::uint32_t> keep;
        bool moveDropped=false,l2Dropped=false,r2Dropped=false;
        for(const auto code:held_){
            const auto was=from.action(code);
            if(was==to.action(code)){keep.push_back(code);continue;}
            blocked_.push_back(code);
            moveDropped=moveDropped||moves(was);
            l2Dropped=l2Dropped||was==PadAction::L2;
            r2Dropped=r2Dropped||was==PadAction::R2;
        }
        held_=std::move(keep);
        wheel_=0;pulseAction_=PadAction::None;moveEdge_=false;
        if(moveDropped){moveX_=moveY_=0;}
        if(l2Dropped)l2_=0;
        if(r2Dropped)r2_=0;
        aim_.reset();fast_.reset();right_.reset();steer_=steerIn_=0;
        menu_=on;
    }
    void clearInputs(){
        held_.clear();fresh_.clear();taps_.clear();wheel_=0;pulseAction_=PadAction::None;moveEdge_=toggle_=menuToggle_=fine_=false;
        l2_=r2_=moveX_=moveY_=steer_=steerIn_=0;
        aim_.reset();fast_.reset();aim_.setFine(false);left_.reset();right_.reset();
    }
    void set(std::uint32_t code,bool down){
        const auto listed=[code](const std::vector<std::uint32_t>& list){return std::find(list.begin(),list.end(),code)!=list.end();};
        if(!down){
            std::erase(blocked_,code);
            // Pressed since the last poll and already let go (input profiles only; the legacy path is as before):
            // it stays held for the one poll that comes next, else no pad state would ever show it.
            if(set_.input.immediate()&&listed(fresh_)&&listed(held_)){if(!listed(taps_))taps_.push_back(code);return;}
            std::erase(held_,code);return;
        }
        std::erase(taps_,code);   // pressed again before that poll: simply held
        if(std::find(blocked_.begin(),blocked_.end(),code)!=blocked_.end())return;
        // Auto-repeat sends the press again: only the first one is a new command.
        if(std::find(held_.begin(),held_.end(),code)!=held_.end()||held_.size()>=kMaxHeld)return;
        held_.push_back(code);
        if(set_.input.immediate())fresh_.push_back(code);
        auto action=keys().action(code);
        // The way out of the menu keys is always there: a key that is the Menu key of the profile in use leaves them
        // too, even when the menu table gives it another meaning or none.
        if(menu_&&action!=PadAction::Menu&&set_[active_].bindings.action(code)==PadAction::Menu)action=PadAction::Menu;
        if(action==PadAction::Drive)toggle_=true;
        else if(action==PadAction::Menu)menuToggle_=true;
        else if(!menu_&&manual_==KbmProfileId::Drive&&set_.leaveDriveWith!=PadAction::None&&action==set_.leaveDriveWith)toggle_=true;
        else if(moves(action))moveEdge_=true;
    }
    KbmProfileSet set_;
    MouseAim aim_;
    KbmImmediateAim fast_;
    KbmContext context_=KbmContext::Walk;
    std::uint64_t movingPolls_=0,saturatedPolls_=0,contextSwitches_=0;
    KbmProfileId manual_=KbmProfileId::Walk,active_=KbmProfileId::Walk;
    std::vector<std::uint32_t> held_,blocked_;
    std::vector<std::uint32_t> fresh_,taps_;   // input profiles: pressed since the last poll; of those, already let go
    StickHandover left_{0},right_{kMouseHoldUs};
    bool moveEdge_=false,toggle_=false,fine_=false;
    bool menu_=false,menuToggle_=false;  // the menu keys are in use; a Menu key went down since the last poll
    double l2_=0,r2_=0;                 // key triggers, 0..1
    double moveX_=0,moveY_=0;           // key stick, unit disc
    double steer_=0,steerIn_=0;         // wheel position -1..1 and counts since the last poll
    std::uint32_t wheel_=0;
    PadAction pulseAction_=PadAction::None;
    std::uint64_t pulseUntilUs_=0,lastUs_=0,motionUs_=0;
};
} // namespace veyra::remoteplay
