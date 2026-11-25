// SPDX-License-Identifier: GPL-3.0-only
// Included by CoreTests.cpp: regression matrix for stuck keys, buttons and mouse input.
// The Windows glue (veyra/ui/RemotePlayKeyboardMouse.h) is modelled here by StuckGlue, which does
// what that class does with the portable parts: KbmCaptureState decides, a release calls
// KbmCalibration::stop, KbmCalibrationKeys::reset, KeyboardMousePad::releaseAll and
// KbmDeviceSet::clear; SessionEnded also restarts the pad; keys reach the pad only while captured,
// through KbmCalibrationKeys first; the calibration's aim hold and deflection are laid over the poll.
// Not modelled (Windows only): moonlight::InputRouter's message translation and its modifier
// tracking, Raw Input registration, the cursor clip. A model, so "unverified on Windows".
#include "veyra/remoteplay/KbmCapture.h"
#include "veyra/remoteplay/KbmCalibration.h"
#include "veyra/remoteplay/KbmProfile.h"
#include <random>
namespace stuck_tests {
using Event=KbmCaptureState::Event;
// Walking, aiming while L2 is held, driving on G (mouse as a wheel that centres itself), with
// ramps on the triggers and the stick so that "released" has to wait for them.
KbmProfileSet profiles(){
    KbmProfileSet set;
    for(auto& p:set.profiles){p.bindings.bind('G',PadAction::Drive);p.triggerRiseMs=80;p.triggerFallMs=60;p.moveRiseMs=40;p.moveFallMs=40;}
    set.aimWhile=PadAction::L2;
    auto& drive=set[KbmProfileId::Drive];
    drive.mouse=KbmProfile::Mouse::Steer;drive.steerReturnMs=400;
    drive.bindings.bind('W',PadAction::R2);drive.bindings.bind('S',PadAction::L2);
    set[KbmProfileId::Walk].bindings.bind('F',PadAction::Square);set[KbmProfileId::Walk].bindings.bind('R',PadAction::Square);
    return set;
}
bool neutral(const ControllerState& s){return s.buttons==0&&s.l2==0&&s.r2==0&&s.leftX==0&&s.leftY==0&&s.rightX==0&&s.rightY==0;}
struct StuckGlue {
    KeyboardMousePad pad{profiles()};
    KbmCaptureState state;
    KbmCalibration calibration;
    KbmCalibrationKeys calKeys;
    KbmDeviceSet devices;
    std::uint64_t now=1000;
    unsigned releases=0;
    void handle(Event e){
        const auto a=state.on(e);
        if(a.releaseInputs){
            if(calibration.active())calibration.stop("released");
            calKeys.reset();pad.releaseAll();devices.clear();++releases;
        }
        if(a.restart)pad.restart();
    }
    bool calibrationInput(KbmCalibrationKeys::Action action){
        using A=KbmCalibrationKeys::Action;
        switch(action){
        case A::Pass:return false;
        case A::Consume:return true;
        case A::Toggle:if(calibration.active())calibration.stop("chord");else{pad.releaseAll();(void)calibration.start(pad.profiles());}break;
        case A::Tap:calibration.tap(now);break;
        case A::Skip:calibration.skip(now);break;
        case A::Exit:calibration.stop("Esc");break;
        }
        return true;
    }
    // Window messages: they reach the glue only while captured (nativeEventFilter's first test).
    void key(std::uint32_t vk,bool down,std::uintptr_t device=1){
        if(!state.captured())return;
        devices.note(device);
        if(!calibrationInput(calKeys.key(vk,down,calibration.active())))pad.key(vk,down);
    }
    void button(int b,bool down,std::uintptr_t device=2){
        if(!state.captured())return;
        devices.note(device);
        if(!calibrationInput(calKeys.mouseButton(b,down,calibration.active())))pad.mouseButton(b,down);
    }
    void move(int dx,int dy){if(state.captured()&&!calibration.running())pad.mouseMove(dx,dy);}
    void wheel(int delta){if(state.captured())pad.scroll(delta);}
    void removed(std::uintptr_t device){if(state.captured()&&devices.remove(device))handle(Event::DeviceRemoved);}
    // One pad poll (4 ms later): the physical controller at rest plus keyboard and mouse.
    ControllerState poll(bool front=true){
        now+=4000;
        handle(front?Event::PollFront:Event::PollAway);
        ControllerState physical;physical.inputActive=true;
        if(!state.captured())return physical;
        auto out=pad.apply(physical,now);
        if(calibration.active()){
            const auto step=calibration.poll(now);
            if(step.deflect){out.rightX=calibration.stickValue(step.x);out.rightY=calibration.stickValue(step.y);}
            if(step.holdAim)out.l2=255;   // aimWhile is L2 here
        }
        return out;
    }
    ControllerState settle(int polls=300){ControllerState out;for(int i=0;i<polls;++i)out=poll();return out;}
};
}

TEST(stuck_input_focus_lost_mid_press_releases_and_the_late_key_up_is_harmless){
    stuck_tests::StuckGlue g;g.poll();CHECK(g.state.captured());
    g.key('W',true);g.button(1,true);g.key(0x20,true);
    auto out=g.poll();CHECK(out.leftY<0&&out.r2>0&&(out.buttons&ControllerState::Cross));
    g.handle(KbmCaptureState::Event::FocusLost);         // the key-ups now go to another window
    CHECK(!g.state.captured());
    out=g.poll(false);CHECK(stuck_tests::neutral(out));
    out=g.poll(true);CHECK(g.state.captured());CHECK(stuck_tests::neutral(out));   // captured again: nothing held
    g.key('W',false);g.button(1,false);g.key(0x20,false);                         // late key-ups
    CHECK(stuck_tests::neutral(g.settle(50)));
    g.key('W',true);CHECK(g.poll().leftY<0);g.key('W',false);CHECK(stuck_tests::neutral(g.settle(50)));
}

TEST(stuck_input_overlay_toggle_and_release_hotkey){
    stuck_tests::StuckGlue g;g.poll();
    g.key('D',true);g.button(3,true);CHECK(g.poll().leftX>0);
    // An overlay or another window comes to the front: the poll sees the window away.
    CHECK(stuck_tests::neutral(g.poll(false)));CHECK(!g.state.captured());
    CHECK(stuck_tests::neutral(g.poll(true)));
    // The release hotkey: suspended, nothing held, not captured again until the window leaves and comes back.
    g.key('A',true);CHECK(g.poll().leftX<0);
    g.handle(KbmCaptureState::Event::ReleaseHotkey);
    CHECK(g.state.state()==KbmCaptureState::State::Suspended);
    CHECK(stuck_tests::neutral(g.poll(true)));CHECK(!g.state.captured());
    g.poll(false);CHECK(stuck_tests::neutral(g.poll(true)));CHECK(g.state.captured());
}

TEST(stuck_input_profile_switch_with_keys_held){
    stuck_tests::StuckGlue g;g.poll();
    g.key('W',true);CHECK(g.poll().leftY<0);
    g.key('G',true);g.key('G',false);                     // to the car: W means the throttle there
    auto out=g.settle(30);
    CHECK(g.pad.driving());CHECK(out.leftY==0&&out.r2==0);   // the held W was let go and stays ignored
    g.key('W',false);g.key('W',true);CHECK(g.settle(30).r2==255);   // pressed again: the throttle
    g.key('W',false);CHECK(stuck_tests::neutral(g.settle(40)));
    // Steering with the mouse returns to the centre by itself.
    g.move(300,0);CHECK(g.poll().leftX!=0);CHECK(stuck_tests::neutral(g.settle(150)));
    g.key('G',true);g.key('G',false);CHECK(stuck_tests::neutral(g.settle(10)));CHECK(!g.pad.driving());
}

TEST(stuck_input_device_removal_and_session_reconnect){
    stuck_tests::StuckGlue g;g.poll();
    g.key('S',true,11);g.button(1,true,12);CHECK(g.poll().leftY>0);
    g.removed(99);CHECK(g.poll().leftY>0);                // a device that never typed: the game goes on
    g.removed(11);CHECK(stuck_tests::neutral(g.poll()));   // the keyboard that held S: everything released
    g.key('S',false,11);g.button(1,false,12);
    // Session reconnect: the stream stops and a new one starts; on foot, nothing held.
    g.key('G',true);g.key('G',false);g.key('E',true);CHECK(g.settle(5).buttons&ControllerState::R1);
    g.handle(KbmCaptureState::Event::SessionEnded);
    CHECK(!g.pad.driving());CHECK(stuck_tests::neutral(g.poll(true)));
    g.key('E',false);CHECK(stuck_tests::neutral(g.settle(10)));
    // The window changed (fullscreen toggle): the same.
    g.key('Q',true);CHECK(g.poll().buttons&ControllerState::L1);
    g.handle(KbmCaptureState::Event::WindowChanged);CHECK(stuck_tests::neutral(g.poll(true)));
}

TEST(stuck_input_calibration_abort_releases_the_aim_hold){
    stuck_tests::StuckGlue g;g.poll();
    // The chord starts a calibration; its modifiers reached the game on the way and are let go.
    g.key(0x11,true);g.key(0x12,true);g.key(0x10,true);g.key('K',true);
    CHECK(g.calibration.active());
    g.key('K',false);g.key(0x10,false);g.key(0x12,false);g.key(0x11,false);
    g.key(0x20,true);g.key(0x20,false);                   // a tap: the calibration runs
    for(int i=0;i<10;++i)(void)g.poll();
    // Focus lost mid-calibration: it stops without writing, the aim hold and deflection end.
    g.handle(KbmCaptureState::Event::FocusLost);
    CHECK(!g.calibration.active());
    CHECK(stuck_tests::neutral(g.poll(true)));CHECK(stuck_tests::neutral(g.settle(20)));
    // Esc stops one too; the Esc key-up does not reach the game as Options.
    g.key(0x11,true);g.key(0x12,true);g.key(0x10,true);g.key('K',true);g.key('K',false);g.key(0x10,false);g.key(0x12,false);g.key(0x11,false);
    CHECK(g.calibration.active());
    g.key(0x1B,true);CHECK(!g.calibration.active());g.key(0x1B,false);
    CHECK(stuck_tests::neutral(g.settle(20)));
}

TEST(stuck_input_two_keys_on_one_button_released_in_either_order){
    for(int order=0;order<2;++order){
        stuck_tests::StuckGlue g;g.poll();
        // F and R are both Square; C and Ctrl both Circle; mouse 4 and Q both L1.
        g.key('F',true);g.key('R',true);g.key('C',true);g.key(0x11,true);g.button(4,true);g.key('Q',true);
        auto out=g.poll();
        CHECK((out.buttons&ControllerState::Square)&&(out.buttons&ControllerState::Circle)&&(out.buttons&ControllerState::L1));
        if(order==0){g.key('F',false);g.key('C',false);g.button(4,false);}else{g.key('R',false);g.key(0x11,false);g.key('Q',false);}
        out=g.poll();
        CHECK((out.buttons&ControllerState::Square)&&(out.buttons&ControllerState::Circle)&&(out.buttons&ControllerState::L1));   // the other key still holds it
        if(order==0){g.key('R',false);g.key(0x11,false);g.key('Q',false);}else{g.key('F',false);g.key('C',false);g.button(4,false);}
        CHECK(stuck_tests::neutral(g.poll()));
        // Auto-repeat presses of one key do not need as many releases.
        for(int i=0;i<5;++i)g.key('F',true);
        g.key('F',false);CHECK(stuck_tests::neutral(g.poll()));
    }
}

TEST(stuck_input_random_sequences_nothing_held_at_the_end){
    // 10,000 seeded sequences of up to 30 events. After every release (focus, overlay, device,
    // hotkey, session, new profiles) the next poll is the physical pad alone; at the end, with
    // every physical key and button let go (the up messages arriving while captured), nothing is
    // held once the ramps, the wheel pulse, the mouse hold and the steering return are over.
    std::mt19937 rng(20261119);
    const std::uint32_t keys[]={'W','A','S','D','G','F','R','C','Q','E','V',0x20,0x10,0x11,0x12,0x26,0x1B,0x09,'K','Z'};
    std::uint64_t releasesChecked=0;
    for(int sequence=0;sequence<10000;++sequence){
        stuck_tests::StuckGlue g;g.poll();
        std::vector<std::uint32_t> down;std::array<bool,6> buttons{};
        const int events=1+int(rng()%30);
        for(int e=0;e<events;++e){
            const unsigned r=unsigned(rng()%100);
            const unsigned before=g.releases;
            if(r<40){
                const auto k=keys[rng()%std::size(keys)];
                const bool isDown=std::find(down.begin(),down.end(),k)!=down.end();
                const bool press=rng()%4==0?true:!isDown;   // auto-repeat
                g.key(k,press,1+rng()%2);
                if(press&&!isDown)down.push_back(k);
                if(!press)std::erase(down,k);
            }else if(r<55){const int b=1+int(rng()%5);buttons[std::size_t(b)]=!buttons[std::size_t(b)];g.button(b,buttons[std::size_t(b)],3);}
            else if(r<65)g.move(int(rng()%200)-100,int(rng()%100)-50);
            else if(r<68)g.wheel(rng()%2?120:-120);
            else if(r<70)g.handle(KbmCaptureState::Event::FocusLost);
            else if(r<72)(void)g.poll(false);                                  // overlay in front
            else if(r<73)g.removed(1+rng()%4);
            else if(r<74)g.handle(KbmCaptureState::Event::ReleaseHotkey);
            else if(r<75)g.handle(KbmCaptureState::Event::SessionEnded);
            else if(r<76){g.pad.setProfiles(stuck_tests::profiles());++g.releases;}   // a calibration finished
            else (void)g.poll(true);
            if(g.releases!=before){
                // Released: what keyboard and mouse did is gone at once (the physical pad is at rest).
                ControllerState rest;rest.inputActive=true;
                const auto out=g.state.captured()?g.pad.apply(rest,g.now+1):rest;
                if(!stuck_tests::neutral(out))throw std::runtime_error("sequence "+std::to_string(sequence)+": input held after a release");
                ++releasesChecked;
            }
        }
        // Back in front, every key and button let go by its own message.
        (void)g.poll(false);(void)g.poll(true);
        for(const auto k:down)g.key(k,false,1);
        for(int b=1;b<=5;++b)if(buttons[std::size_t(b)])g.button(b,false,3);
        if(g.calibration.active()){g.key(0x1B,true);g.key(0x1B,false);}
        const auto out=g.settle(250);   // 1 s
        if(!stuck_tests::neutral(out))throw std::runtime_error("sequence "+std::to_string(sequence)+": input still held at the end");
        g.pad.releaseAll();
        if(!stuck_tests::neutral(g.poll()))throw std::runtime_error("sequence "+std::to_string(sequence)+": input held after the final release");
    }
    CHECK(releasesChecked>1000);
}
