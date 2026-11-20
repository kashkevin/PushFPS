// SPDX-License-Identifier: GPL-3.0-only
// Super-resolution statistics, level experiments and autorun plans (veyra/engine/PushSrPlan.h).
// Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushSrPlan.h"
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
