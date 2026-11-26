// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/engine/PushCadence.h ([slipstream-cadence]). Present call times are made up; what the
// panel really shows is for PresentMon / the owner's eyes on the real PC.
#include "veyra/engine/PushCadence.h"
#include <cmath>
namespace cadence_tests {
// `seconds` of presents at `fps`, alternating real / generated when `fg` (real first), starting at t0.
inline void feed(veyra::engine::PushCadenceWindow& w,double fps,double seconds,bool fg,double t0=1000.0){
    const int n=int(std::lround(fps*seconds));
    for(int i=0;i<n;++i)w.presented(t0+1000.0*i/fps,fg&&(i%2));
}
constexpr double kPanel=179.952; // the owner's Odyssey G50SF (BENCH1)
}

TEST(cadence_120_on_a_fixed_180_panel_alternates_one_and_two_refreshes){
    using namespace cadence_tests;
    veyra::engine::PushCadenceWindow w;
    feed(w,120,5,true);
    const auto s=w.take(1000+5000,kPanel,0,1000.3);
    // What the counter shows: 120 presents a second, half of them generated, evenly spaced calls.
    CHECK(std::abs(s.submittedFps-120)<0.1&&std::abs(s.generatedFps-60)<0.1&&std::abs(s.realFps-60)<0.1);
    CHECK(std::abs(s.intervalP50Ms-8.333)<0.01&&s.offPct==0);
    // What a fixed refresh shows: holds of 1 and 2 refreshes in equal numbers, every frame still shown.
    CHECK(s.hold3==0&&s.hold1>250&&s.hold2>250);
    CHECK(s.unevenPct>45&&s.unevenPct<=50);
    CHECK(std::abs(s.shownFps-120)<0.5);
    // The even alternative: 89.98 FPS (every frame held 2 refreshes).
    CHECK(s.evenDivisor==2&&std::abs(s.evenFps-kPanel/2)<1e-9);
    CHECK(formatSlipstreamCadence(s).find("fixedUnevenPct=")!=std::string::npos);
}

TEST(cadence_even_rates_on_the_same_panel_hold_evenly){
    using namespace cadence_tests;
    veyra::engine::PushCadenceWindow w;
    feed(w,kPanel/2,5,true);
    auto s=w.take(6000,kPanel,0,1000.0-0.1);
    CHECK(s.unevenPct==0&&s.hold2>400&&s.hold1==0&&s.evenDivisor==2);
    feed(w,kPanel/3,5,false);
    s=w.take(6000,kPanel,0,1000.0-0.1);
    CHECK(s.unevenPct==0&&s.hold3>250&&s.generated==0&&s.evenDivisor==3&&std::abs(s.evenFps-kPanel/3)<1e-9);
    // Native 60 from the console (59.98 on this panel's grid would be exact; 60.00 slips one refresh a minute).
    feed(w,60,5,false);
    s=w.take(6000,kPanel,0,1000.0-0.1);
    CHECK(s.unevenPct<1&&s.evenDivisor==3);
}

TEST(cadence_a_59_fps_source_with_2x_generation_shows_a_hole_a_second){
    using namespace cadence_tests;
    // BENCH2: about 59 source frames a second for a 60 profile. With 2x frame generation each missing source
    // frame leaves one interval of a whole source period (no generated frame in it).
    veyra::engine::PushCadenceWindow w;
    double t=1000;
    for(int second=0;second<5;++second){
        for(int f=0;f<60;++f){
            if(f==30){t+=1000.0/60;continue;}            // the frame the console did not send
            w.presented(t,false);w.presented(t+1000.0/120,true);t+=1000.0/60;
        }
    }
    const auto s=w.take(t,kPanel,1);
    CHECK(s.vrr==1&&std::abs(s.submittedFps-118)<0.5&&s.real==295);
    // The hole is 25 ms (generated frame to the next real one); one a second is below P99, so the line's
    // max and offPct carry it, not its percentiles.
    CHECK(s.intervalP50Ms<8.4&&s.intervalP99Ms<8.4&&s.intervalMaxMs>24.9);
    CHECK(s.offPct>0.5&&s.offPct<2);   // 5 of about 590 intervals
}

TEST(cadence_pauses_capacity_and_unknown_refresh){
    using namespace cadence_tests;
    veyra::engine::PushCadenceWindow w;
    feed(w,120,1,false,1000);feed(w,120,1,false,3000);   // a 1 s pause between two runs
    auto s=w.take(4000,0,-1);
    CHECK(s.intervals==238&&s.intervalMaxMs<8.4&&s.refreshHz==0&&s.hold1+s.hold2+s.hold3==0&&s.evenFps==0);
    for(std::size_t i=0;i<veyra::engine::PushCadenceWindow::kCapacity+10;++i)w.presented(double(i),false);
    s=w.take(5000,kPanel,0);
    CHECK(s.overflow==10&&s.real==veyra::engine::PushCadenceWindow::kCapacity+10);
    s=w.take(6000,kPanel,0);
    CHECK(s.seconds==0&&s.real==0&&s.submittedFps==0&&s.intervals==0);
    std::uint32_t d=7;
    CHECK(veyra::engine::pushEvenCadenceFps(144,120,&d)==72&&d==2);
    CHECK(veyra::engine::pushEvenCadenceFps(120,120,&d)==120&&d==1);
    CHECK(veyra::engine::pushEvenCadenceFps(0,120,&d)==0&&d==0);
}

TEST(cadence_even_frame_generation_multiplier_for_a_fixed_refresh){
    using namespace cadence_tests;
    const auto all=[](unsigned){return true;};
    const auto even=[](unsigned m){return m%2==0;};
    CHECK(veyra::engine::pushEvenOnRefresh(kPanel,60)&&veyra::engine::pushEvenOnRefresh(kPanel,90)&&veyra::engine::pushEvenOnRefresh(kPanel,180));
    CHECK(!veyra::engine::pushEvenOnRefresh(kPanel,120)&&!veyra::engine::pushEvenOnRefresh(144,60)&&!veyra::engine::pushEvenOnRefresh(kPanel,240));
    // The owner's panel, a 60 FPS console picture, ceiling 4X, target the refresh: 3X (180), not 2X (120).
    CHECK(veyra::engine::pushEvenFgMultiplier(4,59.9,kPanel,kPanel,kPanel,all)==3);
    // An output cap of 120 leaves nothing even above 1X: 0, the usual choice stands.
    CHECK(veyra::engine::pushEvenFgMultiplier(4,59.9,120,kPanel,120,all)==0);
    // A 30 FPS game: 2X (60), 3X (90) and 6X (180) are even; closest to a 120 target under a 180 limit is 3X.
    CHECK(veyra::engine::pushEvenFgMultiplier(6,30,120,kPanel,kPanel,all)==3);
    CHECK(veyra::engine::pushEvenFgMultiplier(6,30,120,kPanel,kPanel,even)==6);   // 4X (120) is not even; 180 is nearer 120 than 60
    // 120 Hz panel, 60 FPS: 2X. 144 Hz, 60 FPS: nothing even (72 would need 1.2X).
    CHECK(veyra::engine::pushEvenFgMultiplier(4,60,120,120,120,all)==2);
    CHECK(veyra::engine::pushEvenFgMultiplier(4,60,144,144,144,all)==0);
    CHECK(veyra::engine::pushEvenFgMultiplier(1,60,180,180,180,all)==0&&veyra::engine::pushEvenFgMultiplier(4,0,180,180,180,all)==0);
}

TEST(cadence_live_readout_says_what_is_shown_and_whether_it_is_even){
    using namespace cadence_tests;
    veyra::engine::PushCadenceWindow w;
    feed(w,120,1,true);
    const auto fixed=w.take(2000,kPanel,0,1000.3);
    auto c=veyra::engine::pushLiveCadenceFrom(fixed,1,2000);
    CHECK(c.valid&&c.shownKnown&&std::abs(c.shownFps-120)<0.5&&c.unevenKnown&&c.uneven()&&c.vrr==0);
    CHECK(c.fresh(2500)&&!c.fresh(5001)&&!c.fresh(1999));
    // Display sync Automatic (sync interval 0): presents may be replaced unseen, so no shown rate.
    CHECK(!veyra::engine::pushLiveCadenceFrom(fixed,0,2000).shownKnown);
    // The same presents under variable refresh: even calls, even screen.
    feed(w,120,1,true);
    c=veyra::engine::pushLiveCadenceFrom(w.take(2000,kPanel,1),1,2000);
    CHECK(c.shownKnown&&c.unevenKnown&&!c.uneven()&&c.unevenPct==0);
    // VRR state unknown (AMD, Intel, NVAPI not ready): the evenness is not claimed either way.
    feed(w,120,1,true);
    c=veyra::engine::pushLiveCadenceFrom(w.take(2000,kPanel,-1),1,2000);
    CHECK(c.shownKnown&&!c.unevenKnown&&!c.uneven());
    // 240 presents a second on a 180 Hz panel cannot all be shown.
    feed(w,240,1,true);
    c=veyra::engine::pushLiveCadenceFrom(w.take(2000,kPanel,1),1,2000);
    CHECK(std::abs(c.shownFps-kPanel)<1e-9);
    auto& board=veyra::engine::pushLiveCadenceBoard();
    board.publish(c);CHECK(board.read().valid);board.clear();CHECK(!board.read().valid);
    CHECK(!veyra::engine::pushLiveCadenceFrom(veyra::engine::PushCadenceSummary{},1,0).valid);
}

TEST(cadence_window_closed_at_the_newest_present_counts_exactly){
    using namespace cadence_tests;
    // The presenter's loop: note each present, and once a second close the window at the newest present's
    // call time (the reviewer's case: closing later with that present inside read 62.04 for a true 60).
    for(double fps:{60.0,120.0,59.94}){
        veyra::engine::PushCadenceWindow w;
        double next=0;int windows=0;
        for(int i=0;i<int(fps*10);++i){
            const double t=1000.0+1000.0*i/fps;
            w.presented(t,(i%2)!=0);
            if(t>=next){
                const auto s=w.takeAtLastPresent(kPanel,1);
                if(next>0){CHECK(std::abs(s.submittedFps-fps)<1e-6);++windows;}
                next=t+1000;
            }
        }
        CHECK(windows>=8);
    }
    // One present only, then nothing: no rate, and the present is carried, not lost.
    veyra::engine::PushCadenceWindow w;
    w.presented(5000,false);
    auto s=w.takeAtLastPresent(kPanel,0);
    CHECK(s.submittedFps==0&&s.real==0);
    w.presented(5010,true);
    s=w.takeAtLastPresent(kPanel,0);
    CHECK(s.real==1&&s.generated==0&&std::abs(s.submittedFps-100)<1e-9);
    CHECK(w.takeAtLastPresent(kPanel,0).submittedFps==0);   // the carried present alone
}

TEST(cadence_vrr_state_trusts_only_vrr_enabled_after_the_first_present){
    using veyra::engine::pushVrrCadenceState;
    // known, enabled, readAfterFirstPresent, confirmationPending, recentResize
    CHECK(pushVrrCadenceState(true,true,false,true,true)==1);
    // BENCH1, the owner's panel for whole runs: requested=0 active=0 displayInVrrMode=1. After a reading
    // taken once this chain presented: a fixed refresh (0), whatever the display-mode flag says.
    CHECK(pushVrrCadenceState(true,false,true,false,false)==0);
    // Only the start-up reading (before the first present), a pending confirmation, a recent resize: unknown.
    CHECK(pushVrrCadenceState(true,false,false,false,false)==-1);
    CHECK(pushVrrCadenceState(true,false,true,true,false)==-1);
    CHECK(pushVrrCadenceState(true,false,true,false,true)==-1);
    // No driver answer (AMD, Intel, NVAPI not ready): unknown.
    CHECK(pushVrrCadenceState(false,true,true,false,false)==-1);
    // With -1 the readout claims nothing about evenness; the shown rate stays.
    using namespace cadence_tests;
    veyra::engine::PushCadenceWindow w;
    feed(w,120,1,true);
    const auto c=veyra::engine::pushLiveCadenceFrom(w.take(2000,kPanel,pushVrrCadenceState(true,false,false,false,false)),1,2000);
    CHECK(c.shownKnown&&!c.unevenKnown&&!c.uneven());
}

TEST(cadence_window_with_a_pause_is_not_a_rate){
    using namespace cadence_tests;
    // Review: a 3 s pause inside a window read "0 FPS shown". A window with a gap over 250 ms is not valid.
    veyra::engine::PushCadenceWindow w;
    w.presented(1000,false);w.presented(4000,false);
    const auto s=w.takeAtLastPresent(kPanel,1);
    CHECK(s.pauses==1&&s.intervals==0);
    CHECK(!veyra::engine::pushLiveCadenceFrom(s,1,4000).valid);
    CHECK(veyra::engine::formatSlipstreamCadence(s).find("pauses=1")!=std::string::npos);
    feed(w,60,1,false,4000);
    CHECK(veyra::engine::pushLiveCadenceFrom(w.takeAtLastPresent(kPanel,1),1,5000).valid);
}

TEST(cadence_vrr_confirm_reads_at_5_s_and_again_at_15_s_while_not_enabled){
    veyra::engine::PushVrrConfirmSchedule c;
    CHECK(!c.due(100000));                        // nothing before the first present
    c.firstPresent(1000);c.firstPresent(2000);    // once per chain
    CHECK(!c.due(5999)&&c.due(6000)&&!c.due(6001));
    CHECK(!c.readAfterPresent());
    c.applied(6050,false);                        // fixed refresh at 5 s: read once more at about 15 s
    CHECK(c.readAfterPresent()&&c.dueMs()==16000);
    CHECK(!c.due(15999)&&c.due(16000));
    c.applied(16020,true);                        // engaged late: no third read
    CHECK(c.dueMs()<0&&!c.due(1e9));
    // Engaged at the first read: no second one.
    veyra::engine::PushVrrConfirmSchedule d;
    d.firstPresent(0);CHECK(d.due(5000));d.applied(5010,true);
    CHECK(d.readAfterPresent()&&d.dueMs()<0);
    // A state applied without a pending read (the start-up reading) does not count.
    veyra::engine::PushVrrConfirmSchedule e;
    e.applied(10,false);e.firstPresent(20);e.applied(30,false);
    CHECK(!e.readAfterPresent());
    e.otherReadIssued();e.applied(40,false);      // a vrr-confirm / stats read after the first present counts
    CHECK(e.readAfterPresent());
}

TEST(cadence_vrr_confirm_a_resize_forgets_the_old_reading){
    using veyra::engine::pushVrrCadenceState;
    // Review: after full screen the pre-resize "enabled=0" came back after 3 s (a false "uneven" once G-SYNC engaged).
    veyra::engine::PushVrrConfirmSchedule c;
    c.firstPresent(0);CHECK(c.due(5000));c.applied(5010,false);
    CHECK(pushVrrCadenceState(true,false,c.readAfterPresent(),false,c.recentResize(6000))==0);
    c.resized(20000);                             // into full screen
    CHECK(!c.readAfterPresent()&&c.recentResize(21000));
    CHECK(pushVrrCadenceState(true,false,c.readAfterPresent(),false,c.recentResize(24000))==-1);   // stale reading not reused
    CHECK(!c.due(24999)&&c.due(25000));
    c.applied(25020,true);
    CHECK(pushVrrCadenceState(true,true,c.readAfterPresent(),false,c.recentResize(25100))==1);
    c.monitorChanged();CHECK(!c.readAfterPresent());
    c.reset();CHECK(!c.readAfterPresent()&&c.dueMs()<0&&!c.recentResize(25100));
}
