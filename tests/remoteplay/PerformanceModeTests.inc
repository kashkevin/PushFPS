// SPDX-License-Identifier: GPL-3.0-only
// Performance modes (veyra/engine/PushPerformanceMode.h) and the slipstream.ini parser
// (veyra/engine/SlipstreamSettings.h). Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushPerformanceMode.h"
#include "veyra/engine/SlipstreamSettings.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>
namespace {
namespace pm=veyra::engine;
using PmMode=pm::PushPerformanceMode;
constexpr std::int64_t kPmSecond=10000000;
bool pmNear(double a,double b,double tolerance=0.05){return std::abs(a-b)<=tolerance;}
bool pmWants(const pm::PushModeSwitches& list,std::string_view name,std::string_view value){
    for(const auto& item:list)if(item.name==name)return item.value==value;
    return false;
}
bool pmHas(const pm::PushModeSwitches& list,std::string_view name){
    for(const auto& item:list)if(item.name==name)return true;
    return false;
}
pm::PushAutoInputs pmHealthy(std::int64_t now,double sourceFps=30,double displayHz=180){
    pm::PushAutoInputs in;in.now100ns=now;in.sourceFps=sourceFps;in.displayHz=displayHz;in.gpuBusyShare=0.40;in.queueDepth=0;
    in.network=pm::PushNetworkHealth::Healthy;return in;
}
pm::PushAutoInputs pmBusy(std::int64_t now,double sourceFps=30,double displayHz=180){auto in=pmHealthy(now,sourceFps,displayHz);in.gpuBusyShare=0.97;return in;}
const std::string* pmValue(const pm::SlipstreamSettings& s,std::string_view name){
    for(const auto& item:s.switches)if(item.name==name)return &item.value;
    return nullptr;
}
}

// --- the mode table ---------------------------------------------------------------------------
TEST(performance_mode_names_round_trip_and_reject_unknown_text){
    for(const auto mode:{PmMode::UltraLowLatency,PmMode::Competitive,PmMode::Balanced,PmMode::MaximumSmoothness,PmMode::Automatic}){
        const auto parsed=pm::parsePushPerformanceMode(pm::pushPerformanceModeName(mode));
        CHECK(parsed);CHECK(*parsed==mode);
    }
    CHECK(*pm::parsePushPerformanceMode("Ultra Low Latency")==PmMode::UltraLowLatency);
    CHECK(*pm::parsePushPerformanceMode("ultra_low_latency")==PmMode::UltraLowLatency);
    CHECK(*pm::parsePushPerformanceMode("MAXIMUM-SMOOTHNESS")==PmMode::MaximumSmoothness);
    CHECK(*pm::parsePushPerformanceMode("auto")==PmMode::Automatic);
    for(const auto text:{"","lan","lan-optimized","fast","balanced!","ultra low latency please, as low as it can possibly go","0"})
        CHECK(!pm::parsePushPerformanceMode(text));
    CHECK(pm::pushPerformanceModeName(static_cast<PmMode>(77))=="invalid");
}
TEST(performance_mode_ultra_low_latency_never_generates){
    for(const double source:{29.97,30.0,59.94,60.0}){
        const auto plan=pm::pushModePlan(PmMode::UltraLowLatency,source,180);
        CHECK(plan.valid);CHECK(!plan.frameGeneration);CHECK(plan.multiplier==1);CHECK(pmNear(plan.outputFps,source));
        CHECK(plan.minAddedDelayMs==0);CHECK(plan.lowQueue);CHECK(plan.presentNewestAtOnce);CHECK(plan.wantsVrr);CHECK(plan.tearingAcceptable);
        CHECK(!plan.onlyWhileStable);
    }
}
TEST(performance_mode_competitive_generates_only_for_a_sixty_fps_source){
    const auto thirty=pm::pushModePlan(PmMode::Competitive,29.97,180);
    CHECK(thirty.valid);CHECK(!thirty.frameGeneration);CHECK(thirty.multiplier==1);CHECK(thirty.minAddedDelayMs==0);
    const auto sixty=pm::pushModePlan(PmMode::Competitive,59.94,180);
    CHECK(sixty.valid);CHECK(sixty.frameGeneration);CHECK(sixty.multiplier==2);CHECK(pmNear(sixty.outputFps,119.88));
    CHECK(pmNear(sixty.minAddedDelayMs,8.34));CHECK(sixty.lowQueue);CHECK(sixty.unevenWithoutVrr);   // 120 on 180 Hz: 1.5 refreshes a frame
}
TEST(performance_mode_balanced_and_maximum_smoothness_targets){
    const auto b30=pm::pushModePlan(PmMode::Balanced,30,180);
    CHECK(b30.frameGeneration);CHECK(b30.multiplier==3);CHECK(pmNear(b30.outputFps,90));CHECK(pmNear(b30.minAddedDelayMs,22.22));CHECK(!b30.unevenWithoutVrr);
    const auto b60=pm::pushModePlan(PmMode::Balanced,60,180);
    CHECK(b60.multiplier==2);CHECK(pmNear(b60.outputFps,120));CHECK(pmNear(b60.minAddedDelayMs,8.33));CHECK(b60.unevenWithoutVrr);
    const auto s30=pm::pushModePlan(PmMode::MaximumSmoothness,30,180);
    CHECK(s30.multiplier==4);CHECK(pmNear(s30.outputFps,120));CHECK(pmNear(s30.minAddedDelayMs,25.0));CHECK(s30.onlyWhileStable);
    const auto s60=pm::pushModePlan(PmMode::MaximumSmoothness,60,180);
    CHECK(s60.multiplier==3);CHECK(pmNear(s60.outputFps,180));CHECK(pmNear(s60.minAddedDelayMs,11.11));CHECK(!s60.unevenWithoutVrr);
    CHECK(!b30.lowQueue);CHECK(!b30.presentNewestAtOnce);CHECK(b30.wantsVrr);CHECK(!b30.tearingAcceptable);CHECK(!b30.onlyWhileStable);
}
TEST(performance_mode_output_never_exceeds_the_display){
    const auto smooth=pm::pushModePlan(PmMode::MaximumSmoothness,59.94,120);    // 3X would be 180 on a 120 Hz panel
    CHECK(smooth.valid);CHECK(smooth.multiplier==2);CHECK(smooth.reason=="display-limit");
    const auto balanced=pm::pushModePlan(PmMode::Balanced,30,60);                 // 3X would be 90 on a 60 Hz panel
    CHECK(balanced.multiplier==2);CHECK(pmNear(balanced.outputFps,60));CHECK(balanced.reason=="display-limit");
    const auto full=pm::pushModePlan(PmMode::Competitive,60,60);                  // the source already fills the panel
    CHECK(!full.frameGeneration);CHECK(full.multiplier==1);CHECK(full.minAddedDelayMs==0);CHECK(full.reason=="display-limit");
    const auto ntsc=pm::pushModePlan(PmMode::MaximumSmoothness,60.0,179.82);      // a 59.94 x 3 panel mode still takes 3X of 60.00
    CHECK(ntsc.multiplier==3);CHECK(ntsc.reason=="ok");
    for(const auto mode:{PmMode::UltraLowLatency,PmMode::Competitive,PmMode::Balanced,PmMode::MaximumSmoothness})
        for(const double source:{29.97,30.0,59.94,60.0})
            for(const double display:{48.0,60.0,75.0,100.0,120.0,144.0,165.0,180.0,240.0,360.0}){
                const auto plan=pm::pushModePlan(mode,source,display);
                CHECK(plan.valid);CHECK(plan.multiplier>=1);CHECK(plan.multiplier<=pm::kPushModeMaxMultiplier);
                CHECK(plan.multiplier==1||plan.outputFps<=display*pm::kPushModeDisplayFit);
                CHECK(plan.frameGeneration==(plan.multiplier>1));
                CHECK(pmNear(plan.minAddedDelayMs,pm::pushInterpolationDelayMs(plan.multiplier,source),1e-9));
            }
}
TEST(performance_mode_delay_arithmetic){
    CHECK(pm::pushInterpolationDelayMs(1,30)==0);
    CHECK(pmNear(pm::pushInterpolationDelayMs(2,30),16.667,0.001));
    CHECK(pmNear(pm::pushInterpolationDelayMs(3,30),22.222,0.001));
    CHECK(pmNear(pm::pushInterpolationDelayMs(4,30),25.0,0.001));
    CHECK(pmNear(pm::pushInterpolationDelayMs(6,30),27.778,0.001));
    CHECK(pmNear(pm::pushInterpolationDelayMs(2,60),8.333,0.001));
    CHECK(pmNear(pm::pushInterpolationDelayMs(3,60),11.111,0.001));
    CHECK(pmNear(pm::pushInterpolationDelayMs(2,59.94),8.342,0.001));
    for(const double bad:{0.0,-30.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()})
        CHECK(pm::pushInterpolationDelayMs(3,bad)==0);
    CHECK(pm::pushInterpolationDelayMs(0,30)==0);
}
TEST(performance_mode_unavailable_targets_say_why){
    const auto six=pm::pushModeTarget(30,180);              // 6X: the project's own generator offers 2X to 4X
    CHECK(!six.available);CHECK(six.multiplier==6);CHECK(six.reason=="multiplier-above-generator-limit");CHECK(pmNear(six.minAddedDelayMs,27.78));
    const auto half=pm::pushModeTarget(60,90);              // 1.5X
    CHECK(!half.available);CHECK(half.multiplier==0);CHECK(half.reason=="fractional-multiplier");
    const auto ntscHalf=pm::pushModeTarget(59.94,90);
    CHECK(!ntscHalf.available);CHECK(ntscHalf.reason=="fractional-multiplier");
    const auto down=pm::pushModeTarget(60,30);
    CHECK(!down.available);CHECK(down.reason=="below-source-rate");
    const auto same=pm::pushModeTarget(60,60);
    CHECK(same.available);CHECK(same.multiplier==1);CHECK(same.minAddedDelayMs==0);CHECK(same.reason=="no-generation");
    struct Row{double source,output;unsigned multiplier;};
    const Row rows[]={{30.0,60.0,2u},{30.0,90.0,3u},{30.0,120.0,4u},{60.0,120.0,2u},{60.0,180.0,3u},{29.97,89.91,3u},{59.94,179.82,3u},{59.94,180.0,3u}};
    for(const Row& row:rows){
        const auto target=pm::pushModeTarget(row.source,row.output);
        CHECK(target.available);CHECK(target.multiplier==row.multiplier);CHECK(target.reason=="ok");
        CHECK(pmNear(target.minAddedDelayMs,pm::pushInterpolationDelayMs(row.multiplier,row.source),1e-9));
    }
    for(const double bad:{0.0,-1.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}){
        CHECK(!pm::pushModeTarget(bad,120).available);CHECK(pm::pushModeTarget(bad,120).reason=="bad-input");
        CHECK(!pm::pushModeTarget(30,bad).available);CHECK(pm::pushModeTarget(30,bad).reason=="bad-input");
    }
}
TEST(performance_mode_bad_inputs_fall_back_to_no_generation){
    const double nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();
    for(const auto mode:{PmMode::UltraLowLatency,PmMode::Competitive,PmMode::Balanced,PmMode::MaximumSmoothness}){
        for(const double source:{0.0,-30.0,nan,inf,24.0,40.0,45.0,50.0,120.0}){        // only a 30 or 60 fps source has a table row
            const auto plan=pm::pushModePlan(mode,source,180);
            CHECK(!plan.valid);CHECK(!plan.frameGeneration);CHECK(plan.multiplier==1);CHECK(plan.minAddedDelayMs==0);CHECK(plan.reason=="bad-source-rate");
        }
        for(const double display:{0.0,-180.0,nan,inf}){
            const auto plan=pm::pushModePlan(mode,60,display);
            CHECK(!plan.valid);CHECK(!plan.frameGeneration);CHECK(plan.multiplier==1);CHECK(plan.reason=="bad-display-rate");
        }
    }
    const auto automatic=pm::pushModePlan(PmMode::Automatic,60,180);                    // Automatic is a controller, not a table row
    CHECK(!automatic.valid);CHECK(!automatic.frameGeneration);CHECK(automatic.reason=="mode-not-concrete");
    const auto garbage=pm::pushModePlan(static_cast<PmMode>(200),60,180);
    CHECK(!garbage.valid);CHECK(!garbage.frameGeneration);CHECK(garbage.reason=="mode-not-concrete");
    CHECK(pm::pushSourceCadence(29.97)==pm::PushSourceCadence::Thirty);CHECK(pm::pushSourceCadence(59.94)==pm::PushSourceCadence::Sixty);
    CHECK(pm::pushSourceCadence(45)==pm::PushSourceCadence::Unknown);CHECK(pm::pushSourceCadence(nan)==pm::PushSourceCadence::Unknown);
}
TEST(performance_mode_switch_lists){
    const auto ultra=pm::pushModeSwitches(PmMode::UltraLowLatency,false);
    CHECK(pmWants(ultra,"VEYRA_EXP_RP_PAD_POLL_MS","2"));CHECK(!pmHas(ultra,"VEYRA_EXP_RP_ADAPTIVE_PHASE"));   // no generation, no pair phase
    const auto competitive=pm::pushModeSwitches(PmMode::Competitive,false);
    CHECK(pmWants(competitive,"VEYRA_EXP_RP_PAD_POLL_MS","2"));CHECK(pmWants(competitive,"VEYRA_EXP_RP_ADAPTIVE_PHASE","1"));
    for(const auto mode:{PmMode::Balanced,PmMode::MaximumSmoothness,PmMode::Automatic}){
        const auto list=pm::pushModeSwitches(mode,false);
        CHECK(pmWants(list,"VEYRA_EXP_RP_ADAPTIVE_PHASE","1"));CHECK(!pmHas(list,"VEYRA_EXP_RP_PAD_POLL_MS"));
    }
    // No mode ever asks for a legacy (original timing) switch or for keyboard and mouse emulation.
    for(const auto mode:{PmMode::UltraLowLatency,PmMode::Competitive,PmMode::Balanced,PmMode::MaximumSmoothness,PmMode::Automatic})
        for(const bool lan:{false,true})
            for(const auto& item:pm::pushModeSwitches(mode,lan)){
                CHECK(item.name.starts_with("VEYRA_EXP_RP_"));CHECK(!item.value.empty());
                CHECK(!item.name.starts_with("VEYRA_EXP_RP_KBM"));
            }
    CHECK(pm::pushModeSwitches(static_cast<PmMode>(99),false).size()==0);
}
TEST(performance_mode_lan_profile_combines_with_every_mode){
    const auto lanOnly=pm::pushLanProfileSwitches();
    for(const auto mode:{PmMode::UltraLowLatency,PmMode::Competitive,PmMode::Balanced,PmMode::MaximumSmoothness,PmMode::Automatic}){
        const auto plain=pm::pushModeSwitches(mode,false),lan=pm::pushModeSwitches(mode,true);
        CHECK(lan.size()==plain.size()+lanOnly.size());
        for(const auto& item:plain)CHECK(pmWants(lan,item.name,item.value));      // the flag changes nothing the mode asked for
        for(const auto& item:lanOnly){CHECK(pmWants(lan,item.name,item.value));CHECK(!pmHas(plain,item.name));}
    }
    for(std::size_t i=0;i<lanOnly.size();++i)for(std::size_t j=i+1;j<lanOnly.size();++j)CHECK(lanOnly[i].name!=lanOnly[j].name);
    CHECK(pmWants(lanOnly,"VEYRA_EXP_RP_LAN_PROFILE","1"));CHECK(lanOnly.size()==1);   // the network work's master switch, nothing more
}

// --- Automatic --------------------------------------------------------------------------------
TEST(performance_mode_automatic_starts_at_the_ceiling_and_holds_while_healthy){
    pm::PushAutomaticMode automatic(PmMode::Balanced);
    CHECK(automatic.mode()==PmMode::Balanced);CHECK(automatic.ceiling()==PmMode::Balanced);
    const auto first=automatic.update(pmHealthy(0));
    CHECK(first.mode==PmMode::Balanced);CHECK(!first.changed);CHECK(first.reason=="start");
    for(int second=1;second<=600;++second){
        const auto d=automatic.update(pmHealthy(second*kPmSecond));
        CHECK(d.mode==PmMode::Balanced);CHECK(!d.changed);CHECK(d.reason=="at-ceiling");
    }
}
TEST(performance_mode_automatic_steps_down_one_mode_after_sustained_trouble){
    pm::PushAutoConfig config;config.minDwell100ns=5*kPmSecond;config.troubleHold100ns=3*kPmSecond;
    pm::PushAutomaticMode automatic(PmMode::MaximumSmoothness,config);
    std::int64_t now=0;
    (void)automatic.update(pmHealthy(now));
    for(;now<10*kPmSecond;now+=kPmSecond/10)(void)automatic.update(pmHealthy(now));
    // A two-second spike is not sustained: nothing happens.
    for(const std::int64_t end=now+2*kPmSecond;now<end;now+=kPmSecond/10){const auto d=automatic.update(pmBusy(now));CHECK(!d.changed);CHECK(d.reason=="trouble-gpu-busy");}
    for(const std::int64_t end=now+kPmSecond;now<end;now+=kPmSecond/10)CHECK(!automatic.update(pmHealthy(now)).changed);
    // Three seconds of it: one step, and the reason says what it was.
    const std::int64_t began=now;pm::PushAutoDecision d{};
    for(;;now+=kPmSecond/10){d=automatic.update(pmBusy(now));if(d.changed)break;CHECK(now-began<4*kPmSecond);}
    CHECK(now-began>=3*kPmSecond);CHECK(d.mode==PmMode::Balanced);CHECK(d.reason=="step-down-gpu-busy");CHECK(automatic.mode()==PmMode::Balanced);
    // Still busy: the next step needs the dwell and another full trouble period. For a 30 fps game Competitive
    // generates nothing, exactly like Ultra Low Latency, so the next level down is Ultra Low Latency.
    const std::int64_t stepped=now;now+=kPmSecond/10;
    for(;;now+=kPmSecond/10){d=automatic.update(pmBusy(now));if(d.changed)break;CHECK(now-stepped<7*kPmSecond);}
    CHECK(now-stepped>=5*kPmSecond);CHECK(d.mode==PmMode::UltraLowLatency);CHECK(d.reason=="step-down-gpu-busy");
    // With a 60 fps game Competitive is a level of its own (2X), between 3X and none.
    pm::PushAutomaticMode sixty(PmMode::MaximumSmoothness,config);
    for(now=0;now<=30*kPmSecond&&sixty.mode()==PmMode::MaximumSmoothness;now+=kPmSecond/10)(void)sixty.update(pmBusy(now,60));
    CHECK(sixty.mode()==PmMode::Competitive);
    // Other kinds of trouble carry their own reason.
    pm::PushAutomaticMode queue(PmMode::Balanced,config),network(PmMode::Balanced,config);
    for(now=0;now<=20*kPmSecond;now+=kPmSecond/10){
        auto q=pmHealthy(now);q.queueDepth=4;const auto dq=queue.update(q);if(dq.changed)CHECK(dq.reason=="step-down-queue-depth");
        auto n=pmHealthy(now);n.network=pm::PushNetworkHealth::Poor;const auto dn=network.update(n);if(dn.changed)CHECK(dn.reason=="step-down-network");
    }
    CHECK(queue.mode()==PmMode::UltraLowLatency);CHECK(network.mode()==PmMode::UltraLowLatency);
    CHECK(queue.update([&]{auto q=pmHealthy(now);q.queueDepth=4;return q;}()).reason=="at-floor");
}
TEST(performance_mode_automatic_steps_up_only_after_a_sustained_healthy_period){
    pm::PushAutoConfig config;config.minDwell100ns=5*kPmSecond;config.troubleHold100ns=3*kPmSecond;config.healthyHold100ns=30*kPmSecond;
    pm::PushAutomaticMode automatic(PmMode::Balanced,config);
    std::int64_t now=0;
    for(;automatic.mode()==PmMode::Balanced;now+=kPmSecond/4){(void)automatic.update(pmBusy(now));CHECK(now<60*kPmSecond);}
    CHECK(automatic.mode()==PmMode::UltraLowLatency);
    // 29 healthy seconds, then one neutral sample (GPU between the two thresholds): the count starts again.
    const std::int64_t healthyFrom=now;
    for(;now<healthyFrom+29*kPmSecond;now+=kPmSecond/4){const auto d=automatic.update(pmHealthy(now));CHECK(!d.changed);CHECK(d.reason=="healthy-waiting");}
    {auto neutral=pmHealthy(now);neutral.gpuBusyShare=0.80;const auto d=automatic.update(neutral);CHECK(!d.changed);CHECK(d.reason=="hold");now+=kPmSecond/4;}
    const std::int64_t again=now;pm::PushAutoDecision d{};
    for(;;now+=kPmSecond/4){d=automatic.update(pmHealthy(now));if(d.changed)break;CHECK(now-again<=31*kPmSecond);}
    CHECK(now-again>=30*kPmSecond);CHECK(d.mode==PmMode::Balanced);CHECK(d.reason=="step-up-sustained-healthy");
    // A degraded network is not trouble, but it is not healthy either: no step up while it lasts.
    pm::PushAutomaticMode degraded(PmMode::Balanced,config);
    for(now=0;degraded.mode()==PmMode::Balanced;now+=kPmSecond/4)(void)degraded.update(pmBusy(now));
    for(const std::int64_t end=now+300*kPmSecond;now<end;now+=kPmSecond/4){
        auto in=pmHealthy(now);in.network=pm::PushNetworkHealth::Degraded;CHECK(!degraded.update(in).changed);
    }
    CHECK(degraded.mode()==PmMode::UltraLowLatency);
}
TEST(performance_mode_automatic_never_exceeds_the_ceiling_or_raises_itself){
    pm::PushAutoConfig config;config.minDwell100ns=kPmSecond;config.troubleHold100ns=kPmSecond;config.healthyHold100ns=2*kPmSecond;
    // An hour of perfect health at every ceiling: the mode never goes above what the user chose.
    for(const auto ceiling:{PmMode::UltraLowLatency,PmMode::Competitive,PmMode::Balanced,PmMode::MaximumSmoothness}){
        pm::PushAutomaticMode automatic(ceiling,config);
        for(std::int64_t now=0;now<3600*kPmSecond;now+=kPmSecond){
            const auto before=automatic.mode();
            const auto d=automatic.update(pmHealthy(now));
            CHECK(static_cast<unsigned>(d.mode)<=static_cast<unsigned>(ceiling));
            CHECK(static_cast<unsigned>(d.mode)<=static_cast<unsigned>(before));           // never upward: nothing ever went wrong
            if(d.changed)CHECK(d.reason=="step-down-no-benefit");                         // Competitive adds nothing at 30 fps
        }
    }
    // Raising the ceiling is the user's act; the mode itself still waits for a healthy period. Lowering applies at once.
    pm::PushAutomaticMode automatic(PmMode::Competitive,config);
    (void)automatic.update(pmHealthy(0,60));
    CHECK(automatic.setCeiling(PmMode::MaximumSmoothness));CHECK(automatic.mode()==PmMode::Competitive);
    const auto next=automatic.update(pmHealthy(kPmSecond/2,60));CHECK(next.mode==PmMode::Competitive);CHECK(!next.changed);
    std::int64_t now=kPmSecond;
    for(;automatic.mode()!=PmMode::MaximumSmoothness;now+=kPmSecond/2){(void)automatic.update(pmHealthy(now,60));CHECK(now<60*kPmSecond);}
    CHECK(automatic.setCeiling(PmMode::UltraLowLatency));CHECK(automatic.mode()==PmMode::UltraLowLatency);
    const auto lowered=automatic.update(pmHealthy(now,60));CHECK(lowered.mode==PmMode::UltraLowLatency);
    // Automatic is not a ceiling.
    CHECK(!automatic.setCeiling(PmMode::Automatic));CHECK(automatic.ceiling()==PmMode::UltraLowLatency);
    pm::PushAutomaticMode unset(PmMode::Automatic,config);CHECK(unset.ceiling()==PmMode::UltraLowLatency);CHECK(unset.mode()==PmMode::UltraLowLatency);
    pm::PushAutomaticMode garbage(static_cast<PmMode>(250),config);CHECK(garbage.ceiling()==PmMode::UltraLowLatency);
}
TEST(performance_mode_automatic_hysteresis_holds_for_random_input){
    // Whatever the inputs do: never above the ceiling, never two switches closer than the dwell, one level per
    // switch, and a step up only after the full healthy period without a single unhealthy sample.
    std::mt19937 rng(18112026);
    unsigned totalUps=0,totalDowns=0;
    for(int run=0;run<60;++run){
        pm::PushAutoConfig config;
        config.minDwell100ns=(1+static_cast<std::int64_t>(rng()%8))*kPmSecond;
        config.troubleHold100ns=(1+static_cast<std::int64_t>(rng()%4))*kPmSecond;
        config.healthyHold100ns=(2+static_cast<std::int64_t>(rng()%20))*kPmSecond;
        const auto ceiling=static_cast<PmMode>(rng()%4);
        pm::PushAutomaticMode automatic(ceiling,config);
        std::int64_t now=0,lastSwitch=0,healthySince=-1;unsigned ups=0,downs=0;
        auto previous=automatic.mode();
        bool healthyPhase=true;double source=rng()%2?30.0:59.94;int kind=0;
        const auto multiplierOf=[&](PmMode mode){return pm::pushModePlan(mode,source,180).multiplier;};
        for(int step=0;step<6000;++step){
            now+=static_cast<std::int64_t>(1+rng()%4)*(kPmSecond/4);
            if(rng()%120==0){healthyPhase=!healthyPhase;kind=static_cast<int>(rng()%3);}
            if(rng()%400==0)source=source<45?59.94:30.0;
            pm::PushAutoInputs in=pmHealthy(now,source,180);
            if(!healthyPhase){
                if(rng()%40==0)in.gpuBusyShare=0.80;                    // a neutral sample now and then
                else if(kind==0)in.gpuBusyShare=0.99;
                else if(kind==1)in.queueDepth=5;
                else in.network=pm::PushNetworkHealth::Poor;
            }
            const bool healthySample=in.gpuBusyShare<=config.gpuBusyLow&&in.queueDepth<=config.queueDepthLow&&in.network==pm::PushNetworkHealth::Healthy;
            if(!healthySample)healthySince=-1;else if(healthySince<0)healthySince=now;
            const auto d=automatic.update(in);
            CHECK(static_cast<unsigned>(d.mode)<=static_cast<unsigned>(ceiling));
            CHECK(d.changed==(d.mode!=previous));CHECK(!d.reason.empty());
            if(d.changed){
                CHECK(now-lastSwitch>=config.minDwell100ns);
                const unsigned was=multiplierOf(previous),is=multiplierOf(d.mode);
                // One level: no mode between the two has a multiplier strictly between theirs.
                for(unsigned m=0;m<4;++m){const unsigned between=multiplierOf(static_cast<PmMode>(m));CHECK(!(between>(std::min)(was,is)&&between<(std::max)(was,is)));}
                if(static_cast<unsigned>(d.mode)>static_cast<unsigned>(previous)){
                    ++ups;CHECK(is>was);CHECK(healthySample);CHECK(healthySince>=0);CHECK(now-healthySince>=config.healthyHold100ns);
                    CHECK(d.reason=="step-up-sustained-healthy");
                }else{
                    ++downs;CHECK(d.reason.starts_with("step-down-"));
                    if(d.reason=="step-down-no-benefit")CHECK(is==was);else{CHECK(is<was);CHECK(!healthySample);}
                }
                // It always rests on the lowest-latency mode of its level.
                if(static_cast<unsigned>(d.mode)>0)CHECK(multiplierOf(static_cast<PmMode>(static_cast<unsigned>(d.mode)-1))<is);
                lastSwitch=now;
            }
            previous=d.mode;
        }
        CHECK(ups<=downs);                                             // it only ever comes back from a step down
        totalUps+=ups;totalDowns+=downs;
    }
    CHECK(totalDowns>100);CHECK(totalUps>50);                          // the run really exercised both directions
}
TEST(performance_mode_automatic_backs_off_after_a_step_up_that_did_not_last){
    pm::PushAutoConfig config;config.minDwell100ns=kPmSecond;config.troubleHold100ns=kPmSecond;config.healthyHold100ns=10*kPmSecond;
    pm::PushAutomaticMode automatic(PmMode::Balanced,config);
    std::int64_t now=0;
    const auto downThenUp=[&]{
        for(;automatic.mode()==PmMode::Balanced;now+=kPmSecond/4)(void)automatic.update(pmBusy(now));
        const std::int64_t from=now;
        for(;automatic.mode()!=PmMode::Balanced;now+=kPmSecond/4){(void)automatic.update(pmHealthy(now));CHECK(now-from<600*kPmSecond);}
        return now-from;
    };
    const auto first=downThenUp(),second=downThenUp(),third=downThenUp();
    CHECK(first>=10*kPmSecond);CHECK(first<12*kPmSecond);
    CHECK(second>=20*kPmSecond);CHECK(third>=40*kPmSecond);          // each failed return doubles the healthy period asked for
    for(int i=0;i<6;++i)CHECK(downThenUp()<=81*kPmSecond);            // capped at eight times
}
TEST(performance_mode_automatic_uses_source_and_display){
    pm::PushAutoConfig config;config.minDwell100ns=2*kPmSecond;config.healthyHold100ns=4*kPmSecond;
    // 60 fps source: Balanced and Competitive both give 2X, so the lower-latency one is used.
    pm::PushAutomaticMode sixty(PmMode::Balanced,config);
    std::int64_t now=0;pm::PushAutoDecision d=sixty.update(pmHealthy(now,59.94));
    CHECK(d.mode==PmMode::Balanced);
    for(now=kPmSecond/2;now<2*kPmSecond;now+=kPmSecond/2){d=sixty.update(pmHealthy(now,59.94));CHECK(!d.changed);}
    d=sixty.update(pmHealthy(2*kPmSecond,59.94));
    CHECK(d.changed);CHECK(d.mode==PmMode::Competitive);CHECK(d.reason=="step-down-no-benefit");
    for(now=3*kPmSecond;now<120*kPmSecond;now+=kPmSecond){d=sixty.update(pmHealthy(now,59.94));CHECK(d.mode==PmMode::Competitive);CHECK(d.reason=="at-ceiling");}
    // The game drops to 30 fps. Competitive now generates nothing, like Ultra Low Latency, so it rests there; and
    // Balanced now means 3X: going there is a step up, so it waits for the healthy period.
    d=sixty.update(pmHealthy(now,29.97));CHECK(d.changed);CHECK(d.mode==PmMode::UltraLowLatency);CHECK(d.reason=="step-down-no-benefit");
    const std::int64_t from=now;
    for(;;now+=kPmSecond){d=sixty.update(pmHealthy(now,29.97));if(d.changed)break;CHECK(now-from<10*kPmSecond);}
    CHECK(d.mode==PmMode::Balanced);CHECK(d.reason=="step-up-sustained-healthy");CHECK(now-from>=4*kPmSecond);
    // A 60 Hz panel shows nothing more than the 60 fps source: every mode collapses to no generation.
    pm::PushAutomaticMode panel(PmMode::MaximumSmoothness,config);
    for(now=0;now<10*kPmSecond;now+=kPmSecond)(void)panel.update(pmHealthy(now,60,60));
    CHECK(panel.mode()==PmMode::UltraLowLatency);
    // Unknown source rate: stay put and say so; never a step up.
    pm::PushAutomaticMode unknown(PmMode::Balanced,config);
    for(now=0;now<60*kPmSecond;now+=kPmSecond){
        d=unknown.update(pmHealthy(now,0));CHECK(!d.changed);CHECK(d.mode==PmMode::Balanced);
        if(now>0)CHECK(d.reason=="source-unknown");
    }
}
TEST(performance_mode_automatic_rejects_bad_input){
    pm::PushAutoConfig config;config.minDwell100ns=kPmSecond;config.troubleHold100ns=kPmSecond;config.healthyHold100ns=2*kPmSecond;
    pm::PushAutomaticMode automatic(PmMode::Balanced,config);
    std::int64_t now=0;
    for(;automatic.mode()==PmMode::Balanced;now+=kPmSecond/4)(void)automatic.update(pmBusy(now));
    // Garbage never counts as health: no step up however long it lasts.
    const double nan=std::numeric_limits<double>::quiet_NaN();
    for(const std::int64_t end=now+120*kPmSecond;now<end;now+=kPmSecond/4){
        auto in=pmHealthy(now);
        switch((now/(kPmSecond/4))%4){
        case 0:in.gpuBusyShare=nan;break;
        case 1:in.gpuBusyShare=-0.5;break;
        case 2:in.gpuBusyShare=7.0;break;
        default:in.network=static_cast<pm::PushNetworkHealth>(42);break;
        }
        const auto d=automatic.update(in);CHECK(!d.changed);CHECK(d.reason=="bad-input");
    }
    CHECK(automatic.mode()==PmMode::UltraLowLatency);
    // A clock that runs backwards restarts the timers instead of crediting time that did not pass.
    for(const std::int64_t end=now+kPmSecond;now<end;now+=kPmSecond/4)(void)automatic.update(pmHealthy(now));
    const auto back=automatic.update(pmHealthy(now-3600*kPmSecond));CHECK(!back.changed);CHECK(back.reason=="bad-input");
    const auto after=automatic.update(pmHealthy(now-3600*kPmSecond+kPmSecond/4));CHECK(!after.changed);
    // A broken configuration is made safe: negative times become zero, thresholds are put in order.
    pm::PushAutoConfig broken;broken.minDwell100ns=-5;broken.troubleHold100ns=-1;broken.healthyHold100ns=-1;broken.gpuBusyHigh=0.2;broken.gpuBusyLow=0.9;
    broken.queueDepthHigh=0;broken.queueDepthLow=9;
    pm::PushAutomaticMode odd(PmMode::Balanced,broken);
    CHECK(odd.config().minDwell100ns==0);CHECK(odd.config().gpuBusyLow<=odd.config().gpuBusyHigh);CHECK(odd.config().queueDepthLow<odd.config().queueDepthHigh);
    for(std::int64_t t=0;t<100*kPmSecond;t+=kPmSecond)CHECK(static_cast<unsigned>(odd.update(pmHealthy(t)).mode)<=static_cast<unsigned>(PmMode::Balanced));
}
TEST(performance_mode_automatic_without_a_gpu_reading_never_steps_up_and_still_steps_down){
    pm::PushAutoConfig config;config.minDwell100ns=kPmSecond;config.troubleHold100ns=kPmSecond;config.healthyHold100ns=2*kPmSecond;
    pm::PushAutomaticMode automatic(PmMode::Balanced,config);
    std::int64_t now=0;
    for(;automatic.mode()==PmMode::Balanced;now+=kPmSecond/4)(void)automatic.update(pmBusy(now));
    const PmMode lowered=automatic.mode();
    // No GPU reading: everything else is healthy, for two minutes. Never counted as health, never as bad
    // input, whether the unused field holds a stale low number or no number at all.
    for(const double unused:{0.10,std::numeric_limits<double>::quiet_NaN()})
        for(const std::int64_t end=now+120*kPmSecond;now<end;now+=kPmSecond/4){
            auto in=pmHealthy(now);in.gpuKnown=false;in.gpuBusyShare=unused;
            const auto d=automatic.update(in);CHECK(!d.changed);CHECK(d.reason!="bad-input");CHECK(d.reason!="step-up-sustained-healthy");
        }
    CHECK(automatic.mode()==lowered);
    // An unknown GPU is no GPU trouble either, whatever number sits in the field.
    pm::PushAutomaticMode blind(PmMode::Balanced,config);
    for(std::int64_t t=0;t<60*kPmSecond;t+=kPmSecond/4){auto in=pmBusy(t);in.gpuKnown=false;const auto d=blind.update(in);CHECK(!d.changed);}
    CHECK(blind.mode()==PmMode::Balanced);
    // The network and the queue still count without it.
    std::int64_t t=60*kPmSecond;
    for(;blind.mode()==PmMode::Balanced&&t<120*kPmSecond;t+=kPmSecond/4){auto in=pmHealthy(t);in.gpuKnown=false;in.network=pm::PushNetworkHealth::Poor;(void)blind.update(in);}
    CHECK(blind.mode()!=PmMode::Balanced);
    // With a reading again the usual healthy period brings it back, not sooner.
    for(const std::int64_t end=now+60*kPmSecond;now<end&&automatic.mode()==lowered;now+=kPmSecond/4)(void)automatic.update(pmHealthy(now));
    CHECK(automatic.mode()==PmMode::Balanced);
}
TEST(performance_mode_interface_signals_become_controller_input){
    pm::PushAutoSignals seen;seen.sourceFps=59.94;seen.displayHz=180;seen.queueDepth=1;
    // Nothing known about the GPU: not healthy, not trouble.
    auto in=pm::pushAutoInputs(seen,123);
    CHECK(in.now100ns==123);CHECK(pmNear(in.sourceFps,59.94));CHECK(pmNear(in.displayHz,180));CHECK(in.queueDepth==1);
    CHECK(!in.gpuKnown);CHECK(in.network==pm::PushNetworkHealth::Healthy);   // a local source has no network to be unhealthy
    seen.gpuUtilizationKnown=true;seen.gpuUtilizationPercent=55;
    in=pm::pushAutoInputs(seen,0);CHECK(in.gpuKnown);CHECK(pmNear(in.gpuBusyShare,0.55,1e-9));
    seen.gpuUtilizationPercent=250;CHECK(pm::pushAutoInputs(seen,0).gpuBusyShare==1.0);
    seen.gpuUtilizationPercent=-3;CHECK(pm::pushAutoInputs(seen,0).gpuBusyShare==0.0);
    seen.gpuUtilizationPercent=std::numeric_limits<double>::quiet_NaN();CHECK(!pm::pushAutoInputs(seen,0).gpuKnown);
    // The generator skipping pairs is a busy GPU whatever the utilisation figure says, known or not.
    seen.gpuUtilizationPercent=20;seen.generationOverBudget=true;
    in=pm::pushAutoInputs(seen,0);CHECK(in.gpuKnown);CHECK(in.gpuBusyShare==1.0);
    seen.gpuUtilizationKnown=false;in=pm::pushAutoInputs(seen,0);CHECK(in.gpuKnown);CHECK(in.gpuBusyShare==1.0);
    seen.generationOverBudget=false;
    // A network source without a grade is never healthy and never trouble; only Unstable is trouble.
    seen.network=pm::PushAutoNetwork::Unknown;CHECK(pm::pushAutoInputs(seen,0).network==pm::PushNetworkHealth::Degraded);
    seen.network=pm::PushAutoNetwork::Unsteady;CHECK(pm::pushAutoInputs(seen,0).network==pm::PushNetworkHealth::Degraded);
    seen.network=pm::PushAutoNetwork::Stable;CHECK(pm::pushAutoInputs(seen,0).network==pm::PushNetworkHealth::Healthy);
    seen.network=pm::PushAutoNetwork::Unstable;CHECK(pm::pushAutoInputs(seen,0).network==pm::PushNetworkHealth::Poor);
}
TEST(performance_mode_automatic_from_interface_signals_only_lowers_by_itself){
    // A PS5 stream without the health experiment: a busy GPU lowers the mode, and nothing raises it again,
    // because a network nobody graded is never counted as healthy.
    pm::PushAutoConfig config;config.minDwell100ns=kPmSecond;config.troubleHold100ns=kPmSecond;config.healthyHold100ns=2*kPmSecond;
    pm::PushAutomaticMode automatic(PmMode::MaximumSmoothness,config);
    pm::PushAutoSignals seen;seen.sourceFps=30;seen.displayHz=180;seen.gpuUtilizationKnown=true;seen.gpuUtilizationPercent=40;seen.network=pm::PushAutoNetwork::Unknown;
    std::int64_t now=0;
    for(;now<10*kPmSecond;now+=kPmSecond/2){const auto d=automatic.update(pm::pushAutoInputs(seen,now));CHECK(!d.changed);}
    CHECK(automatic.mode()==PmMode::MaximumSmoothness);
    seen.generationOverBudget=true;
    unsigned downs=0;
    for(const std::int64_t end=now+5*kPmSecond;now<end;now+=kPmSecond/2){
        const PmMode before=automatic.mode();const auto d=automatic.update(pm::pushAutoInputs(seen,now));
        if(d.changed){++downs;CHECK(static_cast<unsigned>(d.mode)<static_cast<unsigned>(before));CHECK(d.reason=="step-down-gpu-busy");}
    }
    CHECK(downs>=1);
    const PmMode lowered=automatic.mode();
    seen.generationOverBudget=false;
    for(const std::int64_t end=now+300*kPmSecond;now<end;now+=kPmSecond/2){const auto d=automatic.update(pm::pushAutoInputs(seen,now));CHECK(!d.changed);}
    CHECK(automatic.mode()==lowered);
    // Once the connection is graded Stable the usual healthy period applies, and the ceiling still holds.
    seen.network=pm::PushAutoNetwork::Stable;
    for(const std::int64_t end=now+300*kPmSecond;now<end;now+=kPmSecond/2){
        const auto d=automatic.update(pm::pushAutoInputs(seen,now));
        if(d.changed)CHECK(d.reason=="step-up-sustained-healthy");
        CHECK(static_cast<unsigned>(d.mode)<=static_cast<unsigned>(PmMode::MaximumSmoothness));
    }
    CHECK(automatic.mode()==PmMode::MaximumSmoothness);
}
// The interface applies modes from its own copy of pushModePlan() (qml/Veyra/PushModes.js). These rows are
// the same rows, value for value, as `table` in tests/qml/quick/tst_push_slipstream.qml: change both.
TEST(performance_mode_plan_rows_shared_with_the_interface){
    struct Row{PmMode mode;double sourceFps,displayHz;bool valid;unsigned multiplier;double outputFps,minAddedDelayMs;bool unevenWithoutVrr;std::string_view reason;};
    const Row rows[]={
        {PmMode::UltraLowLatency,29.97,180,true,1,29.9700,0.0000,false,"ok"},
        {PmMode::UltraLowLatency,59.94,180,true,1,59.9400,0.0000,false,"ok"},
        {PmMode::Competitive,30,180,true,1,30.0000,0.0000,false,"ok"},
        {PmMode::Competitive,59.94,180,true,2,119.8800,8.3417,true,"ok"},
        {PmMode::Balanced,30,180,true,3,90.0000,22.2222,false,"ok"},
        {PmMode::Balanced,60,180,true,2,120.0000,8.3333,true,"ok"},
        {PmMode::MaximumSmoothness,30,180,true,4,120.0000,25.0000,true,"ok"},
        {PmMode::MaximumSmoothness,60,180,true,3,180.0000,11.1111,false,"ok"},
        {PmMode::MaximumSmoothness,59.94,120,true,2,119.8800,8.3417,false,"display-limit"},
        {PmMode::Balanced,30,60,true,2,60.0000,16.6667,false,"display-limit"},
        {PmMode::Competitive,60,60,true,1,60.0000,0.0000,false,"display-limit"},
        {PmMode::MaximumSmoothness,60,179.82,true,3,180.0000,11.1111,false,"ok"},
        {PmMode::MaximumSmoothness,30,144,true,4,120.0000,25.0000,true,"ok"},
        {PmMode::MaximumSmoothness,60,144,true,2,120.0000,8.3333,true,"display-limit"},
        {PmMode::Balanced,40,180,false,1,0.0000,0.0000,false,"bad-source-rate"},
        {PmMode::Balanced,30,0,false,1,0.0000,0.0000,false,"bad-display-rate"},
        {PmMode::MaximumSmoothness,60,240,true,3,180.0000,11.1111,true,"ok"},
        {PmMode::MaximumSmoothness,30,360,true,4,120.0000,25.0000,false,"ok"},
        {PmMode::Balanced,60,360,true,2,120.0000,8.3333,false,"ok"},
        {PmMode::MaximumSmoothness,29.97,165,true,4,119.8800,25.0250,true,"ok"},
        {PmMode::Competitive,59.94,100,true,1,59.9400,0.0000,true,"display-limit"},
    };
    for(const Row& row:rows){
        const auto plan=pm::pushModePlan(row.mode,row.sourceFps,row.displayHz);
        CHECK(plan.valid==row.valid);CHECK(plan.multiplier==row.multiplier);CHECK(plan.frameGeneration==(row.multiplier>1));
        CHECK(pmNear(plan.outputFps,row.outputFps,0.0001));CHECK(pmNear(plan.minAddedDelayMs,row.minAddedDelayMs,0.0001));
        CHECK(plan.unevenWithoutVrr==row.unevenWithoutVrr);CHECK(plan.reason==row.reason);
    }
}

// --- slipstream.ini ---------------------------------------------------------------------------
TEST(slipstream_settings_plain_keys_comments_and_blank_lines){
    const auto s=pm::parseSlipstreamSettings("\xEF\xBB\xBF# Slipstream settings\r\n\r\n  VEYRA_EXP_RP_PAD_POLL_MS = 1  \r\nVEYRA_TEST_RP_LEGACY_INPUT=1\n#VEYRA_EXP_RP_KBM=1\n"
                                             "VEYRA_EXP_RP_KBM_BINDS=\"F=Square;Mouse2=None\"\n");
    CHECK(s.valid);CHECK(s.error.empty());CHECK(!s.mode);CHECK(!s.lan);CHECK(s.unknownKeys.empty());
    CHECK(s.switches.size()==3);
    CHECK(*pmValue(s,"VEYRA_EXP_RP_PAD_POLL_MS")=="1");CHECK(*pmValue(s,"VEYRA_TEST_RP_LEGACY_INPUT")=="1");
    CHECK(*pmValue(s,"VEYRA_EXP_RP_KBM_BINDS")=="F=Square;Mouse2=None");CHECK(!pmValue(s,"VEYRA_EXP_RP_KBM"));
    for(const auto& item:s.switches)CHECK(item.origin==pm::SlipstreamOrigin::File);
    const auto empty=pm::parseSlipstreamSettings("");CHECK(empty.valid);CHECK(empty.switches.empty());
    const auto comments=pm::parseSlipstreamSettings("# nothing\n\n   \n#mode=balanced\n");CHECK(comments.valid);CHECK(comments.switches.empty());CHECK(!comments.mode);
}
TEST(slipstream_settings_mode_and_lan_expand_into_switches){
    const auto s=pm::parseSlipstreamSettings("mode = Competitive\nlan=on\n");
    CHECK(s.valid);CHECK(s.mode);CHECK(*s.mode==PmMode::Competitive);CHECK(s.lan);
    const auto wanted=pm::pushModeSwitches(PmMode::Competitive,true);
    CHECK(s.switches.size()==wanted.size());
    for(const auto& item:wanted){const auto* value=pmValue(s,item.name);CHECK(value);CHECK(*value==item.value);}
    for(const auto& item:s.switches)CHECK(item.origin!=pm::SlipstreamOrigin::File);
    const auto off=pm::parseSlipstreamSettings("mode=balanced\nlan=0\n");CHECK(off.valid);CHECK(!off.lan);
    CHECK(off.switches.size()==pm::pushModeSwitches(PmMode::Balanced,false).size());
    const auto lanOnly=pm::parseSlipstreamSettings("lan=yes\n");CHECK(lanOnly.valid);CHECK(!lanOnly.mode);CHECK(lanOnly.lan);
    CHECK(lanOnly.switches.size()==pm::pushLanProfileSwitches().size());
    const auto automatic=pm::parseSlipstreamSettings("mode=automatic\nceiling=balanced\n");
    CHECK(automatic.valid);CHECK(*automatic.mode==PmMode::Automatic);CHECK(automatic.ceiling);CHECK(*automatic.ceiling==PmMode::Balanced);
    const auto noCeiling=pm::parseSlipstreamSettings("mode=automatic\n");CHECK(noCeiling.valid);CHECK(!noCeiling.ceiling);
}
TEST(slipstream_settings_a_written_key_wins_over_the_mode){
    const auto s=pm::parseSlipstreamSettings("mode=competitive\nVEYRA_EXP_RP_PAD_POLL_MS=1\n");
    CHECK(s.valid);CHECK(*pmValue(s,"VEYRA_EXP_RP_PAD_POLL_MS")=="1");
    unsigned count=0;for(const auto& item:s.switches)count+=item.name=="VEYRA_EXP_RP_PAD_POLL_MS";
    CHECK(count==1);CHECK(s.switches.front().origin==pm::SlipstreamOrigin::File);
    // An empty value means "leave this one unset", also when the mode would have set it.
    const auto held=pm::parseSlipstreamSettings("VEYRA_EXP_RP_ADAPTIVE_PHASE=\nmode=balanced\n");
    CHECK(held.valid);CHECK(!pmValue(held,"VEYRA_EXP_RP_ADAPTIVE_PHASE"));CHECK(held.heldOff.size()==1);CHECK(held.heldOff.front()=="VEYRA_EXP_RP_ADAPTIVE_PHASE");
    // The last of two equal keys counts, as in any ini file.
    const auto twice=pm::parseSlipstreamSettings("VEYRA_EXP_RP_PAD_POLL_MS=1\nVEYRA_EXP_RP_PAD_POLL_MS=3\nmode=balanced\nmode=competitive\n");
    CHECK(twice.valid);CHECK(*pmValue(twice,"VEYRA_EXP_RP_PAD_POLL_MS")=="3");CHECK(*twice.mode==PmMode::Competitive);
}
TEST(slipstream_settings_zero_does_not_switch_a_presence_switch_on){
    // These switches are read as "set or not set": VEYRA_EXP_RP_ADAPTIVE_PHASE=0 in the environment would turn the
    // experiment ON. In the file, 0 / off / false / no for them means off.
    for(const auto text:{"VEYRA_EXP_RP_ADAPTIVE_PHASE=0\n","VEYRA_EXP_RP_ADAPTIVE_PHASE=off\n","VEYRA_TEST_RP_LEGACY_TIMING=false\n","VEYRA_TEST_RP_LEGACY_INPUT=No\n",
                         "VEYRA_TEST_RP_LEGACY_OWNER_WAIT=0\n","VEYRA_TEST_RP_LEGACY_ENGINE_WAKE=OFF\n"}){
        const auto s=pm::parseSlipstreamSettings(text);CHECK(s.valid);CHECK(s.switches.empty());CHECK(s.heldOff.size()==1);
    }
    // Any "on" spelling of an on/off switch is written as 1: the engine enables ADAPTIVE_PHASE only for the value 1.
    for(const auto text:{"VEYRA_EXP_RP_ADAPTIVE_PHASE=on\n","VEYRA_EXP_RP_ADAPTIVE_PHASE=true\n","VEYRA_EXP_RP_ADAPTIVE_PHASE=yes\n","VEYRA_EXP_RP_ADAPTIVE_PHASE=2\n","VEYRA_EXP_RP_ADAPTIVE_PHASE=1\n"}){
        const auto on=pm::parseSlipstreamSettings(text);CHECK(on.valid);CHECK(on.switches.size()==1);CHECK(*pmValue(on,"VEYRA_EXP_RP_ADAPTIVE_PHASE")=="1");
    }
    // Switches that carry a value keep a zero: it is a number there.
    const auto number=pm::parseSlipstreamSettings("VEYRA_EXP_RP_KBM_SMOOTH_MS=0\nVEYRA_EXP_RP_KBM=0\n");
    CHECK(number.valid);CHECK(*pmValue(number,"VEYRA_EXP_RP_KBM_SMOOTH_MS")=="0");CHECK(*pmValue(number,"VEYRA_EXP_RP_KBM")=="0");
    const auto heldWithMode=pm::parseSlipstreamSettings("mode=balanced\nVEYRA_EXP_RP_ADAPTIVE_PHASE=0\n");
    CHECK(heldWithMode.valid);CHECK(!pmValue(heldWithMode,"VEYRA_EXP_RP_ADAPTIVE_PHASE"));
}
TEST(slipstream_settings_device_loss_resume_is_an_on_off_switch){
    // The engine enables VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS only for exactly "1" (PushDeviceLossResume.h):
    // every "on" spelling in the file is written as 1, every "off" spelling leaves it unset.
    CHECK(pm::slipstreamPresenceSwitch("VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS"));
    for(const auto text:{"VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=on\n","VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS = true\n","VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=Yes\n","VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1\n"}){
        const auto on=pm::parseSlipstreamSettings(text);CHECK(on.valid);CHECK(on.switches.size()==1);CHECK(*pmValue(on,"VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS")=="1");
    }
    for(const auto text:{"VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=0\n","VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=off\n","VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=false\n","VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=no\n"}){
        const auto off=pm::parseSlipstreamSettings(text);CHECK(off.valid);CHECK(off.switches.empty());CHECK(off.heldOff.size()==1);
    }
}
TEST(slipstream_settings_unknown_keys_are_ignored_and_listed_once){
    const auto s=pm::parseSlipstreamSettings("colour=blue\nVEYRA_EXP_RP_PAD_POLL_MS=2\ncolour=red\nVEYRA_LOG_FILE=C:\\x.log\nPATH=C:\\evil\nveyra_exp_rp_kbm=1\nVEYRA_EXP_RP_=1\n");
    CHECK(s.valid);CHECK(s.switches.size()==1);CHECK(s.switches.front().name=="VEYRA_EXP_RP_PAD_POLL_MS");
    CHECK(s.unknownKeys.size()==5);                                       // colour once; no other variable can be set from the file
    unsigned colour=0;for(const auto& key:s.unknownKeys)colour+=key=="colour";
    CHECK(colour==1);
    std::string many;for(int i=0;i<200;++i)many+="key"+std::to_string(i)+"=1\n";
    const auto capped=pm::parseSlipstreamSettings(many);CHECK(capped.valid);CHECK(capped.unknownKeys.size()==pm::kSlipstreamMaxUnknownKeys);
}
TEST(slipstream_settings_a_malformed_file_changes_nothing){
    const std::string good="mode=balanced\nlan=1\nVEYRA_EXP_RP_PAD_POLL_MS=2\n";
    CHECK(pm::parseSlipstreamSettings(good).valid);
    for(const std::string& bad:{std::string("this line has no equals sign\n"),std::string("=value\n"),std::string("bad key=1\n"),std::string("mode=turbo\n"),
                               std::string("lan=maybe\n"),std::string("ceiling=automatic\n"),std::string("ceiling=fast\n"),std::string("mode=\n"),
                               std::string("VEYRA_EXP_RP_KBM_BINDS=F=Squ\x01re\n"),std::string("VEYRA_EXP_RP_X=caf\xC3\xA9\n"),
                               std::string("VEYRA_EXP_RP_X=")+std::string(600,'a')+"\n",std::string("VEYRA_EXP_RP_X=\"unterminated\n"),
                               std::string("[section]\n"),std::string("a\0b=1\n",6)}){
        const auto s=pm::parseSlipstreamSettings(good+bad);
        CHECK(!s.valid);CHECK(s.switches.empty());CHECK(!s.mode);CHECK(!s.lan);CHECK(!s.ceiling);CHECK(s.heldOff.empty());
        CHECK(s.error.starts_with("line 4: "));
        const auto first=pm::parseSlipstreamSettings(bad+good);CHECK(!first.valid);CHECK(first.switches.empty());CHECK(first.error.starts_with("line 1: "));
    }
    const auto huge=pm::parseSlipstreamSettings(std::string(pm::kSlipstreamMaxBytes+1,'#'));CHECK(!huge.valid);CHECK(huge.error=="file too large");
    std::string lines;for(std::size_t i=0;i<=pm::kSlipstreamMaxSwitches;++i)lines+="VEYRA_EXP_RP_N"+std::to_string(i)+"=1\n";
    const auto many=pm::parseSlipstreamSettings(lines);CHECK(!many.valid);CHECK(many.switches.empty());
    // The error text never repeats the line's content (a value could be something private pasted by mistake).
    const auto secret=pm::parseSlipstreamSettings("registration key 0123456789abcdef0123456789abcdef\n");
    CHECK(!secret.valid);CHECK(secret.error.find("0123456789abcdef")==std::string::npos);
}
TEST(slipstream_settings_describe_lists_names_for_the_log){
    const auto s=pm::parseSlipstreamSettings("mode=competitive\nlan=1\nVEYRA_EXP_RP_PAD_POLL_MS=1\nfoo=bar\n");
    const auto text=pm::describeSlipstreamSettings(s);
    CHECK(text.find("mode=competitive")!=std::string::npos);CHECK(text.find("lan=1")!=std::string::npos);
    CHECK(text.find("VEYRA_EXP_RP_PAD_POLL_MS=1")!=std::string::npos);CHECK(text.find("unknown=[foo]")!=std::string::npos);
    CHECK(pm::describeSlipstreamSettings(pm::parseSlipstreamSettings("")).find("mode=none")!=std::string::npos);
}
