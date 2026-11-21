// SPDX-License-Identifier: GPL-3.0-only
// First-launch recommendation (veyra/engine/PushCalibrationPlan.h). Included by CoreTests.cpp directly above main().
// The PCs below are simulated capability reports (no model names in the code under test): what each family
// typically reports, not measurements of any real card.
#include "veyra/engine/PushCalibrationPlan.h"
#include <array>
#include <cstdint>
#include <string_view>
namespace {
namespace cal=veyra::engine;
using CalCap=cal::PushCapability;
using CalFg=cal::PushFgMethod;
constexpr std::uint64_t kCalGiB=std::uint64_t(1)<<30;
struct CalFamily { std::string_view name; cal::PushHardwareCapabilities caps; };
cal::PushHardwareCapabilities calDesktop(cal::PushGpuVendor vendor,std::uint64_t vramGiB,std::uint32_t methods){
    cal::PushHardwareCapabilities c;c.vendor=vendor;c.dedicatedVramBytes=vramGiB*kCalGiB;c.featureLevel=cal::kPushFeatureLevel12_2;c.uma=CalCap::No;
    c.decodeH264=c.decodeHevc=c.decodeHevc10=CalCap::Yes;c.refreshHz=180;c.vrr=cal::PushVrrState::Active;c.hdrDisplay=CalCap::No;
    c.hybridGpu=CalCap::No;c.power=cal::PushPowerSource::Mains;c.fgMethods=methods;
    c.opticalFlows=cal::pushFlowBit(cal::PushOpticalFlow::AmdFidelityFx)|cal::pushFlowBit(cal::PushOpticalFlow::GpuDis);
    return c;
}
std::array<CalFamily,11> calFamilies(){
    const std::uint32_t common=cal::pushFgBit(CalFg::PushFlow)|cal::pushFgBit(CalFg::Fsr);
    std::array<CalFamily,11> f{};
    f[0]={"rtx30",calDesktop(cal::PushGpuVendor::Nvidia,10,common)};
    f[1]={"rtx40",calDesktop(cal::PushGpuVendor::Nvidia,12,common|cal::pushFgBit(CalFg::Dlss))};
    f[2]={"rtx50",calDesktop(cal::PushGpuVendor::Nvidia,12,common|cal::pushFgBit(CalFg::Dlss))};
    f[3]={"rx6000",calDesktop(cal::PushGpuVendor::Amd,8,common|cal::pushFgBit(CalFg::AmdFrc))};
    f[4]={"rx7000",calDesktop(cal::PushGpuVendor::Amd,16,common|cal::pushFgBit(CalFg::AmdFrc))};
    f[5]={"rx9000",calDesktop(cal::PushGpuVendor::Amd,16,common|cal::pushFgBit(CalFg::Fsr4))};
    f[6]={"arcA",calDesktop(cal::PushGpuVendor::Intel,8,common|cal::pushFgBit(CalFg::XeSS))};
    f[7]={"arcB",calDesktop(cal::PushGpuVendor::Intel,12,common|cal::pushFgBit(CalFg::XeSS))};
    auto igpu=calDesktop(cal::PushGpuVendor::Amd,0,cal::pushFgBit(CalFg::Fsr));igpu.uma=CalCap::Yes;igpu.dedicatedVramBytes=512ull<<20;
    f[8]={"integrated",igpu};
    auto laptop=calDesktop(cal::PushGpuVendor::Nvidia,8,common|cal::pushFgBit(CalFg::Dlss));laptop.hybridGpu=CalCap::Yes;laptop.power=cal::PushPowerSource::Battery;
    f[9]={"hybrid-battery",laptop};
    auto soft=calDesktop(cal::PushGpuVendor::Other,0,cal::pushFgBit(CalFg::Fsr));soft.uma=CalCap::Unknown;soft.featureLevel=cal::kPushFeatureLevel12_1;
    soft.decodeH264=soft.decodeHevc=soft.decodeHevc10=CalCap::No;
    f[10]={"software",soft};
    return f;
}
constexpr std::array<double,7> kCalRefresh{60,120,144,165,180,240,360};
// A comfortable measurement at 2X for the method the plan would choose.
cal::PushCalibrationMeasurement calMeasured(CalFg method,double base=3,double batch=2){
    cal::PushCalibrationCounters a,b;b.presented=1000;b.expired=0;b.pairsCandidate=400;b.pairsSkipped=0;
    return cal::pushCalibrationMeasurement(base,base+batch,batch,a,b,2,method,true);
}
cal::PushCalibrationInput calInput(const cal::PushHardwareCapabilities& caps,cal::PushGameRate game){
    cal::PushCalibrationInput in;in.caps=caps;in.game=game;return in;
}
}

TEST(calibration_empty_capability_report_recommends_nothing_risky){
    const auto p=cal::pushCalibrationPlan(cal::PushCalibrationInput{});
    CHECK(p.mode==cal::PushPerformanceMode::UltraLowLatency);CHECK(!p.frameGeneration);CHECK(p.multiplier==1);
    CHECK(p.codec==cal::PushStreamCodec::H264);CHECK(!p.hdr);CHECK(!p.enhancement);CHECK(!p.fgMethodKnown);CHECK(!p.smootherOffered);
    CHECK(p.fgReason=="game-rate-unknown");CHECK(p.codecReason=="hevc-decode-unknown");CHECK(p.hdrReason=="hdr-display-unknown");
    for(const std::string_view u:{"game-rate","display-refresh","variable-refresh","gpu","gpu-memory","video-decode","hdr-display","power-source","gpu-cost"})CHECK(p.unknown.has(u));
}
TEST(calibration_unmeasured_systems_never_get_frame_generation){
    for(const auto& family:calFamilies())
        for(const double hz:kCalRefresh)
            for(const auto game:{cal::PushGameRate::Unknown,cal::PushGameRate::Thirty,cal::PushGameRate::Sixty}){
                auto caps=family.caps;caps.refreshHz=hz;
                const auto p=cal::pushCalibrationPlan(calInput(caps,game));
                CHECK(p.mode==cal::PushPerformanceMode::UltraLowLatency);CHECK(!p.frameGeneration);CHECK(p.multiplier==1);
                CHECK(p.minAddedDelayMs==0);CHECK(!p.enhancement);CHECK(p.unknown.has("gpu-cost"));
            }
}
TEST(calibration_measured_sixty_fps_game_gets_competitive_only_where_the_output_fits){
    for(const auto& family:calFamilies())
        for(const double hz:kCalRefresh){
            auto caps=family.caps;caps.refreshHz=hz;
            auto in=calInput(caps,cal::PushGameRate::Sixty);
            CalFg method=CalFg::PushFlow;
            const bool any=cal::pushNextFgMethod(caps,in.preferredMethod,method);
            in.measured=calMeasured(method);
            const auto p=cal::pushCalibrationPlan(in);
            const bool desktop=family.name!="integrated"&&family.name!="hybrid-battery"&&family.name!="software";
            const bool fits=hz>=120;
            CHECK(p.frameGeneration==(desktop&&fits&&any));
            if(p.frameGeneration){
                CHECK(p.mode==cal::PushPerformanceMode::Competitive);CHECK(p.multiplier==2);CHECK(p.tier==0);CHECK(p.outputFps==120);
                CHECK(p.outputFps<=hz*cal::kPushHardwareDisplayFit);CHECK(p.minAddedDelayMs>8.3&&p.minAddedDelayMs<8.4);
                CHECK(p.modeReason=="generation-fits");CHECK(p.fgReason=="generation-fits");
            }else{
                CHECK(p.mode==cal::PushPerformanceMode::UltraLowLatency);CHECK(p.multiplier==1);
            }
            if(!fits&&desktop)CHECK(p.fgReason==(hz<=60?std::string_view("source-fills-display"):std::string_view("output-above-refresh")));
        }
    auto battery=calFamilies()[9].caps;CHECK(cal::pushCalibrationPlan([&]{auto in=calInput(battery,cal::PushGameRate::Sixty);in.measured=calMeasured(CalFg::PushFlow);return in;}()).fgReason=="on-battery");
    auto igpu=calFamilies()[8].caps;CHECK(cal::pushCalibrationPlan([&]{auto in=calInput(igpu,cal::PushGameRate::Sixty);in.measured=calMeasured(CalFg::Fsr);return in;}()).fgReason=="integrated-gpu");
}
TEST(calibration_without_vrr_only_even_outputs_are_recommended){
    for(const double hz:kCalRefresh)
        for(const auto vrr:{cal::PushVrrState::Unknown,cal::PushVrrState::Unsupported,cal::PushVrrState::NotNow,cal::PushVrrState::Possible,cal::PushVrrState::Active}){
            auto caps=calFamilies()[2].caps;caps.refreshHz=hz;caps.vrr=vrr;
            auto in=calInput(caps,cal::PushGameRate::Sixty);in.measured=calMeasured(CalFg::PushFlow);
            const auto p=cal::pushCalibrationPlan(in);
            const bool vrrUsable=vrr==cal::PushVrrState::Active;   // possible (3) is not enough
            const bool even=hz==120||hz==240||hz==360;
            CHECK(p.frameGeneration==(hz>=120&&(vrrUsable||even)));
            if(hz>=120&&!vrrUsable&&!even)CHECK(p.fgReason=="uneven-without-vrr");
            if(p.frameGeneration&&!vrrUsable)CHECK(cal::pushOutputEvenWithoutVrr(p.outputFps,hz));
            CHECK(p.unknown.has("variable-refresh")==(vrr==cal::PushVrrState::Unknown));
        }
}
TEST(calibration_thirty_fps_game_is_never_given_generation_but_the_smoother_choice_is_described){
    for(const double hz:kCalRefresh){
        auto caps=calFamilies()[2].caps;caps.refreshHz=hz;
        auto in=calInput(caps,cal::PushGameRate::Thirty);in.measured=calMeasured(CalFg::PushFlow);
        const auto p=cal::pushCalibrationPlan(in);
        CHECK(!p.frameGeneration);CHECK(p.fgReason=="game-thirty");CHECK(p.mode==cal::PushPerformanceMode::UltraLowLatency);
        CHECK(p.smootherOffered);CHECK(p.smootherOutputFps<=hz*cal::kPushHardwareDisplayFit);
        CHECK(p.smootherMultiplier==(hz>=90?3u:2u));CHECK(p.smootherDelayMs>0);
    }
}
TEST(calibration_a_slower_gpu_never_gets_more){
    auto caps=calFamilies()[2].caps;caps.refreshHz=240;
    bool wasOff=false;
    for(double batch=0.5;batch<=20.0;batch+=0.5){
        auto in=calInput(caps,cal::PushGameRate::Sixty);in.measured=calMeasured(CalFg::PushFlow,3,batch);
        const auto p=cal::pushCalibrationPlan(in);
        if(wasOff)CHECK(!p.frameGeneration);
        wasOff=wasOff||!p.frameGeneration;
    }
    CHECK(wasOff);
    // A heavy base cost alone turns it off; so do missed deadlines and skipped pairs.
    auto in=calInput(caps,cal::PushGameRate::Sixty);in.measured=calMeasured(CalFg::PushFlow,9,2);
    CHECK(cal::pushCalibrationPlan(in).fgReason=="gpu-load-high");
    cal::PushCalibrationCounters a,b;b.presented=900;b.expired=100;b.pairsCandidate=400;
    in.measured=cal::pushCalibrationMeasurement(3,5,2,a,b,2,CalFg::PushFlow,true);CHECK(cal::pushCalibrationPlan(in).fgReason=="missed-deadlines");
    b.expired=0;b.pairsSkipped=200;
    in.measured=cal::pushCalibrationMeasurement(3,5,2,a,b,2,CalFg::PushFlow,true);CHECK(cal::pushCalibrationPlan(in).fgReason=="skipped-pairs");
    // Fewer capabilities never add anything: less memory, an unknown vendor, unknown power.
    auto lowVram=caps;lowVram.dedicatedVramBytes=2*kCalGiB;
    in=calInput(lowVram,cal::PushGameRate::Sixty);in.measured=calMeasured(CalFg::PushFlow);CHECK(cal::pushCalibrationPlan(in).fgReason=="fg-vram-low");
    auto unknownVendor=caps;unknownVendor.vendor=cal::PushGpuVendor::Unknown;
    in=calInput(unknownVendor,cal::PushGameRate::Sixty);in.measured=calMeasured(CalFg::PushFlow);CHECK(cal::pushCalibrationPlan(in).fgReason=="gpu-unknown");
    auto unknownPower=caps;unknownPower.power=cal::PushPowerSource::Unknown;
    in=calInput(unknownPower,cal::PushGameRate::Sixty);in.measured=calMeasured(CalFg::PushFlow);CHECK(cal::pushCalibrationPlan(in).fgReason=="power-unknown");
}
TEST(calibration_measurement_must_match_the_method_and_multiplier){
    auto caps=calFamilies()[5].caps;caps.refreshHz=240;
    auto in=calInput(caps,cal::PushGameRate::Sixty);in.preferredMethod=CalFg::Dlss;   // not on this PC: FSR 4 is next
    in.measured=calMeasured(CalFg::Dlss);CHECK(cal::pushCalibrationPlan(in).fgReason=="measured-other-method");
    in.measured=calMeasured(CalFg::Fsr4);
    const auto p=cal::pushCalibrationPlan(in);CHECK(p.frameGeneration);CHECK(p.fgMethod==CalFg::Fsr4);
    cal::PushCalibrationCounters a,b;b.presented=1000;b.pairsCandidate=400;
    in.measured=cal::pushCalibrationMeasurement(3,5,2,a,b,3,CalFg::Fsr4,true);CHECK(cal::pushCalibrationPlan(in).fgReason=="not-measured");
}
TEST(calibration_measurement_counters_turn_into_shares){
    cal::PushCalibrationCounters a{100,0,50,0},b{1100,10,450,40};
    auto m=cal::pushCalibrationMeasurement(2,4,2,a,b,2,CalFg::PushFlow,true);
    CHECK(m.valid);CHECK(m.missedShare>0.0098&&m.missedShare<0.0100);CHECK(m.skippedPairShare==0.1);
    m=cal::pushCalibrationMeasurement(2,4,2,b,a,2,CalFg::PushFlow,true);   // went backwards: another source
    CHECK(!m.valid);CHECK(m.missedShare<0);CHECK(m.skippedPairShare<0);
    m=cal::pushCalibrationMeasurement(-1,4,2,a,b,2,CalFg::PushFlow,true);CHECK(!m.valid);
    m=cal::pushCalibrationMeasurement(2,4,2,a,b,2,CalFg::PushFlow,false);CHECK(!m.valid);
    m=cal::pushCalibrationMeasurement(2,4,2,a,a,2,CalFg::PushFlow,true);CHECK(!m.valid);   // nothing presented
}
TEST(calibration_codec_and_hdr_follow_the_decoder_and_the_display){
    auto caps=calFamilies()[1].caps;
    auto p=cal::pushCalibrationPlan(calInput(caps,cal::PushGameRate::Unknown));
    CHECK(p.codec==cal::PushStreamCodec::Hevc);CHECK(p.codecReason=="hevc-decode");CHECK(!p.hdr);CHECK(p.hdrReason=="hdr-display-off");
    caps.hdrDisplay=CalCap::Yes;p=cal::pushCalibrationPlan(calInput(caps,cal::PushGameRate::Unknown));CHECK(p.hdr);CHECK(p.hdrReason=="hdr-on");
    caps.decodeHevc10=CalCap::No;p=cal::pushCalibrationPlan(calInput(caps,cal::PushGameRate::Unknown));CHECK(!p.hdr);CHECK(p.hdrReason=="hdr-needs-hevc10-decode");
    caps.decodeHevc=CalCap::No;p=cal::pushCalibrationPlan(calInput(caps,cal::PushGameRate::Unknown));
    CHECK(p.codec==cal::PushStreamCodec::H264);CHECK(p.codecReason=="hevc-decode-missing");CHECK(!p.hdr);CHECK(p.hdrReason=="hdr-needs-hevc");
    caps.hdrDisplay=CalCap::Unknown;p=cal::pushCalibrationPlan(calInput(caps,cal::PushGameRate::Unknown));CHECK(p.hdrReason=="hdr-display-unknown");
    const auto soft=cal::pushCalibrationPlan(calInput(calFamilies()[10].caps,cal::PushGameRate::Sixty));
    CHECK(soft.codec==cal::PushStreamCodec::H264);CHECK(!soft.frameGeneration);
}

// --- interface modes -> experiment switches --------------------------------------------------------------------
namespace {
using CalAct=cal::PushSwitchAction;
bool calWants(const cal::PushModeSwitches& list,std::string_view name){for(const auto& s:list)if(s.name==name)return true;return false;}
}
TEST(calibration_interface_modes_ask_for_the_same_switches_as_the_settings_file){
    CHECK(cal::pushInterfaceSwitches(std::nullopt,false).size()==0);
    CHECK(cal::pushInterfaceSwitches(std::nullopt,true).size()==0);
    CHECK(cal::pushInterfaceSwitches(cal::PushPerformanceMode::Automatic,false).size()==0);
    for(const auto mode:{cal::PushPerformanceMode::UltraLowLatency,cal::PushPerformanceMode::Competitive,cal::PushPerformanceMode::Balanced,cal::PushPerformanceMode::MaximumSmoothness}){
        const auto ui=cal::pushInterfaceSwitches(mode,false),file=cal::pushModeSwitches(mode,false);
        CHECK(ui.size()==file.size());
        for(std::size_t i=0;i<ui.size();++i){CHECK(ui[i].name==file[i].name);CHECK(ui[i].value==file[i].value);}
        const auto automatic=cal::pushInterfaceSwitches(mode,true);
        CHECK(calWants(automatic,"VEYRA_EXP_RP_HEALTH"));CHECK(calWants(automatic,"VEYRA_EXP_RP_ADAPTIVE_PHASE"));CHECK(!calWants(automatic,"VEYRA_EXP_RP_PAD_POLL_MS"));
        for(const auto& s:automatic){bool managed=false;for(const auto n:cal::kPushInterfaceSwitchNames)managed=managed||n==s.name;CHECK(managed);}
    }
    CHECK(calWants(cal::pushInterfaceSwitches(cal::PushPerformanceMode::UltraLowLatency,false),"VEYRA_EXP_RP_PAD_POLL_MS"));
    CHECK(!calWants(cal::pushInterfaceSwitches(cal::PushPerformanceMode::UltraLowLatency,false),"VEYRA_EXP_RP_ADAPTIVE_PHASE"));
}
TEST(calibration_interface_switches_never_touch_what_the_user_set){
    using Flags=std::array<bool,3>;
    // Fresh environment, Ultra Low Latency: only the pad poll is set.
    auto s=cal::pushInterfaceSwitchSteps(cal::pushInterfaceSwitches(cal::PushPerformanceMode::UltraLowLatency,false),Flags{},Flags{});
    CHECK(s[0].action==CalAct::Set);CHECK(s[0].value=="2");CHECK(s[1].action==CalAct::None);CHECK(s[2].action==CalAct::None);
    // Then Balanced: the pad poll it set goes, the phase comes.
    s=cal::pushInterfaceSwitchSteps(cal::pushInterfaceSwitches(cal::PushPerformanceMode::Balanced,false),Flags{true,false,false},Flags{true,false,false});
    CHECK(s[0].action==CalAct::Unset);CHECK(s[1].action==CalAct::Set);CHECK(s[1].value=="1");CHECK(s[2].action==CalAct::None);
    // Already in place: nothing to do.
    s=cal::pushInterfaceSwitchSteps(cal::pushInterfaceSwitches(cal::PushPerformanceMode::Balanced,false),Flags{false,true,false},Flags{false,true,false});
    for(const auto& step:s)CHECK(step.action==CalAct::None);
    // The user's (or slipstream.ini's) value stays, wanted or not, and is never removed.
    s=cal::pushInterfaceSwitchSteps(cal::pushInterfaceSwitches(cal::PushPerformanceMode::Competitive,false),Flags{},Flags{true,true,true});
    CHECK(s[0].action==CalAct::UserOwned);CHECK(s[1].action==CalAct::UserOwned);CHECK(s[2].action==CalAct::None);
    s=cal::pushInterfaceSwitchSteps(cal::pushInterfaceSwitches(std::nullopt,false),Flags{},Flags{true,true,true});
    for(const auto& step:s)CHECK(step.action==CalAct::None);
    // Mode cleared: everything it set goes; one removed by someone else is not set again.
    s=cal::pushInterfaceSwitchSteps(cal::pushInterfaceSwitches(std::nullopt,false),Flags{true,true,true},Flags{true,false,true});
    CHECK(s[0].action==CalAct::Unset);CHECK(s[1].action==CalAct::Unset);CHECK(s[2].action==CalAct::Unset);
    // Automatic: health is asked for.
    s=cal::pushInterfaceSwitchSteps(cal::pushInterfaceSwitches(cal::PushPerformanceMode::Balanced,true),Flags{},Flags{});
    CHECK(s[2].action==CalAct::Set);CHECK(s[2].value=="1");CHECK(s[1].action==CalAct::Set);CHECK(s[0].action==CalAct::None);
}

TEST(calibration_slower_measurements_never_get_more){
    // Reviewer's case: base 6, batch 5; a total of 6.1 must not pass where 5.9 failed.
    auto caps=calFamilies()[2].caps;caps.refreshHz=240;
    cal::PushCalibrationCounters a,b;b.presented=1000;b.pairsCandidate=400;
    auto in=calInput(caps,cal::PushGameRate::Sixty);
    in.measured=cal::pushCalibrationMeasurement(6,5.9,5,a,b,2,CalFg::PushFlow,true);CHECK(cal::pushCalibrationPlan(in).fgReason=="gpu-load-high");
    in.measured=cal::pushCalibrationMeasurement(6,6.1,5,a,b,2,CalFg::PushFlow,true);CHECK(cal::pushCalibrationPlan(in).fgReason=="gpu-load-high");
    // Property over a grid: raising any one reading never raises the multiplier.
    const double values[]={0.5,1,2,3,4,5,6,7,8,10,12,16};
    const auto mult=[&](double base,double total,double batch){
        auto x=calInput(caps,cal::PushGameRate::Sixty);x.measured=cal::pushCalibrationMeasurement(base,total,batch,a,b,2,CalFg::PushFlow,true);
        return cal::pushCalibrationPlan(x).multiplier;};
    for(const double base:values)for(const double total:values)for(const double batch:values){
        const unsigned here=mult(base,total,batch);
        for(const double more:values){
            if(more>base)CHECK(mult(more,total,batch)<=here);
            if(more>total)CHECK(mult(base,more,batch)<=here);
            if(more>batch)CHECK(mult(base,total,more)<=here);
        }
    }
}
TEST(calibration_untimed_base_is_not_measured_and_possible_vrr_is_not_active){
    cal::PushCalibrationCounters a,b;b.presented=1000;b.pairsCandidate=400;
    CHECK(!cal::pushCalibrationMeasurement(0,4,2,a,b,2,CalFg::PushFlow,true).valid);
    CHECK(!cal::pushCalibrationMeasurement(-1,4,2,a,b,2,CalFg::PushFlow,true).valid);
    CHECK(!cal::pushCalibrationMeasurement(2,0,2,a,b,2,CalFg::PushFlow,true).valid);
    auto caps=calFamilies()[2].caps;caps.vrr=cal::PushVrrState::Possible;
    for(const double hz:{144.0,165.0,180.0}){
        caps.refreshHz=hz;auto in=calInput(caps,cal::PushGameRate::Sixty);in.measured=calMeasured(CalFg::PushFlow);
        const auto p=cal::pushCalibrationPlan(in);CHECK(!p.frameGeneration);CHECK(p.fgReason=="uneven-without-vrr");
    }
}
