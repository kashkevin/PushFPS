// SPDX-License-Identifier: GPL-3.0-only
// Keyboard and mouse on a PS5 stream (VEYRA_EXP_RP_KBM): the portable parts of the capture, the
// translation and the profiles. Included by CoreTests.cpp above main(); uses its TEST / CHECK.
#include "veyra/remoteplay/KbmCapture.h"
#include "veyra/remoteplay/KbmProfile.h"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// --- Raw Input: the buffered read (GetRawInputBuffer) -------------------------------------------
TEST(kbm_raw_blocks_follow_the_documented_alignment){
    // NEXTRAWINPUTBLOCK: the next block starts at the next pointer-size boundary after dwSize bytes.
    // x64 sizes: header 24, mouse block 48, keyboard block 40, a HID block of 24 + 8 + 5 bytes.
    CHECK(rawInputNextOffset(0,48,8)==48);
    CHECK(rawInputNextOffset(48,40,8)==88);
    CHECK(rawInputNextOffset(88,37,8)==128);
    CHECK(rawInputNextOffset(0,1,8)==8);
    CHECK(rawInputNextOffset(0,33,4)==36);          // a 32-bit process aligns to 4
    CHECK(rawInputNextOffset(16,0,8)==16);
    // A block is used only when it lies whole inside the buffer and has room for its payload.
    CHECK(rawInputBlockFits(0,48,48,3072));
    CHECK(rawInputBlockFits(3024,48,48,3072));
    CHECK(!rawInputBlockFits(3025,48,48,3072));      // would end past the buffer
    CHECK(!rawInputBlockFits(0,40,48,3072));         // too small to hold a mouse report
    CHECK(!rawInputBlockFits(4000,48,48,3072));      // starts past the buffer
    CHECK(!rawInputBlockFits(0,std::size_t(-1),48,3072));
}
TEST(kbm_raw_mouse_batch_adds_relative_reports_and_ignores_absolute_ones){
    RawMouseBatch batch;
    CHECK(batch.empty());
    batch.add(0,3,-2);batch.add(0,-1,5);
    batch.add(RawMouseBatch::kAbsolute,40000,20000);                       // tablet, remote desktop
    batch.add(RawMouseBatch::kAbsolute|RawMouseBatch::kVirtualDesktop,1,1);
    batch.add(0,0,0);                                                      // a button-only report
    CHECK(batch.reports==5&&batch.absolute==2);
    CHECK(batch.x()==2&&batch.y()==3&&!batch.empty());
    // An 8000 Hz mouse for a whole second in one go still adds up exactly.
    RawMouseBatch fast;for(int i=0;i<8000;++i)fast.add(0,127,-127);
    CHECK(fast.x()==8000*127&&fast.y()==-8000*127&&fast.reports==8000);
    // Sums that do not fit the sink's int are clamped, never wrapped.
    RawMouseBatch huge;for(int i=0;i<4;++i)huge.add(0,2147483647,-2147483647-1);
    CHECK(huge.x()==2147483647&&huge.y()==-2147483647);
}
TEST(kbm_raw_stats_keep_the_largest_backlog){
    RawInputStats stats;
    RawMouseBatch one;one.add(0,1,0);stats.message(one,false);
    RawMouseBatch many;for(int i=0;i<9;++i)many.add(0,1,0);many.add(RawMouseBatch::kAbsolute,5,5);stats.message(many,true);
    CHECK(stats.messages==2&&stats.reports==11&&stats.absolute==1&&stats.drains==1&&stats.largest==10);
}

// --- Capture state: nothing stays pressed ----------------------------------------------------------
namespace {
using KbmEvent=KbmCaptureState::Event;
const KbmEvent kKbmEvents[]={KbmEvent::PollFront,KbmEvent::PollAway,KbmEvent::FocusLost,KbmEvent::ReleaseHotkey,
    KbmEvent::SessionEnded,KbmEvent::WindowChanged,KbmEvent::DeviceRemoved,KbmEvent::EngageFailed};
}
TEST(kbm_capture_engages_in_front_and_releases_on_every_way_out){
    KbmCaptureState s;
    CHECK(s.state()==KbmCaptureState::State::Released&&!s.captured());
    CHECK(s.on(KbmEvent::PollAway)==KbmCaptureState::Actions{});
    auto a=s.on(KbmEvent::PollFront);
    CHECK(a.engage&&a.releaseInputs&&!a.disengage&&s.captured());       // starts from nothing held
    CHECK(s.on(KbmEvent::PollFront)==KbmCaptureState::Actions{});        // steady state: no work per poll
    // Focus loss, the window going behind, the session ending, another window: all release.
    for(const auto leave:{KbmEvent::FocusLost,KbmEvent::PollAway,KbmEvent::SessionEnded,KbmEvent::WindowChanged}){
        KbmCaptureState t;t.on(KbmEvent::PollFront);
        const auto out=t.on(leave);
        CHECK(out.disengage&&out.releaseInputs&&!out.engage);
        CHECK(t.state()==KbmCaptureState::State::Released);
        CHECK(out.restart==(leave==KbmEvent::SessionEnded||leave==KbmEvent::WindowChanged)); // a new session starts on foot
    }
    // A keyboard or mouse that was in use is unplugged: its keys can never send their release.
    a=s.on(KbmEvent::DeviceRemoved);
    CHECK(a.releaseInputs&&!a.disengage&&!a.engage&&s.captured());
}
TEST(kbm_capture_release_hotkey_keeps_the_mouse_free_until_the_window_is_left){
    KbmCaptureState s;s.on(KbmEvent::PollFront);
    const auto a=s.on(KbmEvent::ReleaseHotkey);
    CHECK(a.disengage&&a.releaseInputs&&s.state()==KbmCaptureState::State::Suspended);
    for(int i=0;i<1000;++i)CHECK(s.on(KbmEvent::PollFront)==KbmCaptureState::Actions{}); // not taken back
    CHECK(s.on(KbmEvent::PollAway)==KbmCaptureState::Actions{});
    CHECK(s.state()==KbmCaptureState::State::Released);
    CHECK(s.on(KbmEvent::PollFront).engage);                                              // entered again
    s.on(KbmEvent::ReleaseHotkey);
    const auto ended=s.on(KbmEvent::SessionEnded);                                        // the next session captures again
    CHECK(ended.restart&&!ended.disengage&&s.state()==KbmCaptureState::State::Released);
}
TEST(kbm_capture_failed_engage_is_not_captured){
    KbmCaptureState s;
    CHECK(s.on(KbmEvent::PollFront).engage);
    const auto a=s.on(KbmEvent::EngageFailed);
    CHECK(!s.captured()&&a.releaseInputs&&!a.disengage);   // nothing was registered: nothing to undo
    CHECK(s.on(KbmEvent::PollFront).engage);               // tried again on the next poll
}
TEST(kbm_capture_random_walk_never_leaves_keys_held_outside_a_capture){
    // A model of the Win32 side: `engaged` is the Raw Input registration, `held` the pad's keys.
    std::mt19937 rng(4711);
    for(int run=0;run<200;++run){
        KbmCaptureState s;bool engaged=false;unsigned held=0;
        for(int step=0;step<400;++step){
            const auto event=kKbmEvents[rng()%std::size(kKbmEvents)];
            if(event==KbmEvent::EngageFailed&&!s.captured())continue;   // only ever follows an engage
            const bool was=s.captured();
            const auto a=s.on(event);
            if(a.disengage){CHECK(engaged);engaged=false;}
            if(a.releaseInputs)held=0;
            if(a.engage){CHECK(!engaged);engaged=true;CHECK(held==0);}
            if(event==KbmEvent::EngageFailed)engaged=false;
            CHECK(engaged==s.captured());
            if(was&&!s.captured())CHECK(a.releaseInputs);
            if(event==KbmEvent::DeviceRemoved&&was)CHECK(a.releaseInputs);
            if(!s.captured())CHECK(held==0);
            if(s.captured()&&rng()%2)++held;                             // keys go down only while captured
        }
    }
}
TEST(kbm_device_set_knows_who_typed){
    KbmDeviceSet devices;
    CHECK(!devices.contains(7));
    devices.note(7);devices.note(7);devices.note(9);devices.note(0);     // 0: no device handle (injected input)
    CHECK(devices.size()==2&&devices.contains(7)&&devices.contains(9)&&!devices.contains(0)&&!devices.contains(8));
    CHECK(devices.remove(7)&&!devices.contains(7)&&!devices.remove(7)&&devices.size()==1);
    devices.clear();CHECK(devices.size()==0&&!devices.contains(9));
    // More devices than it holds: it can no longer tell, so every removal counts (fail safe).
    for(std::uintptr_t id=1;id<=KbmDeviceSet::kCapacity+1;++id)devices.note(id);
    CHECK(devices.contains(12345)&&devices.remove(12345));
    devices.clear();CHECK(!devices.contains(12345));
}

// --- Hand-over between a physical controller and keyboard / mouse --------------------------------------
namespace {
ControllerState kbmPhysical(std::int16_t lx=0,std::int16_t ly=0,std::int16_t rx=0,std::int16_t ry=0){
    ControllerState s;s.inputActive=true;s.leftX=lx;s.leftY=ly;s.rightX=rx;s.rightY=ry;return s;
}
MouseAimConfig kbmPlainAim(){MouseAimConfig c;c.fullSpeedCounts=4000;c.deadzone=.12;c.smoothingMs=0;return c;}
}
TEST(kbm_handover_last_active_device_wins){
    StickHandover stick(0);std::uint64_t t=0;
    const auto step=[&](double x,double y,bool kbm,bool edge=false){t+=4000;return stick.update(x,y,kbm,edge,t);};
    CHECK(step(0,0,false)==StickHandover::Owner::Pad);
    CHECK(step(0,0,true)==StickHandover::Owner::Kbm);               // a key goes down
    CHECK(step(.1,0,true)==StickHandover::Owner::Kbm);              // the stick is only touched
    CHECK(step(.6,0,true)==StickHandover::Owner::Pad);              // pushed: the pad takes over
    CHECK(step(.6,0,true)==StickHandover::Owner::Pad);              // the key is still down: the pad stays
    CHECK(step(.6,0,true,true)==StickHandover::Owner::Kbm);         // another key: the keyboard again
    CHECK(step(.62,.02,true)==StickHandover::Owner::Kbm);           // the held stick trembles: no take-over
    CHECK(step(-.6,0,true)==StickHandover::Owner::Pad);             // thrown the other way: new input
    CHECK(step(0,0,true)==StickHandover::Owner::Kbm);               // let go while the key is held
    CHECK(step(0,0,false)==StickHandover::Owner::Pad);              // nobody: the physical stick passes through
}
TEST(kbm_handover_ignores_stick_drift_and_chatter){
    StickHandover stick(0);std::uint64_t t=0;
    stick.update(0,0,true,false,t+=4000);
    // A worn stick resting at 25 % and wobbling never becomes "active".
    for(int i=0;i<500;++i)CHECK(stick.update(i%2?.25:.29,0,true,false,t+=4000)==StickHandover::Owner::Kbm);
    // Once pushed past 30 % it stays active down to 20 %: no flapping at the threshold.
    CHECK(stick.update(.31,0,true,false,t+=4000)==StickHandover::Owner::Pad);
    for(int i=0;i<500;++i)CHECK(stick.update(i%2?.21:.31,0,true,false,t+=4000)==StickHandover::Owner::Pad);
    // Values that are not numbers count as a stick at rest.
    CHECK(stick.update(std::nan(""),0,true,false,t+=4000)==StickHandover::Owner::Kbm);
}
TEST(kbm_handover_mouse_keeps_the_stick_between_reports){
    // A slow mouse reports in only some polls. The camera must not snap to a held pad stick in between.
    StickHandover stick(250000);std::uint64_t t=1000000;
    CHECK(stick.update(.8,0,false,false,t)==StickHandover::Owner::Pad);
    CHECK(stick.update(.8,0,true,false,t+=4000)==StickHandover::Owner::Kbm);
    for(int i=0;i<62;++i)CHECK(stick.update(.8,0,false,false,t+=4000)==StickHandover::Owner::Kbm); // 248 ms of silence
    CHECK(stick.update(.8,0,false,false,t+=4000)==StickHandover::Owner::Pad);                       // 252 ms: the pad again
    stick.update(.8,0,true,false,t+=4000);stick.reset();
    CHECK(stick.update(.8,0,false,false,t+=4000)==StickHandover::Owner::Pad);                       // a release forgets the hold
}
TEST(kbm_pad_keyboard_and_controller_share_the_left_stick){
    KeyboardMousePad pad(kbmPlainAim());std::uint64_t t=1000;
    const auto poll=[&](const ControllerState& physical){t+=4000;return pad.apply(physical,t);};
    auto out=poll(kbmPhysical(20000,0));CHECK(out.leftX==20000);              // the controller alone
    pad.key('W',true);out=poll(kbmPhysical(20000,0));
    CHECK(out.leftX==0&&out.leftY==-32767);                                    // W pressed after: the keyboard
    out=poll(kbmPhysical(3000,0));out=poll(kbmPhysical(-25000,0));
    CHECK(out.leftX==-25000&&out.leftY==0);                                    // the stick pushed after: the controller
    out=poll(kbmPhysical(0,0));CHECK(out.leftX==0&&out.leftY==-32767);         // let go, W still held: forward again
    pad.key('W',false);out=poll(kbmPhysical(1200,-300));
    CHECK(out.leftX==1200&&out.leftY==-300);                                   // nothing held: the physical stick, drift and all
}
TEST(kbm_pad_mouse_and_controller_share_the_right_stick){
    KeyboardMousePad pad(kbmPlainAim());std::uint64_t t=1000;
    const auto poll=[&](const ControllerState& physical){t+=4000;return pad.apply(physical,t);};
    auto out=poll(kbmPhysical(0,0,0,18000));CHECK(out.rightY==18000);
    pad.mouseMove(8,0);out=poll(kbmPhysical(0,0,0,18000));
    CHECK(out.rightX>0&&out.rightY==0);                                        // the mouse moved last
    out=poll(kbmPhysical(0,0,0,18000));CHECK(out.rightX==0&&out.rightY==0);    // it stopped: the camera stops, the held stick waits
    for(std::uint64_t i=0;i<KeyboardMousePad::kMouseHoldUs/4000;++i)out=poll(kbmPhysical(0,0,0,18000));
    CHECK(out.rightY==18000);                                                  // quiet for 250 ms: the controller again
    pad.mouseMove(8,0);poll(kbmPhysical(0,0,0,18000));
    out=poll(kbmPhysical(0,0,-30000,0));CHECK(out.rightX==-30000);             // a new push on the stick wins at once
}
TEST(kbm_pad_buttons_and_triggers_add_up){
    KeyboardMousePad pad(kbmPlainAim());
    ControllerState physical=kbmPhysical();physical.buttons=ControllerState::Cross|ControllerState::Up;physical.l2=100;physical.r2=200;
    pad.key('C',true);pad.mouseButton(3,true);                                // Circle, L2
    auto out=pad.apply(physical,5000);
    CHECK(out.buttons==(ControllerState::Cross|ControllerState::Up|ControllerState::Circle));
    CHECK(out.l2==255&&out.r2==200);                                           // the deeper of the two
    pad.key('C',false);pad.mouseButton(3,false);out=pad.apply(physical,9000);
    CHECK(out.buttons==(ControllerState::Cross|ControllerState::Up)&&out.l2==100&&out.r2==200);
}
TEST(kbm_pad_release_all_leaves_nothing_behind){
    KeyboardMousePad pad(kbmPlainAim());std::uint64_t t=1000;
    const ControllerState physical=kbmPhysical(700,-200,-500,100);
    for(const auto key:{std::uint32_t('W'),std::uint32_t('D'),std::uint32_t(0x20),std::uint32_t('Q'),std::uint32_t('E')})pad.key(key,true);
    pad.key('W',true);                                                         // auto-repeat is still one key
    pad.mouseButton(1,true);pad.mouseButton(3,true);pad.scroll(120);
    pad.mouseMove(100000,50000);                                               // a flick with 40 ms of carry behind it
    auto out=pad.apply(physical,t+=4000);
    CHECK(out.buttons!=0&&out.r2==255&&out.l2==255&&out.leftX>20000&&out.rightX>20000);
    pad.releaseAll();
    out=pad.apply(physical,t+=4000);
    ControllerState expected=physical;                                         // exactly the controller, nothing of ours
    CHECK(out==expected);
    for(int i=0;i<100;++i)CHECK(pad.apply(physical,t+=4000)==expected);        // and it stays that way: no carry, no pulse, no hold
    // Releases of keys that are no longer held (they arrive after the capture is back) change nothing.
    pad.key('W',false);pad.mouseButton(1,false);pad.key(0x99,false);
    CHECK(pad.apply(physical,t+=4000)==expected);
    // Without a controller the console gets a pad at rest.
    ControllerState none;pad.key('W',true);pad.apply(none,t+=4000);pad.releaseAll();
    ControllerState rest;rest.inputActive=true;
    CHECK(pad.apply(none,t+=4000)==rest);
}

// --- Mouse to stick: translation quality -------------------------------------------------------------
namespace {
// What a game turns by for one poll of a given stick position, in mouse counts: the console is
// taken to hand the game `bits` of the stick (values cut towards zero), and the game to ignore
// `deadzone` and turn with the rest to the power `curve`. The inverse of MouseAim's mapping.
struct KbmGameModel {
    double full=4000,deadzone=.12,curve=1;int bits=8;
    double x=0,y=0;       // counts turned so far
    void poll(const MouseAim::Output& o,double dtSeconds){
        const int shift=16-bits,levels=(1<<(bits-1))-1;
        const int sx=std::abs(int(o.x))>=32767?(o.x<0?-levels:levels):int(o.x)/(1<<shift);
        const int sy=std::abs(int(o.y))>=32767?(o.y<0?-levels:levels):int(o.y)/(1<<shift);
        const double tilt=std::hypot(double(sx),double(sy))/levels;
        if(tilt<=deadzone)return;
        const double speed=std::pow(std::min(1.0,(tilt-deadzone)/(1-deadzone)),curve)*full;
        x+=sx/(tilt*levels)*speed*dtSeconds;y+=sy/(tilt*levels)*speed*dtSeconds;
    }
};
MouseAimConfig kbmEightBit(){auto c=kbmPlainAim();c.stickBits=8;return c;}
double kbmTilt(const MouseAim::Output& o){return std::hypot(double(o.x),double(o.y))/32767.0;}
}
TEST(kbm_aim_response_curve_gives_one_to_one_for_any_game_curve){
    // Whatever power the game applies to the stick, half the mouse speed must turn half as fast.
    for(const double curve:{.5,1.0,1.5,2.0,3.0}){
        for(const int counts:{1,2,4,8,12,16}){             // per 4 ms poll; 16 is a full tilt
            auto config=kbmPlainAim();config.curve=curve;
            MouseAim aim(config);aim.add(counts,0);
            KbmGameModel game;game.curve=curve;game.bits=16;
            game.poll(aim.step(4000),.004);
            CHECK(std::abs(game.x-counts)<.02);
        }
    }
}
TEST(kbm_aim_dead_zone_compensation_starts_exactly_at_the_dead_zone){
    for(const double deadzone:{0.0,.05,.12,.25,.4}){
        auto config=kbmPlainAim();config.deadzone=deadzone;config.fullSpeedCounts=100000; // one count is next to nothing
        MouseAim aim(config);aim.add(1,0);
        const auto out=aim.step(4000);
        CHECK(out.moving);
        CHECK(kbmTilt(out)>=deadzone&&kbmTilt(out)<deadzone+.004);  // just past the range the game ignores
    }
}
TEST(kbm_aim_horizontal_and_vertical_sensitivity_are_separate){
    auto config=kbmPlainAim();config.sensitivityX=2;config.sensitivityY=.5;
    MouseAim aim(config);aim.add(4,4);
    KbmGameModel game;game.bits=16;game.poll(aim.step(4000),.004);
    CHECK(std::abs(game.x-8)<.02&&std::abs(game.y-2)<.02);
    config.invertY=true;config.invertX=true;
    MouseAim inverted(config);inverted.add(4,4);
    KbmGameModel mirror;mirror.bits=16;mirror.poll(inverted.step(4000),.004);
    CHECK(std::abs(mirror.x+8)<.02&&std::abs(mirror.y+2)<.02);
}
TEST(kbm_aim_acceleration_is_off_unless_asked_for){
    const auto turned=[](double accel,double cap,int counts){
        auto config=kbmPlainAim();config.accel=accel;config.accelCap=cap;config.fullSpeedCounts=16000;
        MouseAim aim(config);aim.add(counts,0);
        KbmGameModel game;game.full=16000;game.bits=16;game.poll(aim.step(4000),.004);return game.x;
    };
    // Off: exactly linear. Twice the hand speed, twice the turn.
    CHECK(std::abs(turned(0,2,8)-8)<.02&&std::abs(turned(0,2,16)-16)<.02&&std::abs(turned(0,2,32)-32)<.03);
    // On: the gain grows with speed, 1 + accel x speed / full speed ...
    CHECK(std::abs(turned(1,4,16)-16*1.25)<.03);          // 16 counts / 4 ms = a quarter of full speed
    CHECK(std::abs(turned(1,4,32)-32*1.5)<.05);
    CHECK(std::abs(turned(2,4,16)-16*1.5)<.03);
    // ... up to the cap.
    CHECK(std::abs(turned(4,1.5,32)-32*1.5)<.05);
}
TEST(kbm_aim_fine_modifier_scales_only_while_held){
    auto config=kbmPlainAim();config.fineScale=.25;
    MouseAim aim(config);
    const auto turned=[&](int counts){aim.add(counts,0);KbmGameModel game;game.bits=16;game.poll(aim.step(4000),.004);return game.x;};
    CHECK(std::abs(turned(8)-8)<.02);
    aim.setFine(true);CHECK(std::abs(turned(8)-2)<.02);
    aim.setFine(false);CHECK(std::abs(turned(8)-8)<.02);
}
TEST(kbm_aim_fractions_of_a_count_accumulate){
    // Sensitivity 0.1: ten reports of one count are one count of turn, not ten times nothing.
    auto config=kbmPlainAim();config.sensitivityX=.1;config.sensitivityY=.1;
    MouseAim aim(config);KbmGameModel game;game.bits=16;
    for(int i=0;i<200;++i){aim.add(1,0);game.poll(aim.step(4000),.004);}
    CHECK(std::abs(game.x-20)<.05);
}
TEST(kbm_aim_slow_movement_is_not_lost_to_the_eight_bit_stick){
    // A game reads the stick in 8 bits. Speeds between two stick steps are sent as a mix of the
    // two, with the rounding error carried, so the turn adds up; without the carry the part below
    // a step would be cut off on every poll.
    for(const double deadzone:{0.0,.12,.125,.2}){
        for(const double curve:{1.0,2.0}){
            for(const double perPoll:{.03,.11,.37,1.0,2.6}){   // counts per 4 ms poll, down to 7.5 counts a second
                auto config=kbmEightBit();config.deadzone=deadzone;config.curve=curve;config.sensitivityX=perPoll;config.sensitivityY=perPoll;
                MouseAim aim(config);KbmGameModel game;game.deadzone=deadzone;game.curve=curve;
                double sent=0;
                for(int i=0;i<2500;++i){aim.add(1,0);sent+=perPoll;game.poll(aim.step(4000),.004);}
                for(int i=0;i<50;++i)game.poll(aim.step(4000),.004);
                CHECK(std::abs(game.x-sent)<=std::max(.02*sent,1.0));
                CHECK(game.y==0);
            }
        }
    }
    // The same movement without the carry, cut to 8 bits by the console: a visible share is gone.
    auto config=kbmPlainAim();config.sensitivityX=.37;
    MouseAim legacy(config);KbmGameModel game;double sent=0;
    for(int i=0;i<2500;++i){legacy.add(1,0);sent+=.37;game.poll(legacy.step(4000),.004);}
    CHECK(game.x<.9*sent);
}
TEST(kbm_aim_eight_bit_output_keeps_direction_and_only_uses_whole_steps){
    auto config=kbmEightBit();
    MouseAim aim(config);KbmGameModel game;
    for(int i=0;i<3000;++i){
        aim.add(3,i%3==0?-1:0);                       // a shallow diagonal: 9 across for 1 up
        const auto out=aim.step(4000);
        CHECK(out.x%256==0||std::abs(int(out.x))==32767);
        CHECK(out.y%256==0||std::abs(int(out.y))==32767);
        CHECK(out.x>=0&&out.y<=0);                    // never a step against the movement
        game.poll(out,.004);
    }
    for(int i=0;i<50;++i)game.poll(aim.step(4000),.004);
    CHECK(std::abs(game.x-9000)<90&&std::abs(game.y+1000)<20);
}
TEST(kbm_aim_comes_to_rest_and_stays_there){
    // After the hand stops the stick returns to the centre as soon as the smoothing window is
    // empty and never twitches again: no remainder trickling out as drift, no flip to the other side.
    for(const double deadzone:{0.0,.05,.12,.125,.2,.33}){
        for(const double smoothing:{0.0,8.0,33.0}){
            auto config=kbmEightBit();config.deadzone=deadzone;config.smoothingMs=smoothing;config.sensitivityX=.7;config.sensitivityY=.7;
            MouseAim aim(config);
            for(int i=0;i<37;++i){aim.add(1,i%5==0?1:0);const auto out=aim.step(4000);CHECK(out.x>=0&&out.y>=0);}
            int lastMoving=-1;
            for(int i=0;i<1000;++i){const auto out=aim.step(4000);CHECK(out.x>=0&&out.y>=0);if(out.moving)lastMoving=i;CHECK(out.moving==(out.x!=0||out.y!=0));}
            CHECK(lastMoving<=std::max(1,int(smoothing/4))-2);   // the polls still in the window, and not one more
        }
    }
}
TEST(kbm_aim_flick_is_carried_through_the_eight_bit_stick_too){
    auto config=kbmEightBit();                      // 16 counts per poll at full tilt, 40 ms = 160 counts of carry
    MouseAim aim(config);KbmGameModel game;
    aim.add(-120,90);                               // 150 counts in one report: nine polls' worth
    int polls=0;
    for(;polls<100;++polls){const auto out=aim.step(4000);if(!out.moving&&polls>12)break;game.poll(out,.004);}
    CHECK(std::abs(game.x+120)<1.5&&std::abs(game.y-90)<1.5);
    CHECK(polls<=14);
    // Past the carry the excess is dropped: the view must not keep turning long after the hand stopped.
    MouseAim wild(config);wild.add(1000000,0);int turning=0;
    for(int i=0;i<500;++i)if(wild.step(4000).moving)++turning;
    CHECK(turning>=11&&turning<=13);                // this poll, 40 ms of carry, the last remainder
}
TEST(kbm_aim_smoothing_delay_is_stated_and_true){
    // What the smoothing window costs at a given poll interval: the mean delay of a count and
    // the delay of its last part.
    CHECK(mouseSmoothingDelay(0,4).meanMs==0&&mouseSmoothingDelay(0,4).lastMs==0);
    CHECK(mouseSmoothingDelay(4,4).meanMs==0);
    CHECK(mouseSmoothingDelay(8,4).meanMs==2&&mouseSmoothingDelay(8,4).lastMs==4);
    CHECK(mouseSmoothingDelay(33,4).meanMs==14&&mouseSmoothingDelay(33,4).lastMs==28);
    CHECK(mouseSmoothingDelay(8,1).meanMs==3.5&&mouseSmoothingDelay(8,1).lastMs==7);
    CHECK(mouseSmoothingDelay(8,0).meanMs==0&&mouseSmoothingDelay(std::nan(""),4).meanMs==0);
    // Measured on the filter itself: one count, where does its turn come out?
    for(const double window:{0.0,8.0,16.0,33.0}){
        for(const int pollUs:{1000,2000,4000,8000}){
            auto config=kbmPlainAim();config.smoothingMs=window;config.deadzone=0;
            MouseAim aim(config);aim.add(1,0);
            double weighted=0,total=0,last=0;
            for(int i=0;i<100;++i){
                const double part=kbmTilt(aim.step(std::uint64_t(pollUs)));
                weighted+=part*i*pollUs/1000.0;total+=part;if(part>0)last=i*pollUs/1000.0;
            }
            const auto stated=mouseSmoothingDelay(window,pollUs/1000.0);
            CHECK(total>0&&std::abs(weighted/total-stated.meanMs)<1e-6&&std::abs(last-stated.lastMs)<1e-6);
        }
    }
}
TEST(kbm_aim_uneven_poll_intervals_do_not_change_the_speed){
    // A timer is never exact. A hand moving at a steady 2000 counts a second must give the same
    // tilt whether a poll came after 3.5, 4 or 4.5 ms, with any smoothing window, from the first
    // full window on. (A window that assumed missing history where a poll had merely been late
    // halved the speed now and then.)
    std::mt19937 rng(99);
    for(const double window:{0.0,8.0,12.0,16.0,33.0,50.0}){
        auto config=kbmPlainAim();config.smoothingMs=window;
        MouseAim aim(config);
        const double expected=.12+.88*.5;
        for(int i=0;i<4000;++i){
            const int choice=int(rng()%3);                        // 3.5, 4 or 4.5 ms: 7, 8 or 9 counts
            aim.add(7+choice,0);
            const auto out=aim.step(std::uint64_t(3500+500*choice));
            if(i>=20)CHECK(std::abs(kbmTilt(out)-expected)<.002);
        }
    }
}
TEST(kbm_aim_first_movement_after_a_release_is_not_exaggerated){
    // Straight after a reset the window is empty. One count must still turn one count's worth
    // in total, not more because the window had not filled yet.
    for(const double window:{8.0,33.0}){
        auto config=kbmPlainAim();config.smoothingMs=window;config.deadzone=0;
        MouseAim aim(config);
        for(int round=0;round<3;++round){
            aim.reset();aim.add(4,0);
            KbmGameModel game;game.deadzone=0;game.bits=16;
            for(int i=0;i<40;++i)game.poll(aim.step(4000),.004);
            CHECK(std::abs(game.x-4)<.02);
        }
    }
}
TEST(kbm_aim_config_is_sanitised){
    MouseAimConfig bad;
    bad.sensitivityX=std::nan("");bad.sensitivityY=-3;bad.fullSpeedCounts=0;bad.deadzone=7;bad.curve=0;bad.accel=-1;bad.accelCap=std::numeric_limits<double>::infinity();
    bad.smoothingMs=1e9;bad.carryMs=-5;bad.fineScale=0;bad.stickBits=3;bad.gyroDegPerCount=std::nan("");
    const MouseAim aim(bad);const auto& c=aim.config();
    CHECK(c.sensitivityX==1&&c.sensitivityY==.01&&c.fullSpeedCounts==100&&c.deadzone==.5&&c.curve==.25&&c.accel==0&&c.accelCap==2);
    CHECK(c.smoothingMs==50&&c.carryMs==0&&c.fineScale==.05&&c.stickBits==8&&c.gyroDegPerCount==.03);
    bad.stickBits=40;CHECK(MouseAim(bad).config().stickBits==16);
    // Nothing a broken configuration or a wild mouse can do produces a value that is not a number.
    MouseAim wild(bad);
    for(int i=0;i<2000;++i){wild.add(i%7?2147483647:-2147483647,i%3?-2147483647:1);const auto out=wild.step(i%5?4000u:0u);CHECK(std::isfinite(out.yawRate)&&std::isfinite(out.pitchRate));}
}
TEST(kbm_pad_fine_key_slows_the_mouse){
    KeyboardPadBindings bindings;CHECK(bindings.parse("X=Fine")==0);
    KeyboardMousePad pad(kbmPlainAim(),bindings);std::uint64_t t=1000;
    pad.mouseMove(8,0);const auto normal=pad.apply(kbmPhysical(),t+=4000);
    pad.key('X',true);pad.apply(kbmPhysical(),t+=4000);          // the key is seen by the poll
    pad.mouseMove(8,0);const auto fine=pad.apply(kbmPhysical(),t+=4000);
    pad.key('X',false);pad.apply(kbmPhysical(),t+=4000);
    pad.mouseMove(8,0);const auto again=pad.apply(kbmPhysical(),t+=4000);
    CHECK(fine.rightX>0&&fine.rightX<normal.rightX&&again.rightX==normal.rightX);
    CHECK(fine.buttons==0);                                       // the modifier is not a pad button
}

// --- Profiles: walking, aiming, driving ---------------------------------------------------------------
namespace {
// Two profiles that differ in everything a switch must change, with ramps that are easy to check.
KbmProfileSet kbmTestProfiles(){
    KbmProfileSet set;
    MouseAimConfig aim=kbmPlainAim();
    for(auto& profile:set.profiles){profile.aim=aim;profile.bindings.clear();}
    auto& walk=set[KbmProfileId::Walk];auto& aiming=set[KbmProfileId::Aim];auto& drive=set[KbmProfileId::Drive];
    for(auto* on:{&walk,&aiming}){
        on->bindings.parse("W=LUp;S=LDown;A=LLeft;D=LRight;Mouse1=R2;Mouse3=L2;Space=Square;Caps=Drive");
    }
    aiming.aim.sensitivityX=.5;aiming.aim.sensitivityY=.5;
    drive.bindings.parse("W=R2;S=L2;A=LLeft;D=LRight;Space=R1;Caps=Drive");
    drive.triggerRiseMs=100;drive.triggerFallMs=50;drive.triggerStart=.2;drive.triggerStep=15;
    drive.moveRiseMs=100;drive.moveFallMs=50;drive.moveDeadzone=.1;
    set.aimWhile=PadAction::L2;
    return set;
}
struct KbmRig {
    KeyboardMousePad pad;std::uint64_t t=1000;ControllerState physical;
    explicit KbmRig(KbmProfileSet set=kbmTestProfiles()):pad(std::move(set)){}
    ControllerState poll(){t+=4000;return pad.apply(physical,t);}
    void drive(){pad.key(0x14,true);poll();pad.key(0x14,false);}
};
}
TEST(kbm_profiles_aiming_applies_while_the_aim_button_is_held){
    KbmRig rig;
    rig.pad.key('W',true);
    rig.pad.mouseMove(8,0);const auto hip=rig.poll();
    CHECK(rig.pad.profile()==KbmProfileId::Walk&&hip.leftY==-32767);
    rig.pad.mouseButton(3,true);rig.poll();                       // right mouse button = L2 = aim
    CHECK(rig.pad.profile()==KbmProfileId::Aim);
    rig.pad.mouseMove(8,0);const auto aimed=rig.poll();
    CHECK(aimed.l2==255&&aimed.leftY==-32767);                    // still aiming, still walking: nothing was released
    CHECK(aimed.rightX>0&&aimed.rightX<hip.rightX);               // the aiming profile's lower sensitivity
    rig.pad.mouseButton(3,false);rig.poll();
    CHECK(rig.pad.profile()==KbmProfileId::Walk);
    rig.pad.mouseMove(8,0);CHECK(rig.poll().rightX==hip.rightX);
    // Aiming with the physical controller's trigger selects it for the mouse as well.
    rig.physical.l2=200;rig.poll();CHECK(rig.pad.profile()==KbmProfileId::Aim);
    rig.physical.l2=20;rig.poll();CHECK(rig.pad.profile()==KbmProfileId::Walk);   // a resting finger is not aiming
}
TEST(kbm_profiles_one_profile_never_switches){
    KeyboardPadBindings bindings;bindings.parse("Caps=Cross");
    KeyboardMousePad pad(kbmPlainAim(),bindings);
    pad.mouseButton(3,true);pad.key(0x14,true);
    const auto out=pad.apply(kbmPhysical(),5000);
    CHECK(pad.profile()==KbmProfileId::Walk&&out.l2==255&&(out.buttons&ControllerState::Cross));
}
TEST(kbm_profiles_drive_key_switches_and_releases_every_key){
    KbmRig rig;
    rig.pad.key('W',true);rig.pad.mouseButton(1,true);
    auto out=rig.poll();CHECK(out.leftY==-32767&&out.r2==255);
    rig.pad.key(0x14,true);out=rig.poll();                        // Caps: into the car
    CHECK(rig.pad.profile()==KbmProfileId::Drive);
    ControllerState rest;rest.inputActive=true;
    CHECK(out==rest);                                             // W would now be the throttle: it is released instead
    rig.pad.key('W',true);rig.pad.key(0x14,true);                 // auto-repeat of keys held through the switch
    out=rig.poll();
    CHECK(out==rest&&rig.pad.profile()==KbmProfileId::Drive);     // neither accelerates nor switches back
    rig.pad.key('W',false);rig.pad.key('W',true);out=rig.poll();  // released and pressed again: now it is the throttle
    CHECK(out.r2>0&&out.leftY==0);
    rig.pad.key(0x14,false);rig.pad.mouseButton(1,false);
    rig.pad.key(0x14,true);out=rig.poll();                        // Caps again: out of the car
    CHECK(rig.pad.profile()==KbmProfileId::Walk&&out==rest);
    rig.pad.key(0x14,false);
    // The end of the session goes back on foot whatever was active.
    rig.drive();CHECK(rig.pad.profile()==KbmProfileId::Drive);
    rig.pad.key('W',true);rig.poll();
    rig.pad.restart();
    CHECK(rig.pad.profile()==KbmProfileId::Walk&&rig.poll()==rest);
    // A release (focus loss) keeps the profile: the car is still there when the window comes back.
    rig.drive();rig.pad.key('W',true);rig.poll();rig.pad.releaseAll();
    CHECK(rig.pad.profile()==KbmProfileId::Drive&&rig.poll()==rest);
}
TEST(kbm_profiles_throttle_and_brake_ramp_the_triggers){
    KbmRig rig;rig.drive();
    rig.pad.key('W',true);
    std::vector<int> rise;for(int i=0;i<30;++i)rise.push_back(rig.poll().r2);
    CHECK(rise[0]==45);                                           // starts at 20 % (51 of 255), on the nearest step of 15
    for(std::size_t i=1;i<rise.size();++i)CHECK(rise[i]>=rise[i-1]);
    for(const int value:rise)CHECK(value%15==0);                  // few distinct values: each one is a packet to the console
    CHECK(rise[19]<255&&rise[20]==255);                           // 20 % + 80 ms at 1 % per ms
    CHECK(rig.poll().l2==0);
    rig.pad.key('W',false);
    std::vector<int> fall;for(int i=0;i<20;++i)fall.push_back(rig.poll().r2);
    for(std::size_t i=1;i<fall.size();++i)CHECK(fall[i]<=fall[i-1]);
    CHECK(fall[0]<255&&fall[11]>0&&fall[12]==0);                  // 50 ms down
    // The brake is its own ramp, and a deeper physical trigger wins.
    rig.pad.key('S',true);rig.physical.l2=200;rig.physical.r2=90;
    auto out=rig.poll();CHECK(out.l2==200&&out.r2==90);
    for(int i=0;i<30;++i){out=rig.poll();}
    CHECK(out.l2==255&&out.r2==90);
    // A release does not ramp down: the trigger is up at once.
    rig.physical=ControllerState{};rig.pad.releaseAll();CHECK(rig.poll().l2==0);
    // On foot the same buttons are instant (no ramp configured).
    KbmRig foot;foot.pad.mouseButton(1,true);CHECK(foot.poll().r2==255);
    foot.pad.mouseButton(1,false);CHECK(foot.poll().r2==0);
}
TEST(kbm_profiles_keyboard_steering_ramps_from_the_dead_zone){
    KbmRig rig;rig.drive();
    rig.pad.key('D',true);
    std::vector<int> right;for(int i=0;i<30;++i)right.push_back(rig.poll().leftX);
    CHECK(std::abs(right[0]-int(std::lround((.1+.9*.04)*32767)))<=1); // first poll: already past the game's dead zone
    for(std::size_t i=1;i<right.size();++i)CHECK(right[i]>=right[i-1]);
    CHECK(right[23]<32767&&right[24]==32767);                     // full lock after 100 ms
    rig.pad.key('D',false);rig.pad.key('A',true);                 // straight to the other lock: through the centre, no jump
    int previous=32767;bool crossed=false;
    for(int i=0;i<60;++i){const int x=rig.poll().leftX;CHECK(x<=previous);if(previous>0&&x<=0)crossed=true;previous=x;}
    CHECK(crossed&&previous==-32767);
    rig.pad.key('A',false);
    std::vector<int> back;for(int i=0;i<20;++i)back.push_back(rig.poll().leftX);
    CHECK(back[0]>-32767&&back[11]<0&&back[12]==0);               // 50 ms to the centre
    // Walking has no ramp: a key is a full tilt at once.
    KbmRig foot;foot.pad.key('D',true);CHECK(foot.poll().leftX==32767);
}
TEST(kbm_profiles_mouse_steering_holds_a_wheel_position){
    auto set=kbmTestProfiles();auto& drive=set[KbmProfileId::Drive];
    drive.mouse=KbmProfile::Mouse::Steer;drive.steerRangeCounts=500;drive.moveDeadzone=.1;
    KbmRig rig(set);rig.drive();rig.physical.rightX=4000;
    rig.pad.mouseMove(250,-999);auto out=rig.poll();
    CHECK(std::abs(out.leftX-int(std::lround((.1+.9*.5)*32767)))<=1&&out.leftY==0); // half the range: half the lock, above the dead zone
    CHECK(out.rightX==4000);                                      // the camera stick is the controller's
    for(int i=0;i<200;++i)CHECK(rig.poll().leftX==out.leftX);     // a position, not a speed: it stays
    rig.pad.mouseMove(-250,0);CHECK(rig.poll().leftX==0);
    rig.pad.mouseMove(100000,0);CHECK(rig.poll().leftX==32767);   // the lock is the end
    rig.pad.mouseMove(-250,0);out=rig.poll();
    CHECK(std::abs(out.leftX-int(std::lround((.1+.9*.5)*32767)))<=1); // and no further: back from the lock at once
    // Keys and the wheel add up, within the lock.
    rig.pad.key('A',true);
    for(int i=0;i<40;++i){out=rig.poll();}
    CHECK(std::abs(out.leftX+int(std::lround((.1+.9*.5)*32767)))<=1);
    rig.pad.key('A',false);
    // With a return time the wheel centres itself while the mouse rests.
    drive.steerReturnMs=200;
    KbmRig spring(set);spring.drive();
    spring.pad.mouseMove(500,0);CHECK(spring.poll().leftX==32767);
    int last=32767;for(int i=0;i<49;++i){const int x=spring.poll().leftX;CHECK(x<last);last=x;}
    CHECK(last>0&&spring.poll().leftX==0);                        // 200 ms from the lock
    // The switch back on foot drops the wheel position.
    rig.pad.mouseMove(300,0);rig.poll();rig.drive();
    CHECK(rig.pad.profile()==KbmProfileId::Walk&&rig.poll().leftX==0);
}

// --- Profile files ---------------------------------------------------------------------------------
TEST(kbm_profile_builtin_is_a_safe_starting_point){
    const auto set=kbmBuiltinProfiles();
    CHECK(set.name.find("untuned")!=std::string::npos);           // nobody mistakes it for a tuned profile
    const auto& walk=set[KbmProfileId::Walk];const auto& aiming=set[KbmProfileId::Aim];const auto& drive=set[KbmProfileId::Drive];
    CHECK(set.aimWhile==PadAction::L2);
    CHECK(walk.bindings.action('W')==PadAction::LUp&&walk.bindings.action(kMouseButtonBase+3)==PadAction::L2&&walk.bindings.action(kMouseButtonBase+1)==PadAction::R2);
    CHECK(walk.bindings.size()==31&&drive.bindings.size()==28);   // every name in the built-in layout was understood
    CHECK(walk.bindings==aiming.bindings);                        // aiming is walking with other numbers
    CHECK(aiming.aim.sensitivityX<walk.aim.sensitivityX);
    CHECK(drive.bindings.action('W')==PadAction::R2&&drive.bindings.action('S')==PadAction::L2);
    CHECK(drive.bindings.action('A')==PadAction::LLeft&&drive.bindings.action('D')==PadAction::LRight);
    CHECK(drive.triggerRiseMs>0&&drive.moveRiseMs>0&&walk.triggerRiseMs==0&&walk.moveRiseMs==0);
    for(const auto& profile:set.profiles){
        CHECK(profile.bindings.action(0x14)==PadAction::Drive);   // Caps Lock changes between foot and car in every profile
        CHECK(profile.aim.stickBits==8&&profile.aim.accel==0&&profile.aim.mode==MouseAimConfig::Mode::Stick);
        CHECK(profile.mouse==KbmProfile::Mouse::Camera);
        CHECK(MouseAim(profile.aim).config()==profile.aim);       // every number is already inside its limits
    }
    // No text at all, or only comments: the built-in set, untouched and without complaints.
    for(const char* text:{"","\n\n","# nothing\r\n   \n; also a comment\n","\xEF\xBB\xBF"}){
        const auto parsed=parseKbmProfiles(text);
        CHECK(parsed.ok()&&parsed.applied==0&&parsed.set==set);
    }
}
TEST(kbm_profile_builtin_default_adds_little_delay){
    // Input delay comes first: the built-in window is 8 ms (33 ms is the opt-in smoother camera),
    // so at a 4 ms poll the stick is centred within two polls plus 8 ms after the mouse stops.
    const auto set=kbmBuiltinProfiles();
    for(const auto& profile:set.profiles){
        CHECK(profile.aim.smoothingMs==8);
        CHECK(mouseSmoothingDelay(profile.aim.smoothingMs,4).meanMs<=2&&mouseSmoothingDelay(profile.aim.smoothingMs,4).lastMs<=4);
        for(const int counts:{1,4,8,12}){                       // slow to about full speed, no carry
            MouseAim aim(profile.aim);
            for(int i=0;i<200;++i){aim.add(counts,0);(void)aim.step(4000);}
            int polls=0;double elapsedMs=0;
            for(;polls<100;++polls){
                const auto out=aim.step(4000);elapsedMs+=4;
                if(out.x==0&&out.y==0)break;
            }
            CHECK(polls<100&&elapsedMs<=2*4+8);
        }
    }
}
TEST(kbm_profile_file_sets_every_value_for_its_profile){
    const char* text=
        "\xEF\xBB\xBF# Slipstream keyboard and mouse profile\r\n"
        "name = My GTA 6 setup\r\n"
        "mode=stick\n"
        "stick_bits=10\n"
        "aim_while=R1\n"
        "all.full = 5200\n"
        "walk.sens_x=1.5\n"
        "walk.sens_y=1.25\n"
        "walk.deadzone=0.2\n"
        "walk.curve=1.8\n"
        "walk.accel=0.5\n"
        "walk.accel_cap=3\n"
        "walk.smooth_ms=16\n"
        "walk.carry_ms=60\n"
        "walk.fine=0.3\n"
        "walk.gyro_deg=0.05\n"
        "walk.invert_y=1\n"
        "AIM.SENS_X=0.4\n"
        "aim.invert_x=1\n"
        "aim.walk_tilt=0.6\n"
        "drive.mouse=steer\n"
        "drive.move_deadzone=0.15\n"
        "drive.move_rise_ms=200\n"
        "drive.move_fall_ms=90\n"
        "drive.trigger_rise_ms=250\n"
        "drive.trigger_fall_ms=120\n"
        "drive.trigger_start=0.3\n"
        "drive.trigger_step=5\n"
        "drive.steer_range=900\n"
        "drive.steer_return_ms=400\n"
        "drive.steer_curve=1.5\n"
        "walk.bind.G=Triangle\n"
        "aim.bind.Mouse2 = Fine\n"
        "drive.bind.Space=None\n"
        "all.bind.F9=PS\n";
    const auto parsed=parseKbmProfiles(text);
    CHECK(parsed.problemCount==0&&parsed.problems.empty()&&!parsed.truncated&&parsed.ok());
    CHECK(parsed.applied==34);
    const auto& set=parsed.set;const auto builtin=kbmBuiltinProfiles();
    const auto& walk=set[KbmProfileId::Walk];const auto& aiming=set[KbmProfileId::Aim];const auto& drive=set[KbmProfileId::Drive];
    CHECK(set.name=="My GTA 6 setup"&&set.aimWhile==PadAction::R1);
    for(const auto& profile:set.profiles)CHECK(profile.aim.stickBits==10&&profile.aim.fullSpeedCounts==5200&&profile.bindings.action(0x78)==PadAction::PS);
    CHECK(walk.aim.sensitivityX==1.5&&walk.aim.sensitivityY==1.25&&walk.aim.deadzone==.2&&walk.aim.curve==1.8&&walk.aim.accel==.5&&walk.aim.accelCap==3);
    CHECK(walk.aim.smoothingMs==16&&walk.aim.carryMs==60&&walk.aim.fineScale==.3&&walk.aim.gyroDegPerCount==.05&&walk.aim.invertY&&!walk.aim.invertX);
    CHECK(aiming.aim.sensitivityX==.4&&aiming.aim.invertX&&!aiming.aim.invertY&&aiming.walkTilt==.6);
    CHECK(aiming.aim.deadzone==builtin[KbmProfileId::Aim].aim.deadzone);                   // numbers are per profile
    CHECK(drive.mouse==KbmProfile::Mouse::Steer&&drive.moveDeadzone==.15&&drive.moveRiseMs==200&&drive.moveFallMs==90);
    CHECK(drive.triggerRiseMs==250&&drive.triggerFallMs==120&&drive.triggerStart==.3&&drive.triggerStep==5);
    CHECK(drive.steerRangeCounts==900&&drive.steerReturnMs==400&&drive.steerCurve==1.5);
    // Bindings: aiming follows walking except where it has a line of its own.
    CHECK(walk.bindings.action('G')==PadAction::Triangle&&aiming.bindings.action('G')==PadAction::Triangle&&drive.bindings.action('G')==PadAction::None);
    CHECK(aiming.bindings.action(kMouseButtonBase+2)==PadAction::Fine&&walk.bindings.action(kMouseButtonBase+2)==builtin[KbmProfileId::Walk].bindings.action(kMouseButtonBase+2));
    CHECK(drive.bindings.action(0x20)==PadAction::None&&walk.bindings.action(0x20)!=PadAction::None);
    // A later walking line does not undo aiming's own key, and does reach aiming otherwise.
    const auto later=parseKbmProfiles("aim.bind.Q=Circle\nwalk.bind.Q=Square\nwalk.bind.E=Triangle\nmode=gyro\naim_while=none\n");
    CHECK(later.ok()&&later.set[KbmProfileId::Aim].bindings.action('Q')==PadAction::Circle&&later.set[KbmProfileId::Walk].bindings.action('Q')==PadAction::Square);
    CHECK(later.set[KbmProfileId::Aim].bindings.action('E')==PadAction::Triangle&&later.set.aimWhile==PadAction::None);
    for(const auto& profile:later.set.profiles)CHECK(profile.aim.mode==MouseAimConfig::Mode::Gyro);
    // The last line for a setting counts.
    CHECK(parseKbmProfiles("walk.curve=2\nwalk.curve=3\n").set[KbmProfileId::Walk].aim.curve==3);
}
TEST(kbm_profile_malformed_lines_are_reported_and_change_nothing){
    const auto builtin=kbmBuiltinProfiles();
    const char* bad[]={
        "walk.sens_x","=1","walk.sens_x=","walk.sens_x=abc","walk.sens_x=1,5","walk.sens_x=1.5 # fast","walk.sens_x=+1","walk.sens_x=0x10",
        "walk.sens_x=nan","walk.sens_x=inf","walk.sens_x=1e999","walk.sens_x=-1","walk.sens_x=0","walk.sens_x=1000",
        "walk.deadzone=0.9","walk.curve=0","walk.smooth_ms=500","walk.invert_x=2","walk.invert_x=yes","drive.trigger_step=2.5","drive.trigger_step=0",
        "stick_bits=7","stick_bits=8.5","stick_bits=17","mode=wheel","aim_while=LUp","aim_while=Fine","aim_while=nothing",
        "sens_x=1","run.sens_x=1","walk.speed=1","walk.=1","walk.bind.=Cross","walk.bind.NoKey=Cross","walk.bind.W=Jump","walk.bind.W=",
        "walk.bind.W.X=Cross","drive.mouse=wheel","name=","walk.sens_x=1\x01","wal\tk.sens_x=1","w\xC3\xA4lk.sens_x=1","[walk]","walk.sens_x 1",
        "name=an unreasonably long profile name that goes on and on and on and on and on and on and on"};
    for(const char* line:bad){
        const auto parsed=parseKbmProfiles(std::string("walk.curve=2\n")+line+"\nwalk.full=3000\n");
        CHECK(parsed.problemCount==1&&parsed.problems.size()==1&&!parsed.ok());
        CHECK(parsed.problems[0].line==2&&!parsed.problems[0].what.empty());
        CHECK(parsed.applied==2);                                 // the good lines around it still count
        auto expected=builtin;expected[KbmProfileId::Walk].aim.curve=2;expected[KbmProfileId::Walk].aim.fullSpeedCounts=3000;
        CHECK(parsed.set==expected);                              // and the bad one changed nothing
    }
    // An embedded zero byte is a control character like any other.
    const char withZero[]="walk.curve=2\nwalk.sens_x=1\0garbage\nwalk.full=3000\n";
    const auto zero=parseKbmProfiles(std::string(withZero,sizeof(withZero)-1));
    CHECK(zero.problemCount==1&&zero.problems[0].line==2&&zero.applied==2);
    // A line that is too long is skipped whole, whatever it contains.
    const auto wide=parseKbmProfiles("walk.sens_x=2"+std::string(600,' ')+"\nwalk.curve=2\n");
    CHECK(wide.problemCount==1&&wide.problems[0].line==1&&wide.set[KbmProfileId::Walk].aim.sensitivityX==builtin[KbmProfileId::Walk].aim.sensitivityX&&wide.set[KbmProfileId::Walk].aim.curve==2);
    // Windows line ends, blank lines, spaces and tabs around both sides are fine.
    const auto tidy=parseKbmProfiles("\r\n \t walk.curve \t=\t 2 \t\r\n\r\n\twalk.bind. g = cross \r\n");
    CHECK(tidy.ok()&&tidy.applied==2&&tidy.set[KbmProfileId::Walk].aim.curve==2&&tidy.set[KbmProfileId::Walk].bindings.action('G')==PadAction::Cross);
    // No final line end.
    CHECK(parseKbmProfiles("walk.curve=2").set[KbmProfileId::Walk].aim.curve==2);
}
TEST(kbm_profile_limits_hold_for_any_input){
    // Many problems: all counted, the first few kept.
    std::string many;for(int i=0;i<500;++i)many+="nonsense\n";
    const auto noisy=parseKbmProfiles(many);
    CHECK(noisy.problemCount==500&&noisy.problems.size()==kKbmProfileMaxProblems&&noisy.problems.back().line==kKbmProfileMaxProblems);
    // More text than a profile can need: the part past the limit is not read, and that is said.
    std::string big;while(big.size()<=kKbmProfileMaxBytes)big+="walk.curve=2\n";
    big+="walk.curve=3\n";
    const auto cut=parseKbmProfiles(big);
    CHECK(cut.truncated&&!cut.ok()&&cut.set[KbmProfileId::Walk].aim.curve==2);
    // A file cut in the middle of a line never applies the half line.
    std::string edge;const std::size_t target=kKbmProfileMaxBytes-13;         // the limit falls inside "3.99": "walk.curve=3." is left
    while(edge.size()+401<=target)edge+="#"+std::string(399,'x')+"\n";
    if(target-edge.size()>=2)edge+="#"+std::string(target-edge.size()-2,'x')+"\n";
    CHECK(edge.size()==target);
    edge+="walk.curve=3.99\n";
    const auto half=parseKbmProfiles(edge);
    CHECK(half.truncated&&half.problemCount==0&&half.set==kbmBuiltinProfiles());
    // More lines than a profile can need.
    const auto tall=parseKbmProfiles(std::string(kKbmProfileMaxLines,'\n')+"walk.curve=3\n");
    CHECK(tall.truncated&&tall.set==kbmBuiltinProfiles());
    CHECK(!parseKbmProfiles(std::string(kKbmProfileMaxLines-1,'\n')+"walk.curve=3\n").truncated);
    // Random bytes, and random damage to a good file, never crash and never leave a number outside its limits.
    const std::string good="walk.sens_x=1.5\nwalk.deadzone=0.2\ndrive.trigger_step=5\naim.bind.Mouse2=Fine\nstick_bits=10\naim_while=R1\nname=x\n";
    std::mt19937 rng(20261118);
    for(int round=0;round<3000;++round){
        std::string text;
        if(round%2){text=good;for(int hits=0;hits<1+int(rng()%4);++hits)text[rng()%text.size()]=char(rng());}
        else{text.resize(rng()%300);for(char& c:text)c=char(rng());}
        const auto parsed=parseKbmProfiles(text);
        CHECK(parsed.problems.size()<=kKbmProfileMaxProblems&&parsed.problemCount>=parsed.problems.size());
        for(const auto& profile:parsed.set.profiles){
            CHECK(MouseAim(profile.aim).config()==profile.aim);
            CHECK(profile.triggerStep>=1&&profile.triggerStep<=64&&profile.steerRangeCounts>=10&&std::isfinite(profile.moveRiseMs));
        }
        KeyboardMousePad pad(parsed.set);pad.key('W',true);pad.mouseMove(5,5);
        const auto out=pad.apply(ControllerState{},5000);CHECK(out.inputActive);
    }
}
TEST(kbm_profile_example_from_the_guide_reads_clean){
    // The example in docs/KEYBOARD_MOUSE.md, line for line.
    const char* text=
        "# Slipstream keyboard and mouse profile for one game.\n"
        "name=GTA 6 first try\n"
        "\n"
        "# On foot\n"
        "walk.sens_x=1.0\n"
        "walk.sens_y=0.8\n"
        "walk.full=4000\n"
        "walk.deadzone=0.12\n"
        "walk.curve=1.0\n"
        "walk.smooth_ms=33\n"
        "\n"
        "# While the aim button (right mouse) is held\n"
        "aim.sens_x=0.6\n"
        "aim.sens_y=0.5\n"
        "aim.deadzone=0.12\n"
        "aim.fine=0.4\n"
        "\n"
        "# In a vehicle (Caps Lock switches)\n"
        "drive.trigger_rise_ms=150\n"
        "drive.trigger_fall_ms=80\n"
        "drive.move_rise_ms=120\n"
        "drive.mouse=camera\n"
        "\n"
        "# Keys\n"
        "walk.bind.G=DLeft\n"
        "aim.bind.Mouse2=Fine\n"
        "drive.bind.Space=R1\n";
    const auto parsed=parseKbmProfiles(text);
    CHECK(parsed.ok()&&parsed.applied==18);
    CHECK(parsed.set.name=="GTA 6 first try"&&parsed.set[KbmProfileId::Walk].aim.sensitivityY==.8&&parsed.set[KbmProfileId::Aim].aim.fineScale==.4);
}
