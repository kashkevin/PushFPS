// SPDX-License-Identifier: GPL-3.0-only
// Keyboard and mouse calibration (VEYRA_EXP_RP_KBM): keys, the step machine, the numbers derived
// from it, the mouse report rate, the profile file, and profile switches in mid-movement.
// Included by CoreTests.cpp above main(); uses its TEST / CHECK. The simulated game and player are
// a Linux model (KbmCalibrationModel.h), not a console.
#include "veyra/remoteplay/KbmCalibration.h"
#include "veyra/remoteplay/KbmCapture.h"
#include "veyra/remoteplay/KbmCalibrationFile.h"
#include "veyra/remoteplay/KbmCalibrationModel.h"
#include "veyra/remoteplay/KbmCalibrationRate.h"
#include "veyra/remoteplay/KbmProfile.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
using CalKey=KbmCalibrationKeys::Action;
KbmProfileSet calSet(){
    auto set=kbmBuiltinProfiles();
    return set;
}
// Runs polls until `untilUs`, returning the last output.
KbmCalibration::Output calPollTo(KbmCalibration& cal,std::uint64_t& t,std::uint64_t untilUs){
    KbmCalibration::Output out;
    while(t<untilUs){t+=4000;out=cal.poll(t);}
    return out;
}
std::array<kbmmodel::Game,3> calGames(kbmmodel::Game::Shape shape=kbmmodel::Game::Shape::Power){
    kbmmodel::Game walk;walk.deadzone=.2;walk.curve=1.8;walk.maxDegPerSecond=240;walk.vertical=.6;walk.shape=shape;
    kbmmodel::Game aim=walk;aim.deadzone=.15;aim.maxDegPerSecond=120;aim.vertical=.75;aim.curve=1.4;
    return {walk,aim,walk};
}
}

// --- Keys ------------------------------------------------------------------------------------------
TEST(kbmcal_chord_toggles_and_only_its_own_keys_are_taken){
    KbmCalibrationKeys keys;
    CHECK(keys.key('K',true,false)==CalKey::Pass&&keys.key('K',false,false)==CalKey::Pass);  // K alone is a game key
    CHECK(keys.key(0x11,true,false)==CalKey::Pass&&keys.key(0x12,true,false)==CalKey::Pass&&keys.key(0x10,true,false)==CalKey::Pass);
    CHECK(keys.key('K',true,false)==CalKey::Toggle);
    CHECK(keys.key('K',true,true)==CalKey::Consume);                          // auto-repeat is not a second toggle
    CHECK(keys.key('K',false,true)==CalKey::Consume);                         // and its release does not reach the game
    for(const std::uint32_t m:{0x11u,0x12u,0x10u})CHECK(keys.key(m,false,true)==CalKey::Pass);
    // While inactive, Space / Enter / Esc / left mouse are the game's.
    CHECK(keys.key(0x20,true,false)==CalKey::Pass&&keys.key(0x20,false,false)==CalKey::Pass);
    CHECK(keys.mouseButton(1,true,false)==CalKey::Pass&&keys.mouseButton(1,false,false)==CalKey::Pass);
    // While active they are ours.
    CHECK(keys.key(0x20,true,true)==CalKey::Tap&&keys.key(0x20,true,true)==CalKey::Consume&&keys.key(0x20,false,true)==CalKey::Consume);
    CHECK(keys.key(0x0D,true,true)==CalKey::Skip&&keys.key(0x0D,false,true)==CalKey::Consume);
    CHECK(keys.key(0x1B,true,true)==CalKey::Exit&&keys.key(0x1B,false,true)==CalKey::Consume);
    CHECK(keys.mouseButton(1,true,true)==CalKey::Tap&&keys.mouseButton(1,false,true)==CalKey::Consume);
    CHECK(keys.mouseButton(3,true,true)==CalKey::Pass&&keys.key('W',true,true)==CalKey::Pass);
    // A key held before the calibration started is released to the game, not swallowed.
    KbmCalibrationKeys held;
    CHECK(held.key(0x20,true,false)==CalKey::Pass);
    CHECK(held.key(0x20,false,true)==CalKey::Pass);
    // Left and right modifier codes count too.
    KbmCalibrationKeys sided;sided.key(0xA2,true,false);sided.key(0xA5,true,false);sided.key(0xA0,true,false);
    CHECK(sided.key('K',true,false)==CalKey::Toggle);
    sided.reset();CHECK(sided.key('K',true,false)==CalKey::Pass);
}

// --- The step machine --------------------------------------------------------------------------------
TEST(kbmcal_idle_between_steps_stops_and_releases_the_aim_button){
    // Agree to the aiming profile (the aim button is held for the game from then on), then do nothing.
    for(const bool tapInBetween:{false,true}){
        KbmCalibration cal;std::uint64_t t=1000000;
        CHECK(cal.start(calSet()));
        bool holding=false;
        for(int i=0;i<40&&!holding;++i){
            cal.tap(t);t+=4000;holding=cal.poll(t).holdAim;
            if(!holding){cal.skip(t);t+=4000;(void)cal.poll(t);}
        }
        CHECK(holding&&cal.active()&&!cal.running());
        (void)cal.takeMessages();
        if(tapInBetween){
            // A tap restarts the minute: 50 s idle, a tap starts the step and Enter skips it; then a new minute.
            CHECK(calPollTo(cal,t,t+50000000).holdAim&&cal.active());
            cal.tap(t);cal.skip(t);
        }
        CHECK(calPollTo(cal,t,t+59000000).holdAim&&cal.active());
        const auto out=calPollTo(cal,t,t+2000000);
        CHECK(!cal.active()&&!cal.finished()&&!out.holdAim&&!out.deflect);
        bool said=false;for(const auto& m:cal.takeMessages())said=said||(m.find("no tap for a minute")!=std::string::npos&&m.find("nothing was written")!=std::string::npos);
        CHECK(said);
        CHECK(!cal.poll(t+4000).holdAim);
    }
}
TEST(kbmcal_refuses_gyro_and_profiles_without_a_mouse_camera){
    KbmCalibration cal;
    auto gyro=calSet();for(auto& p:gyro.profiles)p.aim.mode=MouseAimConfig::Mode::Gyro;
    CHECK(!cal.start(gyro)&&!cal.active()&&!cal.takeMessages().empty());
    auto none=calSet();for(auto& p:none.profiles)p.mouse=KbmProfile::Mouse::None;
    CHECK(!cal.start(none)&&!cal.active());
    CHECK(cal.start(calSet())&&cal.active()&&!cal.running());
    const auto messages=cal.takeMessages();
    CHECK(messages.size()>=2&&messages[0].find("analog stick, not a mouse")!=std::string::npos);
}
TEST(kbmcal_sends_nothing_until_the_player_taps_and_only_plain_deflections){
    KbmCalibration cal;CHECK(cal.start(calSet()));
    std::uint64_t t=1000000;
    auto out=calPollTo(cal,t,t+3000000);
    CHECK(!out.deflect&&!out.holdAim&&out.x==0&&out.y==0);      // waiting: the player has the camera
    CHECK(cal.step()==KbmCalibration::Step::DeadzoneUp);
    cal.tap(t);                                                   // start the rising dead-zone ramp
    double last=0;bool rising=true;
    for(int i=0;i<1000;++i){
        out=cal.poll(t+=4000);
        CHECK(out.deflect&&out.y==0&&!out.holdAim&&out.x>=0&&out.x<=0.6+1e-9);
        rising=rising&&out.x>=last-1e-12;last=out.x;
        CHECK(std::abs(out.x*127-std::round(out.x*127))<1e-9);    // whole 8-bit steps
    }
    CHECK(rising&&last>0.19&&last<0.21);                          // 4 s at 5 % a second
    cal.tap(t);
    CHECK(!cal.running()&&cal.step()==KbmCalibration::Step::DeadzoneDown);
    out=cal.poll(t+=4000);CHECK(!out.deflect);                    // back to the player until the next tap
}
TEST(kbmcal_dead_zone_mean_cancels_the_players_delay){
    // The player is 300 ms late both ways: the rising tap is high, the falling one low.
    for(const double truth:{0.05,0.12,0.2,0.3}){
        KbmCalibration cal;CHECK(cal.start(calSet()));
        std::uint64_t t=1000000;cal.tap(t);
        const double lag=0.3,rate=0.05;
        std::uint64_t start=t;
        calPollTo(cal,t,start+std::uint64_t((truth/rate+lag)*1e6));
        cal.tap(t);
        cal.tap(t+=1000000);start=t;                              // falling: starts at 60 %
        calPollTo(cal,t,start+std::uint64_t(((0.6-truth)/rate+lag)*1e6));
        cal.tap(t);
        CHECK(cal.measured()[0].deadzone.has_value());
        CHECK(std::abs(*cal.measured()[0].deadzone-truth)<1.5/127);   // within the 8-bit steps
        CHECK(cal.step()==KbmCalibration::Step::TurnTime);
    }
}
TEST(kbmcal_missed_and_disagreeing_taps_are_asked_once_more_then_the_value_stays){
    KbmCalibration cal;CHECK(cal.start(calSet()));
    std::uint64_t t=1000000;
    // Dead zone: no tap at all, twice.
    for(int round=0;round<2;++round){
        CHECK(cal.step()==KbmCalibration::Step::DeadzoneUp);
        cal.tap(t);calPollTo(cal,t,t+16000000);
    }
    CHECK(!cal.measured()[0].deadzone&&cal.step()==KbmCalibration::Step::TurnTime);
    auto messages=cal.takeMessages();
    bool again=false,kept=false;
    for(const auto& m:messages){again=again||m.find("once more")!=std::string::npos;kept=kept||m.find("value stays")!=std::string::npos;}
    CHECK(again&&kept);
    // Full turn: intervals 1.5 s and 2.5 s disagree; then 1.5 and 1.6 agree.
    cal.tap(t+=500000);
    cal.tap(t+=1000000);cal.tap(t+=1500000);cal.tap(t+=2500000);
    CHECK(cal.step()==KbmCalibration::Step::TurnTime&&!cal.running()&&!cal.measured()[0].turnSeconds);
    cal.tap(t+=500000);
    cal.tap(t+=1000000);cal.tap(t+=1500000);
    cal.tap(t+=100000);                                           // a bounce: the same tap
    cal.tap(t+=1500000);
    CHECK(cal.measured()[0].turnSeconds&&std::abs(*cal.measured()[0].turnSeconds-1.55)<1e-6);
    CHECK(cal.step()==KbmCalibration::Step::Response);
}
TEST(kbmcal_skipping_the_full_turn_leaves_the_response_out_and_aim_needs_consent){
    KbmCalibration cal;CHECK(cal.start(calSet()));
    std::uint64_t t=1000000;
    cal.skip(t);                                                  // dead zone
    CHECK(cal.step()==KbmCalibration::Step::TurnTime);
    cal.skip(t);                                                  // full turn: the response steps go with it
    CHECK(cal.step()==KbmCalibration::Step::RatioUp&&cal.profile()==KbmProfileId::Walk);
    cal.skip(t);
    CHECK(cal.step()==KbmCalibration::Step::Intro&&cal.profile()==KbmProfileId::Aim);
    CHECK(!calPollTo(cal,t,t+100000).holdAim);                    // nothing held before the player agrees
    cal.tap(t);
    CHECK(cal.step()==KbmCalibration::Step::DeadzoneUp&&cal.profile()==KbmProfileId::Aim);
    auto out=calPollTo(cal,t,t+100000);CHECK(out.holdAim&&!out.deflect);
    cal.tap(t);out=calPollTo(cal,t,t+100000);CHECK(out.holdAim&&out.deflect);
    cal.skip(t);cal.skip(t);cal.skip(t);                          // the rest of aiming
    CHECK(cal.step()==KbmCalibration::Step::Intro&&cal.profile()==KbmProfileId::Drive);
    CHECK(!calPollTo(cal,t,t+100000).holdAim);                    // aiming ends with its steps
    cal.skip(t);                                                  // no vehicle
    CHECK(!cal.active()&&cal.finished());
    for(const auto& m:cal.measured())CHECK(!m.any());
}
TEST(kbmcal_stop_keeps_nothing){
    KbmCalibration cal;CHECK(cal.start(calSet()));
    std::uint64_t t=1000000;cal.skip(t);
    cal.tap(t);cal.tap(t+=1000000);cal.tap(t+=1500000);cal.tap(t+=1500000);
    CHECK(cal.measured()[0].turnSeconds);
    cal.tap(t+=100000);
    cal.stop("Esc");
    CHECK(!cal.active()&&!cal.finished()&&!cal.measured()[0].any());
    CHECK(!cal.poll(t+=4000).deflect);
}
TEST(kbmcal_vertical_swing_is_right_left_up_down_and_grows){
    KbmCalibration cal;CHECK(cal.start(calSet()));
    std::uint64_t t=1000000;cal.skip(t);cal.skip(t);
    CHECK(cal.step()==KbmCalibration::Step::RatioUp);
    cal.tap(t);
    // Without a measured turn the swing is 15 degrees at 200 per second: 75 ms each way.
    std::vector<int> pattern;double upTime[2]={0,0};int cycle=-1;int last=0;
    for(int i=0;i<1500;++i){
        const auto out=cal.poll(t+=4000);
        const int now=out.x>0?1:out.x<0?2:out.y<0?3:out.y>0?4:0;
        if(now!=last&&now!=0){if(now==1&&cycle<1)++cycle;if(pattern.size()<8)pattern.push_back(now);}
        if(now==3&&cycle>=0&&cycle<2)upTime[cycle]+=0.004;
        last=now;
    }
    CHECK((pattern==std::vector<int>{1,2,3,4,1,2,3,4}));
    CHECK(upTime[0]>0.02&&upTime[0]<0.04&&upTime[1]>upTime[0]);   // 0.4 x 75 ms, then 4 % more
}

// --- Numbers from measurements -------------------------------------------------------------------------
TEST(kbmcal_numbers_follow_from_the_measurements){
    auto set=calSet();set.countsPer360=9000;
    std::array<KbmCalibrationMeasured,3> m{};
    m[0].deadzone=.21;m[0].turnSeconds=1.5;m[0].ratio=1.6;
    const double c=1.8;
    for(std::size_t k=0;k<3;++k){m[0].responseAt[k]=.25*double(k+1)+.004;m[0].response[k]=std::pow(m[0].responseAt[k],c);}
    m[1].ratio=1.25;                                              // aiming: only the ratio
    const auto out=kbmCalibratedProfiles(set,m);
    const auto& walk=out[KbmProfileId::Walk].aim;
    CHECK(walk.deadzone==.21&&std::abs(walk.fullSpeedCounts-6000)<1e-9&&walk.sensitivityX==1&&std::abs(walk.sensitivityY-1.6)<1e-12);
    CHECK(walk.carryMs==kKbmCalibratedCarryMaxMs);                // 45 degrees at 240 per second (187.5 ms), capped
    CHECK(std::abs(walk.curve-c)<1e-6);
    for(std::size_t k=0;k<3;++k)CHECK(std::abs(walk.curvePoints[k]-std::pow(.25*double(k+1),c))<1e-6);
    const auto& aiming=out[KbmProfileId::Aim].aim;const auto& before=set[KbmProfileId::Aim].aim;
    CHECK(aiming.deadzone==before.deadzone&&aiming.fullSpeedCounts==before.fullSpeedCounts&&aiming.curve==before.curve);
    CHECK(std::abs(aiming.sensitivityY-1.25*before.sensitivityX)<1e-12);
    CHECK(out[KbmProfileId::Drive]==set[KbmProfileId::Drive]);   // nothing measured: unchanged
    CHECK(out.name==set.name&&out[KbmProfileId::Walk].bindings==set[KbmProfileId::Walk].bindings);
    for(const auto& p:out.profiles)CHECK(MouseAim(p.aim).config()==p.aim);
    // A response that does not rise gives the power alone.
    auto flat=m;flat[0].response={.2,.15,.6};
    CHECK(!MouseAim::table(kbmCalibratedProfiles(set,flat)[KbmProfileId::Walk].aim));
    // Without counts_per_360 the default target is used; a very slow turn keeps carry bounded.
    set.countsPer360=0;auto slow=m;slow[0].turnSeconds=8;
    const auto bounded=kbmCalibratedProfiles(set,slow)[KbmProfileId::Walk].aim;
    CHECK(std::abs(bounded.fullSpeedCounts-kKbmDefaultCountsPer360/8)<1e-9&&bounded.carryMs==kKbmCalibratedCarryMaxMs);
    auto quick=m;quick[0].turnSeconds=.2;                          // a fast game: 25 ms for 45 degrees
    CHECK(std::abs(kbmCalibratedProfiles(set,quick)[KbmProfileId::Walk].aim.carryMs-25)<1e-9);
}
TEST(kbmcal_slow_game_has_no_ghost_turn){
    // A model game with a full turn of 2 s or 4 s: a flick far faster than the game can turn, then the
    // hand stops. The stick must be back in the centre within the carry cap plus one poll (plus the
    // smoothing window, whose tail comes on top: 8 ms built in). Before the cap a 2 s turn gave 250 ms.
    for(const double turn:{2.0,4.0})for(const double window:{0.0,8.0}){
        auto set=calSet();set.countsPer360=8000;
        std::array<KbmCalibrationMeasured,3> m{};m[0].turnSeconds=turn;
        auto aimConfig=kbmCalibratedProfiles(set,m)[KbmProfileId::Walk].aim;aimConfig.smoothingMs=window;
        CHECK(aimConfig.carryMs<=kKbmCalibratedCarryMaxMs);
        MouseAim aim(aimConfig);
        for(int i=0;i<5;++i){aim.add(20000,0);(void)aim.step(4000);}  // the hand moves, then stops
        double tiltedMs=0;
        for(int i=0;i<200;++i){
            const auto out=aim.step(4000);
            if(out.x==0&&out.y==0)break;
            tiltedMs+=4;
        }
        CHECK(tiltedMs<=kKbmCalibratedCarryMaxMs+4+window);
    }
}
TEST(kbmcal_response_table_is_its_own_inverse_and_turns_one_to_one){
    MouseAimConfig c;c.curvePoints={.1,.3,.6};
    c=MouseAim(c).config();CHECK(MouseAim::table(c));
    for(double u=0;u<=1.0;u+=0.01)CHECK(std::abs(MouseAim::inverseResponse(c,MouseAim::response(c,u))-u)<1e-9);
    // A game shaped exactly like the table turns in proportion to the mouse, 16-bit and 8-bit.
    kbmmodel::Game game;game.deadzone=.12;game.maxDegPerSecond=360;game.shape=kbmmodel::Game::Shape::TwoZone;
    MouseAimConfig fitted;fitted.deadzone=.12;fitted.fullSpeedCounts=4000;fitted.smoothingMs=33;fitted.stickBits=8;
    fitted.curvePoints={game.response(.25),game.response(.5),game.response(.75)};
    const auto power=kbmmodel::linearity([&]{auto p=fitted;p.curvePoints={0,0,0};return p;}(),game,360.0/4000);
    const auto table=kbmmodel::linearity(fitted,game,360.0/4000);
    CHECK(table.shape<power.shape&&table.shape<6);
    // Not rising: no table.
    MouseAimConfig bad;bad.curvePoints={.3,.2,.6};CHECK(!MouseAim::table(MouseAim(bad).config()));
    bad.curvePoints={.3,.5,1.0};CHECK(!MouseAim::table(MouseAim(bad).config()));
    bad.curvePoints={std::nan(""),.5,.6};CHECK(!MouseAim::table(MouseAim(bad).config()));
    // The file: all three together, rising, or the file is told and they are left out.
    const auto good=parseKbmProfiles("walk.curve_25=0.1\nwalk.curve_50=0.3\nwalk.curve_75=0.6\n");
    CHECK(good.ok()&&MouseAim::table(good.set[KbmProfileId::Walk].aim)&&!MouseAim::table(good.set[KbmProfileId::Aim].aim));
    const auto partial=parseKbmProfiles("walk.curve_25=0.1\n");
    CHECK(partial.problemCount==1&&partial.problems[0].line==0&&!MouseAim::table(partial.set[KbmProfileId::Walk].aim));
    for(const auto& p:partial.set.profiles)CHECK(MouseAim(p.aim).config()==p.aim);
    const auto parsed=parseKbmProfiles("game=GTA 6\ncounts_per_360=12000\n");
    CHECK(parsed.ok()&&parsed.set.game=="GTA 6"&&parsed.set.countsPer360==12000);
    CHECK(parseKbmProfiles("counts_per_360=5\n").problemCount==1&&parseKbmProfiles("game=\n").problemCount==1);
}

// --- The whole calibration against a simulated game (Linux model) --------------------------------------
TEST(kbmcal_model_recovers_a_simulated_game_and_makes_the_turn_more_linear){
    for(const auto shape:{kbmmodel::Game::Shape::Power,kbmmodel::Game::Shape::TwoZone}){
        const auto games=calGames(shape);
        auto set=calSet();set.countsPer360=8000;
        const auto run=kbmmodel::calibrate(set,games,kbmmodel::Player{},7);
        CHECK(run.finished&&run.seconds<600);                     // a few minutes per game
        const auto& walk=run.measured[0];const auto& aim=run.measured[1];
        CHECK(walk.deadzone&&walk.turnSeconds&&walk.ratio&&walk.response[0]&&walk.response[2]);
        CHECK(aim.deadzone&&aim.turnSeconds&&aim.ratio);
        CHECK(!run.measured[2].any());                            // the player skipped the vehicle
        CHECK(std::abs(*walk.turnSeconds-1.5)<0.15&&std::abs(*aim.turnSeconds-3.0)<0.3);
        CHECK(std::abs(*walk.ratio-1/.6)<0.2&&std::abs(*aim.ratio-1/.75)<0.15);
        CHECK(*walk.deadzone>.19&&*walk.deadzone<.26);            // visible threshold: never below the truth by more than a step
        const auto calibrated=kbmCalibratedProfiles(set,run.measured);
        for(const auto id:{KbmProfileId::Walk,KbmProfileId::Aim}){
            const auto& game=games[std::size_t(id)];
            const auto before=kbmmodel::linearity(set[id].aim,game,360.0/8000);
            const auto after=kbmmodel::linearity(calibrated[id].aim,game,360.0/8000);
            CHECK(after.shape<before.shape/2&&after.shape<15);
            CHECK(std::abs(after.scale)<12);
        }
    }
}
TEST(kbmcal_model_tolerates_tap_errors_of_80_ms){
    const auto games=calGames();
    auto set=calSet();set.countsPer360=8000;
    int finished=0;
    for(std::uint32_t seed=1;seed<=12;++seed){
        const auto run=kbmmodel::calibrate(set,games,kbmmodel::Player{.25,.08,1.0,false},seed);
        if(!run.finished)continue;
        ++finished;
        const auto& walk=run.measured[0];
        if(walk.turnSeconds)CHECK(std::abs(*walk.turnSeconds-1.5)<0.12);
        if(walk.deadzone)CHECK(std::abs(*walk.deadzone-.2)<.06);
        if(walk.ratio)CHECK(std::abs(*walk.ratio/(1/.6)-1)<0.12);
        const auto after=kbmmodel::linearity(kbmCalibratedProfiles(set,run.measured)[KbmProfileId::Walk].aim,games[0],360.0/8000);
        CHECK(after.shape<15);
    }
    CHECK(finished==12);
}

// --- Mouse report rate ---------------------------------------------------------------------------------
TEST(kbmcal_raw_batch_counts_reports_with_movement){
    RawMouseBatch batch;batch.add(0,1,0);batch.add(0,0,0);batch.add(RawMouseBatch::kAbsolute,5,5);batch.add(0,0,-2);
    CHECK(batch.reports==4&&batch.moved==2&&batch.absolute==1);
}
TEST(kbmcal_report_rate_is_found_from_read_times){
    for(const int hz:{125,250,500,1000,2000,4000,8000}){
        KbmMouseRate rate;std::uint64_t t=5000000;
        // Reads come in batches every 1 to 16 ms (a busy interface thread), movement for 3 s.
        const double perUs=hz/1e6;double owed=0;std::uint64_t step=1000;
        for(int i=0;t<8000000;++i){step=1000+std::uint64_t((i*7919)%15)*1000;t+=step;owed+=perUs*double(step);
            const unsigned n=unsigned(owed);owed-=n;rate.add(t,n);}
        rate.add(t+300000,1);                                     // closes the last bins
        CHECK(rate.classHz()==hz);CHECK(rate.takeChange()&&!rate.takeChange());
    }
    // A short flick, slow movement, nothing: no class yet or a lower one, never a higher one.
    KbmMouseRate quiet;CHECK(quiet.classHz()==0);
    quiet.add(1000000,40);quiet.add(1050000,40);CHECK(quiet.classHz()==0);
    CHECK(KbmMouseRate::snap(700)==500||KbmMouseRate::snap(700)==1000);
    CHECK(KbmMouseRate::snap(900)==1000&&KbmMouseRate::snap(6000)==8000&&KbmMouseRate::snap(0)==0);
}
TEST(kbmcal_accumulation_follows_the_report_rate){
    auto a=kbmAccumulationFor(125,4,33);
    CHECK(a.reportIntervalMs==8&&a.shortestUsefulPollMs==8&&a.windowFloorMs==16&&a.pollWastes&&!a.floorRaises);
    a=kbmAccumulationFor(125,4,0);CHECK(a.floorRaises);           // smoothing off with a slow mouse: the floor matters
    a=kbmAccumulationFor(1000,4,0);CHECK(a.shortestUsefulPollMs==1&&a.windowFloorMs==2&&!a.pollWastes&&!a.floorRaises);
    a=kbmAccumulationFor(8000,1,0);CHECK(a.shortestUsefulPollMs==1&&!a.pollWastes&&!a.floorRaises);
    a=kbmAccumulationFor(500,1,0);CHECK(a.pollWastes&&a.shortestUsefulPollMs==2);
    CHECK(kbmAccumulationFor(0,4,8).reportIntervalMs==0);
    // The floor in MouseAim: a 125 Hz mouse at a 4 ms poll, smoothing off, steady hand. Without it
    // every other poll reads as a stop; with it none does.
    for(const double floor:{0.0,16.0}){
        MouseAimConfig c;c.smoothingMs=0;c.stickBits=16;MouseAim aim(c);aim.setWindowFloorMs(floor);
        int stops=0;
        for(int i=0;i<200;++i){if(i%2==0)aim.add(8,0);const auto o=aim.step(4000);if(i>10&&o.x==0)++stops;}
        CHECK(floor==0?stops>80:stops==0);
    }
}

// --- The profile file ----------------------------------------------------------------------------------
TEST(kbmcal_game_name_and_file_name){
    CHECK(kbmGameName("# mine\r\nGame = GTA 6 \r\ngame=other\n")=="GTA 6");
    CHECK(kbmGameName("\xEF\xBB\xBFgame=Red Dead\n")=="Red Dead");
    CHECK(kbmGameName("gamex=1\ngame=\ngame=bad\x01name\n").empty());
    CHECK(kbmGameSlug("GTA 6")=="gta-6"&&kbmGameSlug("  ..Halo: Infinite!!")=="halo-infinite"&&kbmGameSlug("..").empty());
    CHECK(kbmGameSlug(std::string(100,'a')).size()==48);
    CHECK(kbmCalibratedFileName("GTA 6")=="slipstream-kbm-gta-6.txt"&&kbmCalibratedFileName("")=="slipstream-kbm-default.txt");
    CHECK(kbmCalibratedFileName("../../x")=="slipstream-kbm-x.txt");   // never a path
}
TEST(kbmcal_block_reads_back_and_replaces_the_old_one){
    auto set=calSet();set.game="GTA 6";set.countsPer360=8000;
    std::array<KbmCalibrationMeasured,3> m{};
    m[0].deadzone=.2;m[0].turnSeconds=1.5;m[0].ratio=1.6;
    for(std::size_t k=0;k<3;++k){m[0].responseAt[k]=.25*double(k+1);m[0].response[k]=std::pow(m[0].responseAt[k],1.8);}
    m[1].turnSeconds=3;
    const auto calibrated=kbmCalibratedProfiles(set,m);
    const std::string existing="# my file\nname=mine\nwalk.bind.G=Triangle\nwalk.deadzone=0.05\n";
    const auto block=kbmCalibrationBlock(calibrated,m);
    const auto once=kbmMergeCalibration(existing,block);
    CHECK(once.rfind(existing,0)==0);                             // every old line kept, in place
    const auto parsed=parseKbmProfiles(once);
    CHECK(parsed.ok());
    const auto& walk=parsed.set[KbmProfileId::Walk];
    CHECK(walk.bindings.action('G')==PadAction::Triangle&&parsed.set.name=="mine"&&parsed.set.game=="GTA 6");
    CHECK(std::abs(walk.aim.deadzone-.2)<1e-4&&std::abs(walk.aim.fullSpeedCounts-calibrated[KbmProfileId::Walk].aim.fullSpeedCounts)<0.1);
    CHECK(std::abs(walk.aim.sensitivityY-1.6)<1e-4&&MouseAim::table(walk.aim)&&std::abs(walk.aim.curve-1.8)<1e-3);
    CHECK(std::abs(parsed.set[KbmProfileId::Aim].aim.fullSpeedCounts-8000.0/3)<0.1);
    CHECK(parsed.set[KbmProfileId::Drive]==kbmBuiltinProfiles()[KbmProfileId::Drive]);
    // Again with other numbers: one block, the new one.
    auto m2=m;m2[0].deadzone=.1;
    const auto twice=kbmMergeCalibration(once,kbmCalibrationBlock(kbmCalibratedProfiles(set,m2),m2));
    std::size_t blocks=0;for(std::size_t at=twice.find(kKbmCalibrationBegin);at!=std::string::npos;at=twice.find(kKbmCalibrationBegin,at+1))++blocks;
    CHECK(blocks==1&&std::abs(parseKbmProfiles(twice).set[KbmProfileId::Walk].aim.deadzone-.1)<1e-4);
    // No final line end, Windows line ends.
    CHECK(kbmMergeCalibration("name=x\r\nwalk.curve=2",block).find("walk.curve=2\n"+std::string(kKbmCalibrationBegin))!=std::string::npos);
    // A new file starts with a name.
    CHECK(parseKbmProfiles(kbmNewProfileText(set)+block).set.name=="GTA 6 (calibrated)");
}
TEST(kbmcal_file_is_backed_up_before_it_is_replaced){
    const auto dir=std::filesystem::temp_directory_path()/("kbmcal-test-"+std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    const auto path=dir/"slipstream-kbm-gta-6.txt";
    auto first=kbmWriteProfile(path,"name=one\n");
    CHECK(first.ok&&!first.backedUp&&!std::filesystem::exists(dir/"slipstream-kbm-gta-6.txt.bak"));
    auto second=kbmWriteProfile(path,"name=two\n");
    CHECK(second.ok&&second.backedUp);
    bool found=false;
    CHECK(kbmReadSmallFile(path,found)=="name=two\n"&&found);
    CHECK(kbmReadSmallFile(dir/"slipstream-kbm-gta-6.txt.bak",found)=="name=one\n"&&found);
    CHECK(!std::filesystem::exists(dir/"slipstream-kbm-gta-6.txt.new"));
    // A second calibration keeps the FIRST backup: the player's file from before any calibration.
    auto third=kbmWriteProfile(path,"name=three\n");
    CHECK(third.ok&&!third.backedUp&&third.backupKept);
    CHECK(kbmReadSmallFile(path,found)=="name=three\n");
    CHECK(kbmReadSmallFile(dir/"slipstream-kbm-gta-6.txt.bak",found)=="name=one\n");
    // Over the size limit: refused, the file stays whole.
    auto huge=kbmWriteProfile(path,std::string(kKbmProfileMaxBytes+1,'#'));
    CHECK(!huge.ok&&!huge.error.empty()&&kbmReadSmallFile(path,found)=="name=three\n");
    // A file read only in part (over the limit) is never written back cut short.
    std::string why;
    const std::string block=std::string(kKbmCalibrationBegin)+"\nwalk.sens_x=1.1\n"+std::string(kKbmCalibrationEnd)+"\n";
    const auto set=calSet();
    CHECK(!kbmCalibratedProfileText(std::string(kKbmProfileMaxBytes+1,'#'),true,set,block,why)&&!why.empty());
    CHECK(!kbmCalibratedProfileText(std::string(kKbmProfileMaxBytes-4,'#'),true,set,block,why)&&!why.empty());
    const auto fine=kbmCalibratedProfileText("name=mine\n",true,set,block,why);
    CHECK(fine&&why.empty()&&fine->rfind("name=mine\n",0)==0&&fine->find(kKbmCalibrationBegin)!=std::string::npos);
    const auto fresh=kbmCalibratedProfileText("",false,set,block,why);
    CHECK(fresh&&fresh->rfind("# Slipstream",0)==0);
    kbmReadSmallFile(dir/"missing.txt",found);CHECK(!found);
    // A folder that does not exist: an error, nothing half-written.
    const auto bad=kbmWriteProfile(dir/"no"/"such"/"x.txt","name=x\n");
    CHECK(!bad.ok&&!bad.error.empty());
    std::filesystem::remove_all(dir);
}

// --- Switching profiles in the middle of a movement ------------------------------------------------------
TEST(kbmcal_switching_walk_aim_drive_mid_movement_keeps_the_camera_and_unchanged_keys){
    // Every profile turns the camera with the same numbers here, so the right stick must carry on
    // through each switch: per poll it may change by no more than one poll of new movement can
    // change it. Keys that mean the same in both profiles stay held.
    auto set=kbmBuiltinProfiles();
    for(auto& p:set.profiles){p.aim.sensitivityX=p.aim.sensitivityY=1;p.aim.deadzone=.12;p.aim.fullSpeedCounts=4000;}
    KeyboardMousePad pad(set);std::uint64_t t=1000;
    ControllerState physical;physical.inputActive=true;
    // One poll of 8 counts at 4000/s moves the 33 ms window by 1/8 of its sum: the largest honest change.
    const double onePoll=(1-.12)*8.0/(4000*0.004)/8+2.0/127;
    double previous=-1;int polls=0;
    const auto poll=[&]{
        pad.mouseMove(8,0);
        const auto out=pad.apply(physical,t+=4000);
        const double tilt=out.rightX/32767.0;
        if(previous>=0&&polls>12)CHECK(std::abs(tilt-previous)<=onePoll+1e-9);
        previous=tilt;++polls;
        return out;
    };
    pad.key(0x10,true);                                           // Shift: Cross on foot and in the car
    for(int i=0;i<20;++i)poll();
    CHECK(pad.profile()==KbmProfileId::Walk);
    pad.mouseButton(3,true);                                      // aim (L2)
    for(int i=0;i<20;++i)CHECK((poll().buttons&ControllerState::Cross)!=0);
    CHECK(pad.profile()==KbmProfileId::Aim);
    pad.key(0x14,true);                                           // Caps: into the car while moving the mouse
    ControllerState out;
    for(int i=0;i<20;++i)out=poll();
    CHECK(pad.profile()==KbmProfileId::Drive&&pad.driving());
    CHECK((out.buttons&ControllerState::Cross)!=0);               // the same meaning: still held
    CHECK(out.l2==0);                                             // right mouse means nothing in the car: released
    CHECK(out.rightX>0);
    pad.key(0x14,false);pad.mouseButton(3,false);pad.key(0x10,false);
    out=pad.apply(physical,t+=4000);
    CHECK(out.buttons==0);
}
TEST(kbmcal_switch_releases_what_changes_meaning_at_once){
    auto set=kbmBuiltinProfiles();
    KeyboardMousePad pad(set);std::uint64_t t=1000;ControllerState physical;physical.inputActive=true;
    pad.key('W',true);pad.key('A',true);
    auto out=pad.apply(physical,t+=4000);
    CHECK(out.leftY<0&&out.leftX<0);
    pad.key(0x14,true);out=pad.apply(physical,t+=4000);
    // W was forward and is the throttle now: released, the stick back at once; A steers in both
    // and keeps its direction, from the driving profile's dead zone on.
    CHECK(out.r2==0&&out.leftY==0&&out.leftX<0);
    pad.key('W',true);out=pad.apply(physical,t+=4000);CHECK(out.r2==0);   // auto-repeat of a blocked key
    pad.key('W',false);pad.key('W',true);out=pad.apply(physical,t+=4000);CHECK(out.r2>0);
}
