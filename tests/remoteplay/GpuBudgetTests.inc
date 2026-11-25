// SPDX-License-Identifier: GPL-3.0-only
// Hardware capability model (veyra/engine/PushHardwareCapabilities.h) and the GPU workload budget
// (veyra/engine/PushGpuBudget.h). Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushHardwareCapabilities.h"
#include "veyra/engine/PushGpuBudget.h"
#include <cstdint>
#include <limits>
#include <string_view>
namespace {
namespace hw=veyra::engine;
using HwCap=hw::PushCapability;
using HwFg=hw::PushFgMethod;
constexpr std::uint64_t kHwGiB=std::uint64_t(1)<<30;
// A desktop with everything reported: 12 GB, feature level 12_2, every decoder, 180 Hz with VRR active.
hw::PushHardwareCapabilities hwDesktop(hw::PushGpuVendor vendor=hw::PushGpuVendor::Nvidia){
    hw::PushHardwareCapabilities c;c.vendor=vendor;c.dedicatedVramBytes=12*kHwGiB;c.featureLevel=hw::kPushFeatureLevel12_2;c.uma=HwCap::No;
    c.decodeH264=c.decodeHevc=c.decodeHevc10=HwCap::Yes;c.refreshHz=180;c.vrr=hw::PushVrrState::Active;c.hdrDisplay=HwCap::Yes;
    c.hybridGpu=HwCap::No;c.power=hw::PushPowerSource::Mains;
    c.fgMethods=hw::pushFgBit(HwFg::PushFlow)|hw::pushFgBit(HwFg::Fsr)|hw::pushFgBit(HwFg::XeSS);
    c.opticalFlows=hw::pushFlowBit(hw::PushOpticalFlow::AmdFidelityFx)|hw::pushFlowBit(hw::PushOpticalFlow::GpuDis);
    return c;
}
hw::PushHardwareRequest hwWants(HwFg method=HwFg::PushFlow,unsigned multiplier=2,unsigned tier=1,double sourceFps=60){
    hw::PushHardwareRequest r;r.frameGeneration=true;r.fgMethod=method;r.multiplier=multiplier;r.tier=tier;r.sourceFps=sourceFps;return r;
}
}

// --- capabilities and fallbacks ------------------------------------------------------------------------------
TEST(hardware_vendor_comes_from_the_pci_id_only){
    CHECK(hw::pushGpuVendorFromPciId(0x10DE)==hw::PushGpuVendor::Nvidia);CHECK(hw::pushGpuVendorFromPciId(0x1002)==hw::PushGpuVendor::Amd);
    CHECK(hw::pushGpuVendorFromPciId(0x8086)==hw::PushGpuVendor::Intel);CHECK(hw::pushGpuVendorFromPciId(0)==hw::PushGpuVendor::Unknown);
    CHECK(hw::pushGpuVendorFromPciId(0x1414)==hw::PushGpuVendor::Other);   // Microsoft's software adapter id
    CHECK(hw::pushGpuVendorName(hw::PushGpuVendor::Unknown)=="unknown");
    CHECK(hw::pushVrrState(4)==hw::PushVrrState::Active);CHECK(hw::pushVrrState(9)==hw::PushVrrState::Unknown);CHECK(hw::pushVrrState(-1)==hw::PushVrrState::Unknown);
    CHECK(hw::pushCapability(false,true)==HwCap::Unknown);CHECK(hw::pushCapability(true,false)==HwCap::No);
}
TEST(hardware_everything_unknown_is_the_most_conservative_setup_that_still_streams){
    hw::PushHardwareCapabilities unknown;
    hw::PushHardwareRequest r=hwWants(HwFg::Dlss,4,2);r.hdr=true;
    const auto d=hw::pushHardwareDefaults(unknown,r);
    CHECK(d.codec==hw::PushStreamCodec::H264);CHECK(!d.hdr);CHECK(!d.frameGeneration);
    CHECK(d.reasons.has("hevc-decode-unknown"));CHECK(d.reasons.has("hdr-needs-hevc"));CHECK(d.reasons.has("fg-gpu-unknown"));
    CHECK(d.opticalFlow==hw::PushOpticalFlow::GpuDis);
}
TEST(hardware_no_hevc_decoder_requests_h264_and_sdr){
    auto c=hwDesktop();c.decodeHevc=HwCap::No;c.decodeHevc10=HwCap::No;
    hw::PushHardwareRequest r;r.hdr=true;
    const auto d=hw::pushHardwareDefaults(c,r);
    CHECK(d.codec==hw::PushStreamCodec::H264);CHECK(d.reasons.has("hevc-decode-missing"));CHECK(!d.hdr);
    // A request for H.264 is left alone and gives no reason.
    r.codec=hw::PushStreamCodec::H264;r.hdr=false;
    const auto plain=hw::pushHardwareDefaults(c,r);
    CHECK(plain.codec==hw::PushStreamCodec::H264);CHECK(!plain.reasons.has("hevc-decode-missing"));
}
TEST(hardware_hdr_needs_the_10_bit_decoder_and_an_hdr_display){
    hw::PushHardwareRequest r;r.hdr=true;
    auto c=hwDesktop();CHECK(hw::pushHardwareDefaults(c,r).hdr);
    c.decodeHevc10=HwCap::No;CHECK(hw::pushHardwareDefaults(c,r).reasons.has("hdr-needs-hevc10-decode"));
    c=hwDesktop();c.hdrDisplay=HwCap::No;CHECK(!hw::pushHardwareDefaults(c,r).hdr);CHECK(hw::pushHardwareDefaults(c,r).reasons.has("hdr-display-off"));
    c.hdrDisplay=HwCap::Unknown;CHECK(!hw::pushHardwareDefaults(c,r).hdr);CHECK(hw::pushHardwareDefaults(c,r).reasons.has("hdr-display-unknown"));
}
TEST(hardware_unavailable_method_moves_to_the_next_compatible_one_never_an_experimental_one){
    auto c=hwDesktop(hw::PushGpuVendor::Amd);   // FSR 3.1, XeSS, PushFlow
    auto d=hw::pushHardwareDefaults(c,hwWants(HwFg::Dlss,4));
    CHECK(d.frameGeneration);CHECK(d.fgMethod==HwFg::Fsr);CHECK(d.multiplier==2);
    CHECK(d.reasons.has("fg-method-unavailable"));CHECK(d.reasons.has("fg-multiplier-unsupported"));
    c.fgMethods|=hw::pushFgBit(HwFg::Fsr4);CHECK(hw::pushHardwareDefaults(c,hwWants(HwFg::Dlss,2)).fgMethod==HwFg::Fsr4);
    // Only experimental providers left: frame generation off, not ANR, VFG or AMD FRC.
    c.fgMethods=hw::pushFgBit(HwFg::Anr)|hw::pushFgBit(HwFg::Vfg)|hw::pushFgBit(HwFg::AmdFrc);
    d=hw::pushHardwareDefaults(c,hwWants(HwFg::Dlss,2));
    CHECK(!d.frameGeneration);CHECK(d.reasons.has("fg-no-method"));
    // ... but a requested experimental provider that IS available is kept.
    d=hw::pushHardwareDefaults(c,hwWants(HwFg::Vfg,3));CHECK(d.frameGeneration);CHECK(d.fgMethod==HwFg::Vfg);CHECK(d.multiplier==3);
    // No vendor check anywhere: an NVIDIA adapter without the DLSS bit does not get DLSS.
    auto n=hwDesktop(hw::PushGpuVendor::Nvidia);n.fgMethods=hw::pushFgBit(HwFg::XeSS);
    CHECK(hw::pushHardwareDefaults(n,hwWants(HwFg::Dlss,3)).fgMethod==HwFg::XeSS);
}
TEST(hardware_multiplier_follows_the_method_and_the_refresh_rate){
    auto c=hwDesktop();c.fgMethods|=hw::pushFgBit(HwFg::Dlss);
    // DLSS has no 5X: 5 -> 4.
    CHECK(hw::pushHardwareDefaults(c,hwWants(HwFg::Dlss,5,1,30)).multiplier==4);
    // 60 fps on 180 Hz: 4X would be 240 -> 3X.
    auto d=hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,4,1,60));
    CHECK(d.multiplier==3);CHECK(d.reasons.has("fg-display-limit"));
    // 60 fps on 60 Hz: the source fills the panel, generation off.
    c.refreshHz=60;d=hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,2,1,60));
    CHECK(!d.frameGeneration);CHECK(d.reasons.has("fg-source-fills-display"));
    // 60 fps on 100 Hz: no whole multiplier fits.
    c.refreshHz=100;CHECK(!hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,2,1,60)).frameGeneration);
    // Unknown source rate: the method's limit only.
    c.refreshHz=180;CHECK(hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,4,1,0)).multiplier==4);
}
TEST(hardware_output_targets_for_60_to_360_hz){
    // 30 fps game on 180 Hz with PushFlow: 60, 90, 120; 120 is uneven without VRR.
    auto t=hw::pushValidOutputTargets(HwFg::PushFlow,30,180);
    CHECK(t.count==3);CHECK(t.largest()==4);CHECK(t.items[0].evenWithoutVrr);CHECK(t.items[1].evenWithoutVrr);CHECK(!t.items[2].evenWithoutVrr);
    // 59.94 fps on 179.82 Hz: 3X fits (the 2 % tolerance).
    CHECK(hw::pushValidOutputTargets(HwFg::PushFlow,59.94,179.82).largest()==3);
    // 60 Hz panel and a 60 fps source: nothing to generate.
    CHECK(hw::pushValidOutputTargets(HwFg::PushFlow,60,60).sourceFillsDisplay);
    // 30 fps on 60 Hz: 2X only.
    CHECK(hw::pushValidOutputTargets(HwFg::Dlss,30,60).largest()==2);
    // 360 Hz: DLSS 6X of 60 fits, PushFlow stops at its 4X.
    CHECK(hw::pushValidOutputTargets(HwFg::Dlss,60,360).largest()==6);CHECK(hw::pushValidOutputTargets(HwFg::PushFlow,60,360).largest()==4);
    // 144 Hz: 2X of 60 = 120 fits, uneven without VRR.
    t=hw::pushValidOutputTargets(HwFg::PushFlow,60,144);CHECK(t.largest()==2);CHECK(!t.items[0].evenWithoutVrr);
    // 240 Hz: 4X of 60 = 240 is even.
    t=hw::pushValidOutputTargets(HwFg::XeSS,60,240);CHECK(t.largest()==4);CHECK(t.items[t.count-1].evenWithoutVrr);
    // FSR: 2X only.
    CHECK(hw::pushValidOutputTargets(HwFg::Fsr,30,240).count==1);
    // Unknown rates offer nothing.
    CHECK(hw::pushValidOutputTargets(HwFg::PushFlow,0,180).count==0);CHECK(hw::pushValidOutputTargets(HwFg::PushFlow,60,0).count==0);
    CHECK(hw::pushValidOutputTargets(HwFg::PushFlow,std::numeric_limits<double>::quiet_NaN(),180).count==0);
}
TEST(hardware_low_or_unknown_vram_lowers_the_pushflow_tier){
    auto c=hwDesktop();
    CHECK(hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,2,2)).tier==2);
    c.dedicatedVramBytes=8*kHwGiB;CHECK(hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,2,2)).tier==2);
    c.dedicatedVramBytes=7*kHwGiB;auto d=hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,2,2));CHECK(d.tier==1);CHECK(d.reasons.has("tier-vram-low"));
    c.dedicatedVramBytes=5*kHwGiB;CHECK(hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,2,2)).tier==0);
    c.dedicatedVramBytes=3*kHwGiB;d=hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,2,2));CHECK(!d.frameGeneration);CHECK(d.reasons.has("fg-vram-low"));
    c.dedicatedVramBytes=0;d=hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,2,2));CHECK(d.frameGeneration);CHECK(d.tier==0);CHECK(d.reasons.has("tier-vram-unknown"));
    // An integrated GPU's small dedicated figure does not switch generation off, but caps the tier.
    c.dedicatedVramBytes=512ull<<20;c.uma=HwCap::Yes;d=hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,2,1));CHECK(d.frameGeneration);CHECK(d.tier==0);
    // A tier asked lower than the cap is kept, without a reason.
    c=hwDesktop();c.dedicatedVramBytes=7*kHwGiB;d=hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,2,0));CHECK(d.tier==0);CHECK(!d.reasons.has("tier-vram-low"));
    // Methods without tiers: always 0.
    CHECK(hw::pushHardwareDefaults(hwDesktop(),hwWants(HwFg::XeSS,2,2)).tier==0);
}
TEST(hardware_battery_power_and_hybrid_gpus){
    auto c=hwDesktop();c.power=hw::PushPowerSource::Battery;c.hybridGpu=HwCap::Yes;
    const auto d=hw::pushHardwareDefaults(c,hwWants(HwFg::PushFlow,2,2));
    CHECK(d.tier==0);CHECK(d.reasons.has("tier-battery"));CHECK(d.reasons.has("hybrid-gpu"));CHECK(d.frameGeneration);
}
TEST(hardware_old_feature_level_or_generation_not_asked_means_off){
    auto c=hwDesktop();c.featureLevel=0xb100;
    CHECK(hw::pushHardwareDefaults(c,hwWants()).reasons.has("fg-feature-level"));
    hw::PushHardwareRequest r=hwWants();r.frameGeneration=false;r.opticalFlow=hw::PushOpticalFlow::AmdFidelityFx;
    const auto d=hw::pushHardwareDefaults(hwDesktop(),r);CHECK(!d.frameGeneration);CHECK(d.multiplier==1);CHECK(d.reasons.size()==0);
}
TEST(hardware_optical_flow_falls_back_like_the_settings_sanitizer){
    auto c=hwDesktop();
    CHECK(hw::pushHardwareDefaults(c,hw::PushHardwareRequest{}).opticalFlow==hw::PushOpticalFlow::AmdFidelityFx);
    CHECK(hw::pushHardwareDefaults(c,hw::PushHardwareRequest{}).reasons.has("flow-nvidia-unavailable"));
    c.opticalFlows|=hw::pushFlowBit(hw::PushOpticalFlow::Nvidia);
    CHECK(hw::pushHardwareDefaults(c,hw::PushHardwareRequest{}).opticalFlow==hw::PushOpticalFlow::Nvidia);
}
TEST(hardware_reason_list_is_bounded_and_deduplicated){
    hw::PushReasons r;
    for(int i=0;i<40;++i)r.add("x");
    CHECK(r.size()==1);
    const char* names[]={"a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q"};
    for(const char* n:names)r.add(n);
    CHECK(r.size()==16);
    CHECK(!r.has("q"));
}

// --- GPU workload budget -------------------------------------------------------------------------------------
namespace {
constexpr std::int64_t kGbSecond=10000000;
// One second of counters at 30 fps content (33.4 ms) on 120 FPS output (8.3 ms).
hw::PushGpuBudgetSample gbSample(std::int64_t now,double gpuMs,double fgMs,double decodeMs=1,unsigned missed=0){
    hw::PushGpuBudgetSample s;s.now100ns=now;s.sourceIntervalMs=33.4;s.outputIntervalMs=8.3;s.gpuFrameMs=gpuMs;s.fgMs=fgMs;
    s.decodeQueueDelayMs=decodeMs;s.presented=120-missed;s.missedDeadlines=missed;s.skippedPairShare=0;
    s.thermal=hw::PushThermalState::Normal;s.power=hw::PushPowerSource::Mains;return s;
}
hw::PushGpuBudgetSample gbHealthy(std::int64_t now){return gbSample(now,4,8);}      // load 0.36
hw::PushGpuBudgetSample gbBusy(std::int64_t now){return gbSample(now,6,25);}        // load 0.93
hw::PushGpuBudgetSample gbSevere(std::int64_t now){return gbSample(now,10,40);}     // load 1.50
// Feeds one sample a second from `from` for `seconds`, returns the last decision.
template<class F> hw::PushGpuBudgetDecision gbRun(hw::PushGpuBudget& b,std::int64_t& now,int seconds,F sample){
    hw::PushGpuBudgetDecision d;for(int i=0;i<seconds;++i){d=b.update(sample(now));now+=kGbSecond;}return d;
}
}
TEST(gpu_budget_ladder_sheds_tier_then_multiplier_then_generation){
    const auto l=hw::pushGpuBudgetLadder(HwFg::PushFlow,4,2);
    // Quality 4X, Balanced 4X, Fast 4X, Fast 3X, Fast 2X, off.
    CHECK(l.count==6);
    CHECK(l[0].tier==2&&l[0].multiplier==4);CHECK(l[1].tier==1&&l[1].multiplier==4);CHECK(l[2].tier==0&&l[2].multiplier==4);
    CHECK(l[3].multiplier==3);CHECK(l[4].multiplier==2);CHECK(!l[5].frameGeneration());
    // DLSS 6X: 6, 4, 3, 2, off (no 5X); FSR: 2, off; generation off: one level.
    const auto d=hw::pushGpuBudgetLadder(HwFg::Dlss,6,2);CHECK(d.count==5);CHECK(d[0].multiplier==6&&d[1].multiplier==4&&d[0].tier==0);
    CHECK(hw::pushGpuBudgetLadder(HwFg::Fsr,4,0).count==2);
    CHECK(hw::pushGpuBudgetLadder(HwFg::PushFlow,1,2).count==1);
    // VFG 8X stays inside the fixed capacity.
    CHECK(hw::pushGpuBudgetLadder(HwFg::Vfg,8,0).count==8);
}
TEST(gpu_budget_starts_at_the_request_and_stays_while_healthy){
    hw::PushGpuBudget b(hw::pushGpuBudgetLadder(HwFg::PushFlow,4,2));std::int64_t now=kGbSecond;
    auto d=b.update(gbHealthy(now));CHECK(d.index==0);CHECK(!d.changed);CHECK(d.reason=="at-ceiling");
    d=gbRun(b,now,120,gbHealthy);CHECK(d.index==0);CHECK(d.level.tier==2);CHECK(d.level.multiplier==4);
}
TEST(gpu_budget_steps_down_one_level_after_sustained_load_with_dwell_between_steps){
    hw::PushGpuBudget b(hw::pushGpuBudgetLadder(HwFg::PushFlow,4,2));std::int64_t now=kGbSecond;
    gbRun(b,now,40,gbHealthy);
    auto d=b.update(gbBusy(now));CHECK(!d.changed);CHECK(d.reason=="trouble-gpu-load");now+=kGbSecond;
    d=b.update(gbBusy(now));CHECK(!d.changed);now+=kGbSecond;
    d=b.update(gbBusy(now));CHECK(d.changed);CHECK(d.reason=="step-down-gpu-load");CHECK(d.index==1);CHECK(d.level.tier==1);now+=kGbSecond;
    // Still busy: the next ordinary step waits for the dwell (3 s) and the trouble hold (2 s) again.
    int steps=0;std::int64_t lastStep=now-kGbSecond;bool spaced=true;
    for(int i=0;i<30;++i){d=b.update(gbBusy(now));if(d.changed){++steps;spaced=spaced&&now-lastStep>=3*kGbSecond;lastStep=now;}now+=kGbSecond;}
    CHECK(spaced);CHECK(b.index()==b.ladder().count-1);CHECK(!b.level().frameGeneration());CHECK(steps==4);
    // Generation is off now, so the GPU figure is low; decoding still waits: nothing left to shed.
    d=b.update(gbSample(now,6,25,20));CHECK(d.reason=="at-floor");CHECK(!d.changed);
}
TEST(gpu_budget_severe_overload_steps_faster_but_one_level_at_a_time){
    hw::PushGpuBudget b(hw::pushGpuBudgetLadder(HwFg::PushFlow,4,2));
    std::int64_t now=kGbSecond;b.update(gbHealthy(now));
    const std::int64_t half=kGbSecond/2;std::size_t last=0;bool single=true;
    for(int i=0;i<12;++i){now+=half;const auto d=b.update(gbSevere(now));single=single&&d.index<=last+1;last=d.index;if(d.changed)CHECK(d.reason=="severe-step-down-gpu-load");}
    CHECK(single);CHECK(b.index()==5);
}
TEST(gpu_budget_protects_decoding_and_presentation_before_the_gpu_figure){
    // GPU load is fine, but decoded pictures wait half a source interval.
    hw::PushGpuBudget b(hw::pushGpuBudgetLadder(HwFg::PushFlow,3,1));std::int64_t now=kGbSecond;
    auto d=gbRun(b,now,4,[](std::int64_t t){return gbSample(t,4,8,20);});
    CHECK(d.changed||b.index()==1);CHECK(b.index()==1);
    // Missed presentation deadlines (5 %) with an idle GPU.
    hw::PushGpuBudget p(hw::pushGpuBudgetLadder(HwFg::PushFlow,3,1));now=kGbSecond;
    d=p.update(gbSample(now,2,4,1,6));CHECK(d.reason=="trouble-missed-deadlines");
    // A generated frame dearer than an output interval (2X: 9 ms > 8.3 x 0.85) with a low total load.
    hw::PushGpuBudget g(hw::pushGpuBudgetLadder(HwFg::PushFlow,2,0));now=kGbSecond;
    d=g.update(gbSample(now,2,9));CHECK(d.reason=="trouble-generated-frame-cost");
    // Thermal throttling and pairs skipped by the per-pair admission are trouble too.
    auto t=gbSample(now,4,4);t.thermal=hw::PushThermalState::Throttling;hw::PushGpuBudget th(hw::pushGpuBudgetLadder(HwFg::PushFlow,2,0));
    CHECK(th.update(t).reason=="trouble-thermal");
    auto s=gbSample(now,4,4);s.skippedPairShare=0.5;hw::PushGpuBudget sk(hw::pushGpuBudgetLadder(HwFg::PushFlow,2,0));
    CHECK(sk.update(s).reason=="trouble-skipped-pairs");
}
TEST(gpu_budget_recovers_only_after_a_sustained_healthy_period_and_never_above_the_request){
    hw::PushGpuBudget b(hw::pushGpuBudgetLadder(HwFg::PushFlow,3,1));std::int64_t now=kGbSecond;
    gbRun(b,now,5,gbBusy);CHECK(b.index()==1);
    // 19 healthy seconds: still down.
    auto d=gbRun(b,now,19,gbHealthy);CHECK(b.index()==1);CHECK(d.reason=="healthy-waiting");
    // One neutral second (load between the thresholds) restarts the period.
    d=b.update(gbSample(now,8,14));now+=kGbSecond;CHECK(d.reason=="hold");
    gbRun(b,now,19,gbHealthy);CHECK(b.index()==1);
    d=gbRun(b,now,2,gbHealthy);CHECK(b.index()==0);
    // At the request it never goes higher.
    d=gbRun(b,now,200,gbHealthy);CHECK(b.index()==0);CHECK(d.reason=="at-ceiling");
}
TEST(gpu_budget_never_steps_up_on_unknown_input_or_battery){
    hw::PushGpuBudget b(hw::pushGpuBudgetLadder(HwFg::PushFlow,3,1));std::int64_t now=kGbSecond;
    gbRun(b,now,5,gbBusy);const auto down=b.index();CHECK(down==1);
    // No measurements at all: neither trouble nor healthy.
    auto d=gbRun(b,now,120,[](std::int64_t t){hw::PushGpuBudgetSample s;s.now100ns=t;return s;});
    CHECK(b.index()==down);CHECK(d.reason=="hold");
    // Decode delay not measured: no step up either.
    d=gbRun(b,now,120,[](std::int64_t t){auto s=gbHealthy(t);s.decodeQueueDelayMs=-1;return s;});CHECK(b.index()==down);
    // Battery: healthy numbers, still no step up.
    d=gbRun(b,now,120,[](std::int64_t t){auto s=gbHealthy(t);s.power=hw::PushPowerSource::Battery;return s;});
    CHECK(b.index()==down);CHECK(d.reason=="on-battery");
    // NaN counts as unknown, never as healthy or trouble.
    d=b.update([&]{auto s=gbHealthy(now);s.gpuFrameMs=std::numeric_limits<double>::quiet_NaN();return s;}());CHECK(!d.changed);
}
TEST(gpu_budget_failed_returns_double_the_healthy_period){
    hw::PushGpuBudget b(hw::pushGpuBudgetLadder(HwFg::PushFlow,3,1));std::int64_t now=kGbSecond;
    gbRun(b,now,5,gbBusy);gbRun(b,now,21,gbHealthy);CHECK(b.index()==0);
    // Trouble soon after the return: down again, and the next return needs 40 s.
    gbRun(b,now,5,gbBusy);CHECK(b.index()==1);
    gbRun(b,now,35,gbHealthy);CHECK(b.index()==1);
    gbRun(b,now,7,gbHealthy);CHECK(b.index()==0);
}
TEST(gpu_budget_does_not_step_up_into_a_level_predicted_over_budget){
    // Level costs: the last 3X sample was 0.93, the first 2X one 0.62 (ratio 1.5). A healthy 0.55 predicts 0.83 at
    // 3X, over the midpoint 0.725 of healthy and trouble: stays. 0.42 predicts 0.63: steps up.
    hw::PushGpuBudget b(hw::pushGpuBudgetLadder(HwFg::PushFlow,3,0));std::int64_t now=kGbSecond;
    gbRun(b,now,4,[](std::int64_t t){return gbSample(t,6,25);});CHECK(b.index()==1);   // 0.93 at 3X, the step is the last sample
    gbRun(b,now,1,[](std::int64_t t){return gbSample(t,14.7,6);});                       // 0.62 at 2X (neutral)
    auto d=gbRun(b,now,40,[](std::int64_t t){return gbSample(t,12.4,6);});              // 0.55 at 2X: healthy
    CHECK(b.index()==1);CHECK(d.reason=="step-up-predicted-over-budget");
    d=gbRun(b,now,40,[](std::int64_t t){return gbSample(t,8,6);});                      // 0.42 at 2X
    CHECK(b.index()==0);
}
TEST(gpu_budget_clock_going_back_and_reset){
    hw::PushGpuBudget b(hw::pushGpuBudgetLadder(HwFg::PushFlow,3,1));std::int64_t now=10*kGbSecond;
    gbRun(b,now,5,gbBusy);CHECK(b.index()==1);
    CHECK(b.update(gbBusy(kGbSecond)).reason=="bad-input");
    b.reset(hw::pushGpuBudgetLadder(HwFg::Fsr,2,0));CHECK(b.index()==0);CHECK(b.level().multiplier==2);CHECK(b.ladder().count==2);
    // An empty ladder is replaced by "off".
    b.reset(hw::PushGpuBudgetLadder{});CHECK(b.ladder().count==1);CHECK(!b.level().frameGeneration());
    // Silly configuration values are sanitized.
    hw::PushGpuBudgetConfig c;c.loadHigh=-1;c.loadLow=5;c.troubleHold100ns=-5;c.missedSevere=0;
    hw::PushGpuBudget s(hw::pushGpuBudgetLadder(HwFg::PushFlow,2,0),c);
    CHECK(s.config().loadHigh>0);CHECK(s.config().loadLow<=s.config().loadHigh);CHECK(s.config().troubleHold100ns==0);CHECK(s.config().missedSevere>=s.config().missedHigh);
}
