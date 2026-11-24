// SPDX-License-Identifier: GPL-3.0-only
// Super-resolution statistics, level experiments and autorun plans (veyra/engine/PushSrPlan.h).
// Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushSrPlan.h"
#include "veyra/engine/PushHookHold.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
namespace {
namespace srp=veyra::engine;
constexpr std::int64_t kSrSecond=10000000;
}

TEST(sr_level_precedence_autorun_then_switch_then_budget_then_user){
    CHECK(srp::pushSrEffectiveLevel(3,0,0,0).level==3);CHECK(srp::pushSrEffectiveLevel(3,0,0,0).source==srp::PushSrLevelSource::User);
    CHECK(srp::pushSrEffectiveLevel(3,4,1,1).level==4);CHECK(srp::pushSrEffectiveLevel(3,4,1,1).source==srp::PushSrLevelSource::Autorun);
    CHECK(srp::pushSrEffectiveLevel(3,0,4,1).level==4);CHECK(srp::pushSrEffectiveLevel(3,0,4,1).source==srp::PushSrLevelSource::Switch);
    CHECK(srp::pushSrEffectiveLevel(3,0,0,1).level==1);CHECK(srp::pushSrEffectiveLevel(3,0,0,1).source==srp::PushSrLevelSource::Budget);
    // The budget never goes above the user's level.
    CHECK(srp::pushSrEffectiveLevel(2,0,0,4).level==2);CHECK(srp::pushSrEffectiveLevel(2,0,0,4).source==srp::PushSrLevelSource::User);
    // Invalid requests are ignored.
    CHECK(srp::pushSrEffectiveLevel(2,5,9,0).level==2);
}
TEST(sr_level_experiments_never_touch_other_choices){
    for(std::uint32_t q:{0u,5u,6u,16u,19u,21u,23u}){
        CHECK(srp::pushSrEffectiveLevel(q,4,4,1).level==q);
        CHECK(srp::pushSrEffectiveLevel(q,4,4,1).source==srp::PushSrLevelSource::User);
    }
}
TEST(sr_switch_parsing_is_strict){
    CHECK(srp::pushSrLevelSwitch("1")==1);CHECK(srp::pushSrLevelSwitch("4")==4);
    CHECK(srp::pushSrLevelSwitch("")==0);CHECK(srp::pushSrLevelSwitch("0")==0);CHECK(srp::pushSrLevelSwitch("5")==0);
    CHECK(srp::pushSrLevelSwitch("2x")==0);CHECK(srp::pushSrLevelSwitch(" 2")==0);
    CHECK(srp::pushSrBudgetSwitch("3")==3.0);CHECK(srp::pushSrBudgetSwitch("2.5")==2.5);CHECK(srp::pushSrBudgetSwitch("0.5")==0.5);
    CHECK(srp::pushSrBudgetSwitch("0.4")==0.0);CHECK(srp::pushSrBudgetSwitch("51")==0.0);CHECK(srp::pushSrBudgetSwitch("")==0.0);
    CHECK(srp::pushSrBudgetSwitch("1.2.3")==0.0);CHECK(srp::pushSrBudgetSwitch("-1")==0.0);CHECK(srp::pushSrBudgetSwitch(".")==0.0);
    CHECK(srp::pushSrBudgetSwitch("1e3")==0.0);
}
TEST(sr_level_request_is_one_process_wide_value){
    auto& r=srp::pushSrLevelRequest();const auto before=r.load();
    r.store(3);CHECK(srp::pushSrLevelRequest().load()==3);
    r.store(before);
}
TEST(sr_window_percentiles_nearest_rank_and_bounds){
    srp::PushSrWindow w;
    CHECK(w.percentile(50)==-1.0);CHECK(w.max()==-1.0);CHECK(w.frames()==0);
    for(int i=1;i<=100;++i)w.add(double(i)/10.0);   // 0.1 .. 10.0
    CHECK(w.frames()==100);
    CHECK(w.percentile(50)==5.0);CHECK(w.percentile(95)==9.5);CHECK(w.percentile(100)==10.0);CHECK(w.percentile(0)==0.1);
    CHECK(w.max()==10.0);
    w.add(-1.0);w.add(1.0e9);
    CHECK(w.frames()==100);CHECK(w.rejected()==2);
    w.clear();CHECK(w.frames()==0);CHECK(w.percentile(95)==-1.0);
}
TEST(sr_window_counts_past_capacity_without_storing){
    srp::PushSrWindow w;
    for(std::size_t i=0;i<srp::PushSrWindow::kCapacity+10;++i)w.add(1.0);
    w.add(7.0);
    CHECK(w.frames()==srp::PushSrWindow::kCapacity+11);CHECK(w.max()==7.0);CHECK(w.percentile(100)==1.0);
}
TEST(sr_stats_line_has_the_contract_keys_in_order){
    srp::PushSrWindow w;for(int i=0;i<10;++i)w.add(2.0);
    srp::PushSrStatsInfo i;i.choice=2;i.quality=2;i.inW=1920;i.inH=1080;i.outW=3840;i.outH=2160;i.workW=3840;i.workH=2160;
    const auto line=srp::pushSrStatsLine(w,i);
    CHECK(line.rfind("choice=2 quality=2 in=1920x1080 out=3840x2160 gpuP50=2.000 gpuP95=2.000 frames=10 generatedToo=0 ",0)==0);
    CHECK(line.find("levelSource=user")!=std::string::npos);CHECK(line.find("work=3840x2160")!=std::string::npos);
    CHECK(line.find(' ',line.size()-1)==std::string::npos);
    srp::PushSrStatsInfo j=i;CHECK(i.equals(j));j.quality=3;CHECK(!i.equals(j));
    srp::PushSrWindow empty;const auto none=srp::pushSrStatsLine(empty,i);
    CHECK(none.find("gpuP50=-1.000 gpuP95=-1.000 frames=0")!=std::string::npos);
}
TEST(sr_budget_off_does_nothing){
    srp::PushSrBudget b;
    CHECK(!b.enabled());
    CHECK(!b.observe(50.0,300,4,kSrSecond));CHECK(b.level()==0);
}
TEST(sr_budget_steps_down_after_two_windows_and_spaces_changes){
    srp::PushSrBudgetConfig c;c.budgetMs=3.0;srp::PushSrBudget b(c);
    std::int64_t t=kSrSecond;
    CHECK(!b.observe(5.0,300,4,t));t+=5*kSrSecond;            // one window over: not yet
    CHECK(b.observe(5.0,300,4,t));CHECK(b.level()==3);t+=5*kSrSecond;
    CHECK(!b.observe(5.0,300,4,t));t+=5*kSrSecond;            // counter restarted
    CHECK(b.observe(5.0,300,4,t));CHECK(b.level()==2);
    // Two over-windows 2 s apart: the second is refused by the 10 s spacing.
    t+=2*kSrSecond;CHECK(!b.observe(5.0,300,4,t));t+=1*kSrSecond;CHECK(!b.observe(5.0,300,4,t));CHECK(b.level()==2);
    t+=10*kSrSecond;CHECK(b.observe(5.0,300,4,t));CHECK(b.level()==1);
    // At the floor it says so and changes nothing.
    t+=20*kSrSecond;CHECK(!b.observe(9.0,300,4,t));CHECK(b.overAtFloor());CHECK(b.level()==1);
}
TEST(sr_budget_ignores_thin_windows_and_the_band_between){
    srp::PushSrBudgetConfig c;c.budgetMs=3.0;srp::PushSrBudget b(c);
    std::int64_t t=kSrSecond;
    for(int i=0;i<5;++i){CHECK(!b.observe(9.0,10,4,t));t+=5*kSrSecond;}   // too few frames
    CHECK(b.level()==0);
    // Over, then in the band (2.5 ms: under budget, above 60 %): the over count restarts.
    CHECK(!b.observe(4.0,300,4,t));t+=5*kSrSecond;CHECK(!b.observe(2.5,300,4,t));t+=5*kSrSecond;CHECK(!b.observe(4.0,300,4,t));
    CHECK(b.level()==0);
}
TEST(sr_budget_steps_up_slowly_never_above_the_user){
    srp::PushSrBudgetConfig c;c.budgetMs=3.0;srp::PushSrBudget b(c);
    std::int64_t t=kSrSecond;
    b.observe(5.0,300,3,t);t+=5*kSrSecond;CHECK(b.observe(5.0,300,3,t));CHECK(b.level()==2);
    const std::int64_t down=t;
    // Cheap windows: no step up within 60 s of the step down.
    int ups=0;
    for(int i=0;i<30;++i){t+=5*kSrSecond;if(b.observe(1.0,300,3,t)){++ups;CHECK(t-down>=c.upLockout100ns);}}
    CHECK(ups==1);CHECK(b.level()==0);   // back at the user's level 3, never 4
    CHECK(srp::pushSrEffectiveLevel(3,0,0,b.level()).level==3);
}
TEST(sr_budget_resets_when_the_user_chooses_again){
    srp::PushSrBudgetConfig c;c.budgetMs=3.0;srp::PushSrBudget b(c);
    std::int64_t t=kSrSecond;
    b.observe(5.0,300,4,t);t+=5*kSrSecond;b.observe(5.0,300,4,t);CHECK(b.level()==3);
    t+=5*kSrSecond;b.observe(1.0,300,2,t);CHECK(b.level()==0);   // new ceiling 2
    t+=5*kSrSecond;b.observe(1.0,300,6,t);CHECK(b.level()==0);   // NIS: not touched
}
TEST(sr_plan_order_and_restore_fields){
    srp::PushSrUserState user;user.enabled=true;user.quality=4;user.targetIndex=2;
    srp::PushSrAvailable a;a.rtxStandard=true;a.nis=true;a.fsr=true;a.displayTargetIndex=1;
    const auto plan=srp::pushSrPlan(user,a);
    CHECK(plan.size()==1+1+1+4+4);
    CHECK(plan[0].label=="off");CHECK(!plan[0].enabled);
    CHECK(plan[1].label=="nis");CHECK(plan[1].quality==6);CHECK(plan[1].targetIndex==2);
    CHECK(plan[2].label=="fsr");CHECK(plan[2].quality==5);
    for(std::uint32_t l=1;l<=4;++l){CHECK(plan[2+l].levelRequest==l);CHECK(plan[2+l].targetIndex==2);CHECK(plan[2+l].quality==2);}
    for(std::uint32_t l=1;l<=4;++l){CHECK(plan[6+l].levelRequest==l);CHECK(plan[6+l].targetIndex==1);CHECK(plan[6+l].label=="rtx"+std::to_string(l)+"-window");}
}
TEST(sr_plan_without_rtx_or_with_the_window_target_already_chosen){
    srp::PushSrUserState user;user.targetIndex=1;
    srp::PushSrAvailable a;a.nis=true;a.displayTargetIndex=1;
    auto plan=srp::pushSrPlan(user,a);
    CHECK(plan.size()==2);
    a.rtxStandard=true;plan=srp::pushSrPlan(user,a);
    CHECK(plan.size()==6);   // the window target equals the user's: no second set
    user.targetIndex=0;plan=srp::pushSrPlan(user,a);CHECK(plan[1].targetIndex==2);
}
TEST(sr_target_for_window_picks_the_smallest_cover){
    CHECK(srp::pushSrTargetForWindow(2560,1440)==1);CHECK(srp::pushSrTargetForWindow(1920,1080)==1);
    CHECK(srp::pushSrTargetForWindow(3440,1440)==2);CHECK(srp::pushSrTargetForWindow(3840,2160)==2);
    CHECK(srp::pushSrTargetForWindow(5120,1440)==4);CHECK(srp::pushSrTargetForWindow(0,0)==0);
    CHECK(srp::pushSrTargetForWindow(9000,5000)==0);
}
TEST(fg_plan_off_first_then_each_multiplier){
    CHECK((srp::pushFgPlan({2,3,4,6})==std::vector<std::uint32_t>{1,2,3,4,6}));
    CHECK((srp::pushFgPlan({4,2,2,1})==std::vector<std::uint32_t>{1,2,4}));
    CHECK(srp::pushFgPlan({}).empty());CHECK(srp::pushFgPlan({1}).empty());
}
TEST(fg_stats_line_and_pair_delay_model){
    CHECK(srp::pushFgPairDelayMs(1,60.0)==0.0);CHECK(srp::pushFgPairDelayMs(2,0.0)==-1.0);
    CHECK(srp::pushFgPairDelayMs(2,50.0)==20.0);
    srp::PushFgStats s;s.method=7;s.multiplier=2;s.tier=1;s.sourceFps=30;s.outputFps=60;s.generatedShown=120;
    const auto line=srp::pushFgStatsLine(s);
    CHECK(line.rfind("method=7 multiplier=2 tier=1 sourceFps=30.00 outputFps=60.00 generatedShown=120 generatedExpired=-1 skippedPairs=-1 costP50=-1.000",0)==0);
    CHECK(line.find("pairDelayMs=33.333")!=std::string::npos);
}
TEST(step_reason_and_seconds_parsing){
    CHECK(srp::pushStepReason("No source")=="no-source");CHECK(srp::pushStepReason("  ")=="failed");
    CHECK(srp::pushStepReason("timeout")=="timeout");CHECK(srp::pushStepReason("请先连接画面")=="failed");
    CHECK(srp::pushStepReason(std::string(80,'a')).size()==32);
    CHECK(srp::pushStepSeconds("20",10)==20);CHECK(srp::pushStepSeconds("2",10)==10);CHECK(srp::pushStepSeconds("601",10)==10);
    CHECK(srp::pushStepSeconds("",15)==15);CHECK(srp::pushStepSeconds("1a",15)==15);
}
// The review's adversarial model (exp/s4-sr review): one 5 s window of EnhanceGraph::noteSrTimings' budget glue,
// with the GPU cost of the SR stage per evaluated level. Returns the level evaluated after the window.
namespace {
std::uint32_t srBudgetWindow(srp::PushSrBudget& b,std::atomic<std::uint32_t>& shared,std::uint32_t configured,const double cost[5],std::int64_t t){
    const auto eff=srp::pushSrEffectiveLevel(configured,0,0,shared.load());
    const bool steerable=eff.source==srp::PushSrLevelSource::User||eff.source==srp::PushSrLevelSource::Budget;
    if(steerable)srp::pushSrBudgetObserve(b,shared,cost[eff.level],300,configured,t);
    return srp::pushSrEffectiveLevel(configured,0,0,shared.load()).level;
}
}
TEST(sr_budget_shared_level_follows_a_new_user_choice){
    srp::PushSrBudgetConfig c;c.budgetMs=3.0;srp::PushSrBudget b(c);std::atomic<std::uint32_t> shared{0};
    std::int64_t t=kSrSecond;
    const double heavy[5]={0,1.0,2.0,4.0,5.0};
    for(int i=0;i<4;++i){srBudgetWindow(b,shared,4,heavy,t);t+=5*kSrSecond;}
    CHECK(shared.load()==2);CHECK(b.level()==2);   // the budget took 4 -> 2
    // The user picks 3 and the GPU is idle: level 3 is evaluated from the next window on (was: still 2 after 200 s).
    const double idle[5]={0,0.5,0.5,0.5,0.5};
    std::uint32_t evaluated=0;
    evaluated=srBudgetWindow(b,shared,3,idle,t);t+=5*kSrSecond;
    CHECK(evaluated==3);CHECK(shared.load()==0);
    for(int i=0;i<40;++i){evaluated=srBudgetWindow(b,shared,3,idle,t);t+=5*kSrSecond;}
    CHECK(evaluated==3);CHECK(b.changes()==2);
}
TEST(sr_budget_backoff_stops_cycling_across_the_budget){
    // Level 4 costs 4 ms (over a 3 ms budget), level 3 costs 1.5 ms (under 60 %): without the back-off this cycled
    // 103 times an hour, 10 s over budget each time.
    srp::PushSrBudgetConfig c;c.budgetMs=3.0;srp::PushSrBudget b(c);std::atomic<std::uint32_t> shared{0};
    const double edge[5]={0,1.0,1.2,1.5,4.0};
    std::int64_t t=kSrSecond;
    std::uint32_t prev=4;
    const auto hour=[&](unsigned& changes,unsigned& overWindows){
        changes=overWindows=0;
        for(int i=0;i<720;++i){
            if(edge[srp::pushSrEffectiveLevel(4,0,0,shared.load()).level]>c.budgetMs)++overWindows;
            const auto l=srBudgetWindow(b,shared,4,edge,t);
            if(l!=prev){++changes;prev=l;}
            t+=5*kSrSecond;
        }
    };
    unsigned changes=0,overWindows=0;
    hour(changes,overWindows);
    CHECK(changes<=13);CHECK(overWindows<=14);   // model, 2026-11-20: 13 changes, 14 windows (70 s) over budget
    CHECK(b.backoff()==c.maxBackoffShift);CHECK(b.upLockout100ns()==c.upLockout100ns*16);
    hour(changes,overWindows);
    CHECK(changes<=8);CHECK(overWindows<=8);     // then one try every ~16 min: 8 changes, 40 s over budget an hour
    // A step up that then holds clears the back-off: level 4 becomes cheap.
    const double cheap[5]={0,0.5,0.5,0.5,1.0};
    for(int i=0;i<720;++i){srBudgetWindow(b,shared,4,cheap,t);t+=5*kSrSecond;}
    CHECK(shared.load()==0);CHECK(b.backoff()==0);
}
TEST(sr_budget_never_raises_above_the_user_level){
    // Pseudo-random costs and user choices for six simulated hours: the evaluated level never exceeds the user's.
    srp::PushSrBudgetConfig c;c.budgetMs=2.0;srp::PushSrBudget b(c);std::atomic<std::uint32_t> shared{0};
    std::uint32_t seed=12345u,configured=4;std::int64_t t=kSrSecond;bool ok=true;unsigned steps=0;
    for(int i=0;i<4320;++i){
        seed=seed*1664525u+1013904223u;
        if((seed>>24)<4)configured=1u+((seed>>8)%4u);
        double cost[5];
        for(int l=0;l<5;++l){seed=seed*1664525u+1013904223u;cost[l]=double(l)*0.4+double((seed>>16)%300u)/100.0;}
        const auto before=b.changes();
        const auto evaluated=srBudgetWindow(b,shared,configured,cost,t);
        steps+=b.changes()-before;
        if(evaluated>configured||evaluated<1)ok=false;
        t+=5*kSrSecond;
    }
    CHECK(ok);CHECK(steps>0);
}
TEST(fg_restore_puts_back_the_kept_multiplier_when_off){
    const std::vector<std::uint32_t> holds{1,2,3,4};
    CHECK((srp::pushFgRestoreSteps(3,2,holds)==std::vector<std::uint32_t>{3}));      // on: its multiplier
    CHECK((srp::pushFgRestoreSteps(1,3,holds)==std::vector<std::uint32_t>{3,1}));    // off with 3 kept: on at 3, then off
    CHECK((srp::pushFgRestoreSteps(1,0,holds)==std::vector<std::uint32_t>{1}));      // nothing kept
    CHECK((srp::pushFgRestoreSteps(1,1,holds)==std::vector<std::uint32_t>{1}));
    CHECK((srp::pushFgRestoreSteps(1,6,holds)==std::vector<std::uint32_t>{1}));      // not a choice now: never refused
}
TEST(hook_hold_suppresses_the_session_save_only_while_set){
    // Model of QmlPlayerBridge_p.h persistSession: the file keeps the last state saved before the hold.
    auto& active=srp::pushHookHoldActive();
    CHECK(!active.load());CHECK(srp::pushSessionSaveAllowed());
    int file=4,settings=4;   // the user's SR level, in the file and in force
    const auto save=[&]{if(srp::pushSessionSaveAllowed())file=settings;};
    active.store(true);
    settings=2;save();        // a hold changes the settings; the save is skipped
    CHECK(file==4);           // a crash here leaves the user's choice in the file
    active.store(false);      // the step ends: flag cleared first, then the restore is saved
    settings=4;save();CHECK(file==4);
    CHECK(srp::pushSessionSaveAllowed());
}
TEST(sr_budget_tries_a_level_that_fits_after_the_lockout){
    // The delta2 review's model: level 3 costs 2.0 ms (above 60 % of the 3 ms budget), level 4 costs 2.8 ms (fits).
    // One 15 s spike took it to level 3, where it then stayed for 6 hours. Now it tries level 4 once the full
    // lockout has passed and stays there.
    srp::PushSrBudgetConfig c;c.budgetMs=3.0;srp::PushSrBudget b(c);std::atomic<std::uint32_t> shared{0};
    const double spike[5]={0,1.0,1.5,2.0,3.5},band[5]={0,1.0,1.5,2.0,2.8};
    std::int64_t t=kSrSecond,down=0;
    for(int i=0;i<3;++i){if(srBudgetWindow(b,shared,4,spike,t)==3&&!down)down=t;t+=5*kSrSecond;}
    CHECK(down!=0);CHECK(shared.load()==3);
    std::int64_t back=-1;
    for(int i=0;i<720*6;++i){
        const auto l=srBudgetWindow(b,shared,4,band,t);
        if(l==4&&back<0)back=t-down;
        CHECK(l>=3&&l<=4);
        t+=5*kSrSecond;
    }
    CHECK(back>=c.upLockout100ns);CHECK(back<=90*kSrSecond);   // model, 2026-11-20: back at level 4 after 85 s
    CHECK(shared.load()==0);CHECK(b.changes()==2);CHECK(b.backoff()==0);   // and it stays there for the 6 hours
}
TEST(sr_budget_tries_from_the_band_stay_bounded_by_the_backoff){
    // Level 4 costs 3.5 ms (over), level 3 costs 2.5 ms (under the budget, above 60 %): before the delta2 fix it
    // stayed at level 3 (1 change); now every try is taken back and the back-off spaces them.
    srp::PushSrBudgetConfig c;c.budgetMs=3.0;srp::PushSrBudget b(c);std::atomic<std::uint32_t> shared{0};
    const double cost[5]={0,1.0,1.5,2.5,3.5};
    std::int64_t t=kSrSecond;std::uint32_t prev=4;
    for(int h=0;h<4;++h){
        unsigned changes=0,overWindows=0;
        for(int i=0;i<720;++i){
            const auto l0=srp::pushSrEffectiveLevel(4,0,0,shared.load()).level;
            if(cost[l0]>c.budgetMs)++overWindows;
            const auto l=srBudgetWindow(b,shared,4,cost,t);
            CHECK(l>=3&&l<=4);
            if(l!=prev){++changes;prev=l;}
            t+=5*kSrSecond;
        }
        // model, 2026-11-20: 13 changes and 14 windows over in the first hour, then 6 to 8 of each an hour
        if(h==0){CHECK(changes<=13);CHECK(overWindows<=14);}
        else{CHECK(changes<=8);CHECK(overWindows<=8);}
    }
    CHECK(b.backoff()==c.maxBackoffShift);
}
TEST(sr_budget_band_tries_never_go_above_the_user_level){
    // Every window under the budget but above 60 % of it, with the user at level 2 after a step down to 1:
    // the try goes back to 2, never 3 or 4.
    srp::PushSrBudgetConfig c;c.budgetMs=3.0;srp::PushSrBudget b(c);std::atomic<std::uint32_t> shared{0};
    const double over[5]={0,2.0,4.0,4.0,4.0},band[5]={0,2.5,2.5,2.5,2.5};
    std::int64_t t=kSrSecond;
    for(int i=0;i<3;++i){srBudgetWindow(b,shared,2,over,t);t+=5*kSrSecond;}
    CHECK(shared.load()==1);
    for(int i=0;i<720;++i){CHECK(srBudgetWindow(b,shared,2,band,t)<=2);t+=5*kSrSecond;}
    CHECK(shared.load()==0);CHECK(srp::pushSrEffectiveLevel(2,0,0,shared.load()).level==2);
}
