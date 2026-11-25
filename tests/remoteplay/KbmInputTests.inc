// SPDX-License-Identifier: GPL-3.0-only
// Input performance profiles and the immediate mouse transform (KbmInputProfile.h), sprint 5.
// Included by CoreTests.cpp above main(); uses its TEST / CHECK.
#include "veyra/remoteplay/KbmInputProfile.h"
#include "veyra/remoteplay/KeyboardMousePad.h"
#include "veyra/remoteplay/KbmProfile.h"
#include "veyra/remoteplay/KbmPreferenceFit.h"
#include "veyra/remoteplay/KbmCapture.h"
#include <cmath>
#include <string>

namespace kbminputtest {
inline KbmContextTuning linear(double full=4000,double dz=0.1){KbmContextTuning t;t.fullSpeedCounts=full;t.deadzone=dz;return t;}
inline double tiltOf(const KbmImmediateAim::Output& o){return std::hypot(double(o.x),double(o.y))/32767.0;}
inline KbmProfileSet immediateSet(KbmInputProfileKind kind){
    KbmProfileSet set=kbmBuiltinProfiles();
    set.input=kbmInputPreset(kind);
    return set;
}
} // namespace kbminputtest

TEST(kbm_input_curves_are_monotonic_and_end_at_zero_and_one){
    for(const auto kind:{KbmCurveKind::Linear,KbmCurveKind::Power,KbmCurveKind::PrecisionSpeed}){
        KbmCurve c;c.kind=kind;c.exponent=1.7;c.kneeIn=0.3;c.kneeOut=0.1;
        CHECK(kbmCurve(c,0)==0);CHECK(kbmCurve(c,1)==1);CHECK(kbmCurve(c,-1)==0);CHECK(kbmCurve(c,2)==1);
        double previous=0;
        for(int i=1;i<=1000;++i){const double v=kbmCurve(c,i/1000.0);CHECK(v>=previous-1e-12);previous=v;}
    }
    KbmCurve two{KbmCurveKind::PrecisionSpeed,1,0.25,0.15};
    CHECK(std::abs(kbmCurve(two,0.25)-0.15)<1e-12);          // the knee
    CHECK(std::abs(kbmCurve(two,0.125)-0.075)<1e-12);        // the precision segment
    CHECK(std::abs(kbmCurve(two,0.625)-0.575)<1e-12);        // halfway along the speed segment
}

TEST(kbm_input_immediate_output_depends_on_this_poll_only){
    using namespace kbminputtest;
    KbmImmediateAim a;const auto t=linear(4000,0.1);
    // 8 counts in 4 ms = 2000 counts/s = half of full speed: tilt 0.1 + 0.9 * 0.5 = 0.55.
    a.add(8,0);auto o=a.step(t,1,false,false,0,4000,4000);
    CHECK(o.moving&&o.received);CHECK(std::abs(tiltOf(o)-0.55)<1e-3);CHECK(o.y==0&&o.x>0);
    // The next poll with nothing new: the stick is at rest at once (no window, no tail).
    o=a.step(t,1,false,false,0,4000,8000);
    CHECK(!o.moving&&o.x==0&&o.y==0);
    // A large movement, then a small one: the second poll shows only the small one.
    a.add(16,0);(void)a.step(t,1,false,false,0,4000,12000);
    a.add(1,0);o=a.step(t,1,false,false,0,4000,16000);
    CHECK(std::abs(tiltOf(o)-(0.1+0.9*250.0/4000))<1e-3);
}

TEST(kbm_input_smallest_motion_leaves_the_dead_zone_at_once){
    using namespace kbminputtest;
    KbmImmediateAim a;const auto t=linear(4000,0.2);
    a.add(1,0);const auto o=a.step(t,1,false,false,0,4000,4000);
    CHECK(std::abs(tiltOf(o)-(0.2+0.8*250.0/4000))<1e-3);   // 1 count in 4 ms: just above the dead zone
    // Even with no dead zone and the tiniest motion, a moving stick is never sent as 0.
    KbmImmediateAim b;auto z=linear(1e6,0);
    b.add(1,0);const auto q=b.step(z,0.01,false,false,0,4000,4000);
    CHECK(q.moving&&(q.x!=0||q.y!=0));
}

TEST(kbm_input_sensitivity_is_independent_per_axis_and_inverts){
    using namespace kbminputtest;
    KbmImmediateAim a;auto t=linear(4000,0);t.sensX=1;t.sensY=0.5;
    a.add(4,4);const auto o=a.step(t,1,false,true,0,4000,4000);
    CHECK(o.x>0&&o.y<0);
    CHECK(std::abs(double(o.x)/double(-o.y)-2.0)<0.01);   // direction keeps the 2:1 ratio
}

TEST(kbm_input_saturation_drops_or_carries_a_bounded_amount){
    using namespace kbminputtest;
    // 40 counts in 4 ms = 10000/s, 2.5x full speed.
    KbmImmediateAim drop;auto t=linear(4000,0.1);
    drop.add(40,0);auto o=drop.step(t,1,false,false,0,4000,4000);
    CHECK(o.saturated&&o.x==32767);
    o=drop.step(t,1,false,false,0,4000,8000);
    CHECK(!o.moving);                                       // the surplus is gone
    KbmImmediateAim carry;t.saturation=KbmSaturation::Carry;t.carryMs=4;
    carry.add(40,0);o=carry.step(t,1,false,false,0,4000,4000);
    CHECK(o.saturated);
    o=carry.step(t,1,false,false,0,4000,8000);              // 16 counts of 24 surplus, at most 4 ms of full speed
    CHECK(o.moving&&o.x==32767);
    o=carry.step(t,1,false,false,0,4000,12000);
    CHECK(!o.moving);                                       // and nothing after the bound
    // Ultra low latency never carries, whatever the text says.
    auto s=kbmInputPreset(KbmInputProfileKind::UltraLowLatency);
    s.contexts[0].saturation=KbmSaturation::Carry;s.contexts[0].carryMs=40;s.smoothingMs=20;
    s=kbmSanitized(s);
    CHECK(s.contexts[0].saturation==KbmSaturation::Drop&&s.contexts[0].carryMs==0&&s.smoothingMs==0);
}

TEST(kbm_input_slow_mouse_holds_for_one_report_interval_and_keeps_distance){
    using namespace kbminputtest;
    // A 125 Hz mouse (8 ms reports) against a 4 ms poll: reports land on every second poll.
    KbmImmediateAim a;a.setHoldUs(kbmHoldUsFor(125,4));
    CHECK(a.holdUs()==8000);CHECK(kbmHoldUsFor(1000,4)==0);CHECK(kbmHoldUsFor(500,4)==0);CHECK(kbmHoldUsFor(250,4)==0);
    const auto t=linear(4000,0);
    std::uint64_t now=0;std::vector<double> tilts;
    for(int poll=0;poll<20;++poll){
        now+=4000;
        if(poll%2==0)a.add(16,0);                    // 16 counts per 8 ms = 2000/s
        tilts.push_back(tiltOf(a.step(t,1,false,false,0,4000,now)));
    }
    for(std::size_t i=2;i<tilts.size();++i)CHECK(std::abs(tilts[i]-0.5)<2e-3);  // steady, no zero between reports
    // The mouse stops: one repeat at most (one report interval), then rest.
    now+=4000;auto o=a.step(t,1,false,false,0,4000,now);
    CHECK(o.held||!o.moving);
    now+=4000;o=a.step(t,1,false,false,0,4000,now);
    now+=4000;o=a.step(t,1,false,false,0,4000,now);
    CHECK(!o.moving);
}

TEST(kbm_input_custom_smoothing_is_optional_and_off_elsewhere){
    using namespace kbminputtest;
    KbmImmediateAim a;const auto t=linear(4000,0);
    a.add(8,0);auto o=a.step(t,1,false,false,12,4000,4000);
    const double first=tiltOf(o);
    o=a.step(t,1,false,false,12,4000,8000);              // the window still holds the movement
    CHECK(o.moving);CHECK(tiltOf(o)>0&&tiltOf(o)<=first+1e-9);
    o=a.step(t,1,false,false,12,4000,12000);
    o=a.step(t,1,false,false,12,4000,16000);
    CHECK(!o.moving);                                    // gone after the 12 ms window
    CHECK(kbmInputPreset(KbmInputProfileKind::AdaptivePrecision).smoothingMs==0);
    auto c=kbmInputPreset(KbmInputProfileKind::Custom);c.smoothingMs=12;CHECK(kbmSanitized(c).smoothingMs==12);
}

TEST(kbm_input_text_round_trips_and_refuses_out_of_range){
    auto s=kbmInputPreset(KbmInputProfileKind::Custom);
    s[KbmContext::Aim].sensX=0.75;s[KbmContext::Drive].curve={KbmCurveKind::Power,1.8,0.25,0.15};
    s[KbmContext::Sprint].saturation=KbmSaturation::Carry;s[KbmContext::Sprint].carryMs=12;
    s.adsFactor=0.4;s.smoothingMs=6;s.override_=KbmContextOverride::Drive;s.transmit=KbmTransmit::LeadingEdge;
    const auto text=kbmInputSettingsText(s);
    const auto back=parseKbmInputSettings(text,KbmInputSettings{});
    CHECK(back.refused==0);CHECK(back.settings==s);
    const auto bad=parseKbmInputSettings("all.sensX=500;walk.deadzone=0.3;nonsense;walk.curve=wiggly;ads=0.5",KbmInputSettings{});
    CHECK(bad.refused==3);CHECK(bad.applied==2);
    CHECK(bad.settings[KbmContext::Walk].deadzone==0.3);CHECK(bad.settings[KbmContext::Aim].sensX==1.0);
    CHECK(bad.settings.adsFactor==0.5);
    CHECK(parseKbmInputSettings(std::string(3000,'x'),{}).refused==1);
    CHECK(kbmInputProfileFromId("adaptive-precision")==KbmInputProfileKind::AdaptivePrecision);
    CHECK(!kbmInputProfileFromId("fast"));
    CHECK(kbmContextOverrideFromId("sprint")==KbmContextOverride::Sprint);
}

TEST(kbm_input_legacy_is_the_default_and_unchanged){
    // A pad built as before (no input settings) gives exactly what MouseAim gives.
    KeyboardMousePad pad(kbmBuiltinProfiles());
    MouseAim reference(kbmBuiltinProfiles()[KbmProfileId::Walk].aim);
    CHECK(pad.inputSettings().kind==KbmInputProfileKind::Legacy);
    std::uint64_t now=1000;
    (void)pad.apply({},now);
    for(int i=0;i<200;++i){
        now+=4000;
        const int dx=(i*7)%13-6,dy=(i*5)%9-4;
        pad.mouseMove(dx,dy);reference.add(dx,dy);
        const auto state=pad.apply({},now);
        const auto r=reference.step(4000);
        CHECK(state.rightX==r.x&&state.rightY==r.y);
    }
}

TEST(kbm_input_context_switch_acts_on_the_next_sample_and_keeps_keys){
    using namespace kbminputtest;
    auto set=immediateSet(KbmInputProfileKind::AdaptivePrecision);
    set.sprintWhile=PadAction::Cross;                 // Shift in the GTA layout
    KeyboardMousePad pad(set);
    std::uint64_t now=1000;(void)pad.apply({},now);
    pad.key('W',true);
    now+=4000;auto s=pad.apply({},now);
    CHECK(pad.context()==KbmContext::Walk);CHECK(s.leftY<0);
    pad.key(0x10,true);                               // Shift: sprint while moving
    now+=4000;s=pad.apply({},now);
    CHECK(pad.context()==KbmContext::Sprint);CHECK(s.leftY<0);CHECK(s.buttons&ControllerState::Cross);
    pad.mouseButton(3,true);                          // aim (L2)
    pad.mouseMove(8,0);
    now+=4000;s=pad.apply({},now);
    CHECK(pad.context()==KbmContext::Aim);CHECK(s.l2>0);CHECK(s.leftY<0);CHECK(s.rightX>0);
    // The aim context's sensitivity (ADS factor) applies to this very sample.
    KbmProfileSet plain=set;KeyboardMousePad walkPad(plain);
    std::uint64_t t2=1000;(void)walkPad.apply({},t2);walkPad.mouseMove(8,0);t2+=4000;
    const auto w=walkPad.apply({},t2);
    CHECK(s.rightX<w.rightX);
    // Manual override: drive tuning while still on foot; nothing released.
    pad.setContextOverride(KbmContextOverride::Drive);
    now+=4000;s=pad.apply({},now);
    CHECK(pad.context()==KbmContext::Drive);CHECK(s.leftY<0&&s.l2>0&&(s.buttons&ControllerState::Cross));
    CHECK(pad.contextSwitches()>=3);
}

TEST(kbm_input_profile_change_during_play_loses_no_state_and_adds_no_gap){
    using namespace kbminputtest;
    for(const auto from:{KbmInputProfileKind::Legacy,KbmInputProfileKind::UltraLowLatency,KbmInputProfileKind::AdaptivePrecision,KbmInputProfileKind::Custom})
    for(const auto to:{KbmInputProfileKind::Legacy,KbmInputProfileKind::UltraLowLatency,KbmInputProfileKind::AdaptivePrecision,KbmInputProfileKind::Custom}){
        KeyboardMousePad pad(immediateSet(from));
        std::uint64_t now=1000;(void)pad.apply({},now);
        pad.key('W',true);pad.key('D',true);pad.mouseButton(1,true);   // move, fire
        for(int i=0;i<10;++i){pad.mouseMove(6,1);now+=4000;(void)pad.apply({},now);}
        pad.mouseMove(6,1);
        pad.setInputSettings(kbmInputPreset(to));                         // between two polls, mouse moving
        now+=4000;const auto s=pad.apply({},now);
        CHECK(s.leftX>0&&s.leftY<0);CHECK(s.r2==255);                    // keys still held
        CHECK(s.rightX>0);                                                // this poll's motion arrived: no gap
        CHECK(pad.inputSettings().kind==to);
    }
}

TEST(kbm_names_of_keys_and_actions_read_back){
    for(std::uint32_t code=1;code<0x100;++code){
        const std::string name=kbmKeyName(code);
        if(!name.empty())CHECK(KeyboardPadBindings::keyCode(name)==code);
    }
    for(const std::uint32_t code:{kMouseButtonBase+1,kMouseButtonBase+5,kWheelUp,kWheelDown})CHECK(KeyboardPadBindings::keyCode(kbmKeyName(code))==code);
    CHECK(kbmKeyName('W')=="W");CHECK(kbmKeyName(0x20)=="Space");CHECK(kbmKeyName(kMouseButtonBase+1)=="Mouse1");
    CHECK(kbmKeyName(0x74)=="F5");CHECK(kbmKeyName(0x63)=="Num3");CHECK(kbmKeyName(0x14)=="Caps");CHECK(kbmKeyName(0x07).empty());
    CHECK(KeyboardPadBindings::keyCode("capslock")==0x14);CHECK(KeyboardPadBindings::keyCode("SPACE")==0x20);
    for(int a=0;a<=int(PadAction::Drive);++a){
        PadAction back=PadAction::None;
        CHECK(KeyboardPadBindings::actionName(kbmActionName(PadAction(a)),back));CHECK(back==PadAction(a));
    }
    CHECK(kbmActionName(PadAction::L2)=="l2");CHECK(kbmActionName(PadAction(200))=="none");
    // entries(): every binding once, sorted by key code.
    const auto entries=kbmBuiltinProfiles()[KbmProfileId::Walk].bindings.entries();
    CHECK(entries.size()==kbmBuiltinProfiles()[KbmProfileId::Walk].bindings.size());
    for(std::size_t i=1;i<entries.size();++i)CHECK(entries[i-1].first<entries[i].first);
}

TEST(kbm_profile_text_writes_a_file_that_reads_back_as_the_same_set){
    // The built-in set, the GTA layout, and a set changed the way the page changes one.
    KbmProfileSet edited=kbmGtaProfiles();
    edited.name="My GTA keys";edited.game="GTA 6";edited.countsPer360=9600;edited.aimWhile=PadAction::L1;edited.sprintWhile=PadAction::L3;
    edited[KbmProfileId::Walk].bindings.bind('W',PadAction::None);          // a built-in key unbound
    edited[KbmProfileId::Walk].bindings.bind(0x74,PadAction::LUp);          // F5 walks forward instead
    edited[KbmProfileId::Aim].bindings=edited[KbmProfileId::Walk].bindings;
    edited[KbmProfileId::Aim].bindings.bind(kMouseButtonBase+2,PadAction::Fine);   // differs while aiming only
    edited[KbmProfileId::Aim].bindings.bind('Q',PadAction::None);
    edited[KbmProfileId::Drive].bindings.bind('H',PadAction::None);
    edited[KbmProfileId::Drive].mouse=KbmProfile::Mouse::Steer;
    edited[KbmProfileId::Walk].aim.sensitivityX=1.2345678;edited[KbmProfileId::Aim].aim.deadzone=0.0731;
    edited[KbmProfileId::Aim].aim.curvePoints={0.1,0.4,0.8};edited[KbmProfileId::Drive].triggerStep=7;
    edited[KbmProfileId::Walk].aim.invertY=true;
    for(auto& p:edited.profiles){p.aim.stickBits=12;p.aim.mode=MouseAimConfig::Mode::Stick;}
    edited.input=kbmInputPreset(KbmInputProfileKind::Custom);
    edited.input[KbmContext::Aim].sensX=0.75;edited.input[KbmContext::Drive].curve={KbmCurveKind::Power,1.8,0.25,0.15};
    edited.input.smoothingMs=4;edited.input.override_=KbmContextOverride::Aim;
    for(const auto& set:{kbmBuiltinProfiles(),kbmGtaProfiles(),edited}){
        const std::string text=kbmProfileText(set);
        CHECK(text.size()<kKbmProfileMaxBytes);
        const auto back=parseKbmProfiles(text);
        CHECK(back.ok());CHECK(back.problemCount==0);
        CHECK(back.set.input==set.input);CHECK(back.set.profiles[0]==set.profiles[0]);CHECK(back.set.profiles[1]==set.profiles[1]);
        CHECK(back.set.profiles[2]==set.profiles[2]);
        CHECK(back.set==set);
        CHECK(kbmProfileText(back.set)==text);          // and writing it again changes nothing
        CHECK(text.find('\\')==std::string::npos&&text.find(":/")==std::string::npos);   // no path in it
    }
}

TEST(kbm_old_profile_files_still_load_and_stay_legacy){
    // A profile written before the input profiles existed (the guide's example): every line is
    // read as before, and nothing of the new parts is switched on by it.
    const char* old=
        "# Slipstream keyboard and mouse profile for one game.\n"
        "name=GTA 6 first try\n"
        "walk.sens_x=1.0\nwalk.sens_y=0.8\nwalk.full=4000\nwalk.deadzone=0.12\nwalk.curve=1.0\nwalk.smooth_ms=8\n"
        "aim.sens_x=0.6\naim.sens_y=0.5\naim.deadzone=0.12\naim.fine=0.4\n"
        "drive.trigger_rise_ms=150\ndrive.trigger_fall_ms=80\ndrive.move_rise_ms=120\ndrive.mouse=camera\n"
        "walk.bind.G=DLeft\naim.bind.Mouse2=Fine\ndrive.bind.Space=R1\n";
    const auto parsed=parseKbmProfiles(old);
    CHECK(parsed.ok());CHECK(parsed.applied==18);
    CHECK(parsed.set.input==KbmInputSettings{});CHECK(parsed.set.input.kind==KbmInputProfileKind::Legacy);
    CHECK(!parsed.set.input.immediate());
    CHECK(parsed.set[KbmProfileId::Walk].aim.sensitivityY==0.8);CHECK(parsed.set[KbmProfileId::Walk].aim.smoothingMs==8);
    CHECK(parsed.set[KbmProfileId::Aim].bindings.action(kMouseButtonBase+2)==PadAction::Fine);
    CHECK(parsed.set[KbmProfileId::Walk].bindings.action('G')==PadAction::DLeft);
    // The same file with input lines: only they change the input part; a bad one is reported and ignored.
    const std::string with=std::string(old)+"input.profile=adaptive-precision\ninput.aim.sensX=0.7\ninput.walk.deadzone=0.9\nsprint_while=L3\n";
    const auto extended=parseKbmProfiles(with);
    CHECK(extended.problemCount==1);CHECK(extended.problems.size()==1&&extended.problems[0].line==22);
    CHECK(extended.set.input.kind==KbmInputProfileKind::AdaptivePrecision);CHECK(extended.set.input[KbmContext::Aim].sensX==0.7);
    CHECK(extended.set.input[KbmContext::Walk].deadzone==KbmContextTuning{}.deadzone);
    CHECK(extended.set.sprintWhile==PadAction::L3);
    CHECK(extended.set.profiles==parsed.set.profiles);
    // A named profile never smooths, whatever a file says.
    CHECK(parseKbmProfiles("input.profile=ultra-low-latency\ninput.smoothingMs=20\n").set.input.smoothingMs==0);
    CHECK(parseKbmProfiles("input.profile=custom\ninput.smoothingMs=20\n").set.input.smoothingMs==20);
}

TEST(kbm_gta_layout_is_marked_untuned_and_covers_foot_aim_and_car){
    const auto gta=kbmGtaProfiles();
    CHECK(gta.name.find("untuned")!=std::string::npos&&gta.name.find("unverified")!=std::string::npos&&gta.name.find("press tables")!=std::string::npos);
    CHECK(gta.name.size()<=kKbmProfileMaxName);
    CHECK(gta.input.kind==KbmInputProfileKind::Legacy);                       // choosing a layout chooses no input profile
    const auto& walk=gta[KbmProfileId::Walk].bindings;const auto& aim=gta[KbmProfileId::Aim].bindings;const auto& drive=gta[KbmProfileId::Drive].bindings;
    const std::uint32_t shift=0x10,space=0x20,ctrl=0x11,tab=0x09,left=kMouseButtonBase+1,right=kMouseButtonBase+3;
    // On foot, as the press tables give the controller: run L3, jump Cross, crouch Circle, melee Square,
    // interact Triangle, cover R1, shoot R2, aim L2, holster L1, phone D-pad up.
    CHECK(walk.action('W')==PadAction::LUp&&walk.action('A')==PadAction::LLeft&&walk.action('S')==PadAction::LDown&&walk.action('D')==PadAction::LRight);
    CHECK(walk.action(shift)==PadAction::L3&&walk.action(space)==PadAction::Cross&&walk.action(ctrl)==PadAction::Circle);
    CHECK(walk.action('R')==PadAction::Square&&walk.action('F')==PadAction::Triangle&&walk.action('Q')==PadAction::R1&&walk.action(tab)==PadAction::L1);
    CHECK(walk.action(left)==PadAction::R2&&walk.action(right)==PadAction::L2&&walk.action(0x26)==PadAction::DUp);
    // Aiming: the same keys, plus shoulder switch (D-pad left) and scope zoom (D-pad up / down) on the wheel.
    CHECK(aim.action('W')==PadAction::LUp&&aim.action(left)==PadAction::R2&&aim.action('F')==PadAction::Triangle);
    CHECK(aim.action('E')==PadAction::DLeft&&aim.action(kWheelUp)==PadAction::DUp&&aim.action(kWheelDown)==PadAction::DDown);
    CHECK(walk.action('E')==PadAction::None&&walk.action(kWheelUp)==PadAction::None);
    // Driving: accelerate R2, brake L2, handbrake Cross, exit Triangle, shoot R1, horn L3.
    CHECK(drive.action('W')==PadAction::R2&&drive.action('S')==PadAction::L2&&drive.action('A')==PadAction::LLeft&&drive.action('D')==PadAction::LRight);
    CHECK(drive.action(space)==PadAction::Cross&&drive.action('F')==PadAction::Triangle&&drive.action(left)==PadAction::R1&&drive.action('E')==PadAction::L3);
    CHECK(drive.action(shift)==PadAction::None);
    // In every profile: pause, the on-foot / car switch, fine aim.
    for(const auto* b:{&walk,&aim,&drive})CHECK(b->action('P')==PadAction::Options&&b->action(0x14)==PadAction::Drive&&b->action('X')==PadAction::Fine);
    CHECK(gta.aimWhile==PadAction::L2&&gta.sprintWhile==PadAction::L3);
    // Sprint context: Shift (run) held while moving; and the pad plays the layout.
    auto set=gta;set.input=kbmInputPreset(KbmInputProfileKind::AdaptivePrecision);
    KeyboardMousePad pad(set);std::uint64_t now=1000;(void)pad.apply({},now);
    pad.key('W',true);pad.key(shift,true);now+=4000;
    auto s=pad.apply({},now);
    CHECK(pad.context()==KbmContext::Sprint&&(s.buttons&ControllerState::L3)&&s.leftY<0);
    pad.key(0x14,true);now+=4000;(void)pad.apply({},now);pad.key(0x14,false);                 // Caps: into the car
    pad.key('W',false);pad.key('W',true);now+=400000;s=pad.apply({},now);
    CHECK(pad.context()==KbmContext::Drive&&s.r2>0&&s.leftY==0);      // W is the throttle now (a ramp), not the stick
    // The built-in set (what a start without any choice uses) is unchanged: the earlier from-memory layout, same numbers.
    const auto builtin=kbmBuiltinProfiles();
    CHECK(builtin[KbmProfileId::Walk].bindings.action(shift)==PadAction::Cross&&builtin[KbmProfileId::Walk].bindings.action('P')==PadAction::None);
    CHECK(builtin[KbmProfileId::Walk].aim==gta[KbmProfileId::Walk].aim&&builtin[KbmProfileId::Drive].triggerRiseMs==gta[KbmProfileId::Drive].triggerRiseMs);
}

TEST(kbm_input_game_response_is_the_legacy_inverse_from_this_poll_alone){
    using namespace kbminputtest;
    // The inverse the immediate path uses is the one MouseAim uses, for a power and for measured points.
    MouseAimConfig power;power.curve=1.8;power.deadzone=0.2;power.fullSpeedCounts=5000;
    MouseAimConfig table=power;table.curvePoints={0.08,0.3,0.65};
    for(const auto& config:{power,table}){
        const KbmGameResponse g{config.deadzone,config.fullSpeedCounts,config.curve,config.curvePoints,1.0};
        for(int i=0;i<=100;++i)CHECK(std::abs(kbmGameInverse(g,i/100.0)-MouseAim::inverseResponse(config,i/100.0))<1e-12);
    }
    // A calibrated game (dead zone 0.2, turning speed ~ tilt^1.8, full turn = 5000 counts/s): half
    // the full speed must land where the game turns at half its fastest: 0.2 + 0.8 * 0.5^(1/1.8).
    const KbmGameResponse g{0.2,5000,1.8,{0,0,0},0.5};
    KbmContextTuning t;t.game=true;t.deadzone=0.4;t.fullSpeedCounts=100;        // its own numbers are not used
    KbmImmediateAim a;
    a.add(10,0);auto o=a.step(t,1,false,false,0,4000,4000,&g);                   // 10 counts in 4 ms = 2500/s
    CHECK(std::abs(tiltOf(o)-(0.2+0.8*std::pow(0.5,1/1.8)))<1e-3);
    // The game then turns at (tilt above the dead zone)^1.8 = half its fastest: the turn follows the hand.
    const double turns=std::pow((tiltOf(o)-0.2)/0.8,1.8);
    CHECK(std::abs(turns-0.5)<2e-3);
    // The next poll with nothing new is at rest: no window, no tail (unlike the legacy path).
    o=a.step(t,1,false,false,0,4000,8000,&g);
    CHECK(!o.moving&&o.x==0&&o.y==0);
    // The vertical ratio of the calibration scales vertical counts only.
    a.add(0,10);o=a.step(t,1,false,false,0,4000,12000,&g);
    CHECK(o.x==0&&std::abs(tiltOf(o)-(0.2+0.8*std::pow(0.25,1/1.8)))<1e-3);
    // Through the pad: a profile file's calibration reaches every new input profile, and legacy is untouched.
    auto parsed=parseKbmProfiles("all.deadzone=0.2\nall.full=5000\nall.curve=1.8\nstick_bits=16\n");
    CHECK(parsed.ok());
    for(const auto kind:{KbmInputProfileKind::UltraLowLatency,KbmInputProfileKind::AdaptivePrecision,KbmInputProfileKind::Custom}){
        auto set=parsed.set;set.input=kbmInputPreset(kind);
        for(auto& c:set.input.contexts)c.curve={};                                // the player's own curve straight: the game's inverse alone
        CHECK(set.input[KbmContext::Walk].game);
        KeyboardMousePad pad(set);std::uint64_t now=1000;(void)pad.apply({},now);
        pad.mouseMove(10,0);now+=4000;const auto state=pad.apply({},now);
        CHECK(std::abs(double(state.rightX)/32767.0-(0.2+0.8*std::pow(0.5,1/1.8)))<1e-3);
        now+=4000;CHECK(pad.apply({},now).rightX==0);
    }
    // `game` off: the context's own numbers, a straight response.
    auto own=parsed.set;own.input=kbmInputPreset(KbmInputProfileKind::Custom);
    for(auto& c:own.input.contexts){c.game=false;c.deadzone=0.1;c.fullSpeedCounts=4000;}
    KeyboardMousePad pad(own);std::uint64_t now=1000;(void)pad.apply({},now);
    pad.mouseMove(8,0);now+=4000;
    CHECK(std::abs(double(pad.apply({},now).rightX)/32767.0-0.55)<1e-3);
}

TEST(kbm_input_max_tilt_caps_the_stick_and_saturation_counts_from_the_cap){
    using namespace kbminputtest;
    auto t=linear(4000,0.1);t.maxTilt=0.55;                     // the cap is reached at half of full speed
    KbmImmediateAim a;
    a.add(4,0);auto o=a.step(t,1,false,false,0,4000,4000);      // a quarter of full speed: under the cap, unchanged
    CHECK(!o.saturated);CHECK(std::abs(tiltOf(o)-(0.1+0.9*0.25))<1e-3);
    a.add(12,0);o=a.step(t,1,false,false,0,4000,8000);          // three quarters: capped
    CHECK(o.saturated);CHECK(std::abs(tiltOf(o)-0.55)<1e-3);
    o=a.step(t,1,false,false,0,4000,12000);CHECK(!o.moving);    // Drop: the surplus is gone
    // Carry keeps what the capped stick could not turn: 12 counts asked, 8 turned at the cap (half of
    // full speed for 4 ms), 4 kept and played out in the next poll.
    t.saturation=KbmSaturation::Carry;t.carryMs=8;
    KbmImmediateAim c;
    c.add(12,0);o=c.step(t,1,false,false,0,4000,4000);
    CHECK(o.saturated&&std::abs(tiltOf(o)-0.55)<1e-3);
    o=c.step(t,1,false,false,0,4000,8000);
    CHECK(o.moving&&!o.saturated);CHECK(std::abs(tiltOf(o)-(0.1+0.9*0.25))<2e-3);   // 4 counts in 4 ms
    o=c.step(t,1,false,false,0,4000,12000);CHECK(!o.moving);
    // And never more than carryMs of turning at the cap: 8 ms at 2000 counts/s = 16 counts.
    c.add(400,0);(void)c.step(t,1,false,false,0,4000,16000);
    double played=0;std::uint64_t now=16000;
    for(int i=0;i<10;++i){now+=4000;o=c.step(t,1,false,false,0,4000,now);if(o.moving)played+=1;}
    CHECK(played==2);
    // maxTilt 1 changes nothing; a cap inside the dead zone is lifted just above it.
    CHECK(kbmSanitized(KbmInputSettings{}).contexts[0].maxTilt==1.0);
    auto low=linear(4000,0.3);low.maxTilt=0.1;
    KbmImmediateAim d;d.add(40,0);o=d.step(low,1,false,false,0,4000,4000);
    CHECK(std::abs(tiltOf(o)-0.35)<1e-3);
    const auto parsed=parseKbmInputSettings("walk.maxTilt=0.9;aim.game=1;drive.maxTilt=3;sprint.game=2",KbmInputSettings{});
    CHECK(parsed.applied==2&&parsed.refused==2);CHECK(parsed.settings[KbmContext::Walk].maxTilt==0.9);CHECK(parsed.settings[KbmContext::Aim].game);
}

TEST(kbm_fit_is_deterministic_explained_and_needs_enough_motions){
    KbmFitInput in;
    in.turnCounts={2000,2100,1900,2050,1950};                       // median 2000: 8000 counts per turn
    in.flicks={{1200,0.10},{1500,0.10},{1000,0.10},{1400,0.10}};    // 10000..15000 counts/s, P90 = 15000
    in.slow={{100,0.5},{150,0.5},{120,0.5}};                        // 200..300 counts/s, P10 = 200
    in.gameFullTurnSeconds=1.6;
    KbmContextTuning base;base.sensX=1;base.sensY=0.8;base.deadzone=0.18;base.saturation=KbmSaturation::Carry;base.carryMs=12;base.game=true;
    const auto r=kbmFitPreference(in,base);
    CHECK(r.ok&&r.reason.empty());
    CHECK(r.countsPer360==8000);CHECK(r.flickP90CountsPerSec==15000);CHECK(r.slowP10CountsPerSec==200);
    CHECK(r.tuning.fullSpeedCounts==5000);                          // 8000 counts / 1.6 s
    CHECK(r.tuning.sensX==1&&r.tuning.sensY==0.8);                  // the vertical ratio is kept
    CHECK(r.tuning.deadzone==0.18&&r.tuning.saturation==KbmSaturation::Carry&&r.tuning.carryMs==12);
    CHECK(!r.tuning.game);                                          // its own numbers from now on
    CHECK(r.tuning.curve.kind==KbmCurveKind::PrecisionSpeed);
    CHECK(std::abs(r.tuning.curve.kneeIn-0.08)<1e-9);CHECK(std::abs(r.tuning.curve.kneeOut-0.048)<1e-9);
    CHECK(r.explanation.find("8000 counts")!=std::string::npos);CHECK(r.explanation.find("3.0 times faster")!=std::string::npos);
    // The same input, the same result; and the numbers survive the text form.
    const auto again=kbmFitPreference(in,base);
    CHECK(again.tuning==r.tuning&&again.explanation==r.explanation);
    KbmInputSettings s=kbmInputPreset(KbmInputProfileKind::Custom);s[KbmContext::Aim]=r.tuning;
    CHECK(parseKbmInputSettings(kbmInputSettingsText(s),KbmInputSettings{}).settings==s);
    // Without the game's full-turn time: full stick = the fast flick speed, and it says so.
    in.gameFullTurnSeconds=0;
    const auto untimed=kbmFitPreference(in,base);
    CHECK(untimed.ok&&untimed.tuning.fullSpeedCounts==15000);CHECK(untimed.explanation.find("not known")!=std::string::npos);
    // Slow and fast too close: a straight line. No slow motions: a straight line.
    in.slow={{4000,0.5},{4200,0.5},{4100,0.5}};
    CHECK(kbmFitPreference(in,base).tuning.curve.kind==KbmCurveKind::Linear);
    in.slow.clear();
    CHECK(kbmFitPreference(in,base).tuning.curve.kind==KbmCurveKind::Linear);
    // Too few or broken motions: refused with a reason, the base numbers returned unchanged.
    KbmFitInput few=in;few.turnCounts={2000,2000};
    CHECK(!kbmFitPreference(few,base).ok&&kbmFitPreference(few,base).reason=="need-turns"&&kbmFitPreference(few,base).tuning==base);
    few=in;few.flicks.resize(2);CHECK(kbmFitPreference(few,base).reason=="need-flicks");
    few=in;few.flicks[0].seconds=0;CHECK(kbmFitPreference(few,base).reason=="bad-motion");
    few=in;few.turnCounts[1]=std::nan("");CHECK(kbmFitPreference(few,base).reason=="bad-motion");
    // Results stay inside the profile's ranges whatever the hand did.
    KbmFitInput wild;wild.turnCounts={1e9,1e9,1e9};wild.flicks={{1e9,0.001},{1e9,0.001},{1e9,0.001}};wild.gameFullTurnSeconds=0.01;
    const auto clamped=kbmFitPreference(wild,base);
    CHECK(clamped.ok&&clamped.tuning.fullSpeedCounts==1e6);
}

TEST(kbm_fit_recorder_measures_one_motion_and_nothing_else){
    KbmFitRecorder r;
    r.add(50,0,1000);CHECK(!r.active());                           // not recording: ignored
    r.start(KbmFitKind::Flick);CHECK(r.active()&&r.kind()==KbmFitKind::Flick);
    r.add(0,0,5000);                                               // a report without movement does not start the clock
    r.add(300,0,10000);r.add(300,400,60000);r.add(0,0,900000);
    const auto m=r.stop();
    CHECK(std::abs(m.counts-std::hypot(600.0,400.0))<1e-9);CHECK(std::abs(m.seconds-0.05)<1e-9);
    CHECK(!r.active());
    r.start(KbmFitKind::Turn);const auto none=r.stop();CHECK(none.counts==0&&none.seconds==0);
    // The longest text the settings can become still fits the preference.
    KbmInputSettings s=kbmInputPreset(KbmInputProfileKind::Custom);
    for(auto& c:s.contexts){c.sensX=99.9999;c.sensY=99.9999;c.fullSpeedCounts=999999.9999;c.curve={KbmCurveKind::PrecisionSpeed,3.9999,0.9499,0.9899};
        c.deadzone=0.4999;c.carryMs=49.9999;c.saturation=KbmSaturation::Carry;c.maxTilt=0.9999;c.game=true;}
    s.smoothingMs=49.9999;s.adsFactor=3.9999;s.override_=KbmContextOverride::Sprint;s.transmit=KbmTransmit::LeadingEdge;
    CHECK(kbmInputSettingsText(s).size()<kKbmInputTextMaxBytes);
    CHECK(parseKbmInputSettings(kbmInputSettingsText(s),KbmInputSettings{}).settings==s);
}

TEST(kbm_fallback_switch_keeps_held_input_and_the_players_numbers){
    using namespace kbminputtest;
    auto set=immediateSet(KbmInputProfileKind::Custom);
    set.input.smoothingMs=12;set.input.transmit=KbmTransmit::LeadingEdge;set.input.adsFactor=0.45;
    for(auto& c:set.input.contexts){c.sensX=1.3;c.sensY=0.9;c.maxTilt=0.9;c.saturation=KbmSaturation::Carry;c.carryMs=20;c.curve={KbmCurveKind::Power,2.0,0.25,0.15};}
    const auto simple=kbmFallbackSettings(set.input);
    CHECK(simple.kind==KbmInputProfileKind::UltraLowLatency&&simple.smoothingMs==0&&simple.transmit==KbmTransmit::Proven);
    CHECK(simple.adsFactor==0.45);
    for(const auto& c:simple.contexts){
        CHECK(c.sensX==1.3&&c.sensY==0.9&&c.maxTilt==0.9&&c.game);
        CHECK(c.curve==KbmCurve{}&&c.saturation==KbmSaturation::Drop&&c.carryMs==0);
    }
    // The switch itself, in the middle of play: keys, trigger and movement survive, the very next poll carries this poll's motion.
    KeyboardMousePad pad(set);std::uint64_t now=1000;(void)pad.apply({},now);
    pad.key('W',true);pad.key(0x10,true);pad.mouseButton(1,true);pad.mouseButton(3,true);
    for(int i=0;i<8;++i){pad.mouseMove(5,-2);now+=4000;(void)pad.apply({},now);}
    const std::size_t held=pad.heldCount();
    pad.mouseMove(5,-2);
    pad.setInputSettings(kbmFallbackSettings(pad.inputSettings()));
    CHECK(pad.heldCount()==held);
    now+=4000;const auto s=pad.apply({},now);
    CHECK(s.leftY<0&&s.r2==255&&s.l2==255&&(s.buttons&ControllerState::Cross));
    CHECK(s.rightX>0&&s.rightY<0);
    CHECK(pad.inputSettings().kind==KbmInputProfileKind::UltraLowLatency);
    // The shadow step used for the timing comparison never changes what the pad sends.
    KeyboardMousePad a(set),b(set);std::uint64_t ta=1000,tb=1000;(void)a.apply({},ta);(void)b.apply({},tb);
    for(int i=0;i<50;++i){
        a.mouseMove(i%7,3-i%5);b.mouseMove(i%7,3-i%5);
        (void)a.shadowStep(false,4000,ta+4000);(void)a.shadowStep(true,4000,ta+4000);
        ta+=4000;tb+=4000;
        CHECK(a.apply({},ta)==b.apply({},tb));
    }
}

TEST(kbm_capture_can_be_released_and_given_back_by_the_page){
    using S=KbmCaptureState::State;using E=KbmCaptureState::Event;
    KbmCaptureState c;
    CHECK(c.on(E::Allow)==KbmCaptureState::Actions{});CHECK(c.state()==S::Released);       // nothing to give back
    CHECK(c.on(E::PollFront).engage);
    CHECK(c.on(E::Allow)==KbmCaptureState::Actions{});CHECK(c.state()==S::Captured);       // no effect while captured
    const auto released=c.on(E::ReleaseHotkey);                                            // the page's release = the hotkey
    CHECK(released.disengage&&released.releaseInputs);CHECK(c.state()==S::Suspended);
    CHECK(c.on(E::PollFront)==KbmCaptureState::Actions{});CHECK(c.state()==S::Suspended);  // stays released while the page is used
    CHECK(c.on(E::Allow)==KbmCaptureState::Actions{});CHECK(c.state()==S::Released);
    const auto again=c.on(E::PollFront);
    CHECK(again.engage&&again.releaseInputs);CHECK(c.state()==S::Captured);                // the next poll in front captures again, from nothing held
}

TEST(kbm_input_shadow_step_on_saved_counts_matches_the_one_before_the_poll){
    // Review 8B finding 3: the self-check's timing runs after the hand-off, on counts copied before
    // the poll; it must time the same work as before (same output) and change nothing in the pad.
    KbmProfileSet set=kbmBuiltinProfiles();set.input=kbmInputPreset(KbmInputProfileKind::AdaptivePrecision);
    KeyboardMousePad pad(set),twin(set);std::uint64_t now=1000;(void)pad.apply({},now);(void)twin.apply({},now);
    for(int i=0;i<50;++i){
        pad.mouseMove(i%7,3-i%5);twin.mouseMove(i%7,3-i%5);
        const KbmImmediateAim saved=pad.immediateAim();
        const auto before=pad.shadowStep(i%2==0,4000,now+4000);
        now+=4000;
        CHECK(pad.apply({},now)==twin.apply({},now));
        const auto after=pad.shadowStep(saved,i%2==0,4000,now);
        CHECK(before.x==after.x&&before.y==after.y&&before.moving==after.moving&&before.saturated==after.saturated);
    }
}
