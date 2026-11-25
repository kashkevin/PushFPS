// SPDX-License-Identifier: GPL-3.0-only
// Camera feel of the immediate mouse transform (stream "kbm", docs/drafts/KBM_AUDIT.md D1, D2, D4): timing=report,
// stickSteps=8, the stop rule of the GTA V-style preset, polling rates, diagonals and rest. Included by CoreTests.cpp
// above main(); uses its TEST / CHECK. A Linux model of the mouse, the pad poll and a console that reads the stick in
// 8 bits: unverified on Windows and on a PS5.
#include "veyra/remoteplay/KbmInputProfile.h"
#include "veyra/remoteplay/KeyboardMousePad.h"
#include "veyra/remoteplay/KbmProfile.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace kbm_camera {
// A hand moving at (vx, vy) counts a second, reported by a mouse of `rateHz` (whole counts per report, the rest
// carried as a sensor does; a report with no count is not sent), read `readDelayUs` later (0..50 us), and a pad poll
// every `pollUs` +- `jitterUs`. Everything in microseconds of one steady clock.
struct Rig {
    int rateHz;double pollUs=4000,jitterUs=0;
    KbmTiming timing=KbmTiming::Report;int steps=16;
    KbmContextTuning tuning;
    KbmImmediateAim aim;
    std::mt19937 rng;
    double now=1e6,nextReport=1e6+137,carryX=0,carryY=0;
    double lastEventUs=-1;      // the last report that carried counts
    bool countsThisPoll=false;
    bool fixedRead=false;       // read every report 25 us after it was made (two rigs then see the same times)
    explicit Rig(int rate,unsigned seed=1,double jitter=0):rateHz(rate),jitterUs(jitter),rng(seed){
        tuning.deadzone=0.12;tuning.fullSpeedCounts=20000;
        aim.setPollUs(4000);aim.setReportIntervalUs(std::uint64_t(1000000/rate));
        aim.setHoldUs(kbmHoldUsFor(rate,4));
    }
    double uniform(double a,double b){return std::uniform_real_distribution<double>(a,b)(rng);}
    KbmImmediateAim::Output poll(double vx,double vy){
        const double dt=pollUs+(jitterUs>0?uniform(-jitterUs,jitterUs):0.0);
        const double end=now+dt,interval=1e6/rateHz;
        countsThisPoll=false;
        while(nextReport<=end){
            carryX+=vx/rateHz;carryY+=vy/rateHz;
            const int dx=int(carryX),dy=int(carryY);carryX-=dx;carryY-=dy;
            if(dx!=0||dy!=0){
                aim.add(dx,dy);aim.stamp(std::uint64_t(nextReport+(fixedRead?25.0:uniform(0,50))));
                lastEventUs=nextReport;countsThisPoll=true;
            }
            nextReport+=interval;
        }
        now=end;
        return aim.step(tuning,1.0,false,false,0,std::uint64_t(dt),std::uint64_t(now),nullptr,timing,steps);
    }
};
// The tilt a console reads from a value: 8-bit steps (the value cut to its upper byte) or the 16-bit value.
inline double axis8(std::int16_t v){return double(v>=0?v>>8:-((-v)>>8))/127.0;}
inline double tilt16(const KbmImmediateAim::Output& o){return std::hypot(double(o.x),double(o.y))/32767.0;}
inline double tilt8(const KbmImmediateAim::Output& o){return std::hypot(axis8(o.x),axis8(o.y));}
// Hand speed the console turns at for a tilt (radial dead zone, straight response, as the rig's tuning).
inline double speedFor(double tilt,double dz=0.12,double full=20000){return tilt>dz?(tilt-dz)/(1-dz)*full:0.0;}
struct Spread {double mean=0,cv=0;int zeros=0;};
inline Spread spread(const std::vector<double>& v){
    Spread s;if(v.empty())return s;
    for(const double x:v){s.mean+=x;if(x==0)++s.zeros;}
    s.mean/=double(v.size());
    double var=0;for(const double x:v)var+=(x-s.mean)*(x-s.mean);
    s.cv=s.mean>0?std::sqrt(var/double(v.size()))/s.mean:0;
    return s;
}
// Per-poll speed a steady hand gives over 3000 polls.
inline Spread steady(int rate,KbmTiming timing,double jitter,unsigned seed){
    Rig r(rate,seed,jitter);r.timing=timing;
    std::vector<double> speeds;
    for(int i=0;i<3000;++i){const auto o=r.poll(6000,0);if(i>=100)speeds.push_back(speedFor(tilt16(o)));}
    return spread(speeds);
}
} // namespace kbm_camera

TEST(kbm_camera_report_timing_keeps_a_steady_hand_steady_at_every_polling_rate){
    using namespace kbm_camera;
    // D1 / D4: with a jittered poll the count of reports per poll varies; timing=report divides by the time the
    // reports cover instead. Mice faster than the poll: the stick is steady and right at every rate.
    for(const int rate:{500,1000,2000,4000,8000}){
        const Spread poll=steady(rate,KbmTiming::Poll,700,unsigned(rate));
        const Spread report=steady(rate,KbmTiming::Report,700,unsigned(rate));
        CHECK(std::abs(report.mean-6000)<0.015*6000);
        CHECK(report.cv<0.04);
        CHECK(report.zeros==0);
        CHECK(std::abs(poll.mean-6000)<0.02*6000);       // the poll path is right on average ...
        if(rate<=2000)CHECK(poll.cv>2*report.cv);        // ... but jitters from poll to poll
    }
    // A mouse as slow as the poll or slower has nothing to time a poll by: the poll interval, as timing=poll without
    // its hold (the stop rule below wins). Right on average.
    for(const int rate:{125,250}){
        const Spread report=steady(rate,KbmTiming::Report,700,7u);
        CHECK(std::abs(report.mean-6000)<0.03*6000);
    }
}

TEST(kbm_camera_report_timing_never_shows_a_value_one_poll_after_the_last_report){
    using namespace kbm_camera;
    // The preset's stop rule (orchestrator, 21 Nov): once a whole send period has passed since the last mouse report
    // with counts, every poll without new counts sends a stick at rest, whatever the mouse's rate, the poll's
    // jitter, the motion or the step mode. And a poll that carries counts always moves the stick.
    for(const int rate:{125,250,500,1000,2000,4000,8000})for(const int steps:{16,8})for(unsigned seed=1;seed<=3;++seed){
        Rig r(rate,seed*977u+unsigned(rate),1200);r.steps=steps;
        for(int i=0;i<1500;++i){
            // Bursts of motion in random directions and speeds, and stops of random length.
            const bool moving=((i/37)%3)!=2&&((i*7919+int(seed))%11)!=0;
            const double speed=moving?r.uniform(50,40000):0,angle=r.uniform(0,6.283);
            const auto o=r.poll(speed*std::cos(angle),speed*std::sin(angle));
            if(!r.countsThisPoll&&r.lastEventUs>0&&r.now-r.lastEventUs>=r.pollUs)CHECK(o.x==0&&o.y==0&&!o.moving);
            if(r.countsThisPoll)CHECK(o.moving);
        }
    }
}

TEST(kbm_camera_report_timing_without_a_known_rate_is_the_poll_interval){
    using namespace kbm_camera;
    // Before the capture has measured the mouse (rate unknown) timing=report times a poll by its interval and holds
    // nothing: exactly timing=poll with no hold.
    Rig a(1000,5,700),b(1000,5,700);
    a.aim.setReportIntervalUs(0);b.aim.setReportIntervalUs(0);a.aim.setHoldUs(0);b.aim.setHoldUs(0);
    b.timing=KbmTiming::Poll;
    for(int i=0;i<600;++i){
        const double v=(i/50)%2?7000.0:0.0;
        const auto oa=a.poll(v,v/3),ob=b.poll(v,v/3);
        CHECK(oa.x==ob.x&&oa.y==ob.y&&oa.moving==ob.moving);
    }
}

TEST(kbm_camera_poll_timing_and_sixteen_bits_are_exactly_as_before){
    // The defaults of step() are the behaviour from before timing / stickSteps existed: same values, poll by poll.
    KbmImmediateAim a,b;a.setHoldUs(8000);b.setHoldUs(8000);
    KbmContextTuning t;t.deadzone=0.1;t.fullSpeedCounts=5000;t.saturation=KbmSaturation::Carry;t.carryMs=20;
    std::mt19937 rng(11);std::uint64_t now=1000000;
    for(int i=0;i<4000;++i){
        const int dx=int(rng()%41)-20,dy=int(rng()%41)-20;const bool move=rng()%3!=0;
        if(move){a.add(dx,dy);b.add(dx,dy);b.stamp(now);}   // a stamp is ignored by timing=poll
        const std::uint64_t dt=2000+rng()%5000;now+=dt;
        const auto oa=a.step(t,0.8,false,true,i>2000?6.0:0.0,dt,now);
        const auto ob=b.step(t,0.8,false,true,i>2000?6.0:0.0,dt,now,nullptr,KbmTiming::Poll,16);
        CHECK(oa.x==ob.x&&oa.y==ob.y&&oa.moving==ob.moving&&oa.held==ob.held&&oa.saturated==ob.saturated);
    }
    // The settings text of every built-in profile is unchanged: no timing / stickSteps key unless set.
    for(const auto kind:{KbmInputProfileKind::Legacy,KbmInputProfileKind::UltraLowLatency,KbmInputProfileKind::AdaptivePrecision,KbmInputProfileKind::Custom}){
        const auto text=kbmInputSettingsText(kbmInputPreset(kind));
        CHECK(text.find("timing")==std::string::npos&&text.find("stickSteps")==std::string::npos);
        CHECK(kbmInputPreset(kind).timing==KbmTiming::Poll&&kbmInputPreset(kind).stickSteps==16);
    }
    // The new keys read, write back and refuse what they do not know.
    auto parsed=parseKbmInputSettings("profile=ultra-low-latency;timing=report;stickSteps=8",{});
    CHECK(parsed.refused==0&&parsed.settings.timing==KbmTiming::Report&&parsed.settings.stickSteps==8);
    CHECK(parseKbmInputSettings(kbmInputSettingsText(parsed.settings),{}).settings==parsed.settings);
    CHECK(parseKbmInputSettings("timing=fast;stickSteps=12;stickSteps=7",{}).refused==3);
    KbmInputSettings odd;odd.stickSteps=12;odd.timing=KbmTiming(9);
    CHECK(kbmSanitized(odd).stickSteps==16&&kbmSanitized(odd).timing==KbmTiming::Poll);
    // The fallback of the self-check keeps the player's timing and steps (they cost nothing).
    CHECK(kbmFallbackSettings(parsed.settings).timing==KbmTiming::Report&&kbmFallbackSettings(parsed.settings).stickSteps==8);
}

TEST(kbm_camera_eight_bit_steps_keep_the_smallest_motion_out_of_the_dead_zone){
    using namespace kbm_camera;
    // D2: with a dead zone of 0.12 (15.24 steps) the 16-bit value of the smallest motion, cut to 8 bits, is step 15:
    // inside the dead zone, the camera does not turn. Whole steps send step 16 at least, and only whole steps.
    KbmContextTuning t;t.deadzone=0.12;t.fullSpeedCounts=60000;
    KbmImmediateAim sixteen,eight;std::uint64_t now=1000000;
    int cutInside=0;
    for(int i=0;i<200;++i){
        sixteen.add(1,0);eight.add(1,0);now+=4000;   // 250 counts a second: 0.4 % of full speed
        const auto a=sixteen.step(t,1,false,false,0,4000,now);
        const auto b=eight.step(t,1,false,false,0,4000,now,nullptr,KbmTiming::Poll,8);
        if(a.x>0&&axis8(a.x)<=0.12)++cutInside;
        CHECK(b.x==32767||b.x%256==0);CHECK(b.y==0);
        CHECK(b.x==0||axis8(b.x)>0.12);
    }
    CHECK(cutInside>150);     // the defect the steps remove (if the console reads 8 bits; that is not known)
    // Left is the same as right: the 8-bit cut of a negative 16-bit value rounds away from zero, a whole step does not.
    KbmImmediateAim left;now=1000000;
    for(int i=0;i<50;++i){left.add(-1,0);now+=4000;const auto o=left.step(t,1,false,false,0,4000,now,nullptr,KbmTiming::Poll,8);CHECK(o.x==0||o.x%256==0);}
}

TEST(kbm_camera_eight_bit_steps_turn_the_distance_the_hand_moved){
    using namespace kbm_camera;
    // A console that reads 8 bits turns (step - dead zone) / (1 - dead zone) of its full speed: with whole steps the
    // distance of slow, medium and fast steady aim is the hand's within 3 %; with 16-bit values cut to 8 bits slow
    // aim is lost. A game whose full turn needs 60000 counts a second (a fast hand, little sensitivity); mice of 1000
    // Hz, jittered poll, timing=report.
    for(const double v:{150.0,300.0,1200.0,6000.0,15000.0,40000.0}){
        double want=0,turned8=0,turnedCut=0;
        for(const int steps:{8,16}){
            Rig r(1000,3,700);r.steps=steps;r.tuning.fullSpeedCounts=60000;
            double prev=r.now;double turned=0;KbmImmediateAim::Output last;
            for(int i=0;i<1500;++i){
                const auto o=r.poll(v,0);
                turned+=speedFor(tilt8(last),0.12,60000)*(r.now-prev)/1e6;   // the previous value held until this poll
                last=o;prev=r.now;
            }
            if(steps==8){turned8=turned;want=v*(r.now-1e6-4000)/1e6;}else turnedCut=turned;
        }
        CHECK(std::abs(turned8-want)<0.03*want);
        if(v<=300)CHECK(turnedCut<0.5*want);
    }
}

TEST(kbm_camera_diagonals_turn_as_fast_as_straight_moves_and_keep_their_direction){
    using namespace kbm_camera;
    for(const int steps:{16,8})for(const double speed:{800.0,5000.0,14000.0}){
        for(const double degrees:{0.0,20.0,45.0,70.0,135.0,200.0}){
            const double a=degrees*3.14159265358979/180;
            Rig straight(1000,9,0),diagonal(1000,9,0);straight.steps=diagonal.steps=steps;
            double ts=0,td=0,sx=0,sy=0;
            for(int i=0;i<500;++i){
                const auto s=straight.poll(speed,0),d=diagonal.poll(speed*std::cos(a),speed*std::sin(a));
                if(i>=20){ts+=steps==8?tilt8(s):tilt16(s);td+=steps==8?tilt8(d):tilt16(d);sx+=steps==8?axis8(d.x):d.x/32767.0;sy+=steps==8?axis8(d.y):d.y/32767.0;}
            }
            CHECK(std::abs(ts-td)<0.02*ts);                                    // the same tilt, any direction
            double err=std::atan2(sy,sx)-a;while(err>3.14159265358979)err-=6.28318530717959;while(err<-3.14159265358979)err+=6.28318530717959;
            CHECK(std::abs(err)<0.035);                                        // within 2 degrees on average
        }
    }
}

TEST(kbm_camera_no_drift_and_nothing_left_over_at_rest){
    using namespace kbm_camera;
    for(const int rate:{125,500,1000,8000})for(const int steps:{16,8}){
        Rig r(rate,4,700);r.steps=steps;
        for(int i=0;i<300;++i)(void)r.poll(i%40<20?2500.0:-1700.0,i%7*90.0);
        for(int i=0;i<3;++i)(void)r.poll(0,0);
        for(int i=0;i<2000;++i){const auto o=r.poll(0,0);CHECK(o.x==0&&o.y==0&&!o.moving);}
        // What the next motion inherits is at most the 8-bit remainder (one step's worth above the dead zone): over
        // the same motion it turns what a transform that never moved turns, within that step (16 bits: exactly).
        Rig fresh(rate,4,0);fresh.steps=steps;fresh.now=r.now;fresh.nextReport=r.nextReport;fresh.carryX=fresh.carryY=0;
        r.carryX=r.carryY=0;r.jitterUs=0;r.fixedRead=fresh.fixedRead=true;
        double used=0,clean=0;
        for(int i=0;i<100;++i){
            const auto a=r.poll(900,0),c=fresh.poll(900,0);
            CHECK(a.y==0&&c.y==0);
            used+=std::max(0.0,(steps==8?tilt8(a):tilt16(a))*127-0.12*127);clean+=std::max(0.0,(steps==8?tilt8(c):tilt16(c))*127-0.12*127);
        }
        CHECK(std::abs(used-clean)<=(steps==8?1.0+1e-9:1e-9));
    }
}

TEST(kbm_camera_legacy_turns_on_after_a_flick_the_preset_does_not){
    // D3, the reason the preset uses the immediate transform: after a flick beyond full stick the legacy path (its
    // built-in 40 ms carry and 8 ms window) keeps the stick tilted for polls after the hand stopped.
    KbmProfileSet legacy=kbmBuiltinProfiles();
    KeyboardMousePad old(legacy);
    KbmProfileSet preset=kbmGtaPcProfiles();
    KeyboardMousePad now(preset);
    std::uint64_t t=1000000;ControllerState pad;pad.inputActive=true;
    for(int i=0;i<10;++i){old.mouseMove(160,0);now.mouseMove(160,0);now.mouseStamp(t+3000);t+=4000;(void)old.apply(pad,t);(void)now.apply(pad,t);}
    int oldAfter=0,nowAfter=0;
    for(int i=0;i<20;++i){t+=4000;if(old.apply(pad,t).rightX!=0)++oldAfter;if(now.apply(pad,t).rightX!=0)++nowAfter;}
    CHECK(oldAfter>=5);
    CHECK(nowAfter==0);
}

// ---- The GTA V-style PC preset (kbmGtaPcProfiles) and the menu keys -------------------------------------------------
TEST(kbm_gta_pc_preset_binds_the_owners_keys_and_leaves_reload_unbound){
    const KbmProfileSet set=kbmGtaPcProfiles();
    const auto& walk=set[KbmProfileId::Walk].bindings;const auto& aim=set[KbmProfileId::Aim].bindings;const auto& drive=set[KbmProfileId::Drive].bindings;
    const auto code=[](const char* name){return KeyboardPadBindings::keyCode(name);};
    // The owner's list on foot, as GTA VI's PS5 buttons (Shacknews 18 Nov, GTABase 21 Nov 2026).
    CHECK(walk.action(code("W"))==PadAction::LUp&&walk.action(code("A"))==PadAction::LLeft&&walk.action(code("S"))==PadAction::LDown&&walk.action(code("D"))==PadAction::LRight);
    CHECK(walk.action(code("Mouse1"))==PadAction::R2&&walk.action(code("Mouse3"))==PadAction::L2);   // left click fire, right click aim
    CHECK(walk.action(code("Shift"))==PadAction::L3&&walk.action(code("Space"))==PadAction::Cross&&walk.action(code("F"))==PadAction::Triangle);
    CHECK(walk.action(code("Q"))==PadAction::R1&&walk.action(code("Ctrl"))==PadAction::Circle&&walk.action(code("Tab"))==PadAction::L1);
    CHECK(walk.action(code("E"))==PadAction::Triangle&&walk.action(code("Esc"))==PadAction::Menu);
    // Reload has no published GTA VI button ("check in game", in the set's name). R is GTA V's melee key too: Square,
    // GTA VI's melee in both tables (else nothing would reach Square).
    CHECK(walk.action(code("R"))==PadAction::Square&&aim.action(code("R"))==PadAction::Square&&drive.action(code("R"))==PadAction::None);
    CHECK(set.menu.action(code("R"))==PadAction::None);
    CHECK(set.name.find("reload")!=std::string::npos);
    CHECK(set.leaveDriveWith==PadAction::Triangle);   // F (exit vehicle) also brings the walking keys back
    // The wheel switches weapons where GTA VI has a quick swap (aiming); on foot no table names one.
    CHECK(aim.action(kWheelUp)==PadAction::Triangle&&aim.action(kWheelDown)==PadAction::Triangle);
    CHECK(walk.action(kWheelUp)==PadAction::None);
    CHECK(set.aimWhile==PadAction::L2&&set.sprintWhile==PadAction::L3);
    // In a vehicle.
    CHECK(drive.action(code("W"))==PadAction::R2&&drive.action(code("S"))==PadAction::L2&&drive.action(code("Space"))==PadAction::Cross);
    CHECK(drive.action(code("F"))==PadAction::Triangle&&drive.action(code("Mouse1"))==PadAction::R1&&drive.action(code("E"))==PadAction::L3);
    CHECK(set[KbmProfileId::Drive].triggerRiseMs==0&&set[KbmProfileId::Drive].moveRiseMs==0);   // no ramps: minimal processing
    // Its input: the immediate transform without window or carry, report timing, whole 8-bit steps.
    CHECK(set.input.kind==KbmInputProfileKind::UltraLowLatency&&set.input.smoothingMs==0);
    CHECK(set.input.timing==KbmTiming::Report&&set.input.stickSteps==8);
    for(const auto& t:set.input.contexts)CHECK(t.saturation==KbmSaturation::Drop&&t.carryMs==0);
    // A profile file holds all of it and reads back as the same set.
    const auto parsed=parseKbmProfiles(kbmProfileText(set));
    CHECK(parsed.problemCount==0&&parsed.set==set);
}

TEST(kbm_gta_pc_menu_keys_change_by_the_players_own_key){
    KeyboardMousePad pad(kbmGtaPcProfiles());
    ControllerState physical;physical.inputActive=true;std::uint64_t t=1000000;
    const auto poll=[&]{t+=4000;return pad.apply(physical,t);};
    pad.key('W',true);CHECK(poll().leftY<0);
    pad.key(0x1B,true);ControllerState s=poll();                      // Esc: Options, and the menu keys
    CHECK((s.buttons&ControllerState::Options)!=0&&pad.inMenu());
    CHECK(s.leftY==0&&(s.buttons&ControllerState::Up)==0);           // W, held through the change, lets go at once ...
    pad.key(0x1B,false);(void)poll();
    pad.key('W',false);pad.key('W',true);s=poll();
    CHECK(s.leftY==0&&(s.buttons&ControllerState::Up)!=0);           // ... and is the D-pad when pressed again
    pad.mouseMove(800,0);s=poll();
    CHECK(s.rightX==0);                                              // the camera stands still in a menu
    pad.key(0x08,true);CHECK((poll().buttons&ControllerState::Circle)!=0);pad.key(0x08,false);   // Backspace: back
    // A release of the capture (focus loss, Ctrl+Alt+Shift+Z, device removal) leaves the menu keys as well.
    pad.releaseAll();CHECK(!pad.inMenu());
    pad.mouseMove(800,0);CHECK(poll().rightX>0);                    // the camera turns again
    // The end of a session starts on foot.
    pad.key(0x1B,true);(void)poll();pad.key(0x1B,false);CHECK(pad.inMenu());
    pad.restart();CHECK(!pad.inMenu());
    // A set without menu keys: a Menu key presses Options and changes nothing else.
    KbmProfileSet plain=kbmGtaProfiles();
    plain[KbmProfileId::Walk].bindings.bind(0x1B,PadAction::Menu);
    KeyboardMousePad other(plain);
    other.key(0x1B,true);t+=4000;s=other.apply(physical,t);
    CHECK((s.buttons&ControllerState::Options)!=0&&!other.inMenu());
}

TEST(kbm_profile_menu_lines_read_and_older_sets_write_none){
    auto parsed=parseKbmProfiles("menu.bind.W=dup\nmenu.bind.Enter=cross\nmenu.sens_x=2\nwalk.bind.Esc=menu\n");
    CHECK(parsed.applied==3&&parsed.problemCount==1);
    CHECK(parsed.set.menu.action('W')==PadAction::DUp&&parsed.set.menu.action(0x0D)==PadAction::Cross);
    CHECK(parsed.set[KbmProfileId::Walk].bindings.action(0x1B)==PadAction::Menu);
    // Sets from before the menu keys write no menu line (their files are as they were).
    CHECK(kbmProfileText(kbmBuiltinProfiles()).find("menu.")==std::string::npos);
    CHECK(kbmProfileText(kbmGtaProfiles()).find("menu.")==std::string::npos);
    CHECK(kbmBuiltinProfiles().menu.size()==0&&kbmGtaProfiles().menu.size()==0);
    CHECK(kbmActionName(PadAction::Menu)=="menu");
}

TEST(kbm_stats_stick_steadiness_is_measured_after_the_contract_keys){
    // The kit's measure of D1 / D4 on the owner's PC: poll-to-poll change of the right stick's tilt while it moves,
    // and polls at rest between two moving ones.
    KbmInputMeter m;
    for(const double t:{0.0,0.40,0.42,0.40,0.0,0.41,0.41,0.0,0.0,0.3})m.stick(t);
    CHECK(m.stickDips==1);                          // 0.40 -> 0 -> 0.41; two polls at rest are a stop, not a dip
    CHECK(m.stickChange.count()==3);                // 0.40-0.42, 0.42-0.40, 0.41-0.41; nothing across a rest
    CHECK(m.stickChange.max()<5.0);
    KbmInputLineInfo info;
    const std::string line=m.line(info);
    const auto fallback=line.find("fallback=");
    CHECK(fallback!=std::string::npos&&line.find("stickChangePctP50=",fallback)!=std::string::npos&&line.find("stickDips=1",fallback)!=std::string::npos);
    m.clear();CHECK(m.stickDips==0&&m.stickChange.count()==0);
}

TEST(kbm_gta_pc_preset_over_a_calibrated_set_keeps_every_number){
    KbmProfileSet mine=kbmBuiltinProfiles();
    for(auto& p:mine.profiles){p.aim.deadzone=0.21;p.aim.fullSpeedCounts=7300;p.aim.curvePoints={0.2,0.45,0.7};p.aim.sensitivityY=0.8;}
    mine.countsPer360=9100;mine.game="GTA VI";
    mine[KbmProfileId::Drive].moveDeadzone=0.17;mine[KbmProfileId::Drive].triggerStart=0.2;
    KbmProfileSet applied=mine;kbmUseGtaPcPreset(applied);
    const KbmProfileSet preset=kbmGtaPcProfiles();
    for(std::size_t i=0;i<3;++i){
        CHECK(applied.profiles[i].aim==mine.profiles[i].aim);                       // the calibration stays
        CHECK(applied.profiles[i].bindings==preset.profiles[i].bindings);
    }
    CHECK(applied.countsPer360==9100&&applied.game=="GTA VI");
    CHECK(applied[KbmProfileId::Drive].moveDeadzone==0.17&&applied[KbmProfileId::Drive].triggerStart==0.2);
    CHECK(applied[KbmProfileId::Drive].triggerRiseMs==0&&applied[KbmProfileId::Drive].moveRiseMs==0);
    CHECK(applied.menu==preset.menu&&applied.input==preset.input&&applied.name==preset.name);
}

// ---- The independent review's scenarios (review-kbm/adv/adv_tests.cpp, 21 Nov 2026), made tests ------------------------
TEST(kbm_review_menu_keys_always_have_a_way_out){
    // The menu table rebinds Esc and drops P: the profile's own Menu key still leaves the menu keys (else the camera
    // would stay frozen until the session restarted).
    KbmProfileSet s=kbmGtaPcProfiles();s.menu.bind(0x1B,PadAction::Circle);s.menu.bind('P',PadAction::None);
    KeyboardMousePad pad(s);std::uint64_t now=1000000;ControllerState in;in.inputActive=true;
    const auto tap=[&](std::uint32_t k){pad.key(k,true);now+=4000;(void)pad.apply(in,now);pad.key(k,false);now+=4000;return pad.apply(in,now);};
    (void)tap(0x1B);CHECK(pad.inMenu());
    (void)tap(0x1B);CHECK(!pad.inMenu());
    pad.mouseMove(500,0);now+=4000;CHECK(pad.apply(in,now).rightX>0);
    // P too, and a release of the capture.
    (void)tap('P');CHECK(pad.inMenu());(void)tap('P');CHECK(!pad.inMenu());
    (void)tap(0x1B);CHECK(pad.inMenu());pad.releaseAll();CHECK(!pad.inMenu());
    // Keys held through the change into the menu are let go and not re-pressed until pressed again.
    KeyboardMousePad held(kbmGtaPcProfiles());
    held.key('W',true);held.mouseButton(1,true);now+=4000;(void)held.apply(in,now);
    held.key(0x1B,true);now+=4000;const auto b=held.apply(in,now);
    CHECK(b.leftY==0&&b.r2==0&&(b.buttons&ControllerState::Up)==0&&(b.buttons&ControllerState::Cross)==0);
}

TEST(kbm_review_leaving_the_car_with_f_brings_the_walking_keys_back){
    KeyboardMousePad pad(kbmGtaPcProfiles());std::uint64_t now=1000000;ControllerState in;in.inputActive=true;
    const auto tap=[&](std::uint32_t k){pad.key(k,true);now+=4000;const auto pressed=pad.apply(in,now);pad.key(k,false);now+=4000;(void)pad.apply(in,now);return pressed;};
    (void)tap(0x14);CHECK(pad.driving());                          // Caps: the car
    const auto exit=tap('F');
    CHECK((exit.buttons&ControllerState::Triangle)!=0);             // the exit press reaches the console
    CHECK(!pad.driving());                                          // and W is walking again, not R2 (shooting)
    pad.key('W',true);now+=4000;const auto o=pad.apply(in,now);
    CHECK(o.r2==0&&o.leftY<0);
    // An older set (no leave_drive_with): F stays in the car, as before.
    KbmProfileSet old=kbmGtaPcProfiles();old.leaveDriveWith=PadAction::None;
    KeyboardMousePad other(old);
    other.key(0x14,true);now+=4000;(void)other.apply(in,now);other.key(0x14,false);
    other.key('F',true);now+=4000;(void)other.apply(in,now);other.key('F',false);now+=4000;(void)other.apply(in,now);
    CHECK(other.driving());
    // The file holds it; sets from before write no such line.
    CHECK(kbmProfileText(kbmGtaPcProfiles()).find("leave_drive_with=triangle")!=std::string::npos);
    CHECK(kbmProfileText(kbmGtaProfiles()).find("leave_drive_with")==std::string::npos);
    CHECK(parseKbmProfiles("leave_drive_with=triangle\n").set.leaveDriveWith==PadAction::Triangle);
}

TEST(kbm_review_step_remainder_never_crosses_a_pause_or_a_reversal){
    // A slow right motion leaves a remainder; after a 10 s rest, or at once in the other direction, the first left poll
    // is what a transform that never moved sends.
    KbmContextTuning t;t.deadzone=0.12;t.fullSpeedCounts=60000;
    for(const std::uint64_t restUs:{std::uint64_t(10000000),std::uint64_t(0)}){
        KbmImmediateAim used,fresh;std::uint64_t now=1000000;
        for(int i=0;i<7;++i){used.add(1,0);now+=4000;(void)used.step(t,1,false,false,0,4000,now,nullptr,KbmTiming::Poll,8);}
        now+=restUs;
        used.add(-1,0);fresh.add(-1,0);now+=4000;
        const auto a=used.step(t,1,false,false,0,4000,now,nullptr,KbmTiming::Poll,8);
        const auto b=fresh.step(t,1,false,false,0,4000,now,nullptr,KbmTiming::Poll,8);
        CHECK(a.x==b.x&&a.y==b.y);
    }
}

TEST(kbm_review_report_timing_survives_a_busy_interface_and_a_rate_drop){
    // Reads held back by a busy interface thread (6 ms every 5th poll, read in one go) never give a poll far above the
    // hand's speed; a mouse that drops from 1000 to 125 Hz while the capture still believes 1000 never leaves the
    // stick on after one send period.
    using namespace kbm_camera;
    for(const int rate:{1000,4000,8000}){
        Rig r(rate,7,0);r.steps=16;
        double worst=0;
        for(int i=0;i<1500;++i){
            const bool busy=i%5==0;
            const double dt=busy?10000:4000;
            const double end=r.now+dt,interval=1e6/rate;
            r.countsThisPoll=false;
            while(r.nextReport<=end){
                r.carryX+=6000.0/rate;const int dx=int(r.carryX);r.carryX-=dx;
                if(dx){const double read=busy&&r.nextReport>=r.now+3000?r.now+9900:r.nextReport+20;r.aim.add(dx,0);r.aim.stamp(std::uint64_t(read));}
                r.nextReport+=interval;
            }
            r.now=end;
            const auto o=r.aim.step(r.tuning,1,false,false,0,std::uint64_t(dt),std::uint64_t(r.now),nullptr,KbmTiming::Report,16);
            if(i>50)worst=std::max(worst,speedFor(tilt16(o))/6000);
        }
        CHECK(worst<1.6);
    }
    Rig r(1000,3,0);
    for(int i=0;i<300;++i)(void)r.poll(5000,0);
    r.rateHz=125;                                    // the mouse changed its rate; the capture has not measured it yet
    for(int i=0;i<1500;++i){
        const bool moving=(i/50)%2==0;
        const auto o=r.poll(moving?5000:0,0);
        if(!r.countsThisPoll&&r.lastEventUs>0&&r.now-r.lastEventUs>=r.pollUs)CHECK(o.x==0&&o.y==0);
    }
}

// ---- Defaults are the old code's, poll by poll -------------------------------------------------------------------
// The review's differential (review-kbm/adv/diff_driver.cpp): one random stream of keys, buttons, wheel, releases and
// mouse motion (with read stamps, which the defaults ignore) through the built-in and the GTA VI sets under every
// built-in input profile; the FNV-1a digest of every poll's state and of the profile and settings texts. The value
// below was made with the headers of exp/rel-9 (71e49426, the code before this stream) and equals this code's
// (g++ 13, -O0 and -O2). The digest goes through std::pow / std::hypot: another C library may round one poll
// differently, so the value is checked under GCC and Clang only; elsewhere the stream still runs for its other checks.
namespace kbm_regression {
inline void mix(std::uint64_t& h,const std::string& s){for(const unsigned char c:s){h^=c;h*=1099511628211ull;}}
inline void mixState(std::uint64_t& h,const ControllerState& s,unsigned long long i){
    char line[160];
    std::snprintf(line,sizeof(line),"%llu %u %d %d %d %d %u %u %d\n",i,static_cast<unsigned>(s.buttons),int(s.leftX),int(s.leftY),int(s.rightX),int(s.rightY),unsigned(s.l2),unsigned(s.r2),int(s.inputActive));
    mix(h,line);
}
inline std::uint64_t digest(unsigned long long events){
    std::uint64_t h=1469598103934665603ull;
    const KbmInputProfileKind kinds[]={KbmInputProfileKind::Legacy,KbmInputProfileKind::UltraLowLatency,KbmInputProfileKind::AdaptivePrecision,KbmInputProfileKind::Custom};
    const std::uint32_t keys[]={'W','A','S','D',0x10,0x11,0x20,'F','E','Q','R','C','X',0x09,0x1B,'P','M',0x12,0x14,0x25,0x26,0x27,0x28,0x0D,0x08};
    for(int setNo=0;setNo<2;++setNo)for(const auto kind:kinds){
        KbmProfileSet set=setNo==0?kbmBuiltinProfiles():kbmGtaProfiles();
        set.input=kbmInputPreset(kind);
        mix(h,kbmProfileText(set));mix(h,"--");mix(h,kbmInputSettingsText(set.input));
        KeyboardMousePad pad(set);
        pad.immediateAim().setHoldUs(kbmHoldUsFor(1000,4));
        std::mt19937 rng(1234u+unsigned(setNo*10)+unsigned(kind));
        std::uint64_t now=1000000;
        for(unsigned long long i=0;i<events;++i){
            const unsigned r=static_cast<unsigned>(rng()%100);
            if(r<55){int dx=int(rng()%61)-30,dy=int(rng()%61)-30;if(rng()%50==0){dx*=400;dy*=300;}
                pad.mouseMove(dx,dy);
                pad.mouseStamp(now+rng()%3000);
            }else if(r<75){pad.key(keys[rng()%(sizeof keys/sizeof keys[0])],rng()%2);}
            else if(r<82){pad.mouseButton(int(rng()%5)+1,rng()%2);}
            else if(r<85){pad.scroll(rng()%2?120:-120);}
            else if(r<86){pad.releaseAll();}
            else{
                now+=1000+rng()%7000;
                ControllerState in;in.inputActive=rng()%2;if(rng()%20==0)in.l2=200;
                mixState(h,pad.apply(in,now),i);
            }
        }
    }
    return h;
}
} // namespace kbm_regression
TEST(kbm_defaults_are_the_code_before_this_stream_poll_by_poll){
    const std::uint64_t got=kbm_regression::digest(200000);
#if defined(__GNUC__) || defined(__clang__)
    CHECK(got==0xbd2626cd61104cc4ull);
#else
    (void)got;
#endif
}

TEST(kbm_review_slow_diagonal_alternating_axes_keeps_its_distance){
    // A very slow diagonal reports (1,0) and (0,1) in turn: the directions are at right angles, and the 8-bit remainder
    // must survive that (only a turn of more than 90 degrees drops it), else every alternation loses it. An 8-bit
    // console with a radial dead zone turns the same total as for the same counts on one axis, within 3 %.
    for(const double dz:{0.12,0.2,0.3}){
        KbmContextTuning t;t.deadzone=dz;t.fullSpeedCounts=60000;
        double turned[2]={0,0};
        for(int pass=0;pass<2;++pass){
            KbmImmediateAim a;std::uint64_t now=1000000;KbmImmediateAim::Output last;
            for(int i=0;i<4000;++i){
                if(pass==0){if(i%2)a.add(0,1);else a.add(1,0);}else a.add(1,0);
                now+=4000;
                const double x=kbm_camera::axis8(last.x),y=kbm_camera::axis8(last.y),m=std::hypot(x,y);
                turned[pass]+=kbm_camera::speedFor(m,dz,60000)*0.004;
                last=a.step(t,1,false,false,0,4000,now,nullptr,KbmTiming::Poll,8);
            }
        }
        CHECK(std::abs(turned[0]-turned[1])<0.03*turned[1]);
    }
}
