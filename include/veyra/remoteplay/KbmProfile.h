// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental (VEYRA_EXP_RP_KBM): keyboard and mouse profiles as a plain text file, one
// `key=value` per line (VEYRA_EXP_RP_KBM_PROFILE=<path>), and the built-in set the file starts
// from. Portable and unit-tested, malformed input included. The guide is docs/KEYBOARD_MOUSE.md.
//
//   # comment (a whole line; `;` works too)
//   name=GTA 6 first try
//   game=GTA 6                   the player's name for the game; picks the calibrated profile (KbmCalibrationFile.h)
//   counts_per_360=8000          calibration target: mouse counts for one full turn
//   mode=stick | gyro            how the mouse aims, all profiles
//   stick_bits=8..16             stick resolution the output is rounded to, all profiles
//   aim_while=L2 | none | ...    the pad button or trigger that selects the aim profile while held
//   <profile>.<setting>=<number> profile: walk, aim, drive, or all for the three of them
//   <profile>.mouse=camera | steer | none
//   <profile>.bind.<key>=<action>
//
// The aim profile uses the walking profile's keys; `aim.bind.<key>` makes one key differ while
// aiming. Numbers are per profile. A line that cannot be read is reported with its number and
// changes nothing; every other line still counts. Values outside their range are refused, not
// clamped, so a typing error never becomes a surprising setting.
#include "KeyboardMousePad.h"
#include <array>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
namespace veyra::remoteplay {
inline constexpr std::size_t kKbmProfileMaxBytes=65536,kKbmProfileMaxLines=2000,kKbmProfileMaxLineLength=512,
    kKbmProfileMaxProblems=32,kKbmProfileMaxName=64;
inline constexpr double kKbmCountsPer360Min=100,kKbmCountsPer360Max=1e6;

// The set used when no file is given, and the one a file changes. A layout in the style of the
// earlier GTA games' keyboard defaults mapped onto their controller layout, written from memory:
// GTA 6 was not available to check it against, and no number here was tuned on a console.
// Every value is a starting point for docs/KEYBOARD_MOUSE.md's tuning procedure.
inline KbmProfileSet kbmBuiltinProfiles(){
    KbmProfileSet set;
    set.name="built-in GTA-style layout (untuned guess)";
    set.aimWhile=PadAction::L2;
    MouseAimConfig aim;
    aim.stickBits=8;          // a game reads the stick in 8 bits
    aim.smoothingMs=8;        // little added delay (2 ms mean at a 4 ms poll); 33 = opt-in smoother camera (VEYRA_EXP_RP_KBM_SMOOTH_MS or smooth_ms)
    for(auto& profile:set.profiles){profile.aim=aim;profile.bindings.clear();}
    auto& walk=set[KbmProfileId::Walk];auto& aiming=set[KbmProfileId::Aim];auto& drive=set[KbmProfileId::Drive];
    walk.bindings.parse(
        "W=LUp;S=LDown;A=LLeft;D=LRight;Alt=Walk;"
        "Shift=Cross;Space=Square;F=Triangle;R=Circle;"
        "Q=R1;Tab=L1;Ctrl=L3;C=R3;V=Touchpad;E=DRight;"
        "Up=DUp;Down=DDown;Left=DLeft;Right=DRight;"
        "Esc=Options;Back=Share;Home=PS;"
        "Mouse1=R2;Mouse3=L2;Mouse2=R3;Mouse4=L1;Mouse5=R1;WheelUp=DLeft;WheelDown=DRight;"
        "X=Fine;Caps=Drive");
    aiming.bindings=walk.bindings;
    aiming.aim.sensitivityX=aiming.aim.sensitivityY=0.6;
    drive.bindings.parse(
        "W=R2;S=L2;A=LLeft;D=LRight;"
        "Space=R1;F=Triangle;E=L3;C=R3;R=Circle;Shift=Cross;Ctrl=Square;Q=DLeft;H=DRight;V=Touchpad;Tab=L1;"
        "Up=DUp;Down=DDown;Left=DLeft;Right=DRight;"
        "Esc=Options;Back=Share;Home=PS;"
        "Mouse1=L1;Mouse2=R3;WheelUp=DLeft;WheelDown=DRight;"
        "X=Fine;Caps=Drive");
    drive.moveDeadzone=0.10;drive.moveRiseMs=120;drive.moveFallMs=80;
    drive.triggerRiseMs=150;drive.triggerFallMs=80;drive.triggerStart=0.15;drive.triggerStep=15;
    return set;
}

struct KbmProfileProblem {unsigned line=0;std::string what;};
struct KbmProfileParse {
    KbmProfileSet set;
    unsigned applied=0;                       // lines that set something
    std::size_t problemCount=0;               // all of them; `problems` keeps the first kKbmProfileMaxProblems
    std::vector<KbmProfileProblem> problems;
    bool truncated=false;                     // longer than the limits: the rest was not read
    bool ok()const noexcept{return problemCount==0&&!truncated;}
};

namespace kbmprofile {
enum class Field : std::uint8_t {SensX,SensY,Full,Deadzone,Curve,Accel,AccelCap,SmoothMs,CarryMs,Fine,GyroDeg,InvertX,InvertY,
    Curve25,Curve50,Curve75,
    MoveDeadzone,MoveRiseMs,MoveFallMs,WalkTilt,TriggerRiseMs,TriggerFallMs,TriggerStart,TriggerStep,SteerRange,SteerReturnMs,SteerCurve};
struct FieldInfo {std::string_view name;Field field;double low,high;bool whole;};
// The ranges are those MouseAim and KeyboardMousePad accept; a value read here is never changed again.
inline constexpr FieldInfo kFields[]={
    {"sens_x",Field::SensX,0.01,100,false},{"sens_y",Field::SensY,0.01,100,false},{"full",Field::Full,100,1e6,false},
    {"deadzone",Field::Deadzone,0,0.5,false},{"curve",Field::Curve,0.25,4,false},{"accel",Field::Accel,0,4,false},
    {"accel_cap",Field::AccelCap,1,8,false},{"smooth_ms",Field::SmoothMs,0,50,false},{"carry_ms",Field::CarryMs,0,250,false},
    {"fine",Field::Fine,0.05,1,false},{"gyro_deg",Field::GyroDeg,0.0005,1,false},
    {"curve_25",Field::Curve25,0,1,false},{"curve_50",Field::Curve50,0,1,false},{"curve_75",Field::Curve75,0,1,false},
    {"invert_x",Field::InvertX,0,1,true},{"invert_y",Field::InvertY,0,1,true},
    {"move_deadzone",Field::MoveDeadzone,0,0.5,false},{"move_rise_ms",Field::MoveRiseMs,0,2000,false},{"move_fall_ms",Field::MoveFallMs,0,2000,false},
    {"walk_tilt",Field::WalkTilt,0.1,1,false},
    {"trigger_rise_ms",Field::TriggerRiseMs,0,2000,false},{"trigger_fall_ms",Field::TriggerFallMs,0,2000,false},
    {"trigger_start",Field::TriggerStart,0,0.9,false},{"trigger_step",Field::TriggerStep,1,64,true},
    {"steer_range",Field::SteerRange,10,100000,false},{"steer_return_ms",Field::SteerReturnMs,0,5000,false},{"steer_curve",Field::SteerCurve,0.25,4,false}};
inline void assign(KbmProfile& p,Field field,double v){
    switch(field){
    case Field::SensX:p.aim.sensitivityX=v;break;
    case Field::SensY:p.aim.sensitivityY=v;break;
    case Field::Full:p.aim.fullSpeedCounts=v;break;
    case Field::Deadzone:p.aim.deadzone=v;break;
    case Field::Curve:p.aim.curve=v;break;
    case Field::Accel:p.aim.accel=v;break;
    case Field::AccelCap:p.aim.accelCap=v;break;
    case Field::SmoothMs:p.aim.smoothingMs=v;break;
    case Field::CarryMs:p.aim.carryMs=v;break;
    case Field::Fine:p.aim.fineScale=v;break;
    case Field::GyroDeg:p.aim.gyroDegPerCount=v;break;
    case Field::InvertX:p.aim.invertX=v!=0;break;
    case Field::InvertY:p.aim.invertY=v!=0;break;
    case Field::Curve25:p.aim.curvePoints[0]=v;break;
    case Field::Curve50:p.aim.curvePoints[1]=v;break;
    case Field::Curve75:p.aim.curvePoints[2]=v;break;
    case Field::MoveDeadzone:p.moveDeadzone=v;break;
    case Field::MoveRiseMs:p.moveRiseMs=v;break;
    case Field::MoveFallMs:p.moveFallMs=v;break;
    case Field::WalkTilt:p.walkTilt=v;break;
    case Field::TriggerRiseMs:p.triggerRiseMs=v;break;
    case Field::TriggerFallMs:p.triggerFallMs=v;break;
    case Field::TriggerStart:p.triggerStart=v;break;
    case Field::TriggerStep:p.triggerStep=int(v);break;
    case Field::SteerRange:p.steerRangeCounts=v;break;
    case Field::SteerReturnMs:p.steerReturnMs=v;break;
    case Field::SteerCurve:p.steerCurve=v;break;
    }
}
inline std::string_view trim(std::string_view s)noexcept{
    while(!s.empty()&&(s.front()==' '||s.front()=='\t'))s.remove_prefix(1);
    while(!s.empty()&&(s.back()==' '||s.back()=='\t'))s.remove_suffix(1);
    return s;
}
inline std::string lowered(std::string_view s){std::string out(s);for(char& c:out)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return out;}
// A plain decimal number within [low, high]: no sign prefix other than '-', no hex, nothing after it.
inline bool number(std::string_view text,double low,double high,bool whole,double& value)noexcept{
    if(text.empty())return false;
    double v=0;
    const auto result=std::from_chars(text.data(),text.data()+text.size(),v);
    if(result.ec!=std::errc{}||result.ptr!=text.data()+text.size())return false;
    if(!(v>=low&&v<=high))return false;                 // also refuses values that are not numbers
    if(whole&&v!=double(int(v)))return false;
    value=v;return true;
}
// aim_while: a button or a trigger, something that can be "held" on a pad.
inline bool holdable(PadAction action)noexcept{
    switch(action){
    case PadAction::Cross:case PadAction::Circle:case PadAction::Square:case PadAction::Triangle:
    case PadAction::L1:case PadAction::R1:case PadAction::L2:case PadAction::R2:case PadAction::L3:case PadAction::R3:return true;
    default:return false;
    }
}
} // namespace kbmprofile

// Reads profile text on top of `base`. Never throws on bad text and never returns a value outside
// its range; what could not be read is listed in `problems`.
inline KbmProfileParse parseKbmProfiles(std::string_view text,KbmProfileSet base=kbmBuiltinProfiles()){
    using namespace kbmprofile;
    KbmProfileParse out;out.set=std::move(base);
    if(text.size()>kKbmProfileMaxBytes){
        // Stop at the last whole line inside the limit: half a line is not a setting.
        out.truncated=true;
        const auto end=text.rfind('\n',kKbmProfileMaxBytes-1);
        text=end==std::string_view::npos?std::string_view{}:text.substr(0,end+1);
    }
    if(text.size()>=3&&text.substr(0,3)=="\xEF\xBB\xBF")text.remove_prefix(3);   // UTF-8 byte order mark
    std::vector<std::uint32_t> aimOwn;      // keys the aim profile binds itself: walking's lines do not reach them
    const auto problem=[&out](unsigned line,const char* what){
        ++out.problemCount;
        if(out.problems.size()<kKbmProfileMaxProblems)out.problems.push_back({line,what});
    };
    unsigned lineNumber=0;
    while(!text.empty()){
        if(lineNumber>=kKbmProfileMaxLines){out.truncated=true;break;}
        ++lineNumber;
        const auto end=text.find('\n');
        std::string_view line=text.substr(0,end);
        text=end==std::string_view::npos?std::string_view{}:text.substr(end+1);
        if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
        if(line.size()>kKbmProfileMaxLineLength){problem(lineNumber,"line too long");continue;}
        line=trim(line);
        if(line.empty()||line.front()=='#'||line.front()==';')continue;
        bool plain=true;
        for(const char c:line){const auto byte=static_cast<unsigned char>(c);if((byte<0x20&&c!='\t')||byte>=0x7F)plain=false;}
        if(!plain){problem(lineNumber,"not plain text (control or non-ASCII character)");continue;}
        const auto equals=line.find('=');
        if(equals==std::string_view::npos){problem(lineNumber,"no '='");continue;}
        const std::string key=lowered(trim(line.substr(0,equals)));
        const std::string_view value=trim(line.substr(equals+1));
        const std::string valueLower=lowered(value);
        const auto dot=key.find('.');
        if(dot==std::string::npos){
            // Settings for the whole set.
            if(key=="name"){
                if(value.empty()||value.size()>kKbmProfileMaxName){problem(lineNumber,"name must be 1 to 64 characters");continue;}
                out.set.name=std::string(value);
            }else if(key=="game"){
                if(value.empty()||value.size()>kKbmProfileMaxName){problem(lineNumber,"game must be 1 to 64 characters");continue;}
                out.set.game=std::string(value);
            }else if(key=="counts_per_360"){
                double counts=0;
                if(!number(value,kKbmCountsPer360Min,kKbmCountsPer360Max,false,counts)){problem(lineNumber,"counts_per_360 is a number from 100 to 1000000");continue;}
                out.set.countsPer360=counts;
            }else if(key=="mode"){
                if(valueLower!="stick"&&valueLower!="gyro"){problem(lineNumber,"mode is stick or gyro");continue;}
                for(auto& profile:out.set.profiles)profile.aim.mode=valueLower=="gyro"?MouseAimConfig::Mode::Gyro:MouseAimConfig::Mode::Stick;
            }else if(key=="stick_bits"){
                double bits=0;
                if(!number(value,8,16,true,bits)){problem(lineNumber,"stick_bits is a whole number from 8 to 16");continue;}
                for(auto& profile:out.set.profiles)profile.aim.stickBits=int(bits);
            }else if(key=="aim_while"){
                PadAction action=PadAction::None;
                if(!KeyboardPadBindings::actionName(value,action)||(action!=PadAction::None&&!holdable(action))){
                    problem(lineNumber,"aim_while is none, a face or shoulder button, a stick click or a trigger");continue;}
                out.set.aimWhile=action;
            }else{problem(lineNumber,"unknown setting (profile settings start with walk., aim., drive. or all.)");continue;}
            ++out.applied;continue;
        }
        // <profile>.<setting>
        const std::string_view scope=std::string_view(key).substr(0,dot),setting=std::string_view(key).substr(dot+1);
        bool to[3]={scope=="walk"||scope=="all",scope=="aim"||scope=="all",scope=="drive"||scope=="all"};
        if(!to[0]&&!to[1]&&!to[2]){problem(lineNumber,"unknown profile (walk, aim, drive or all)");continue;}
        if(setting.substr(0,5)=="bind."){
            const std::uint32_t code=KeyboardPadBindings::keyCode(trim(setting.substr(5)));
            PadAction action=PadAction::None;
            if(!code){problem(lineNumber,"unknown key name");continue;}
            if(!KeyboardPadBindings::actionName(value,action)){problem(lineNumber,"unknown action");continue;}
            const bool own=std::find(aimOwn.begin(),aimOwn.end(),code)!=aimOwn.end();
            if(scope=="aim"){if(!own)aimOwn.push_back(code);}
            else if(scope=="all")std::erase(aimOwn,code);
            else if(scope=="walk"&&!own)to[1]=true;      // aiming follows walking
            for(std::size_t i=0;i<3;++i)if(to[i])out.set.profiles[i].bindings.bind(code,action);
            ++out.applied;continue;
        }
        if(setting=="mouse"){
            KbmProfile::Mouse mouse=KbmProfile::Mouse::Camera;
            if(valueLower=="steer")mouse=KbmProfile::Mouse::Steer;
            else if(valueLower=="none")mouse=KbmProfile::Mouse::None;
            else if(valueLower!="camera"){problem(lineNumber,"mouse is camera, steer or none");continue;}
            for(std::size_t i=0;i<3;++i)if(to[i])out.set.profiles[i].mouse=mouse;
            ++out.applied;continue;
        }
        const FieldInfo* info=nullptr;
        for(const auto& candidate:kFields)if(candidate.name==setting)info=&candidate;
        if(!info){problem(lineNumber,"unknown setting");continue;}
        double v=0;
        if(!number(value,info->low,info->high,info->whole,v)){problem(lineNumber,"not a number in this setting's range");continue;}
        for(std::size_t i=0;i<3;++i)if(to[i])assign(out.set.profiles[i],info->field,v);
        ++out.applied;
    }
    // curve_25 / curve_50 / curve_75 count only together and rising (MouseAimConfig::curvePoints).
    // A set that does not is reported (line 0: the file as a whole) and left out.
    for(auto& profile:out.set.profiles){
        auto& points=profile.aim.curvePoints;
        if(points==std::array<double,3>{0,0,0})continue;
        if(!(points[0]>0&&points[0]<points[1]&&points[1]<points[2]&&points[2]<1)){
            points={0,0,0};
            problem(0,"curve_25, curve_50 and curve_75 must all be set and rise between 0 and 1 (left out)");
        }
    }
    return out;
}
} // namespace veyra::remoteplay
