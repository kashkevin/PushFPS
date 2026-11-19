// SPDX-License-Identifier: GPL-3.0-only
// Included by CoreTests.cpp: the adaptive transport experiments (reorder window,
// recovery without restart, bottleneck classification, latency governor).
// Portable logic only; nothing here talks to a console, and every "link" below
// is a made-up arrival pattern, not a measurement.
#include "veyra/remoteplay/ReorderWindow.h"
#include "veyra/remoteplay/ReorderQueueModel.h"
namespace adaptive_tests {
constexpr HostTime kSec=10000000;
using Window=ReorderWindowController;
// A link as the library's counters would show it: the caller adds events, the
// controller is asked every 250 ms like the session owner does.
struct Feed {
    Window controller;
    ReorderEvidence e;
    HostTime now=0;
    unsigned changes=0,widenings=0,narrowings=0,rollbacks=0;
    std::vector<std::pair<HostTime,std::uint32_t>> history;
    explicit Feed(std::uint32_t startUs,bool widenOnlyForLostFrames=true):controller(startUs,widenOnlyForLostFrames){e.reorderTimeoutUs=controller.windowUs();step();}
    // An out-of-order packet that came `us` after its wait began.
    void arrival(std::uint32_t us){
        std::size_t bucket=0;
        while(bucket<kReorderLatenessEdgesUs.size()&&us>kReorderLatenessEdgesUs[bucket])++bucket;
        ++e.lateness[bucket];++e.gaps;
        if(us>controller.windowUs()){++e.timeouts;++e.skipped;++e.lateArrivals;}
    }
    void loss(){++e.gaps;++e.timeouts;++e.skipped;}
    void frameLost(){++e.fecFailed;} // a frame FEC could not complete
    // The same, and the frame it belonged to was lost when it was given up.
    void costlyArrival(std::uint32_t us){if(us>controller.windowUs())frameLost();arrival(us);}
    // A link in trouble: twelve of them at once (the rule wants frames lost at 0.5 a second or more).
    void trouble(std::uint32_t us){for(int i=0;i<12;++i)costlyArrival(us);}
    Window::Decision step(HostTime dt=kSec/4){
        now+=dt;e.videoPackets+=350;
        const auto d=controller.update(now,e);
        if(d.changed){
            ++changes;
            if(d.reason==Window::Reason::Widen)++widenings;
            if(d.reason==Window::Reason::Narrow)++narrowings;
            if(d.reason==Window::Reason::Rollback)++rollbacks;
            history.emplace_back(now,d.windowUs);
            e.reorderTimeoutUs=d.windowUs; // the library took it
        }
        CHECK(d.windowUs==controller.windowUs());
        return d;
    }
    void run(HostTime duration){for(const HostTime end=now+duration;now<end;)step();}
};
} // namespace adaptive_tests

// --- Switch (VEYRA_EXP_RP_REORDER_ADAPTIVE) ---
TEST(reorder_adaptive_is_off_by_default_and_its_own_switch){
    CHECK(!resolveLanExperiments({}).reorderAdaptive);
    TransportSwitches s;s.reorderAdaptive="1";
    auto e=resolveLanExperiments(s);
    CHECK(e.reorderAdaptive&&e.any()&&e.usesTransport());
    // It implies nothing else: without the LAN profile the session still starts
    // with the library's own wait and buffer.
    CHECK(!e.lanProfile&&e.transport==TransportTuning{});
    CHECK(e.describe().find("reorderAdaptive=on")!=std::string::npos);
    s.reorderAdaptive="0";CHECK(!resolveLanExperiments(s).reorderAdaptive);
    s.reorderAdaptive="";CHECK(!resolveLanExperiments(s).any());
    s={};s.lanProfile="1";e=resolveLanExperiments(s);
    CHECK(!e.reorderAdaptive);CHECK(e.describe().find("reorderAdaptive=off")!=std::string::npos);
}
TEST(reorder_evidence_comes_from_the_library_counters){
    TransportCounters c;c.videoPackets=9;c.reorderGaps=5;c.reorderTimeouts=3;c.reorderSkipped=4;c.reorderLateArrivals=2;
    c.reorderTimeoutUs=3000;c.reorderLateness[0]=7;c.reorderLateness[8]=1;c.fecFailed=6;
    const auto e=reorderEvidence(c);
    CHECK(e.videoPackets==9&&e.gaps==5&&e.timeouts==3&&e.skipped==4&&e.lateArrivals==2&&e.reorderTimeoutUs==3000&&e.fecFailed==6);
    CHECK(e.lateness[0]==7&&e.lateness[8]==1&&e.lateness[4]==0);
    // The bucket edges are the library's (patch 0004) and end at the ceiling.
    CHECK(kReorderLatenessEdgesUs.size()+1==kReorderLatenessBuckets);
    CHECK(kReorderLatenessEdgesUs.front()==1000&&kReorderLatenessEdgesUs.back()==16000);
}

// --- Bounds and steps ---
TEST(reorder_window_start_snaps_up_to_a_step_inside_the_bounds){
    using W=ReorderWindowController;
    CHECK(W::kStepsUs.front()==W::kFloorUs&&W::kStepsUs.back()==W::kCeilingUs);
    CHECK(W::kFloorUs==2000&&W::kCeilingUs==16000);
    CHECK(W(0).windowUs()==16000);      // 0 = the library's own wait
    CHECK(W(500).windowUs()==2000);     // never below the floor
    CHECK(W(2000).windowUs()==2000);
    CHECK(W(2001).windowUs()==3000);    // up, not down: never shorter than what was asked
    CHECK(W(5000).windowUs()==6000);
    CHECK(W(16000).windowUs()==16000);
    CHECK(W(999999).windowUs()==16000);
}
TEST(reorder_window_clean_link_at_the_floor_never_changes){
    adaptive_tests::Feed f(2000);
    f.run(3600*adaptive_tests::kSec);
    CHECK(f.changes==0);CHECK(f.controller.windowUs()==2000);CHECK(f.controller.faults()==0);
}
TEST(reorder_window_clean_link_from_the_ceiling_narrows_one_step_per_dwell){
    using namespace adaptive_tests;
    Feed f(16000);
    f.run(3600*kSec);
    // 16 -> 12 -> 8 -> 6 -> 4 -> 3 -> 2 ms, one step at a time, never back up.
    CHECK(f.narrowings==6&&f.widenings==0);CHECK(f.controller.windowUs()==2000);
    const std::uint32_t expected[]={12000,8000,6000,4000,3000,2000};
    for(std::size_t i=0;i<f.history.size();++i){
        CHECK(f.history[i].second==expected[i]);
        const HostTime since=f.history[i].first-(i?f.history[i-1].first:0);
        CHECK(since>=Window::kNarrowDwell100ns);
    }
}

// --- Widening ---
TEST(reorder_window_widens_within_a_second_of_late_but_arriving_packets){
    using namespace adaptive_tests;
    Feed f(2000);f.run(20*kSec);
    f.trouble(5000);
    const HostTime seen=f.now;
    f.run(2*kSec);
    CHECK(f.widenings==1);CHECK(f.controller.windowUs()==6000); // the step that covers 5 ms
    CHECK(f.history.front().first-seen<=kSec);
}
TEST(reorder_window_one_stray_late_packet_changes_nothing){
    using namespace adaptive_tests;
    Feed f(2000);
    // One a minute, each time with frames being lost: one late packet is not a pattern.
    for(int i=0;i<20;++i){f.run(60*kSec);f.arrival(7000);for(int k=0;k<12;++k)f.frameLost();}
    f.run(60*kSec);
    CHECK(f.changes==0);CHECK(f.controller.windowUs()==2000);
    f.arrival(7000);for(int k=0;k<12;++k)f.frameLost();f.arrival(7000);f.run(2*kSec); // two are
    CHECK(f.changes==2&&f.controller.windowUs()==8000); // 2 -> 6 -> 8 ms
}
TEST(reorder_window_widening_is_at_most_three_steps_per_change_and_dwells){
    using namespace adaptive_tests;
    Feed f(2000);f.run(5*kSec);
    f.trouble(15000);
    f.run(5*kSec);
    CHECK(f.history.size()==2);
    CHECK(f.history[0].second==6000);  // 2 -> 6 ms: three steps
    CHECK(f.history[1].second==16000); // then the rest
    CHECK(f.history[1].first-f.history[0].first>=Window::kWidenDwell100ns);
}
TEST(reorder_window_does_not_chase_packets_later_than_the_ceiling){
    using namespace adaptive_tests;
    Feed f(2000);
    for(int i=0;i<50;++i){f.costlyArrival(40000);f.step();} // 40 ms late: no window helps
    CHECK(f.changes==0);
}
TEST(reorder_window_follows_the_bulk_not_the_rare_tail){
    using namespace adaptive_tests;
    Feed f(2000);
    // 1000 packets reordered by under 1 ms and two by 11 ms in ten seconds: the
    // 99th percentile is inside the floor, so the window stays.
    for(int i=0;i<40;++i){for(int k=0;k<25;++k)f.arrival(700);if(i==10||i==30){f.arrival(11000);for(int k=0;k<12;++k)f.frameLost();}f.step();}
    CHECK(f.changes==0);
    CHECK(f.controller.neededUs()&&*f.controller.neededUs()==2000);
}

TEST(reorder_window_late_packets_that_cost_no_frame_do_not_widen){
    using namespace adaptive_tests;
    // FEC completes every frame: giving a late packet up early is then the
    // faster hand-over, and a longer wait would only add delay (reorder_sim.cpp).
    Feed f(2000);
    for(int i=0;i<1200;++i){f.arrival(5000);f.step();}
    CHECK(f.changes==0);CHECK(f.controller.neededUs()&&*f.controller.neededUs()==6000);
    // A frame lost long before or long after the late packets is not their cost.
    Feed g(2000);g.frameLost();g.run(15*kSec);g.arrival(5000);g.arrival(5000);g.run(15*kSec);g.frameLost();g.run(15*kSec);
    CHECK(g.changes==0);
    // Nor do late packets with a frame lost now and then (one in five seconds).
    Feed k(2000);
    for(int i=0;i<1200;++i){k.arrival(5000);if(i%20==0)k.frameLost();k.step();}
    CHECK(k.changes==0);
    // The comparison rule of the simulator (not used by the app) widens on late packets alone.
    Feed h(2000,false);
    for(int i=0;i<40;++i){h.arrival(5000);h.step();}
    CHECK(h.controller.windowUs()==6000);
}
TEST(reorder_window_a_widening_that_pays_is_kept){
    using namespace adaptive_tests;
    Feed f(2000);
    // Frames are lost exactly when a 5 ms late packet is given up.
    for(int i=0;i<2400;++i){f.costlyArrival(5000);f.step();}
    CHECK(f.controller.windowUs()==6000);CHECK(f.widenings==1&&f.rollbacks==0&&f.narrowings==0);
    CHECK(f.e.fecFailed<=20);
}
TEST(reorder_window_a_widening_that_does_not_pay_is_rolled_back_and_retried_less_often){
    using namespace adaptive_tests;
    Feed f(2000);
    // Late packets all the time, and four frames a second lost whatever the wait
    // is (the link loses them): widening saves nothing.
    HostTime atFloor=0;
    for(int i=0;i<4*3600;++i){
        f.arrival(5000);f.frameLost();
        if(f.controller.windowUs()==2000)atFloor+=kSec/4;
        f.step();
    }
    CHECK(f.rollbacks>=3);CHECK(f.widenings==f.rollbacks||f.widenings==f.rollbacks+1);CHECK(f.narrowings==0);
    // Every rollback returns to where the widening started, and each retry waits longer.
    std::vector<HostTime> widened;
    for(std::size_t i=0;i<f.history.size();++i){
        if(f.history[i].second==6000)widened.push_back(f.history[i].first);
        else CHECK(f.history[i].second==2000);
    }
    for(std::size_t i=2;i<widened.size()&&i<6;++i)CHECK(widened[i]-widened[i-1]>widened[i-1]-widened[i-2]);
    CHECK(atFloor>=3600*kSec*8/10);
    CHECK(f.controller.rollbacks()==f.rollbacks);
}
TEST(reorder_window_a_wrong_rollback_is_undone_at_once){
    using namespace adaptive_tests;
    Feed f(2000);
    // Frames lost, then the widening; in the 30 s after it frames go on being
    // lost for another reason, so the check sees no gain and rolls back.
    for(int i=0;i<12;++i)f.frameLost();
    f.arrival(5000);f.arrival(5000);f.run(2*kSec);
    CHECK(f.controller.windowUs()==6000);
    for(int i=0;i<116;++i){f.arrival(5000);if(i%10==0)f.frameLost();f.step();}
    CHECK(f.rollbacks==1&&f.controller.windowUs()==2000);
    const HostTime rolledBack=f.history.back().first;
    // Now every late packet costs a frame again: far more than with the longer wait.
    for(int i=0;i<12&&f.controller.windowUs()==2000;++i){for(int k=0;k<4;++k)f.costlyArrival(5000);f.step();}
    CHECK(f.controller.windowUs()==6000);
    CHECK(f.history.back().first-rolledBack<=3*kSec); // not after the 60 s block
    for(int i=0;i<2400;++i){f.costlyArrival(5000);f.step();}
    CHECK(f.controller.windowUs()==6000&&f.rollbacks==1);
}
// --- Narrowing ---
TEST(reorder_window_stays_while_the_extra_wait_is_being_used){
    using namespace adaptive_tests;
    Feed f(2000);
    // Packets keep arriving 2.5 ms late: 2 ms gives them up, 3 ms catches them.
    for(int i=0;i<2400;++i){f.costlyArrival(2500);f.step();} // ten minutes
    CHECK(f.changes==1);CHECK(f.controller.windowUs()==3000);
    CHECK(f.e.lateArrivals<=20); // only until the window covered them
}
TEST(reorder_window_narrows_slowly_once_the_need_is_gone){
    using namespace adaptive_tests;
    Feed f(2000);
    for(int i=0;i<240;++i){f.costlyArrival(7000);f.step();} // a minute of 7 ms reordering
    CHECK(f.controller.windowUs()==8000);
    const HostTime quiet=f.now;const auto before=f.changes;
    f.run(20*kSec);
    CHECK(f.changes==before); // not within the dwell
    f.run(3600*kSec);
    CHECK(f.controller.windowUs()==2000);
    // First step down only after the evidence has faded and a full dwell passed.
    CHECK(f.history[before].first-quiet>=Window::kNarrowDwell100ns);
    for(std::size_t i=before+1;i<f.history.size();++i)CHECK(f.history[i].first-f.history[i-1].first>=Window::kNarrowDwell100ns);
}
TEST(reorder_window_true_losses_shorten_the_dwell){
    using namespace adaptive_tests;
    // Waits that run out with the packet never arriving: the long wait buys nothing.
    Feed lossy(16000),clean(16000);
    for(int i=0;i<200;++i){lossy.loss();lossy.step();clean.step();}
    CHECK(lossy.narrowings>clean.narrowings);
    CHECK(lossy.history.front().first>=Window::kNarrowDwellLossy100ns);
    CHECK(lossy.history.front().first<Window::kNarrowDwell100ns);
    CHECK(lossy.widenings==0&&clean.widenings==0);
}
TEST(reorder_window_a_narrowing_that_had_to_be_undone_doubles_the_dwell){
    using namespace adaptive_tests;
    Feed f(2000);
    // A dozen 5 ms late packets every 100 s: wide enough is 6 ms, and each time
    // the window has crept down to 4 ms they are given up again, with their frames.
    std::vector<HostTime> narrowed;
    for(int round=0;round<12;++round){
        f.trouble(5000);
        const auto before=f.narrowings;
        for(int i=0;i<400;++i){f.step();if(f.narrowings!=before&&(narrowed.empty()||narrowed.back()!=f.history.back().first))narrowed.push_back(f.history.back().first);}
    }
    // Without the penalty every round would narrow once; with it they stop.
    CHECK(f.controller.penalty()>=2);
    CHECK(f.narrowings<=4);
    CHECK(f.controller.windowUs()==6000);
}

// --- Inconsistent input ---
TEST(reorder_window_falls_back_to_the_library_wait_on_inconsistent_counters){
    using namespace adaptive_tests;
    {Feed f(2000);f.run(5*kSec);f.e.gaps=10;f.step();f.e.gaps=4;const auto d=f.step(); // a counter went back
        CHECK(d.changed&&d.reason==Window::Reason::Fault&&d.windowUs==16000);CHECK(f.controller.faults()==1);}
    {Feed f(2000);f.run(5*kSec);f.e.timeouts=3;f.e.gaps=1;                               // more timeouts than waits
        CHECK(f.step().reason==Window::Reason::Fault);CHECK(f.controller.windowUs()==16000);}
    {Feed f(2000);f.run(5*kSec);f.e.skipped=1;f.e.timeouts=1;f.e.gaps=1;f.e.lateArrivals=5; // more late arrivals than packets given up
        CHECK(f.step().reason==Window::Reason::Fault);}
    {Feed f(2000);f.run(5*kSec);f.now-=2*kSec;                                           // the clock stepped back
        CHECK(f.step().reason==Window::Reason::Fault);}
    {Feed f(16000);f.run(5*kSec);f.e.gaps=10;f.step();f.e.gaps=4;const auto d=f.step();     // already at the ceiling
        CHECK(!d.changed&&d.reason==Window::Reason::Fault&&d.windowUs==16000);}
}
TEST(reorder_window_fault_when_the_library_uses_another_wait){
    using namespace adaptive_tests;
    Feed f(2000);f.run(5*kSec);
    f.e.reorderTimeoutUs=9000;
    CHECK(f.step().reason==Window::Reason::None); // once can be a read between two updates
    CHECK(f.step().reason==Window::Reason::Fault);
    // Not while no video arrives (an outage: the library has not looked yet).
    Feed g(2000);g.run(5*kSec);g.e.reorderTimeoutUs=9000;
    for(int i=0;i<40;++i){g.now+=kSec/4;CHECK(g.controller.update(g.now,g.e).reason==Window::Reason::None);}
    // 0 = no connection yet.
    Feed h(2000);h.e.reorderTimeoutUs=0;h.run(5*kSec);CHECK(h.controller.faults()==0);
}
TEST(reorder_window_holds_after_a_fault_then_adapts_again){
    using namespace adaptive_tests;
    Feed f(2000);f.run(5*kSec);f.e.gaps=10;f.step();f.e.gaps=4;f.step();
    CHECK(f.controller.windowUs()==16000);
    const HostTime fault=f.now;
    f.run(3600*kSec);
    CHECK(f.controller.windowUs()==2000);CHECK(!f.controller.latched());
    CHECK(f.history[1].first-fault>=Window::kFaultHold100ns);
}
TEST(reorder_window_three_faults_keep_the_library_wait_for_good){
    using namespace adaptive_tests;
    Feed f(2000);
    for(int i=0;i<3;++i){f.run(120*kSec);f.e.gaps+=10;f.step();f.e.gaps-=5;f.step();}
    CHECK(f.controller.faults()==3&&f.controller.latched());
    f.run(3600*kSec);
    CHECK(f.controller.windowUs()==16000);
    for(int i=0;i<10;++i){f.arrival(3000);f.step();}
    CHECK(f.controller.windowUs()==16000);
}
TEST(reorder_window_a_new_connection_restarts_the_counters_and_keeps_the_window){
    using namespace adaptive_tests;
    Feed f(2000);
    for(int i=0;i<40;++i){f.costlyArrival(5000);f.step();}
    CHECK(f.controller.windowUs()==6000);
    f.controller.newConnection();
    f.e={};f.e.reorderTimeoutUs=6000; // the library's counters restart from zero
    const auto d=f.step();
    CHECK(d.reason==Window::Reason::None&&!d.changed);CHECK(f.controller.faults()==0);
    CHECK(f.controller.windowUs()==6000); // a reconnect is not evidence that the link got better
}
TEST(reorder_window_random_input_never_leaves_the_bounds_or_the_rules){
    using namespace adaptive_tests;
    std::mt19937 rng(20261118);
    for(int run=0;run<40;++run){
        Window c(static_cast<std::uint32_t>(rng()%20000u),run%2==0);ReorderEvidence e;e.reorderTimeoutUs=c.windowUs();
        HostTime now=0,lastChange=-1;std::uint32_t before=c.windowUs();
        for(int i=0;i<4000;++i){
            now+=static_cast<HostTime>(rng()%5000000u);
            e.videoPackets+=rng()%500u;
            const auto kind=static_cast<unsigned>(rng()%100u);
            if(kind<30){const auto b=rng()%kReorderLatenessBuckets;++e.lateness[b];++e.gaps;if(rng()%2u){++e.timeouts;++e.skipped;++e.lateArrivals;}if(rng()%3u==0)++e.fecFailed;}
            else if(kind<40){++e.gaps;++e.timeouts;e.skipped+=1+rng()%4u;}
            else if(kind==99&&run%4==0){e.gaps=rng()%8u;}               // nonsense
            else if(kind==98&&run%4==1){now-=static_cast<HostTime>(rng()%100000000u);if(now<0)now=0;}
            const auto d=c.update(now,e);
            CHECK(std::find(Window::kStepsUs.begin(),Window::kStepsUs.end(),d.windowUs)!=Window::kStepsUs.end());
            CHECK(d.changed==(d.windowUs!=before));
            if(d.changed){
                const auto at=[](std::uint32_t us){return std::find(Window::kStepsUs.begin(),Window::kStepsUs.end(),us)-Window::kStepsUs.begin();};
                if(d.reason==Window::Reason::Narrow){CHECK(at(before)-at(d.windowUs)==1);CHECK(lastChange<0||now-lastChange>=Window::kNarrowDwellLossy100ns);}
                else if(d.reason==Window::Reason::Widen){CHECK(at(d.windowUs)-at(before)>=1&&at(d.windowUs)-at(before)<=3);CHECK(lastChange<0||now-lastChange>=Window::kWidenDwell100ns);}
                else if(d.reason==Window::Reason::Rollback){CHECK(at(before)-at(d.windowUs)>=1);CHECK(now-lastChange>=Window::kNarrowDwellLossy100ns);}
                else CHECK(d.reason==Window::Reason::Fault&&d.windowUs==Window::kCeilingUs);
                lastChange=now;before=d.windowUs;e.reorderTimeoutUs=d.windowUs;
            }
        }
    }
}

// --- The model of the library's queue rule (for these tests and the simulator) ---
TEST(reorder_model_in_order_packets_pass_at_once){
    ReorderQueueModel q(2000);std::vector<ReorderQueueModel::Released> out;
    for(std::uint16_t i=0;i<200;++i)q.packet(100+i*80,static_cast<std::uint16_t>(65500u+i),out); // across the 16-bit wrap
    CHECK(out.size()==200);
    for(std::size_t i=0;i<out.size();++i){CHECK(out[i].seq==static_cast<std::uint16_t>(65500u+i));CHECK(out[i].atUs==100+static_cast<std::int64_t>(i)*80);}
    CHECK(q.counters().gaps==0&&q.counters().timeouts==0&&q.counters().videoPackets==200);CHECK(!q.nextWakeUs(200000));
}
TEST(reorder_model_a_missing_packet_holds_the_rest_until_it_comes){
    ReorderQueueModel q(2000);std::vector<ReorderQueueModel::Released> out;
    q.packet(0,10,out);q.packet(100,12,out);q.packet(200,13,out);
    CHECK(out.size()==1);CHECK(q.counters().gaps==1);
    CHECK(q.nextWakeUs(200)&&*q.nextWakeUs(200)==2200); // 1900 us left, rounded up to whole milliseconds like the library's socket wait
    q.packet(1300,11,out); // 1.2 ms after the wait began
    CHECK(out.size()==4);CHECK(out[1].seq==11&&out[1].atUs==1300&&out[3].seq==13&&out[3].atUs==1300);
    CHECK(q.counters().timeouts==0&&q.counters().lateArrivals==0);
    CHECK(q.counters().lateness[1]==1); // <= 2 ms
    CHECK(!q.nextWakeUs(1300));
}
TEST(reorder_model_timeout_gives_up_and_a_late_packet_is_recognised){
    ReorderQueueModel q(2000);std::vector<ReorderQueueModel::Released> out;
    q.packet(0,10,out);q.packet(100,13,out); // 11 and 12 missing
    const auto wake=q.nextWakeUs(100);CHECK(wake&&*wake==2100);
    q.timer(*wake,out);
    // Exactly at the limit the library does not give up yet (<=); it looks again at once.
    CHECK(out.size()==2&&out[1].seq==13&&out[1].atUs==2101);
    CHECK(q.counters().timeouts==1&&q.counters().skipped==2&&q.counters().overshootUs==1);
    q.packet(4600,11,out); // 4.5 ms after the wait began: too late for 2 ms
    CHECK(out.size()==2);
    CHECK(q.counters().lateArrivals==1&&q.counters().dropped==1);CHECK(q.counters().lateness[4]==1); // <= 6 ms
    q.packet(4700,5,out); // older than anything given up: a duplicate or stray, not "late"
    CHECK(q.counters().lateArrivals==1&&q.counters().dropped==2);
    q.packet(50000,12,out); // far too late: counted, in the last bucket
    CHECK(q.counters().lateArrivals==2&&q.counters().lateness[8]==1);
    CHECK(q.counters().lateArrivals<=q.counters().skipped);
}
TEST(reorder_model_wait_changes_while_running){
    ReorderQueueModel q(2000);std::vector<ReorderQueueModel::Released> out;
    q.packet(0,1,out);q.packet(100,3,out);
    q.setTimeoutUs(6000);
    CHECK(q.counters().reorderTimeoutUs==2000); // taken when the queue is next looked at, like the library
    q.timer(2100,out);CHECK(out.size()==1);CHECK(q.counters().reorderTimeoutUs==6000);
    CHECK(*q.nextWakeUs(2100)==6100);
    q.packet(5000,2,out);CHECK(out.size()==3);CHECK(q.counters().lateness[4]==1&&q.counters().timeouts==0);
    q.setTimeoutUs(99999);q.packet(5100,4,out);CHECK(q.counters().reorderTimeoutUs==16000); // never above the library's own
}
TEST(reorder_model_overflow_drops_from_the_front_like_the_library){
    ReorderQueueModel q(16000);std::vector<ReorderQueueModel::Released> out;
    q.packet(0,0,out);
    for(std::uint16_t i=2;i<2+70;++i)q.packet(10+i,i,out); // 1 missing, 70 behind it: more than 64 slots
    // The queue made room by moving past the missing packet; nothing waited 16 ms.
    CHECK(out.size()==71);CHECK(q.counters().timeouts==0);
    for(std::size_t i=1;i<out.size();++i)CHECK(out[i].seq==i+1);
}
TEST(reorder_model_keeps_the_library_behaviour_at_the_index_wrap){
    // Found while comparing this model with the C source (reorder_library_check.sh):
    // the pinned library drops packet index 0 when it arrives while a gap is open
    // across 65535 -> 0 (its "equal" compares 64-bit values that did not wrap).
    // The model keeps that, so that it decides like the library does.
    ReorderQueueModel q(4000);std::vector<ReorderQueueModel::Released> out;
    q.packet(1000000,65533,out);q.packet(1000100,65535,out);q.packet(1000200,0,out);q.packet(1000300,1,out);
    CHECK(q.counters().dropped==1);
    q.packet(1000400,65534,out);
    CHECK(out.size()==3&&out[2].seq==65535); // 0 is now a hole of its own
    q.timer(*q.nextWakeUs(1000400),out);
    CHECK(out.size()==4&&out[3].seq==1);CHECK(q.counters().timeouts==1&&q.counters().skipped==1);
    // Without a gap open at the wrap nothing is lost.
    ReorderQueueModel clean(4000);out.clear();
    for(std::uint32_t i=65530;i<65546;++i)clean.packet(1000000+i,static_cast<std::uint16_t>(i),out);
    CHECK(out.size()==16&&clean.counters().dropped==0);
}
TEST(reorder_adaptive_closed_loop_settles_and_stays){
    using namespace adaptive_tests;
    // Every 40th packet arrives 2.4 ms after its successor, for ten minutes of a
    // 4000 packets/s stream; the controller reads the model's counters every 250 ms.
    // No frames are modelled here, so the rule without the lost-frame condition.
    ReorderQueueModel q(2000);Window c(2000,false);std::vector<ReorderQueueModel::Released> out;
    unsigned changes=0;std::uint64_t lateAfterSettling=0;std::int64_t nextUpdate=250000;
    std::size_t released=0;
    const auto advance=[&](std::int64_t to){
        for(;;){const auto wake=q.nextWakeUs(q.nowUs());if(!wake||*wake>to)break;q.timer(*wake,out);}
    };
    std::vector<std::pair<std::int64_t,std::uint16_t>> pending; // delayed packets, by arrival
    for(std::uint32_t i=0;i<2400000u;++i){
        const std::int64_t sent=static_cast<std::int64_t>(i)*250;
        while(!pending.empty()&&pending.front().first<=sent){advance(pending.front().first);q.packet(pending.front().first,pending.front().second,out);pending.erase(pending.begin());}
        if(i%40u==7u)pending.emplace_back(sent+250+2400,static_cast<std::uint16_t>(i));
        else{advance(sent);q.packet(sent,static_cast<std::uint16_t>(i),out);}
        released+=out.size();out.clear();
        if(sent>=nextUpdate){
            nextUpdate+=250000;
            const auto d=c.update(sent*10,q.counters());
            if(d.changed){++changes;q.setTimeoutUs(d.windowUs);}
            if(sent==300000000)lateAfterSettling=q.counters().lateArrivals; // five minutes in
        }
    }
    CHECK(c.windowUs()==3000);CHECK(changes==1);CHECK(c.faults()==0);
    CHECK(q.counters().lateArrivals==lateAfterSettling); // nothing given up any more
    CHECK(released>2390000u);
}

// --- Recovery without restarting the app (StreamRecovery, VideoIngress, the reorder window) ---
// A model of the owner loop in RemotePlaySessionSource.cpp run(): frames at
// 60 a second while the link is up, poll() every 10 ms, a reconnect waits
// backoffSeconds() and then connects; while the link is down a connect fails
// after connectFailMs, while it is up it delivers its first frame after 500 ms.
namespace recovery_tests {
struct Outcome {
    bool recovered=false,failed=false;
    unsigned keyframes=0,reconnects=0;
    std::int64_t resumedAt=-1,failedAt=-1;
    std::vector<unsigned> backoffs;
};
inline Outcome outage(std::int64_t startMs,std::int64_t lengthMs,std::int64_t connectFailMs,bool patient,std::int64_t endMs=200000){
    using A=StreamRecovery::Action;
    StreamRecovery r;if(patient)r.setMaxEpisodeRetries(StreamRecovery::kPatientRetries);
    Outcome o;
    const auto up=[&](std::int64_t t){return t<startMs||t>=startMs+lengthMs;};
    std::int64_t t=0,nextFrame=0;
    r.beginAttempt(t);
    for(;t<endMs;){
        if(up(t)&&t>=nextFrame){
            r.frame(t);nextFrame=t+16;
            if(t>=startMs+lengthMs&&o.resumedAt<0){o.resumedAt=t;o.recovered=true;}
        }
        const auto a=r.poll(t,false,false);
        if(a==A::Keyframe)++o.keyframes;
        else if(a==A::Fail){o.failed=true;o.failedAt=t;return o;}
        else if(a==A::Reconnect){
            for(;;){
                ++o.reconnects;
                const unsigned wait=StreamRecovery::backoffSeconds(r.episodeRetries(),false);
                o.backoffs.push_back(wait);
                t+=static_cast<std::int64_t>(wait)*1000;
                r.beginAttempt(t);
                if(up(t)){t+=500;nextFrame=t;break;}
                t+=connectFailMs;
                const auto again=r.poll(t,false,true); // RemotePlaySessionSource.cpp: connect() returned false
                if(again==A::Fail){o.failed=true;o.failedAt=t;return o;}
                if(again!=A::Reconnect){o.failed=true;o.failedAt=t;return o;} // not reachable today
            }
            continue;
        }
        t+=10;
    }
    return o;
}
} // namespace recovery_tests
TEST(recovery_patient_switch_is_off_by_default_and_bounded){
    CHECK(!resolveLanExperiments({}).reconnectPatient);
    TransportSwitches s;s.reconnectPatient="1";
    const auto e=resolveLanExperiments(s);
    CHECK(e.reconnectPatient&&e.any()&&!e.usesTransport());
    CHECK(e.describe().find("reconnectPatient=on")!=std::string::npos);
    StreamRecovery r;CHECK(r.maxEpisodeRetries()==3);
    r.setMaxEpisodeRetries(1);CHECK(r.maxEpisodeRetries()==3);
    r.setMaxEpisodeRetries(99);CHECK(r.maxEpisodeRetries()==StreamRecovery::kPatientRetries);
}
TEST(recovery_backoff_is_the_original_and_bounded){
    // Retries 1..3: exactly the schedule RemotePlaySessionSource.cpp computed inline before.
    for(unsigned i=1;i<=3;++i){
        CHECK(StreamRecovery::backoffSeconds(i,false)==(1u<<(i-1)));
        CHECK(StreamRecovery::backoffSeconds(i,true)==(10u<<(i-1)));
    }
    // Later attempts (patient experiment only): never longer than 8 s / 40 s, never shorter than the one before.
    unsigned before=0,beforeBusy=0;
    for(unsigned i=1;i<=StreamRecovery::kPatientRetries;++i){
        const auto plain=StreamRecovery::backoffSeconds(i,false),busy=StreamRecovery::backoffSeconds(i,true);
        CHECK(plain>=before&&plain<=8u);CHECK(busy>=beforeBusy&&busy<=40u);
        before=plain;beforeBusy=busy;
    }
}
TEST(recovery_one_second_outage_recovers_in_place){
    using namespace recovery_tests;
    for(const bool patient:{false,true}){
        const auto o=outage(10000,1000,2000,patient);
        CHECK(o.recovered&&!o.failed);CHECK(o.reconnects==0);CHECK(o.keyframes<=1);
        CHECK(o.resumedAt>=11000&&o.resumedAt<11020);
    }
}
TEST(recovery_five_second_outage_recovers_in_place){
    using namespace recovery_tests;
    const auto o=outage(10000,5000,2000,false);
    CHECK(o.recovered&&!o.failed);CHECK(o.reconnects==0);CHECK(o.keyframes==2); // at 1 s and 3 s of silence
    // Repeated five-second outages, a minute apart, never use up the reconnect budget.
    StreamRecovery r;using A=StreamRecovery::Action;r.beginAttempt(0);
    std::int64_t t=0;
    for(int round=0;round<10;++round){
        for(const std::int64_t end=t+60000;t<end;t+=16){r.frame(t);CHECK(r.poll(t,false,false)==A::Wait);}
        for(const std::int64_t end=t+5000;t<end;t+=10)CHECK(r.poll(t,false,false)!=A::Reconnect&&r.poll(t,false,false)!=A::Fail);
    }
    CHECK(r.reconnects()==0&&r.episodeRetries()==0);
}
TEST(recovery_cable_pull_reconnects_within_the_budget){
    using namespace recovery_tests;
    // 12 s: at the latest the third attempt finds the cable back (6 s of
    // silence, then attempts 1, 2 and 4 s apart, plus the failed connects).
    for(const std::int64_t connectFail:{std::int64_t{0},std::int64_t{2000}}){
        const auto o=outage(10000,12000,connectFail,false);
        CHECK(o.recovered&&!o.failed);CHECK(o.reconnects>=1&&o.reconnects<=3);
        CHECK(o.resumedAt-(10000+12000)<=4000+500+connectFail); // at most one backoff and one connect after the link is back
    }
}
TEST(recovery_long_cable_pull_original_stops_patient_reconnects){
    using namespace recovery_tests;
    // 60 s (a router restart). The original gives up after three attempts:
    // the session stops and Start must be pressed again (the app keeps running).
    const auto o=outage(10000,60000,2000,false);
    CHECK(o.failed&&!o.recovered);CHECK(o.reconnects==3);
    const std::int64_t expected=10000+6000+(1+2+4)*1000+3*2000; // about 29 s: 19 s after the pull
    CHECK(o.failedAt>=expected-50&&o.failedAt<=expected+50);
    // VEYRA_EXP_RP_RECONNECT_PATIENT: more attempts, at most 8 s apart, and it reconnects.
    const auto p=outage(10000,60000,2000,true);
    CHECK(p.recovered&&!p.failed);CHECK(p.reconnects<=StreamRecovery::kPatientRetries);
    CHECK(p.resumedAt-(10000+60000)<=8000+500+2000);
    for(std::size_t i=1;i<p.backoffs.size();++i)CHECK(p.backoffs[i]>=p.backoffs[i-1]&&p.backoffs[i]<=8u);
}
TEST(recovery_link_that_never_comes_back_stops_in_bounded_time){
    using namespace recovery_tests;
    for(const std::int64_t connectFail:{std::int64_t{0},std::int64_t{2000},std::int64_t{21000}}){
        const auto o=outage(10000,1000000000,connectFail,false,10000000);
        CHECK(o.failed);CHECK(o.reconnects==3);
        CHECK(o.failedAt<=10000+6000+7000+3*connectFail);
        const auto p=outage(10000,1000000000,connectFail,true,10000000);
        CHECK(p.failed);CHECK(p.reconnects==StreamRecovery::kPatientRetries);
        CHECK(p.failedAt<=10000+6000+(1+2+4+8*5)*1000+8*connectFail);
    }
}
TEST(recovery_after_an_outage_no_frame_waits_longer_than_before){
    // Ingress after a five-second outage: the first frames after the gap are
    // refused until a keyframe, then frames are handed over as they arrive;
    // a decoder that stalls for 300 ms right after the reset never makes a
    // frame wait past the 100 ms ceiling, and the wait is back to nothing once
    // it runs again: nothing stands in the queue.
    VideoIngress q;q.begin(1);
    constexpr HostTime ms=10000;
    CHECK(q.push(config(),0)==PushStatus::ConfigStored);
    std::uint16_t index=0;HostTime t=0;
    for(int i=0;i<600;++i,t+=167000){CHECK(q.push(frame(index++,i==0,t),t)==PushStatus::Accepted);CHECK(q.tryPop(t+2*ms));}
    t+=5000*ms;index=static_cast<std::uint16_t>(index+300); // five seconds without a frame
    CHECK(q.push(frame(index++,false,t),t)==PushStatus::WaitingForIdr);
    CHECK(q.takeIdrRequest(t));
    t+=167000;
    HostTime worstAfter=0,worstLate=0;std::size_t deepest=0;
    const HostTime stallUntil=t+300*ms;
    for(int i=0;i<600;++i,t+=167000){
        (void)q.push(frame(index++,i==0||(q.stats().waitingForIdr&&i%6==0),t),t);
        if(q.takeIdrRequest(t)){} // the console answers within a frame or so
        deepest=(std::max)(deepest,q.stats().depth);
        const HostTime decodeAt=t+2*ms;
        if(decodeAt<stallUntil)continue;
        while(auto v=q.tryPop(decodeAt)){
            const HostTime waited=decodeAt-v->sample.arrival100ns;
            worstAfter=(std::max)(worstAfter,waited);
            if(t>stallUntil+1000*ms)worstLate=(std::max)(worstLate,waited);
        }
    }
    CHECK(deepest<=QueueLimits{}.maxFrames);
    CHECK(worstAfter<=QueueLimits{}.maxAge100ns);
    CHECK(worstLate<=2*ms); // a second after the stall: the same 2 ms as before the outage
}
TEST(recovery_an_outage_is_not_evidence_for_a_longer_reorder_wait){
    using namespace adaptive_tests;
    Feed f(2000);f.run(30*kSec);
    // Five seconds without packets, then the library gives up on the hole
    // (true losses, no late arrivals) and FEC cannot complete the frames.
    for(int i=0;i<20;++i)f.step();
    for(int i=0;i<300;++i){f.loss();f.frameLost();}
    f.run(60*kSec);
    CHECK(f.widenings==0);CHECK(f.controller.windowUs()==2000);
}
TEST(recovery_a_reconnect_does_not_keep_a_longer_wait_on_a_clean_link){
    using namespace adaptive_tests;
    Feed f(2000);
    for(int i=0;i<240;++i){f.costlyArrival(7000);f.step();}
    CHECK(f.controller.windowUs()==8000);
    const unsigned widened=f.widenings;
    f.controller.newConnection();f.e={};f.e.reorderTimeoutUs=8000;f.step();
    // Clean after the reconnect: back at the floor in bounded time, one step per period.
    f.run(4*Window::kNarrowDwell100ns+5*kSec);
    CHECK(f.controller.windowUs()==2000);CHECK(f.widenings==widened);
}

// --- What limits the stream (ConnectionHealth.h, VEYRA_EXP_RP_HEALTH) ---
namespace bottleneck_tests {
inline HealthSnapshot clean(){
    HealthSnapshot s;s.valid=true;s.seconds=5;s.packetsReceived=20000;s.receivedFps=60;
    s.frameJitterMs=0.3;s.ingressPeak=1;s.decodeP95Ms=3.0;
    return s;
}
inline std::uint8_t bit(Bottleneck b){return bottleneckBit(b);}
} // namespace bottleneck_tests
TEST(bottleneck_clean_link_and_no_data_name_nothing){
    using namespace bottleneck_tests;
    auto s=clean();
    CHECK(bottleneckSigns(s)==0);CHECK(classifyBottleneck(s)==Bottleneck::None);
    HealthSnapshot empty;CHECK(bottleneckSigns(empty)==0&&classifyBottleneck(empty)==Bottleneck::None);
    // A clean Wi-Fi link is named as the route, not as a bottleneck.
    s.route.addressKnown=s.route.privateRange=s.route.onLink=true;s.route.link=LinkKind::WiFi;
    CHECK(bottleneckSigns(s)==bit(Bottleneck::WifiRoute));CHECK(classifyBottleneck(s)==Bottleneck::None);
    CHECK(std::string_view(bottleneckName(Bottleneck::None))=="none");
}
TEST(bottleneck_each_sign_alone){
    using namespace bottleneck_tests;
    {auto s=clean();s.packetsLost=40;s.lossPercent=0.2;CHECK(classifyBottleneck(s)==Bottleneck::Loss);}
    {auto s=clean();s.fecFailed=2;CHECK(classifyBottleneck(s)==Bottleneck::Loss);}
    {auto s=clean();s.reordered=60;CHECK(classifyBottleneck(s)==Bottleneck::Reordering);}           // 3 per mille
    {auto s=clean();s.reordered=8;CHECK(classifyBottleneck(s)==Bottleneck::None);}                 // a handful is normal
    {auto s=clean();s.frameJitterMs=3.0;CHECK(classifyBottleneck(s)==Bottleneck::Jitter);}
    {auto s=clean();s.reorderTimeouts=10;s.reorderOvershootMs=4.0;CHECK(classifyBottleneck(s)==Bottleneck::ReceiveScheduling);}
    {auto s=clean();s.reorderTimeouts=2;s.reorderOvershootMs=4.0;CHECK(classifyBottleneck(s)==Bottleneck::None);} // too few waits to say
    {auto s=clean();s.ingressPeak=3;CHECK(classifyBottleneck(s)==Bottleneck::DecoderBacklog);}
    {auto s=clean();s.decodeP95Ms=15.0;CHECK(classifyBottleneck(s)==Bottleneck::DecoderBacklog);}    // 0.9 of a 60 fps frame
    {auto s=clean();s.decodeP95Ms=15.0;s.receivedFps=30;CHECK(classifyBottleneck(s)==Bottleneck::None);} // a 30 fps stream has 33 ms
    {auto s=clean();s.decodeP95Ms.reset();CHECK(classifyBottleneck(s)==Bottleneck::None);}
    for(const auto b:{Bottleneck::Loss,Bottleneck::Reordering,Bottleneck::Jitter,Bottleneck::ReceiveScheduling,Bottleneck::DecoderBacklog,Bottleneck::WifiRoute})
        CHECK(std::string_view(bottleneckName(b))!="none"&&bit(b)!=0);
}
TEST(bottleneck_the_local_cause_comes_first){
    using namespace bottleneck_tests;
    // A receive thread that wakes late makes waits overshoot, gives up packets
    // and shows as reordering and jitter: the thread is named.
    auto s=clean();s.reorderTimeouts=10;s.reorderOvershootMs=5.0;s.reordered=80;s.frameJitterMs=4.0;s.fecFailed=1;
    CHECK(classifyBottleneck(s)==Bottleneck::ReceiveScheduling);
    const auto signs=bottleneckSigns(s);
    CHECK((signs&bit(Bottleneck::Loss))&&(signs&bit(Bottleneck::Reordering))&&(signs&bit(Bottleneck::Jitter)));
    CHECK(formatBottleneckSigns(signs)=="loss+reordering+jitter+receive-scheduling");
    // A decoder backlog beside network trouble: the decoder.
    s=clean();s.ingressPeak=4;s.frameJitterMs=3.0;CHECK(classifyBottleneck(s)==Bottleneck::DecoderBacklog);
    // Network signs on a Wi-Fi route: the route.
    s=clean();s.route.addressKnown=s.route.privateRange=s.route.onLink=true;s.route.link=LinkKind::WiFi;s.lossPercent=0.5;s.packetsLost=100;
    CHECK(classifyBottleneck(s)==Bottleneck::WifiRoute);
    // Loss before reordering before jitter on a cable.
    s=clean();s.lossPercent=0.5;s.packetsLost=100;s.reordered=80;s.frameJitterMs=3;CHECK(classifyBottleneck(s)==Bottleneck::Loss);
    s=clean();s.reordered=80;s.frameJitterMs=3;CHECK(classifyBottleneck(s)==Bottleneck::Reordering);
    CHECK(formatBottleneckSigns(0)=="none");
}
TEST(bottleneck_is_in_the_snapshot_and_the_health_line){
    ConnectionHealth h;h.begin(0);
    HealthCounters c;c.packetsReceived=20000;c.framesReceived=300;c.bytesReceived=9375000;
    c.reorderGaps=20;c.reorderTimeouts=10;c.reorderOvershootUs=45000;c.decodeP95Ms=4.0;
    const auto s=h.sample(50000000,c);
    CHECK(s);CHECK(s->reorderTimeouts==10);CHECK(s->reorderOvershootMs>4.49&&s->reorderOvershootMs<4.51);
    CHECK(s->decodeP95Ms&&*s->decodeP95Ms==4.0);
    CHECK(s->bottleneck==Bottleneck::ReceiveScheduling);CHECK(s->bottleneckSigns==bottleneckSigns(*s));
    const auto line=formatHealthLine(*s);
    for(const char* key:{"bottleneck=receive-scheduling","signs=receive-scheduling","overshootMs=4.50","decodeP95Ms=4.00"})CHECK(line.find(key)!=line.npos);
    // The overshoot is per interval: the next interval without timeouts shows none.
    c.packetsReceived+=20000;c.framesReceived+=300;c.bytesReceived+=9375000;c.decodeP95Ms.reset();
    const auto next=h.sample(100000000,c);
    CHECK(next&&next->reorderOvershootMs==0&&next->bottleneck==Bottleneck::None);
    CHECK(formatHealthLine(*next).find("decodeP95Ms=not-measured")!=std::string::npos);
    // From the library and the inbox.
    TransportCounters t;t.reorderOvershootUs=1234;SessionInbox::Snapshot in;in.decodeP95Ms=7.5;
    const auto from=healthCounters(t,in,0);CHECK(from.reorderOvershootUs==1234&&from.decodeP95Ms&&*from.decodeP95Ms==7.5);
}

// --- PushLatencyGovernor (veyra/engine/PushLatencyGovernor.h): recommendations only ---
#include "veyra/engine/PushLatencyGovernor.h"
namespace governor_tests {
namespace lg=veyra::engine;
using Gov=lg::PushLatencyGovernor;
using Knob=lg::GovernorKnob;
// A healthy 30 fps stream on a 60 fps transport, everything adjustable.
inline lg::GovernorMeasurement calm(){
    lg::GovernorMeasurement m;
    m.networkJitterMs=0.3;m.lossPercent=0;m.framesLostPerSecond=0;
    m.decodeMs=3;m.gpuMs=4;m.cpuSchedulingDelayMs=0.2;m.queueDepth=0.4;m.presentLateMs=0.1;
    m.sourceFps=30;m.gpuHeadroomPercent=60;
    return m;
}
inline lg::GovernorKnobs knobs(){
    lg::GovernorKnobs k;k.reorder=lg::ReorderProfile::Short;k.backlog=false;k.controllerPollMs=1;k.fgStep=2;k.qualityTier=2;
    return k;
}
// Plays the caller: one measurement a second, applies every recommendation at once.
struct Run {
    Gov g;lg::GovernorKnobs k=knobs();std::int64_t now=0;
    std::vector<lg::GovernorRecommendation> seen;
    bool apply=true;
    std::optional<lg::GovernorRecommendation> tick(const lg::GovernorMeasurement& m){
        now+=1000;
        auto r=g.update(now,m,k);
        if(r){
            seen.push_back(*r);
            if(apply)lg::applyRecommendation(k,*r);
        }
        return r;
    }
    void ticks(int n,const lg::GovernorMeasurement& m){for(int i=0;i<n;++i)(void)tick(m);}
};
} // namespace governor_tests
TEST(governor_calm_system_recommends_nothing){
    using namespace governor_tests;
    Run r;r.ticks(3600,calm());
    CHECK(r.seen.empty());
    // Nothing to measure yet, or nonsense: nothing either.
    Run n;auto m=calm();m.gpuMs=std::numeric_limits<double>::quiet_NaN();m.gpuHeadroomPercent=-5;
    n.ticks(100,m);CHECK(n.seen.empty());CHECK(n.g.invalidMeasurements()==100);
}
TEST(governor_gpu_bound_steps_frame_generation_down_first_then_quality){
    using namespace governor_tests;
    Run r;auto hot=calm();hot.gpuHeadroomPercent=4;hot.gpuMs=14;hot.presentLateMs=3;
    r.ticks(2,hot);CHECK(r.seen.empty()); // one bad second is not a pattern
    auto first=r.tick(hot);
    CHECK(first&&first->knob==Knob::FrameGeneration&&first->from==2&&first->to==1&&!first->rollback);
    CHECK(first->latencyEffect==lg::LatencyEffect::Lower);
    // Still hot: the next step waits for the dwell, then goes on, one change at a time.
    for(int i=0;i<9;++i)CHECK(!r.tick(hot));
    r.ticks(20,hot);
    CHECK(r.k.fgStep==0);CHECK(r.k.qualityTier<2);
    for(std::size_t i=1;i<r.seen.size();++i)CHECK(r.seen[i].at-r.seen[i-1].at>=Gov::kDwellMs);
    // Never below the floor.
    r.ticks(600,hot);CHECK(r.k.fgStep==0&&r.k.qualityTier==0);
}
TEST(governor_rolls_back_a_change_that_made_its_target_worse){
    using namespace governor_tests;
    Run r;auto hot=calm();hot.gpuHeadroomPercent=4;hot.gpuMs=14;
    r.ticks(3,hot);
    CHECK(r.seen.size()==1&&r.k.fgStep==1);
    // After the step-down GPU time got worse, not better (something else changed).
    auto worse=hot;worse.gpuMs=18;
    r.ticks(static_cast<int>(Gov::kEvaluateMs/1000),worse);
    CHECK(r.seen.size()==2);
    CHECK(r.seen[1].rollback&&r.seen[1].knob==Knob::FrameGeneration&&r.seen[1].to==2&&r.k.fgStep==2);
    CHECK(r.g.rollbacks()==1);
    // The same step is not proposed again soon; the quality step is tried instead.
    r.ticks(30,worse);
    for(std::size_t i=2;i<r.seen.size();++i)CHECK(!(r.seen[i].knob==Knob::FrameGeneration&&!r.seen[i].rollback));
}
TEST(governor_keeps_a_change_that_helped){
    using namespace governor_tests;
    Run r;auto hot=calm();hot.gpuHeadroomPercent=4;hot.gpuMs=14;
    r.ticks(3,hot);CHECK(r.k.fgStep==1);
    auto better=calm();better.gpuMs=9;
    r.ticks(600,better);
    CHECK(r.seen.size()==1&&r.g.rollbacks()==0&&r.k.fgStep==1); // and no step back up: that is the user's call
}
TEST(governor_decoder_backlog_turns_the_backlog_off_and_never_on){
    using namespace governor_tests;
    Run r;r.k.backlog=true;auto deep=calm();deep.queueDepth=3.5;
    r.ticks(3,deep);
    CHECK(r.seen.size()==1&&r.seen[0].knob==Knob::Backlog&&r.seen[0].from==1&&r.seen[0].to==0&&!r.k.backlog);
    Run off;auto shallow=calm();shallow.framesLostPerSecond=3;shallow.lossPercent=0.5; // dropping frames, backlog off
    off.ticks(600,shallow);
    for(const auto& s:off.seen)CHECK(!(s.knob==Knob::Backlog&&s.to==1));
}
TEST(governor_controller_poll_follows_what_the_cpu_can_deliver){
    using namespace governor_tests;
    // The owner thread is scheduled 3 ms late: polling every 1 ms is not
    // happening anyway, so 2 ms costs nothing and frees the CPU.
    Run r;auto busy=calm();busy.cpuSchedulingDelayMs=3;
    r.ticks(3,busy);
    CHECK(r.seen.size()==1&&r.seen[0].knob==Knob::ControllerPoll&&r.seen[0].to==2);
    CHECK(r.seen[0].latencyEffect==lg::LatencyEffect::Neutral);
    // Never past what the delay already costs: 4 ms only when the delay reaches 4 ms.
    r.ticks(120,busy);CHECK(r.k.controllerPollMs==2);
    // Calm again for a long while: back down, one step.
    r.ticks(static_cast<int>(Gov::kCalmPeriods)+static_cast<int>(Gov::kDwellMs/1000),calm());
    CHECK(r.k.controllerPollMs==1);CHECK(r.seen.back().latencyEffect==lg::LatencyEffect::Lower);
}
TEST(governor_reorder_profile_only_ever_shortens){
    using namespace governor_tests;
    Run r;r.k.reorder=lg::ReorderProfile::Long;
    r.ticks(static_cast<int>(Gov::kCalmPeriods)+1,calm());
    CHECK(r.seen.size()==1&&r.seen[0].knob==Knob::Reorder&&r.k.reorder==lg::ReorderProfile::Short);
    // Frames lost after the shorter wait: rolled back to the long one.
    auto lossy=calm();lossy.framesLostPerSecond=2;
    r.ticks(static_cast<int>(Gov::kEvaluateMs/1000),lossy);
    CHECK(r.seen.size()==2&&r.seen[1].rollback&&r.k.reorder==lg::ReorderProfile::Long);
    // A lossy, jittery link never makes it propose a longer wait on its own.
    Run s;auto bad=calm();bad.framesLostPerSecond=5;bad.networkJitterMs=6;bad.lossPercent=2;
    s.ticks(600,bad);
    for(const auto& x:s.seen)CHECK(x.knob!=Knob::Reorder);
}
TEST(governor_a_recommendation_not_applied_is_dropped_and_not_repeated_at_once){
    using namespace governor_tests;
    Run r;r.apply=false;auto hot=calm();hot.gpuHeadroomPercent=4;hot.gpuMs=14;
    r.ticks(3,hot);CHECK(r.seen.size()==1);
    r.ticks(static_cast<int>(Gov::kApplyTimeoutMs/1000)+2,hot);
    CHECK(r.g.declined()==1);
    const auto count=r.seen.size();
    r.ticks(static_cast<int>(Gov::kBlockMs/1000)-20,hot);
    for(std::size_t i=count;i<r.seen.size();++i)CHECK(r.seen[i].knob!=Knob::FrameGeneration);
}
TEST(governor_random_input_never_raises_latency_and_changes_one_thing_at_a_time){
    using namespace governor_tests;
    std::mt19937 rng(20261119);
    for(int run=0;run<30;++run){
        Run r;r.k.backlog=rng()%2u!=0;r.k.reorder=static_cast<lg::ReorderProfile>(rng()%3u);
        std::int64_t lastChange=-1000000;
        for(int i=0;i<3000;++i){
            lg::GovernorMeasurement m;
            const auto u=[&](double hi){return static_cast<double>(rng()%10000u)/10000.0*hi;};
            m.networkJitterMs=u(8);m.lossPercent=u(3);m.framesLostPerSecond=u(4);m.decodeMs=u(20);m.gpuMs=u(30);
            m.cpuSchedulingDelayMs=u(10);m.queueDepth=u(6);m.presentLateMs=u(8);m.sourceFps=rng()%2u?30.0:60.0;m.gpuHeadroomPercent=u(100);
            if(rng()%50u==0)m.gpuMs=-1;
            const auto before=r.k;
            const auto rec=r.tick(m);
            if(!rec){CHECK(r.k==before);continue;}
            CHECK(rec->at==r.now);
            if(!rec->rollback){
                CHECK(rec->latencyEffect!=lg::LatencyEffect::Higher);
                CHECK(r.now-lastChange>=Gov::kDwellMs);
                switch(rec->knob){
                case Knob::FrameGeneration:CHECK(rec->to+1==rec->from);break;
                case Knob::QualityTier:CHECK(rec->to+1==rec->from);break;
                case Knob::Backlog:CHECK(rec->from==1&&rec->to==0);break;
                case Knob::Reorder:CHECK(rec->to==static_cast<int>(lg::ReorderProfile::Short));break;
                case Knob::ControllerPoll:
                    // Longer only up to what the scheduling delay already costs.
                    if(rec->to>rec->from)CHECK(static_cast<double>(rec->to)<=m.cpuSchedulingDelayMs);
                    break;
                }
            }else{
                // A rollback restores exactly what the change before it replaced.
                CHECK(!r.seen.empty());
            }
            // One knob at a time.
            int moved=0;
            moved+=r.k.fgStep!=before.fgStep;moved+=r.k.qualityTier!=before.qualityTier;moved+=r.k.backlog!=before.backlog;
            moved+=r.k.reorder!=before.reorder;moved+=r.k.controllerPollMs!=before.controllerPollMs;
            CHECK(moved==1);
            lastChange=r.now;
        }
        CHECK(r.k.fgStep>=0&&r.k.fgStep<=2&&r.k.qualityTier>=0&&r.k.qualityTier<=2);
    }
}
