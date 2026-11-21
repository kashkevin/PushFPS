// SPDX-License-Identifier: GPL-3.0-only
// Simulated hardware profiles (veyra/engine/PushHardwareProfiles.h) run through the capability model and the
// start-up model. These are MODELS of capability reports: a pass says the decision code has a safe answer for
// every report, never that a real PC of that family works. Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushHardwareProfiles.h"
#include <cstdint>
#include <string_view>
namespace {
namespace hp=veyra::engine;
using HpFg=hp::PushFgMethod;
using HpCap=hp::PushCapability;
const hp::PushHardwareProfile& hpProfile(const std::array<hp::PushHardwareProfile,hp::kPushHardwareProfileCount>& all,std::string_view id){
    for(const auto& p:all)if(p.id==id)return p;
    throw std::runtime_error(std::string("no profile ")+std::string(id));
}
// The requests a user can make: every method x a low and a high multiplier x every tier, with and without HDR,
// HEVC and H.264, for a 30 and a 60 fps game.
template<class F> void hpForEachRequest(F&& f){
    for(unsigned m=0;m<hp::kPushFgMethodCount;++m)
        for(unsigned mult:{2u,4u,8u})
            for(unsigned tier=0;tier<3;++tier)
                for(bool hdr:{false,true})
                    for(auto codec:{hp::PushStreamCodec::Hevc,hp::PushStreamCodec::H264})
                        for(double fps:{30.0,60.0}){
                            hp::PushHardwareRequest r;r.frameGeneration=true;r.fgMethod=static_cast<HpFg>(m);r.multiplier=mult;r.tier=tier;r.hdr=hdr;
                            r.codec=codec;r.sourceFps=fps;r.opticalFlow=hp::PushOpticalFlow::Nvidia;f(r);
                        }
}
constexpr std::uint32_t kHpProprietary=hp::pushFgBit(HpFg::Dlss)|hp::pushFgBit(HpFg::Vfg)|hp::pushFgBit(HpFg::Fsr4)|hp::pushFgBit(HpFg::AmdFrc)|hp::pushFgBit(HpFg::Anr);
}

TEST(hardware_profiles_table_is_complete_and_named_uniquely){
    const auto all=hp::pushHardwareProfiles();
    bool nvidia=false,amd=false,intel=false,hybrid=false,odd=false;
    for(std::size_t i=0;i<all.size();++i){
        CHECK(!all[i].id.empty());
        for(std::size_t j=0;j<i;++j)CHECK(all[i].id!=all[j].id);
        nvidia=nvidia||all[i].family==hp::PushProfileFamily::Nvidia;amd=amd||all[i].family==hp::PushProfileFamily::Amd;
        intel=intel||all[i].family==hp::PushProfileFamily::Intel;hybrid=hybrid||all[i].family==hp::PushProfileFamily::Hybrid;
        odd=odd||all[i].family==hp::PushProfileFamily::Odd;
    }
    CHECK(nvidia&&amd&&intel&&hybrid&&odd);
}
TEST(hardware_profiles_gates_follow_the_bridge_rules){
    const auto all=hp::pushHardwareProfiles();
    // NVIDIA-only methods never on AMD / Intel; AMD-only never on NVIDIA / Intel; PushFlow on every target family.
    for(const auto& p:all){
        if(p.gpu.vendor!=0x10DE){CHECK(!p.caps.hasFg(HpFg::Dlss));CHECK(!p.caps.hasFg(HpFg::Vfg));CHECK(!p.caps.hasFlow(hp::PushOpticalFlow::Nvidia));}
        if(p.gpu.vendor!=0x1002&&p.id!="contradictory-uma-16gb-fsr4-bit-on-nvidia"){CHECK(!p.caps.hasFg(HpFg::Fsr4));CHECK(!p.caps.hasFg(HpFg::AmdFrc));}
        CHECK(!p.caps.hasFg(HpFg::Anr));   // never shipped: the user installs it
        CHECK(p.caps.hasFlow(hp::PushOpticalFlow::GpuDis));
    }
    CHECK(hpProfile(all,"nvidia-rtx50-12gb-180hz-hdr").caps.hasFg(HpFg::Dlss));CHECK(hpProfile(all,"nvidia-rtx50-12gb-180hz-hdr").caps.hasFg(HpFg::PushFlow));
    CHECK(hpProfile(all,"nvidia-rtx50-12gb-180hz-hdr").caps.hasFlow(hp::PushOpticalFlow::Nvidia));
    CHECK(!hpProfile(all,"nvidia-rtx40-fork-build-no-nvof").caps.hasFlow(hp::PushOpticalFlow::Nvidia));
    CHECK(hpProfile(all,"amd-rx9000-16gb-240hz").caps.hasFg(HpFg::Fsr4));CHECK(!hpProfile(all,"amd-rx7000-20gb-144hz-hdr").caps.hasFg(HpFg::Fsr4));
    CHECK(hpProfile(all,"amd-rx6000-8gb-165hz").caps.hasFg(HpFg::AmdFrc));CHECK(!hpProfile(all,"amd-rx6000-no-amf-runtime").caps.hasFg(HpFg::AmdFrc));
    for(const auto* id:{"intel-arc-b-12gb-180hz","intel-arc-a-8gb-120hz","amd-rx6000-8gb-165hz","nvidia-rtx30-10gb-60hz-sdr"}){
        const auto& p=hpProfile(all,id);CHECK(p.caps.hasFg(HpFg::PushFlow));CHECK(p.caps.hasFg(HpFg::Fsr));CHECK(p.caps.hasFg(HpFg::XeSS));
    }
    // An adapter of no target vendor gets no cross-vendor method either (the bridge's CrossVendor gate).
    CHECK(hpProfile(all,"unknown-vendor-12_0").caps.fgMethods==0);
    CHECK(hpProfile(all,"non-rtx-nvidia-name").caps.hasFg(HpFg::PushFlow));CHECK(!hpProfile(all,"non-rtx-nvidia-name").caps.hasFg(HpFg::Dlss));
}
TEST(hardware_profiles_every_request_gives_a_valid_configuration){
    const auto all=hp::pushHardwareProfiles();
    unsigned checked=0;
    for(const auto& p:all)
        hpForEachRequest([&](const hp::PushHardwareRequest& r){
            const auto d=hp::pushHardwareDefaults(p.caps,r);
            CHECK(hp::pushDefaultsValid(p.caps,r,d));
            // A proprietary method is only ever the answer where its own bit says it exists.
            if(d.frameGeneration&&(hp::pushFgBit(d.fgMethod)&kHpProprietary))CHECK(p.caps.hasFg(d.fgMethod));
            // A method the user did not ask for is never an experimental one.
            if(d.frameGeneration&&d.fgMethod!=r.fgMethod)
                CHECK(d.fgMethod==HpFg::Fsr4||d.fgMethod==HpFg::Fsr||d.fgMethod==HpFg::XeSS||d.fgMethod==HpFg::Dlss);
            ++checked;
        });
    CHECK(checked==all.size()*8u*3u*3u*2u*2u*2u);
}
TEST(hardware_profiles_proprietary_features_are_optional_everywhere){
    // Take every proprietary bit away from every profile: each request still has a valid answer, and wherever a
    // cross-vendor method exists, generation stays possible.
    for(auto p:hp::pushHardwareProfiles()){
        p.caps.fgMethods&=~kHpProprietary;p.caps.opticalFlows&=~hp::pushFlowBit(hp::PushOpticalFlow::Nvidia);
        hpForEachRequest([&](const hp::PushHardwareRequest& r){
            const auto d=hp::pushHardwareDefaults(p.caps,r);
            CHECK(hp::pushDefaultsValid(p.caps,r,d));
            CHECK(d.opticalFlow!=hp::PushOpticalFlow::Nvidia);
            if(d.frameGeneration)CHECK(!(hp::pushFgBit(d.fgMethod)&kHpProprietary));
        });
    }
}
TEST(hardware_profiles_first_launch_is_safe_on_every_profile){
    // A fresh profile: frame generation, NR and SR off (EnhancementSettings defaults); the starting method is
    // DLSS / FSR / FSR 4 by vendor, then the sanitizer keeps it only when available.
    for(const auto& p:hp::pushHardwareProfiles()){
        const auto start=hp::pushFirstLaunchFgMethod(p.gpu.vendor,p.gpu.rx9000);
        HpFg chosen=start;
        const bool any=hp::pushNextFgMethod(p.caps,start,chosen);
        if(any)CHECK(p.caps.hasFg(chosen));
        if(p.caps.hasFg(start))CHECK(chosen==start);
        hp::PushHardwareRequest off;off.frameGeneration=false;off.codec=hp::PushStreamCodec::H264;off.opticalFlow=hp::PushOpticalFlow::Nvidia;
        const auto d=hp::pushHardwareDefaults(p.caps,off);
        CHECK(!d.frameGeneration);CHECK(d.multiplier==1);CHECK(hp::pushDefaultsValid(p.caps,off,d));
    }
    CHECK(hp::pushFirstLaunchFgMethod(0x1002,true)==HpFg::Fsr4);CHECK(hp::pushFirstLaunchFgMethod(0x8086,false)==HpFg::Fsr);
    CHECK(hp::pushFirstLaunchFgMethod(0x10DE,false)==HpFg::Dlss);CHECK(hp::pushFirstLaunchFgMethod(0,false)==HpFg::Dlss);
}
TEST(hardware_profiles_failed_backends_land_on_the_next_choice_then_plain_presentation){
    const auto all=hp::pushHardwareProfiles();
    for(const auto& p:all){
        if(!p.engineAdapter)continue;
        hpForEachRequest([&](const hp::PushHardwareRequest& r){
            // Every subset of failing methods that matters: none, the chosen one, all.
            for(std::uint32_t fails:{0u,0xFFu,hp::pushFgBit(r.fgMethod),hp::pushFgBit(HpFg::Fsr4)|hp::pushFgBit(HpFg::Fsr)}){
                hp::PushRuntimeFaults faults;faults.fgInitFails=fails;
                const auto o=hp::pushSimulateStartup(p,r,faults);
                CHECK(o.presents);
                CHECK(o.fgAttempts<=6);
                CHECK(o.decode!=hp::PushDecodePath::None);   // Automatic decode always ends on a decoder
                if(o.frameGeneration){
                    CHECK(!(fails&hp::pushFgBit(o.fgMethod)));
                    CHECK(hp::pushFgMultiplierSelectable(o.fgMethod,o.multiplier));
                    CHECK(o.tier<hp::pushFgTierCount(o.fgMethod));
                }else CHECK(o.multiplier==1);
                if(fails==0xFFu)CHECK(!o.frameGeneration);
            }
        });
    }
    // FSR 4 ML refused by the driver: FSR 3.1 at 2X; FSR 3.1 refused as well: plain presentation.
    const auto& rx9000=hpProfile(all,"amd-rx9000-16gb-240hz");
    hp::PushHardwareRequest r;r.frameGeneration=true;r.fgMethod=HpFg::Fsr4;r.multiplier=2;r.sourceFps=60;
    hp::PushRuntimeFaults f;f.fgInitFails=hp::pushFgBit(HpFg::Fsr4);
    auto o=hp::pushSimulateStartup(rx9000,r,f);CHECK(o.frameGeneration);CHECK(o.fgMethod==HpFg::Fsr);CHECK(o.multiplier==2);CHECK(o.fgAttempts==2);
    f.fgInitFails|=hp::pushFgBit(HpFg::Fsr);o=hp::pushSimulateStartup(rx9000,r,f);CHECK(!o.frameGeneration);CHECK(o.presents);
    // PushFlow failing on an Intel Arc: generation off (the engine does not move PushFlow to another method).
    const auto& arc=hpProfile(all,"intel-arc-b-12gb-180hz");
    r.fgMethod=HpFg::PushFlow;r.multiplier=3;f.fgInitFails=hp::pushFgBit(HpFg::PushFlow);
    o=hp::pushSimulateStartup(arc,r,f);CHECK(!o.frameGeneration);CHECK(o.presents);CHECK(o.fgAttempts==1);
}
TEST(hardware_profiles_decode_fallback){
    const auto all=hp::pushHardwareProfiles();
    const auto& noHevc=hpProfile(all,"no-hevc-decode");
    hp::PushHardwareRequest r;r.codec=hp::PushStreamCodec::Hevc;
    // The model asks for H.264 there; if the app still requests HEVC (it does not apply the model yet), Automatic
    // decode falls back to software and Hardware mode fails the stream with its message.
    auto o=hp::pushSimulateStartup(noHevc,r);CHECK(o.codec==hp::PushStreamCodec::H264);CHECK(o.decode==hp::PushDecodePath::Hardware);
    CHECK(o.defaults.reasons.has("hevc-decode-missing"));
    CHECK(hp::pushDecodePath(noHevc,hp::PushStreamCodec::Hevc,false,hp::PushDecodeMode::Automatic,false)==hp::PushDecodePath::SoftwareAfterHardwareFailed);
    CHECK(hp::pushDecodePath(noHevc,hp::PushStreamCodec::Hevc,false,hp::PushDecodeMode::Hardware,false)==hp::PushDecodePath::None);
    CHECK(hp::pushDecodePath(noHevc,hp::PushStreamCodec::Hevc,false,hp::PushDecodeMode::Software,false)==hp::PushDecodePath::Software);
    // Listed but refused at run time: software after the failure.
    const auto& rtx=hpProfile(all,"nvidia-rtx50-12gb-180hz-hdr");
    hp::PushRuntimeFaults f;f.hardwareDecodeFails=true;
    CHECK(hp::pushSimulateStartup(rtx,r,f).decode==hp::PushDecodePath::SoftwareAfterHardwareFailed);
    // Unknown profiles: H.264 requested, the hardware decoder is still tried.
    const auto& unknown=hpProfile(all,"decode-profiles-unknown");
    o=hp::pushSimulateStartup(unknown,r);CHECK(o.codec==hp::PushStreamCodec::H264);CHECK(o.defaults.reasons.has("hevc-decode-unknown"));CHECK(o.decode==hp::PushDecodePath::Hardware);
    // No engine adapter: nothing decodes, nothing presents, and the model still answers.
    const auto& sw=hpProfile(all,"software-adapter-only");
    o=hp::pushSimulateStartup(sw,r);CHECK(!o.presents);CHECK(o.decode==hp::PushDecodePath::None);CHECK(o.codec==hp::PushStreamCodec::H264);
}
TEST(hardware_profiles_hdr_chain){
    const auto all=hp::pushHardwareProfiles();
    hp::PushHardwareRequest r;r.hdr=true;
    CHECK(hp::pushSimulateStartup(hpProfile(all,"nvidia-rtx50-12gb-180hz-hdr"),r).hdr);
    CHECK(hp::pushSimulateStartup(hpProfile(all,"amd-rx7000-20gb-144hz-hdr"),r).hdr);
    auto o=hp::pushSimulateStartup(hpProfile(all,"hevc-8-bit-only-hdr-display"),r);CHECK(!o.hdr);CHECK(o.codec==hp::PushStreamCodec::Hevc);
    CHECK(o.defaults.reasons.has("hdr-needs-hevc10-decode"));
    o=hp::pushSimulateStartup(hpProfile(all,"nvidia-rtx30-10gb-60hz-sdr"),r);CHECK(!o.hdr);CHECK(o.defaults.reasons.has("hdr-display-off"));
    o=hp::pushSimulateStartup(hpProfile(all,"contradictory-uma-16gb-fsr4-bit-on-nvidia"),r);CHECK(!o.hdr);CHECK(o.codec==hp::PushStreamCodec::H264);
}
TEST(hardware_profiles_recording_encoder){
    const auto all=hp::pushHardwareProfiles();
    hp::PushHardwareRequest r;r.hdr=true;
    // HDR recording only through NVENC; AMD with HDR shown records nothing (the recorder refuses with its message).
    auto o=hp::pushSimulateStartup(hpProfile(all,"nvidia-rtx50-12gb-180hz-hdr"),r);CHECK(o.recording==hp::PushRecordingEncoder::Nvenc);CHECK(o.recordingHdr);
    o=hp::pushSimulateStartup(hpProfile(all,"amd-rx7000-20gb-144hz-hdr"),r);CHECK(o.hdr);CHECK(o.recording==hp::PushRecordingEncoder::None);
    r.hdr=false;
    CHECK(hp::pushSimulateStartup(hpProfile(all,"intel-arc-a-8gb-120hz"),r).recording==hp::PushRecordingEncoder::MediaFoundation);
    CHECK(hp::pushSimulateStartup(hpProfile(all,"no-hardware-encoder"),r).recording==hp::PushRecordingEncoder::None);
    hp::PushRuntimeFaults f;f.nvencFails=true;   // no Media Foundation fallback in the live recorder once NVENC is chosen
    CHECK(hp::pushSimulateStartup(hpProfile(all,"nvidia-rtx40-8gb-144hz"),r,f).recording==hp::PushRecordingEncoder::None);
}
TEST(hardware_profiles_odd_reports_are_conservative){
    const auto all=hp::pushHardwareProfiles();
    hp::PushHardwareRequest r;r.frameGeneration=true;r.fgMethod=HpFg::PushFlow;r.multiplier=4;r.tier=2;r.sourceFps=30;r.hdr=true;
    auto d=hp::pushHardwareDefaults(hpProfile(all,"driver-reports-nothing").caps,r);
    CHECK(!d.frameGeneration);CHECK(d.codec==hp::PushStreamCodec::H264);CHECK(!d.hdr);CHECK(d.reasons.has("fg-gpu-unknown"));
    d=hp::pushHardwareDefaults(hpProfile(all,"contradictory-vendor-known-level-unknown").caps,r);CHECK(!d.frameGeneration);CHECK(d.reasons.has("fg-feature-level-unknown"));
    d=hp::pushHardwareDefaults(hpProfile(all,"unknown-vendor-12_0").caps,r);CHECK(!d.frameGeneration);CHECK(d.reasons.has("fg-no-method"));
    d=hp::pushHardwareDefaults(hpProfile(all,"nvidia-no-runtime-files").caps,r);CHECK(!d.frameGeneration);CHECK(d.reasons.has("fg-no-method"));
    d=hp::pushHardwareDefaults(hpProfile(all,"nvidia-4gb-vram").caps,r);CHECK(!d.frameGeneration);CHECK(d.reasons.has("fg-vram-low"));
    // UMA with a large "dedicated" figure and no refresh rate: Fast only, the method's own limit.
    d=hp::pushHardwareDefaults(hpProfile(all,"contradictory-uma-16gb-fsr4-bit-on-nvidia").caps,r);CHECK(d.frameGeneration);CHECK(d.tier==0);CHECK(d.multiplier==4);
    // Hybrid laptop on battery at 240 Hz: Fast, flagged hybrid.
    d=hp::pushHardwareDefaults(hpProfile(all,"hybrid-rtx40-laptop-battery").caps,r);CHECK(d.frameGeneration);CHECK(d.tier==0);
    CHECK(d.reasons.has("tier-battery"));CHECK(d.reasons.has("hybrid-gpu"));
    // Integrated GPU on a 60 Hz panel: a 30 fps game gets 2X at most, Fast.
    d=hp::pushHardwareDefaults(hpProfile(all,"intel-integrated-uma").caps,r);CHECK(d.frameGeneration);CHECK(d.multiplier==2);CHECK(d.tier==0);
    // 360 Hz: PushFlow keeps its 4X of 60 fps; 60 Hz SDR with a 60 fps game: nothing to generate.
    r.sourceFps=60;
    d=hp::pushHardwareDefaults(hpProfile(all,"nvidia-rtx50-16gb-360hz").caps,r);CHECK(d.multiplier==4);CHECK(d.tier==2);
    d=hp::pushHardwareDefaults(hpProfile(all,"nvidia-rtx30-10gb-60hz-sdr").caps,r);CHECK(!d.frameGeneration);CHECK(d.reasons.has("fg-source-fills-display"));
}
TEST(hardware_vrr_dxgi_report_is_a_diagnostic_only_without_nvapi){
    // Recorded only with the switch on and NVAPI not ready; the VRR level itself never comes from it
    // (static_assert in NvidiaVrr.h: dxgiTearing set leaves vrrLevel as for an empty state).
    for(auto t:{HpCap::Unknown,HpCap::No,HpCap::Yes}){
        CHECK(hp::pushDxgiTearingReport(true,false,t)==t);
        CHECK(hp::pushDxgiTearingReport(false,false,t)==HpCap::Unknown);
        CHECK(hp::pushDxgiTearingReport(true,true,t)==HpCap::Unknown);
        CHECK(hp::pushDxgiTearingReport(false,true,t)==HpCap::Unknown);
    }
}
TEST(hardware_pushflow_crash_guard_prefers_non_experimental_methods){
    // The non-experimental prefix is the capability model's fallback order.
    hp::PushHardwareCapabilities c;
    for(std::uint32_t mask=0;mask<(1u<<hp::kPushFgMethodCount);++mask){
        HpFg guard=HpFg::PushFlow;const bool found=hp::pushCrashGuardChoice(mask,guard);
        CHECK(found==((mask&~hp::pushFgBit(HpFg::PushFlow))!=0));
        if(!found)continue;
        CHECK(guard!=HpFg::PushFlow);CHECK((mask&hp::pushFgBit(guard))!=0);
        c.fgMethods=mask&~hp::pushFgBit(HpFg::PushFlow);
        HpFg model=HpFg::PushFlow;
        if(hp::pushNextFgMethod(c,HpFg::PushFlow,model))CHECK(guard==model);   // a non-experimental one exists: the same choice
        else CHECK(guard==HpFg::Vfg||guard==HpFg::Anr||guard==HpFg::AmdFrc);
    }
    std::size_t i=0;for(const auto m:hp::kPushCrashGuardOrder){CHECK(m!=HpFg::PushFlow);if(i++<hp::kPushCrashGuardNonExperimental)CHECK(m!=HpFg::Vfg&&m!=HpFg::Anr&&m!=HpFg::AmdFrc);}
    // An NVIDIA desktop with every file: FSR 3.1 before DLSS (the model's order), VFG only when nothing else is there.
    HpFg chosen=HpFg::PushFlow;
    CHECK(hp::pushCrashGuardChoice(hp::pushFgBit(HpFg::Dlss)|hp::pushFgBit(HpFg::Fsr)|hp::pushFgBit(HpFg::Vfg),chosen));CHECK(chosen==HpFg::Fsr);
    CHECK(hp::pushCrashGuardChoice(hp::pushFgBit(HpFg::Vfg)|hp::pushFgBit(HpFg::AmdFrc),chosen));CHECK(chosen==HpFg::Vfg);
}
