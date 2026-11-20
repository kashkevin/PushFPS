// SPDX-License-Identifier: GPL-3.0-only
// Included by CoreTests.cpp: the stuck-input and recovery matrix of sprint 5 (third directive, item 5):
// rapid movement, fine aiming, direction changes, 125 Hz to 8 kHz mice, keys and mouse together,
// controller-to-mouse transitions, connection interruption, focus change, an 8-hour session and a
// starved input thread, for every input performance profile. After every interruption all buttons,
// triggers and sticks are neutral in the very next pad state.
// InputGlue models what veyra/ui/RemotePlayKeyboardMouse.h does with the portable parts (capture
// state machine, release, counters, the per-poll hand-off to the statistics). Not modelled: Windows'
// message translation, Raw Input, the Qt timer. A Linux model: "unverified on Windows".
#include "veyra/remoteplay/KbmCalibrationRate.h"
#include "veyra/remoteplay/KbmCapture.h"
#include "veyra/remoteplay/KbmInputProfile.h"
#include "veyra/remoteplay/KbmInputStats.h"
#include "veyra/remoteplay/KbmProfile.h"
#include "veyra/remoteplay/ControllerQueue.h"
#include <random>
#include <vector>
namespace kbm_matrix {
using Event=KbmCaptureState::Event;
inline constexpr KbmInputProfileKind kKinds[]={KbmInputProfileKind::Legacy,KbmInputProfileKind::UltraLowLatency,KbmInputProfileKind::AdaptivePrecision,KbmInputProfileKind::Custom};
inline constexpr int kRates[]={125,500,1000,2000,4000,8000};
inline bool neutral(const ControllerState& s){return s.buttons==0&&s.l2==0&&s.r2==0&&s.leftX==0&&s.leftY==0&&s.rightX==0&&s.rightY==0;}
inline KbmProfileSet profiles(KbmInputProfileKind kind){
    KbmProfileSet set=kbmGtaProfiles();
    set.input=kbmInputPreset(kind);
    if(kind==KbmInputProfileKind::Custom){set.input.smoothingMs=8;for(auto& c:set.input.contexts){c.saturation=KbmSaturation::Carry;c.carryMs=40;c.maxTilt=0.9;}}
    return set;
}
struct InputGlue {
    explicit InputGlue(KbmInputProfileKind kind,int rateHz=1000):pad(profiles(kind)){
        pad.immediateAim().setHoldUs(kbmHoldUsFor(rateHz,4));
        if(rateHz>0)pad.aim().setWindowFloorMs(kbmAccumulationFor(rateHz,4,pad.aim().config().smoothingMs).windowFloorMs);
    }
    KeyboardMousePad pad;
    KbmCaptureState state;
    KbmReleaseCounters releases;
    std::uint64_t now=1000;
    void handle(Event e){
        const auto a=state.on(e);
        if(a.releaseInputs){releases.released(pad.heldCount(),e==Event::SessionEnded);pad.releaseAll();}
        if(a.restart)pad.restart();
    }
    void key(std::uint32_t vk,bool down){if(state.captured())pad.key(vk,down);}
    void button(int b,bool down){if(state.captured())pad.mouseButton(b,down);}
    void move(int dx,int dy){if(state.captured())pad.mouseMove(dx,dy);}
    // One pad poll `us` after the previous one.
    ControllerState poll(bool front=true,ControllerState physical={},std::uint64_t us=4000){
        now+=us;
        handle(front?Event::PollFront:Event::PollAway);
        if(!state.captured())return physical;
        return pad.apply(std::move(physical),now);
    }
};
// A hand moving at `countsPerSecond` for one 4 ms poll, as a mouse of `rateHz` reports it: whole
// counts per report, the fraction carried (a real sensor does the same).
struct Mouse {
    int rateHz;double carryX=0,carryY=0,phase=0;
    // Reports that fall into this poll, each with its counts.
    template<class F> void run(double vx,double vy,double pollMs,F&& report){
        phase+=pollMs*rateHz/1000.0;
        while(phase>=1){
            phase-=1;
            carryX+=vx/rateHz;carryY+=vy/rateHz;
            const int dx=int(carryX),dy=int(carryY);
            carryX-=dx;carryY-=dy;
            if(dx||dy)report(dx,dy);
        }
    }
};
} // namespace kbm_matrix

TEST(kbm_matrix_every_interruption_leaves_the_next_state_neutral){
    using namespace kbm_matrix;
    // Profiles x mouse rates x interruptions x seeds: random keys, buttons and mouse movement, an
    // interruption at a random moment, and the very next pad state must be neutral. 4 x 6 x 6 x 40
    // = 5760 sequences.
    const Event interruptions[]={Event::FocusLost,Event::PollAway,Event::ReleaseHotkey,Event::DeviceRemoved,Event::SessionEnded,Event::WindowChanged};
    const std::uint32_t keys[]={'W','A','S','D',0x10,0x20,0x11,'F','Q','R','E',0x09,0x14,'X',0x12};
    unsigned sequences=0;std::uint64_t stuck=0,lost=0;
    for(const auto kind:kKinds)for(const int rate:kRates)for(const auto interruption:interruptions)for(unsigned seed=1;seed<=40;++seed){
        std::mt19937 rng(seed*7919u+unsigned(rate)+unsigned(kind)*131u+unsigned(interruption)*17u);
        InputGlue g(kind,rate);Mouse mouse{rate};
        (void)g.poll();
        const int length=40+int(rng()%200);
        double vx=0,vy=0;
        for(int i=0;i<length;++i){
            if(rng()%4==0)g.key(keys[rng()%std::size(keys)],rng()%3!=0);
            if(rng()%9==0)g.button(1+int(rng()%3),rng()%2==0);
            if(rng()%7==0){vx=double(int(rng()%24000))-12000;vy=double(int(rng()%8000))-4000;}   // up to 3x the full speed: saturation too
            if(rng()%11==0)vx=vy=0;
            mouse.run(vx,vy,4,[&](int dx,int dy){g.move(dx,dy);});
            (void)g.poll();
        }
        // Something is certainly held at the interruption.
        g.key('W',false);g.button(1,false);(void)g.poll();     // (a key blocked by an on-foot / car switch counts again once let go)
        g.key('W',true);g.button(1,true);g.move(30,-10);
        (void)g.poll();
        const auto before=g.releases;const std::size_t held=g.pad.heldCount();
        CHECK(held>=2);
        g.handle(interruption);
        const bool away=interruption==Event::FocusLost||interruption==Event::PollAway;
        const auto next=g.poll(!away);
        CHECK(neutral(next));                                  // within one send
        CHECK(g.pad.heldCount()==0);
        CHECK(g.releases.stuckReleases==before.stuckReleases+1);
        CHECK(g.releases.heldAtLoss==before.heldAtLoss+(interruption==Event::SessionEnded?held:0));
        // And it stays neutral: no ramp, window, carry or hold plays anything out afterwards.
        for(int i=0;i<30;++i)CHECK(neutral(g.poll(!away)));
        // A key-up that arrives late for a key the release already let go is harmless.
        g.key('W',false);g.button(1,false);
        CHECK(neutral(g.poll(!away)));
        stuck+=g.releases.stuckReleases;lost+=g.releases.heldAtLoss;++sequences;
    }
    CHECK(sequences==5760);CHECK(stuck>=5760);CHECK(lost>0);
}

TEST(kbm_matrix_rapid_fine_and_reversing_motion_at_every_polling_rate){
    using namespace kbm_matrix;
    for(const auto kind:kKinds)for(const int rate:kRates){
        const bool immediate=kind!=KbmInputProfileKind::Legacy;
        // (a) A fast swipe right, then at once left: the first poll that holds only leftward counts points left
        // (legacy: once its window has passed).
        {
            InputGlue g(kind,rate);Mouse mouse{rate};(void)g.poll();
            ControllerState s;
            for(int i=0;i<25;++i){mouse.run(6000,0,4,[&](int dx,int dy){g.move(dx,dy);});s=g.poll();}
            CHECK(s.rightX>0);
            int pollsToTurn=0;
            for(int i=0;i<25;++i){mouse.run(-6000,0,4,[&](int dx,int dy){g.move(dx,dy);});s=g.poll();if(s.rightX<0)break;++pollsToTurn;}
            CHECK(s.rightX<0);
            if(immediate&&kind!=KbmInputProfileKind::Custom)CHECK(pollsToTurn<=(rate<250?2:1));   // no window to drain (the first poll may hold counts of both directions)
        }
        // (b) Fine aiming: 100 counts a second (less than one count per poll) still turns, and stops with the hand.
        {
            InputGlue g(kind,rate);Mouse mouse{rate};(void)g.poll();
            int moving=0;ControllerState s;
            for(int i=0;i<250;++i){mouse.run(100,0,4,[&](int dx,int dy){g.move(dx,dy);});s=g.poll();if(s.rightX>0)++moving;CHECK(s.rightX>=0);}
            CHECK(moving>=20);                                  // 100 counts in the second: the camera turned, in small steps
            // The hand stops: at rest within one poll (one report interval for a 125 Hz mouse); legacy within its window.
            int pollsToRest=0;
            for(int i=0;i<40;++i){s=g.poll();if(s.rightX==0&&s.rightY==0)break;++pollsToRest;}
            CHECK(s.rightX==0&&s.rightY==0);
            if(immediate&&kind!=KbmInputProfileKind::Custom)CHECK(pollsToRest<=(rate<250?2:0));
            for(int i=0;i<60;++i)CHECK(g.poll().rightX==0);     // and no drift afterwards
        }
        // (c) Keys and mouse together through it all: the movement keys never let go while the mouse does anything.
        {
            InputGlue g(kind,rate);Mouse mouse{rate};(void)g.poll();
            g.key('W',true);g.key('D',true);g.button(1,true);
            std::mt19937 rng(unsigned(rate)*3u+unsigned(kind));
            for(int i=0;i<400;++i){
                const double v=(i/20)%2?9000.0:-9000.0;        // reversing every 80 ms, beyond the full speed
                mouse.run(v+double(int(rng()%200)),double(int(rng()%400))-200,4,[&](int dx,int dy){g.move(dx,dy);});
                const auto s=g.poll();
                CHECK(s.leftX>0&&s.leftY<0&&s.r2==255);
            }
            g.key('W',false);g.key('D',false);g.button(1,false);
            ControllerState s;for(int i=0;i<40;++i)s=g.poll();
            CHECK(neutral(s));
        }
    }
}

TEST(kbm_matrix_pad_and_mouse_never_fight_over_a_stick){
    using namespace kbm_matrix;
    // The rule: each stick follows the device that acted last; a poll's stick value is the pad's
    // or the keyboard / mouse's, never a mix, and no mode switch is needed.
    for(const auto kind:kKinds){
        InputGlue g(kind,1000);Mouse mouse{1000};(void)g.poll();
        ControllerState physical;physical.inputActive=true;physical.rightX=26000;physical.leftY=-30000;   // both sticks pushed
        ControllerState s;
        for(int i=0;i<20;++i)s=g.poll(true,physical);
        CHECK(s.rightX==26000&&s.leftY==-30000);                // the controller has both
        // The mouse starts moving left: the camera stick is the mouse's, at once; the left stick stays the pad's.
        for(int i=0;i<50;++i){
            mouse.run(-3000,0,4,[&](int dx,int dy){g.move(dx,dy);});s=g.poll(true,physical);
            CHECK(s.rightX<=0);                                 // never the pad's +26000, never a sum
            CHECK(s.leftY==-30000);
        }
        // The mouse rests: the camera stands still (0) for the hold time, then the stick still held on the pad has it again.
        s=g.poll(true,physical);
        for(int i=0;i<10;++i)s=g.poll(true,physical);
        CHECK(s.rightX==0||kind==KbmInputProfileKind::Legacy||kind==KbmInputProfileKind::Custom);
        for(int i=0;i<80;++i)s=g.poll(true,physical);           // past kMouseHoldUs (250 ms)
        CHECK(s.rightX==26000);
        // A movement key takes the left stick from the pad; letting it go gives it back. Buttons add up meanwhile.
        physical.buttons=ControllerState::Circle;
        g.key('S',true);s=g.poll(true,physical);
        CHECK(s.leftY>0&&s.leftX==0);CHECK(s.buttons&ControllerState::Circle);
        g.key(0x20,true);s=g.poll(true,physical);
        CHECK((s.buttons&ControllerState::Circle)&&(s.buttons&ControllerState::Cross));
        g.key('S',false);g.key(0x20,false);s=g.poll(true,physical);
        CHECK(s.leftY==-30000);CHECK(s.buttons==ControllerState::Circle);
        // The pad pushes the camera stick somewhere new while the mouse has it: the pad takes over (it acted last).
        for(int i=0;i<10;++i){mouse.run(3000,0,4,[&](int dx,int dy){g.move(dx,dy);});s=g.poll(true,physical);}
        CHECK(s.rightX>0&&s.rightX!=26000);
        physical.rightX=-26000;physical.rightY=20000;
        s=g.poll(true,physical);
        CHECK(s.rightX==-26000&&s.rightY==20000);
        // A resting stick with drift never takes the camera from the mouse.
        physical.rightX=3000;physical.rightY=-2000;
        for(int i=0;i<30;++i)s=g.poll(true,physical);
        for(int i=0;i<10;++i){mouse.run(3000,0,4,[&](int dx,int dy){g.move(dx,dy);});s=g.poll(true,physical);CHECK(s.rightX!=3000);}
    }
}

TEST(kbm_matrix_starved_input_thread_loses_no_press_and_sticks_nothing){
    using namespace kbm_matrix;
    // CPU or GPU saturation as the input code sees it: pad polls arrive late (20 to 300 ms) while
    // keys and mouse reports keep queueing and are all delivered before the late poll.
    for(const auto kind:kKinds){
        const bool immediate=kind!=KbmInputProfileKind::Legacy;
        std::mt19937 rng(77u+unsigned(kind));
        InputGlue g(kind,1000);(void)g.poll();
        unsigned taps=0,seen=0;
        for(int round=0;round<400;++round){
            // A whole tap of the jump key (Space = Cross) between two polls, with mouse movement around it.
            const std::uint64_t stall=20000+std::uint64_t(rng()%280000);
            g.move(int(rng()%400),int(rng()%100));
            g.key(0x20,true);g.move(5,5);g.key(0x20,false);++taps;
            auto s=g.poll(true,{},stall);
            if(s.buttons&ControllerState::Cross)++seen;
            CHECK(s.rightX>=0);
            // The poll after it: the tap is over, and (the mouse rested) so is the turn, however long the stall was.
            s=g.poll();
            CHECK((s.buttons&ControllerState::Cross)==0);
            if(immediate&&kind!=KbmInputProfileKind::Custom)CHECK(s.rightX==0&&s.rightY==0);
            for(int i=0;i<15;++i)s=g.poll();
            CHECK(neutral(s));
            CHECK(g.pad.heldCount()==0);
        }
        if(immediate)CHECK(seen==taps);          // every tap reached exactly one pad state
        else CHECK(seen==0);                      // legacy, as before: a tap wholly between two polls is not seen (docs: known limit)
        // A key held across stalls stays held; pressed again inside a stall it is simply held.
        g.key('W',true);
        for(int i=0;i<20;++i)CHECK(g.poll(true,{},std::uint64_t(50000+rng()%200000)).leftY<0);
        g.key('W',false);g.key('W',true);g.key('W',false);g.key('W',true);
        CHECK(g.poll(true,{},150000).leftY<0);CHECK(g.poll().leftY<0);
        g.key('W',false);
        CHECK(g.poll(true,{},90000).leftY==0);
        // The session goes away in the middle of a stall with keys down: counted, and neutral at once.
        g.key('W',true);g.button(1,true);(void)g.poll();
        g.handle(Event::SessionEnded);
        CHECK(g.releases.heldAtLoss>=2);
        CHECK(neutral(g.poll(true,{},250000)));
    }
}

TEST(kbm_matrix_connection_interruptions_fail_neutral_through_the_queue){
    using namespace kbm_matrix;
    // The pad poll stops (the interface hangs) or the session stalls while keys are held: the
    // controller queue's own rule sends a neutral state after 100 ms without a fresh poll, so the
    // console never keeps a pressed button from a PC that went quiet, whatever the input profile.
    for(const auto kind:kKinds){
        InputGlue g(kind,1000);(void)g.poll();
        ControllerQueue queue;
        HostTime t=10000000;
        g.key('W',true);g.button(1,true);
        ControllerState sent;
        for(int i=0;i<50;++i){t+=40000;queue.push(t,g.poll());sent=queue.take(t);}
        CHECK(sent.r2==255&&sent.leftY<0);
        // No poll for 120 ms (the owner still ticks every 4 ms).
        for(int i=0;i<30;++i){t+=40000;sent=queue.take(t);}
        CHECK(neutral(sent)&&!sent.inputActive);
        // The polls come back with the keys still down: they are sent again (the hands never let go).
        t+=40000;queue.push(t,g.poll(true,{},124000));sent=queue.take(t);
        CHECK(sent.r2==255&&sent.leftY<0);
        // The connection is lost for good: the bridge stops the capture.
        g.handle(Event::SessionEnded);
        t+=40000;queue.push(t,g.poll());sent=queue.take(t);
        CHECK(neutral(sent));CHECK(g.releases.heldAtLoss>=2);
        // The next session starts on foot with nothing held.
        CHECK(!g.pad.driving());CHECK(g.pad.heldCount()==0);
    }
}

TEST(kbm_matrix_eight_hour_session_stays_exact){
    using namespace kbm_matrix;
    // Eight hours of play at a 4 ms poll in model time (7.2 million polls) for the two profiles that
    // differ most, legacy and adaptive-precision: bursts of keys and mouse, a focus change every few
    // minutes, the statistics probe written and drained as in the app. Under a sanitizer (many times
    // slower) the same pattern runs for 20 minutes of model time.
#if defined(__SANITIZE_THREAD__)||defined(__SANITIZE_ADDRESS__)
    const std::uint64_t polls=20ull*60*250;
#else
    const std::uint64_t polls=8ull*3600*250;
#endif
    for(const auto kind:{KbmInputProfileKind::Legacy,KbmInputProfileKind::AdaptivePrecision}){
        InputGlue g(kind,1000);Mouse mouse{1000};(void)g.poll();
        std::mt19937 rng(2026u+unsigned(kind));
        KbmInputProbe probe;KbmInputMeter meter;KbmMouseRate rate;
        ControllerState last;std::uint64_t handed=0,restChecks=0;
        bool keyDown=false,fire=false;double vx=0;
        for(std::uint64_t i=0;i<polls;++i){
            const std::uint64_t inMinute=i%15000;                      // 60 s of polls
            const bool burst=inMinute<5000;                            // 20 s of play, 40 s of rest
            if(burst){
                if(inMinute%250==0){keyDown=!keyDown;g.key('W',keyDown);}
                if(inMinute%375==0){fire=!fire;g.button(1,fire);}
                if(inMinute%50==0)vx=double(int(rng()%9000))-4500;
                unsigned reports=0;
                mouse.run(vx,vx/4,4,[&](int dx,int dy){g.move(dx,dy);++reports;});
                rate.add(g.now,reports);
            }else if(inMinute==5000){
                if(keyDown){g.key('W',false);keyDown=false;}
                if(fire){g.button(1,false);fire=false;}
                vx=0;
            }
            if(i%75000==74999)g.handle(Event::FocusLost);              // every 5 minutes the window loses the focus for a poll
            const auto s=g.poll();
            if(!(s==last)){probe.handed(std::int64_t(g.now)*10,std::int64_t(g.now)*10-5000);last=s;++handed;
                probe.sent(std::int64_t(g.now)*10+2000,10000000,true,1,std::int64_t(g.now)*10);}
            meter.takeProbe(probe);
            if(i%1250==0)meter.clear();                                // the 5 s window
            if(!burst&&inMinute>5100){CHECK(neutral(s));++restChecks;} // at rest between bursts: nothing drifts, nothing sticks
        }
        CHECK(restChecks>0);CHECK(handed>0);CHECK(probe.lost()==0);
        CHECK(g.pad.heldCount()==0);
        CHECK(rate.classHz()==1000);
        CHECK(g.now>polls*4000);                                       // model time really advanced that far
        if(kind==KbmInputProfileKind::AdaptivePrecision)CHECK(g.pad.movingPolls()>0&&g.pad.saturatedPolls()<=g.pad.movingPolls());
    }
}
