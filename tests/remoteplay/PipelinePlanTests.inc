// SPDX-License-Identifier: GPL-3.0-or-later
// Enhancement pipeline planner (veyra/engine/PushPipelinePlan.h). Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushPipelinePlan.h"
namespace {
namespace ppl = veyra::engine;
ppl::PipelineCaps pplAllCaps() {
    ppl::PipelineCaps c;
    c.dlssSr = c.vsr = c.nis = c.fsr = true;
    c.nrRuntime = true;
    c.vfg = c.dlssFg = c.pushFlow = c.fsrFg = true;
    return c;
}
ppl::PipelineRequest pplBase() {
    ppl::PipelineRequest r;
    r.streamWidth = 1920; r.streamHeight = 1080; r.streamFps = 59.94; r.contentFps = 30.0; r.hdr = true;
    r.displayWidth = 2560; r.displayHeight = 1440; r.displayHz = 180.0;
    return r;
}
}  // namespace

TEST(pipeline_plan_order_clean_first_then_upscale_neural_framegen_present){
    auto r = pplBase();
    r.clean = 40; r.upscaler = ppl::PipeUpscaler::Dlss; r.srTarget = ppl::PipeSrTarget::Qhd; r.nr = true; r.fg = ppl::PipeFg::Vfg;
    const auto p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.stageCount == 6);
    CHECK(p.stages[0].stage == ppl::PipeStage::Decode);
    CHECK(p.stages[1].stage == ppl::PipeStage::Clean);
    CHECK(p.stages[1].input.width == 1920 && p.stages[1].output.height == 1080);  // stream size
    CHECK(p.stages[1].detail == 40);
    CHECK(p.stages[2].stage == ppl::PipeStage::Upscale);
    CHECK(p.stages[2].output.width == 2560 && p.stages[2].output.height == 1440);
    CHECK(p.stages[3].stage == ppl::PipeStage::Neural);
    CHECK(p.stages[4].stage == ppl::PipeStage::FrameGeneration);
    CHECK(p.stages[5].stage == ppl::PipeStage::Present);
    CHECK(p.output.width == 2560 && p.output.height == 1440);
    // Nothing was changed; the one entry is the note that the neural layer is not verified for HDR.
    CHECK(p.changeCount == 1 && p.changes[0].action == ppl::PipeAction::Noted && p.changes[0].reason == ppl::PipeReason::NeuralHdrUnverified);
    CHECK(p.experimental);  // the neural layer is experimental
    r.hdr = false;
    CHECK(ppl::planPipeline(r, pplAllCaps()).changeCount == 0);
}
TEST(pipeline_plan_neural_before_upscale_runs_on_cleaned_stream){
    auto r = pplBase();
    r.clean = 20; r.upscaler = ppl::PipeUpscaler::Dlss; r.nr = true; r.nrPlace = ppl::PipeNrPlace::BeforeUpscale;
    const auto p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.stages[1].stage == ppl::PipeStage::Clean);
    CHECK(p.stages[2].stage == ppl::PipeStage::Neural);
    CHECK(p.stages[2].input.height == 1080);
    CHECK(p.stages[3].stage == ppl::PipeStage::Upscale);
}
TEST(pipeline_plan_neural_size_policy_and_cap){
    auto r = pplBase();
    r.upscaler = ppl::PipeUpscaler::Dlss; r.nr = true;
    struct Row { ppl::PipeNrSize size; std::uint32_t h; veyra::pipeline::NrSizePolicy policy; };
    const Row rows[] = {{ppl::PipeNrSize::P480, 480, veyra::pipeline::NrSizePolicy::P480},
                        {ppl::PipeNrSize::P720, 720, veyra::pipeline::NrSizePolicy::P720},
                        {ppl::PipeNrSize::P900, 900, veyra::pipeline::NrSizePolicy::P900},
                        {ppl::PipeNrSize::Realtime1080, 1080, veyra::pipeline::NrSizePolicy::Realtime},
                        {ppl::PipeNrSize::P1440, 1440, veyra::pipeline::NrSizePolicy::P1440},
                        {ppl::PipeNrSize::Native, 1440, veyra::pipeline::NrSizePolicy::Native}};
    for (const auto& row : rows) {
        r.nrSize = row.size;
        const auto p = ppl::planPipeline(r, pplAllCaps());
        const auto* n = p.find(ppl::PipeStage::Neural);
        CHECK(n != nullptr);
        CHECK(n->internal.height == row.h);
        CHECK(n->input.height == 1440 && n->output.height == 1440);  // fills back at its input size
        CHECK(p.nrPolicy == row.policy);
    }
    // A runtime cap of 1080 lines: 1440 and native are reduced, with a reason.
    auto caps = pplAllCaps(); caps.nrMaxHeight = 1080;
    r.nrSize = ppl::PipeNrSize::P1440;
    auto p = ppl::planPipeline(r, caps);
    CHECK(p.find(ppl::PipeStage::Neural)->internal.height == 1080);
    CHECK(p.change(ppl::PipeStage::Neural, ppl::PipeReason::NrSizeCap) != nullptr);
    CHECK(p.change(ppl::PipeStage::Neural, ppl::PipeReason::NrSizeCap)->from == 1440);
    CHECK(p.nrPolicy == veyra::pipeline::NrSizePolicy::Realtime);
    r.nrSize = ppl::PipeNrSize::Native;
    p = ppl::planPipeline(r, caps);
    CHECK(p.find(ppl::PipeStage::Neural)->internal.height == 1080);
    // 1440 lines asked on a 1080 input (no upscaler): runs at the input, reported.
    r.upscaler = ppl::PipeUpscaler::Off; r.nrSize = ppl::PipeNrSize::P1440;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.find(ppl::PipeStage::Neural)->internal.height == 1080);
    CHECK(p.change(ppl::PipeStage::Neural, ppl::PipeReason::NrAboveInput) != nullptr);
}
TEST(pipeline_plan_neural_size_matches_resolution_plan){
    // The same 16:9 fit as pipeline::ResolutionPlan for every fixed policy.
    for (auto size : {ppl::PipeNrSize::P480, ppl::PipeNrSize::P720, ppl::PipeNrSize::P900, ppl::PipeNrSize::Realtime1080}) {
        auto r = pplBase(); r.upscaler = ppl::PipeUpscaler::Dlss; r.nr = true; r.nrSize = size;
        const auto p = ppl::planPipeline(r, pplAllCaps());
        const auto rp = veyra::pipeline::ResolutionPlan::make({1920, 1080}, true, ppl::pipeNrPolicy(size), false, 0, veyra::pipeline::SrTarget::Qhd);
        CHECK(p.find(ppl::PipeStage::Neural)->internal.width == rp.nr.width);
        CHECK(p.find(ppl::PipeStage::Neural)->internal.height == rp.nr.height);
    }
}
TEST(pipeline_plan_missing_neural_runtime_is_dropped_with_reason){
    auto r = pplBase(); r.nr = true; r.upscaler = ppl::PipeUpscaler::Dlss;
    auto caps = pplAllCaps(); caps.nrRuntime = false;
    const auto p = ppl::planPipeline(r, caps);
    CHECK(!p.has(ppl::PipeStage::Neural));
    CHECK(p.change(ppl::PipeStage::Neural, ppl::PipeReason::RuntimeMissing) != nullptr);
    CHECK(p.change(ppl::PipeStage::Neural, ppl::PipeReason::RuntimeMissing)->action == ppl::PipeAction::Dropped);
    CHECK(!p.experimental);
    CHECK(p.has(ppl::PipeStage::Upscale));  // the rest still runs
}
TEST(pipeline_plan_target_above_display_is_reduced_unless_supersampling){
    auto r = pplBase(); r.upscaler = ppl::PipeUpscaler::Dlss; r.srTarget = ppl::PipeSrTarget::Uhd4K;
    auto p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.output.width == 2560 && p.output.height == 1440);
    const auto* c = p.change(ppl::PipeStage::Upscale, ppl::PipeReason::AboveDisplay);
    CHECK(c != nullptr && c->action == ppl::PipeAction::Reduced && c->from == 2160 && c->to == 1440);
    r.supersample = true;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.output.width == 3840 && p.output.height == 2160);
    CHECK(p.changeCount == 0);
    // On a 4K display the 4K target is not above it.
    r.supersample = false; r.displayWidth = 3840; r.displayHeight = 2160;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.output.height == 2160 && p.changeCount == 0);
}
TEST(pipeline_plan_scale_one_and_double_upscaler_are_dropped){
    auto r = pplBase(); r.upscaler = ppl::PipeUpscaler::Nis;
    r.displayWidth = 1920; r.displayHeight = 1080;  // the stream already fills the display
    auto p = ppl::planPipeline(r, pplAllCaps());
    CHECK(!p.has(ppl::PipeStage::Upscale));
    CHECK(p.change(ppl::PipeStage::Upscale, ppl::PipeReason::ScaleOne) != nullptr);
    r = pplBase(); r.upscaler = ppl::PipeUpscaler::Dlss; r.secondUpscaler = ppl::PipeUpscaler::Vsr;
    p = ppl::planPipeline(r, pplAllCaps());
    std::size_t upscales = 0;
    for (std::size_t i = 0; i < p.stageCount; ++i) upscales += p.stages[i].stage == ppl::PipeStage::Upscale ? 1u : 0u;
    CHECK(upscales == 1);
    CHECK(p.change(ppl::PipeStage::Upscale, ppl::PipeReason::DoubleUpscaler) != nullptr);
}
TEST(pipeline_plan_upscaler_runtime_missing_falls_back_vendor_neutral){
    // DLSS SR (temporal) missing: FSR (temporal, any DirectX 12 card) stands in, and the plan says so.
    auto r = pplBase(); r.upscaler = ppl::PipeUpscaler::Dlss;
    auto caps = pplAllCaps(); caps.dlssSr = false;
    auto p = ppl::planPipeline(r, caps);
    CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Fsr));
    CHECK(p.output.height == 1440);
    const auto* c = p.change(ppl::PipeStage::Upscale, ppl::PipeReason::FallbackVendorNeutral);
    CHECK(c != nullptr && c->action == ppl::PipeAction::Replaced);
    CHECK(c->from == std::uint32_t(ppl::PipeUpscaler::Dlss) && c->to == std::uint32_t(ppl::PipeUpscaler::Fsr));
    caps.fsr = false;  // then NIS
    p = ppl::planPipeline(r, caps);
    CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Nis));
    // A spatial RTX Video SR family missing: NIS (spatial) first, then FSR.
    caps = pplAllCaps(); caps.vsr = false;
    for (auto family : {ppl::PipeUpscaler::Vsr, ppl::PipeUpscaler::VsrStream, ppl::PipeUpscaler::VsrHighBitrate}) {
        r.upscaler = family;
        p = ppl::planPipeline(r, caps);
        CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Nis));
    }
    caps.nis = false;
    p = ppl::planPipeline(r, caps);
    CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Fsr));
    // Nothing vendor neutral either: dropped, with the missing runtime as the reason.
    caps.fsr = false;
    p = ppl::planPipeline(r, caps);
    CHECK(!p.has(ppl::PipeStage::Upscale));
    CHECK(p.change(ppl::PipeStage::Upscale, ppl::PipeReason::RuntimeMissing) != nullptr);
    CHECK(p.change(ppl::PipeStage::Upscale, ppl::PipeReason::RuntimeMissing)->action == ppl::PipeAction::Dropped);
    CHECK(p.output.height == 1080);
    // A vendor-neutral choice that is present is never replaced.
    r.upscaler = ppl::PipeUpscaler::Nis;
    p = ppl::planPipeline(r, ppl::PipelineCaps{});
    CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Nis) && p.changeCount == 0);
    CHECK(ppl::pipeUpscalerVendorNeutral(ppl::PipeUpscaler::Nis) && ppl::pipeUpscalerVendorNeutral(ppl::PipeUpscaler::Fsr));
    CHECK(!ppl::pipeUpscalerVendorNeutral(ppl::PipeUpscaler::Dlss) && !ppl::pipeUpscalerVendorNeutral(ppl::PipeUpscaler::Vsr));
}
TEST(pipeline_plan_frame_generation_falls_back_vendor_neutral){
    auto r = pplBase(); r.fg = ppl::PipeFg::Vfg; r.fgMultiplier = 2;
    auto caps = pplAllCaps(); caps.vfg = false; caps.dlssFg = false;
    auto p = ppl::planPipeline(r, caps);
    const auto* c = p.change(ppl::PipeStage::FrameGeneration, ppl::PipeReason::FallbackVendorNeutral);
    CHECK(c != nullptr && c->action == ppl::PipeAction::Replaced);
    CHECK(c->from == std::uint32_t(ppl::PipeFg::Vfg) && c->to == std::uint32_t(ppl::PipeFg::Fsr));
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 2);
    // The stand-in's own multipliers apply: FSR offers 2x only.
    r.fgMultiplier = 3; r.fgTargetFps = 180; r.fgExperimental180 = true;
    p = ppl::planPipeline(r, caps);
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 2);
    CHECK(p.change(ppl::PipeStage::FrameGeneration, ppl::PipeReason::FgMultiplierUnsupported) != nullptr);
    caps.fsrFg = false;  // then PushFlow (DirectML), which offers 3x
    p = ppl::planPipeline(r, caps);
    CHECK(p.change(ppl::PipeStage::FrameGeneration, ppl::PipeReason::FallbackVendorNeutral)->to == std::uint32_t(ppl::PipeFg::PushFlow));
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 3);
    caps.pushFlow = false;
    p = ppl::planPipeline(r, caps);
    CHECK(!p.has(ppl::PipeStage::FrameGeneration) && !p.interpolationHold);
    CHECK(p.change(ppl::PipeStage::FrameGeneration, ppl::PipeReason::RuntimeMissing) != nullptr);
    CHECK(ppl::pipeFgVendorNeutral(ppl::PipeFg::Fsr) && ppl::pipeFgVendorNeutral(ppl::PipeFg::PushFlow) && !ppl::pipeFgVendorNeutral(ppl::PipeFg::Vfg));
}
TEST(pipeline_plan_no_two_sharpening_stages_in_a_row){
    // A separate sharpening stage after an upscaler that sharpens by itself is dropped, with the reason.
    for (auto family : {ppl::PipeUpscaler::Nis, ppl::PipeUpscaler::Vsr, ppl::PipeUpscaler::VsrStream, ppl::PipeUpscaler::VsrHighBitrate}) {
        auto r = pplBase(); r.upscaler = family; r.sharpen = 40;
        const auto p = ppl::planPipeline(r, pplAllCaps());
        CHECK(p.has(ppl::PipeStage::Upscale) && !p.has(ppl::PipeStage::Sharpen));
        const auto* c = p.change(ppl::PipeStage::Sharpen, ppl::PipeReason::DoubleSharpen);
        CHECK(c != nullptr && c->action == ppl::PipeAction::Dropped && c->from == 40 && c->to == 0);
    }
    // DLSS SR and FSR do not sharpen in this app: the stage stays, after the upscaler and before frame generation.
    for (auto family : {ppl::PipeUpscaler::Dlss, ppl::PipeUpscaler::Fsr, ppl::PipeUpscaler::Off}) {
        auto r = pplBase(); r.upscaler = family; r.sharpen = 40; r.fg = ppl::PipeFg::Vfg; r.nr = true; r.hdr = false;
        const auto p = ppl::planPipeline(r, pplAllCaps());
        CHECK(p.changeCount == 0);
        std::size_t at = 0, upscale = 0, neural = 0, framegen = 0;
        for (std::size_t i = 0; i < p.stageCount; ++i) {
            if (p.stages[i].stage == ppl::PipeStage::Sharpen) at = i;
            if (p.stages[i].stage == ppl::PipeStage::Upscale) upscale = i;
            if (p.stages[i].stage == ppl::PipeStage::Neural) neural = i;
            if (p.stages[i].stage == ppl::PipeStage::FrameGeneration) framegen = i;
        }
        CHECK(at > upscale && at > neural && at < framegen);
        CHECK(p.stages[at].detail == 40 && p.stages[at].input == p.output);
    }
    // NIS with its own sharpening at 0 only scales: the separate stage is then the only one and stays.
    auto r = pplBase(); r.upscaler = ppl::PipeUpscaler::Nis; r.srSharpness = 0; r.sharpen = 30;
    auto p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.has(ppl::PipeStage::Sharpen) && p.changeCount == 0);
    // NIS as a second step behind a sharpening upscaler is a second sharpening stage; behind DLSS SR it is
    // a second upscaler. Dropped either way.
    r = pplBase(); r.upscaler = ppl::PipeUpscaler::Vsr; r.secondUpscaler = ppl::PipeUpscaler::Nis;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.change(ppl::PipeStage::Upscale, ppl::PipeReason::DoubleSharpen) != nullptr);
    r.upscaler = ppl::PipeUpscaler::Dlss;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.change(ppl::PipeStage::Upscale, ppl::PipeReason::DoubleUpscaler) != nullptr);
    CHECK(p.change(ppl::PipeStage::Upscale, ppl::PipeReason::DoubleSharpen) == nullptr);
    // A stand-in never brings sharpening nobody asked for: DLSS SR missing, FSR missing, NIS stands in at
    // sharpness 0 (DLSS SR does not sharpen), so the separate sharpening stage that was asked for stays.
    r = pplBase(); r.upscaler = ppl::PipeUpscaler::Dlss; r.sharpen = 50;
    auto caps = pplAllCaps(); caps.dlssSr = false; caps.fsr = false;
    p = ppl::planPipeline(r, caps);
    CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Nis) && p.srSharpness == 0);
    CHECK(p.has(ppl::PipeStage::Sharpen) && p.change(ppl::PipeStage::Sharpen, ppl::PipeReason::DoubleSharpen) == nullptr);
    // A NIS the user chose keeps the request's sharpness (its default when -1), and an RTX Video SR family
    // replaced by NIS keeps sharpening, as the family did.
    r = pplBase(); r.upscaler = ppl::PipeUpscaler::Nis; r.srSharpness = 35;
    CHECK(ppl::planPipeline(r, pplAllCaps()).srSharpness == 35);
    r.srSharpness = -1;
    CHECK(ppl::planPipeline(r, pplAllCaps()).srSharpness == -1);
    r = pplBase(); r.upscaler = ppl::PipeUpscaler::Vsr; caps = pplAllCaps(); caps.vsr = false;
    p = ppl::planPipeline(r, caps);
    CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Nis) && p.srSharpness == -1);
    // The budget rule's NIS stand-in for DLSS SR does not sharpen either.
    r = pplBase(); r.upscaler = ppl::PipeUpscaler::Dlss; r.srTarget = ppl::PipeSrTarget::Qhd;
    ppl::PipelineCaps tight; tight.dlssSr = true; tight.gpuBudgetMs = 2.0; tight.srMs = 3.0;
    p = ppl::planPipeline(r, tight);
    CHECK(p.change(ppl::PipeStage::Upscale, ppl::PipeReason::GpuBudget) != nullptr);
    CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Nis) && p.srSharpness == 0);
    // No upscaler: no sharpness.
    CHECK(ppl::planPipeline(pplBase(), pplAllCaps()).srSharpness == -1);
}
TEST(pipeline_plan_no_clean_up_twice){
    // The streaming RTX Video SR family cleans hardest: our clean-up stage is dropped beside it.
    auto r = pplBase(); r.clean = 60; r.upscaler = ppl::PipeUpscaler::VsrStream;
    auto p = ppl::planPipeline(r, pplAllCaps());
    CHECK(!p.has(ppl::PipeStage::Clean) && p.has(ppl::PipeStage::Upscale));
    const auto* c = p.change(ppl::PipeStage::Clean, ppl::PipeReason::DoubleClean);
    CHECK(c != nullptr && c->action == ppl::PipeAction::Dropped && c->from == 60 && c->to == 0);
    // The standard family is trained on compressed video: ours is weakened, not dropped.
    r.upscaler = ppl::PipeUpscaler::Vsr;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.find(ppl::PipeStage::Clean)->detail == ppl::kPipeCleanBesideCleaningUpscaler);
    c = p.change(ppl::PipeStage::Clean, ppl::PipeReason::DoubleClean);
    CHECK(c != nullptr && c->action == ppl::PipeAction::Reduced && c->from == 60 && c->to == ppl::kPipeCleanBesideCleaningUpscaler);
    r.clean = 10;  // already weaker than the cap: untouched
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.find(ppl::PipeStage::Clean)->detail == 10 && p.change(ppl::PipeStage::Clean, ppl::PipeReason::DoubleClean) == nullptr);
    // Families without their own clean-up keep ours in full: DLSS SR, FSR, NIS, RTX Video SR high bitrate.
    for (auto family : {ppl::PipeUpscaler::Dlss, ppl::PipeUpscaler::Fsr, ppl::PipeUpscaler::Nis, ppl::PipeUpscaler::VsrHighBitrate, ppl::PipeUpscaler::Off}) {
        r = pplBase(); r.clean = 60; r.upscaler = family;
        p = ppl::planPipeline(r, pplAllCaps());
        CHECK(p.find(ppl::PipeStage::Clean)->detail == 60);
        CHECK(p.change(ppl::PipeStage::Clean, ppl::PipeReason::DoubleClean) == nullptr);
    }
    // A cleaning family that is missing does not clean: its vendor-neutral stand-in keeps our stage whole.
    r = pplBase(); r.clean = 60; r.upscaler = ppl::PipeUpscaler::VsrStream;
    auto caps = pplAllCaps(); caps.vsr = false;
    p = ppl::planPipeline(r, caps);
    CHECK(p.find(ppl::PipeStage::Clean)->detail == 60 && p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Nis));
}
TEST(pipeline_plan_notes_debanding_lost_in_an_8_bit_upscaler_input){
    // Clean-up kept in front of an upscaler that takes an 8-bit picture: noted, nothing changed.
    auto r = pplBase(); r.clean = 20; r.upscaler = ppl::PipeUpscaler::Vsr;
    auto p = ppl::planPipeline(r, pplAllCaps());
    const auto* c = p.change(ppl::PipeStage::Clean, ppl::PipeReason::DebandRequantised);
    CHECK(c != nullptr && c->action == ppl::PipeAction::Noted && c->from == 20 && c->to == 20);
    CHECK(p.find(ppl::PipeStage::Clean)->detail == 20);
    r.upscaler = ppl::PipeUpscaler::VsrHighBitrate; r.clean = 60;
    CHECK(ppl::planPipeline(r, pplAllCaps()).change(ppl::PipeStage::Clean, ppl::PipeReason::DebandRequantised) != nullptr);
    r.ditheredOutput = true;  // the final write is dithered: no note
    CHECK(ppl::planPipeline(r, pplAllCaps()).changeCount == 0);
    // The graph's own FP16 colour goes into DLSS SR, FSR and NIS: no note; and none when clean-up was dropped.
    for (auto family : {ppl::PipeUpscaler::Dlss, ppl::PipeUpscaler::Fsr, ppl::PipeUpscaler::Nis, ppl::PipeUpscaler::VsrStream}) {
        r = pplBase(); r.clean = 60; r.upscaler = family;
        CHECK(ppl::planPipeline(r, pplAllCaps()).change(ppl::PipeStage::Clean, ppl::PipeReason::DebandRequantised) == nullptr);
    }
    CHECK(ppl::pipeActionName(ppl::PipeAction::Noted) == "noted" && ppl::pipeReasonName(ppl::PipeReason::DebandRequantised) == "deband-requantised");
}
TEST(pipeline_plan_frame_generation_target_refresh_and_experimental_180){
    auto r = pplBase(); r.fg = ppl::PipeFg::Vfg; r.fgMultiplier = 2;
    auto p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 2);
    CHECK(p.submittedFps > 119.8 && p.submittedFps < 119.9);
    // 30 fps game in a 60 fps stream: at most 60 pictures a second show new motion.
    CHECK(p.uniqueMotionFpsMax > 59.9 && p.uniqueMotionFpsMax < 60.1);
    CHECK(!p.experimental);
    // 3x = 180: reduced to the 120 target unless explicitly experimental.
    r.fgMultiplier = 3;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 2);
    CHECK(p.change(ppl::PipeStage::FrameGeneration, ppl::PipeReason::FgAboveTarget) != nullptr);
    r.fgTargetFps = 180; r.fgExperimental180 = true;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 3);
    CHECK(p.experimental);
    // A 180 target without the experimental flag stays at 120.
    r.fgExperimental180 = false;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 2);
    // A 120 Hz display limits 3x even when 180 is allowed.
    r.fgExperimental180 = true; r.displayHz = 120;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 2);
    CHECK(p.change(ppl::PipeStage::FrameGeneration, ppl::PipeReason::FgAboveRefresh) != nullptr);
    // A 60 Hz display leaves nothing to generate.
    r.displayHz = 60;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(!p.has(ppl::PipeStage::FrameGeneration));
    CHECK(p.change(ppl::PipeStage::FrameGeneration, ppl::PipeReason::FgMultiplierOne) != nullptr);
    CHECK(p.submittedFps > 59.9 && p.submittedFps < 60.0);
}
TEST(pipeline_plan_frame_generation_backend_limits){
    auto r = pplBase(); r.fg = ppl::PipeFg::Fsr; r.fgMultiplier = 4; r.fgExperimental180 = true; r.fgTargetFps = 180;
    auto p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 2);
    CHECK(p.change(ppl::PipeStage::FrameGeneration, ppl::PipeReason::FgMultiplierUnsupported) != nullptr);
    r.fg = ppl::PipeFg::Dlss; r.fgMultiplier = 5; r.streamFps = 30; r.contentFps = 0; r.displayHz = 240;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 4);  // DLSS offers 2/3/4/6, and 4x30 = 120
    auto caps = pplAllCaps(); caps.dlssFg = false; caps.fsrFg = false; caps.pushFlow = false;
    p = ppl::planPipeline(r, caps);
    CHECK(!p.has(ppl::PipeStage::FrameGeneration));
    CHECK(p.change(ppl::PipeStage::FrameGeneration, ppl::PipeReason::RuntimeMissing) != nullptr);
}
TEST(pipeline_plan_headroom_reduces_the_most_expensive_image_stage_first){
    auto r = pplBase(); r.clean = 30; r.upscaler = ppl::PipeUpscaler::Dlss; r.srTarget = ppl::PipeSrTarget::Qhd; r.nr = true; r.fg = ppl::PipeFg::Vfg;
    auto caps = pplAllCaps();
    // 12 ms of stages in a 10 ms budget; the neural layer (6 ms at 1080 lines) is the most expensive image
    // stage: its inference size steps down (900: 4.17 ms, still over; 720: 2.67 ms, fits). Nothing else moves.
    caps.gpuBudgetMs = 10.0; caps.cleanMs = 1.0; caps.srMs = 2.0; caps.fgMs = 3.0; caps.nrMs = 6.0;
    auto p = ppl::planPipeline(r, caps);
    CHECK(p.find(ppl::PipeStage::Neural)->internal.height == 720);
    CHECK(p.nrPolicy == veyra::pipeline::NrSizePolicy::P720);
    const auto* c = p.change(ppl::PipeStage::Neural, ppl::PipeReason::GpuBudget);
    CHECK(c != nullptr && c->action == ppl::PipeAction::Reduced && c->from == 1080 && c->to == 900);
    CHECK(!p.overBudget && p.costEstimated);
    CHECK(p.plannedCostMs > 8.6 && p.plannedCostMs < 8.7);
    CHECK(p.stages[p.stageCount - 1].stage == ppl::PipeStage::Present);
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 2);  // frame generation is never the one to give way
    CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Dlss) && p.find(ppl::PipeStage::Clean)->detail == 30);
    // Everything fits: nothing changes and nothing is an estimate.
    caps.nrMs = 3.0;
    p = ppl::planPipeline(r, caps);
    CHECK(p.find(ppl::PipeStage::Neural)->internal.height == 1080 && p.change(ppl::PipeStage::Neural, ppl::PipeReason::GpuBudget) == nullptr && !p.costEstimated && !p.overBudget);
    CHECK(p.plannedCostMs > 8.99 && p.plannedCostMs < 9.01);
    // A tighter budget: the neural layer goes below 720 lines = off; then the upscaler is the most
    // expensive (2 ms): NIS stands in (cost unknown); clean-up (1 ms) + frame generation (3 ms) fit 4.5 ms.
    caps.nrMs = 6.0; caps.gpuBudgetMs = 4.5;
    p = ppl::planPipeline(r, caps);
    CHECK(!p.has(ppl::PipeStage::Neural) && !p.experimental);
    CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Nis));
    bool replaced = false, neuralDropped = false;
    for (std::size_t i = 0; i < p.changeCount; ++i) {
        replaced = replaced || (p.changes[i].stage == ppl::PipeStage::Upscale && p.changes[i].action == ppl::PipeAction::Replaced && p.changes[i].reason == ppl::PipeReason::GpuBudget);
        neuralDropped = neuralDropped || (p.changes[i].stage == ppl::PipeStage::Neural && p.changes[i].action == ppl::PipeAction::Dropped && p.changes[i].reason == ppl::PipeReason::GpuBudget);
    }
    CHECK(replaced && neuralDropped);
    CHECK(p.has(ppl::PipeStage::Clean) && p.has(ppl::PipeStage::FrameGeneration) && !p.overBudget);
    // Frame generation alone over the budget: every image stage with a known cost gives way, the
    // multiplier stays, and the plan says it does not fit.
    caps.gpuBudgetMs = 2.0;
    p = ppl::planPipeline(r, caps);
    CHECK(!p.has(ppl::PipeStage::Neural) && !p.has(ppl::PipeStage::Clean));
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 2 && p.overBudget);
    CHECK(p.submittedFps > 119.8);
    // Unknown costs never move anything.
    caps = pplAllCaps(); caps.gpuBudgetMs = 1.0;
    p = ppl::planPipeline(r, caps);
    CHECK(p.has(ppl::PipeStage::Neural) && !p.overBudget && !p.costEstimated && p.change(ppl::PipeStage::Neural, ppl::PipeReason::GpuBudget) == nullptr);
    // Measured video memory below the layer's measured need.
    caps.vramFreeMiB = 500; caps.nrVramMiB = 900;
    p = ppl::planPipeline(r, caps);
    CHECK(!p.has(ppl::PipeStage::Neural));
    CHECK(p.change(ppl::PipeStage::Neural, ppl::PipeReason::VramBudget) != nullptr);
}
TEST(pipeline_plan_headroom_takes_supersampling_before_the_upscaler){
    // The owner's reference (4K target on a 1440p screen) with the upscaler as the most expensive stage:
    // supersampling goes first (cost taken as proportional to output pixels: 2.25x fewer), the family stays.
    auto r = ppl::pipelineProfileRequest("max-fidelity", pplBase());
    auto caps = pplAllCaps(); caps.gpuBudgetMs = 8.0; caps.srMs = 6.0; caps.fgMs = 4.0;
    auto p = ppl::planPipeline(r, caps);
    CHECK(p.output.width == 2560 && p.output.height == 1440);
    CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Dlss));
    const auto* c = p.change(ppl::PipeStage::Upscale, ppl::PipeReason::GpuBudget);
    CHECK(c != nullptr && c->action == ppl::PipeAction::Reduced && c->from == 2160 && c->to == 1440);
    CHECK(!p.overBudget && p.costEstimated && p.plannedCostMs > 6.6 && p.plannedCostMs < 6.7);
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->input.height == 1440);  // the later stage shrinks with it
    // The clean-up stage is the most expensive only when nothing else has a known cost: then it goes.
    r = pplBase(); r.clean = 50; r.fg = ppl::PipeFg::Vfg;
    caps = pplAllCaps(); caps.gpuBudgetMs = 3.0; caps.cleanMs = 1.5; caps.fgMs = 2.0;
    p = ppl::planPipeline(r, caps);
    CHECK(!p.has(ppl::PipeStage::Clean) && p.has(ppl::PipeStage::FrameGeneration) && !p.overBudget);
    CHECK(p.change(ppl::PipeStage::Clean, ppl::PipeReason::GpuBudget)->action == ppl::PipeAction::Dropped);
}
TEST(pipeline_plan_no_image_stage_adds_a_frame_of_delay){
    // Every profile and every benchmark arm, with and without the NVIDIA runtimes: no image stage waits
    // for a later frame; only frame generation holds a real frame, and the plan says so.
    for (auto stage : {ppl::PipeStage::Decode, ppl::PipeStage::Clean, ppl::PipeStage::Neural, ppl::PipeStage::Upscale, ppl::PipeStage::Sharpen, ppl::PipeStage::Present})
        CHECK(ppl::pipeStageLookahead(stage) == 0);
    for (const auto& caps : {pplAllCaps(), ppl::PipelineCaps{}}) {
        for (auto id : ppl::kPipeProfileIds) {
            const auto p = ppl::planPipeline(ppl::pipelineProfileRequest(id, pplBase()), caps);
            CHECK(p.imageDelayFrames == 0);
            CHECK(p.interpolationHold == p.has(ppl::PipeStage::FrameGeneration));
        }
        for (char arm = 'A'; arm <= 'E'; ++arm) {
            const auto p = ppl::planPipeline(ppl::pipelineArmRequest(arm, pplBase()), caps);
            CHECK(p.imageDelayFrames == 0 && !p.interpolationHold);
        }
    }
}
TEST(pipeline_plan_benchmark_arms_a_to_e){
    const auto caps = pplAllCaps();
    auto p = ppl::planPipeline(ppl::pipelineArmRequest('A', pplBase()), caps);
    CHECK(p.stageCount == 2 && p.output.height == 1080);
    p = ppl::planPipeline(ppl::pipelineArmRequest('B', pplBase()), caps);
    CHECK(p.stageCount == 3 && p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Dlss) && p.output.height == 1440);
    p = ppl::planPipeline(ppl::pipelineArmRequest('C', pplBase()), caps);
    CHECK(p.stageCount == 4 && p.stages[1].stage == ppl::PipeStage::Clean && p.stages[2].stage == ppl::PipeStage::Upscale);
    p = ppl::planPipeline(ppl::pipelineArmRequest('D', pplBase()), caps);
    CHECK(p.stageCount == 3 && p.stages[1].stage == ppl::PipeStage::Neural && p.output.height == 1080);
    p = ppl::planPipeline(ppl::pipelineArmRequest('E', pplBase()), caps);
    CHECK(p.stageCount == 5 && p.has(ppl::PipeStage::Clean) && p.has(ppl::PipeStage::Neural) && p.has(ppl::PipeStage::Upscale));
    CHECK(p.changeCount == 1 && p.change(ppl::PipeStage::Neural, ppl::PipeReason::NeuralHdrUnverified) != nullptr);  // pplBase is HDR
    auto custom = pplBase(); custom.clean = 77;
    CHECK(ppl::pipelineArmRequest('F', custom).clean == 77 && ppl::pipelineArmRequest('a', custom).clean == 77);
}
TEST(pipeline_plan_invalid_request_plans_nothing){
    auto r = pplBase(); r.streamWidth = 0; r.upscaler = ppl::PipeUpscaler::Dlss;
    auto p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.stageCount == 2 && p.change(ppl::PipeStage::Decode, ppl::PipeReason::InvalidRequest) != nullptr);
    r = pplBase(); r.displayHz = 0;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.stageCount == 2);
}
TEST(pipeline_plan_profiles_map_to_default_requests){
    const auto base = pplBase();
    const auto caps = pplAllCaps();
    auto p = ppl::planPipeline(ppl::pipelineProfileRequest("original", base), caps);
    CHECK(p.stageCount == 2 && p.output.height == 1080);
    p = ppl::planPipeline(ppl::pipelineProfileRequest("ultra-low-latency", base), caps);
    CHECK(p.stageCount == 2);
    p = ppl::planPipeline(ppl::pipelineProfileRequest("clean-1440", base), caps);
    CHECK(p.has(ppl::PipeStage::Clean) && p.has(ppl::PipeStage::Upscale) && !p.has(ppl::PipeStage::Neural));
    CHECK(p.output.width == 2560 && p.output.height == 1440 && p.changeCount == 0);
    p = ppl::planPipeline(ppl::pipelineProfileRequest("enhanced-1440", base), caps);
    CHECK(p.has(ppl::PipeStage::Clean) && p.has(ppl::PipeStage::Neural) && p.has(ppl::PipeStage::Upscale));
    CHECK(p.find(ppl::PipeStage::Neural)->detail == std::uint32_t(ppl::PipeNrLook::Conservative));
    const auto mf = ppl::pipelineProfileRequest("max-fidelity", base);
    CHECK(mf.upscaler == ppl::PipeUpscaler::Dlss && mf.srModel == 1 && mf.srTarget == ppl::PipeSrTarget::Uhd4K && mf.supersample);
    CHECK(mf.fg == ppl::PipeFg::Vfg && mf.fgMultiplier == 2 && mf.clean == 0 && !mf.nr);
    p = ppl::planPipeline(mf, caps);
    CHECK(p.output.width == 3840 && p.find(ppl::PipeStage::FrameGeneration)->detail == 2);
    // The DLSS SR model is the caller's (not settled for 1080p -> 1440p); only the owner's reference fixes M.
    auto withK = base; withK.srModel = 0;
    CHECK(ppl::pipelineProfileRequest("clean-1440", withK).srModel == 0 && ppl::pipelineProfileRequest("enhanced-1440", withK).srModel == 0);
    CHECK(ppl::pipelineProfileRequest("max-fidelity", withK).srModel == 1 && ppl::pipelineArmRequest('B', withK).srModel == 0);
    // The neural layer on an HDR stream: planned when asked, experimental, and noted as not verified for HDR.
    p = ppl::planPipeline(ppl::pipelineProfileRequest("enhanced-1440", base), caps);
    const auto* hdrNote = p.change(ppl::PipeStage::Neural, ppl::PipeReason::NeuralHdrUnverified);
    CHECK(base.hdr && hdrNote != nullptr && hdrNote->action == ppl::PipeAction::Noted && p.experimental && p.has(ppl::PipeStage::Neural));
    auto sdr = base; sdr.hdr = false;
    CHECK(ppl::planPipeline(ppl::pipelineProfileRequest("enhanced-1440", sdr), caps).change(ppl::PipeStage::Neural, ppl::PipeReason::NeuralHdrUnverified) == nullptr);
    CHECK(ppl::pipeReasonName(ppl::PipeReason::NeuralHdrUnverified) == "neural-hdr-unverified");
    // Profiles keep stream, display and HDR; an unknown id changes nothing.
    CHECK(mf.hdr && mf.displayHz == 180.0 && mf.streamFps == base.streamFps);
    auto custom = base; custom.clean = 77;
    CHECK(ppl::pipelineProfileRequest("bogus", custom).clean == 77);
    // On a PC without the NVIDIA runtimes (decided by capability, never by a card's name) the same profile
    // keeps the vendor-neutral clean-up, upscales with FSR and generates frames with FSR when it is there;
    // the neural layer has no stand-in and is dropped. Every change carries its reason.
    ppl::PipelineCaps plain;
    p = ppl::planPipeline(ppl::pipelineProfileRequest("enhanced-1440", base), plain);
    CHECK(p.has(ppl::PipeStage::Clean) && !p.has(ppl::PipeStage::Neural) && !p.has(ppl::PipeStage::FrameGeneration));
    CHECK(p.find(ppl::PipeStage::Clean)->detail == ppl::kPipeProfileCleanStrength);
    CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Fsr) && p.output.height == 1440);
    CHECK(p.change(ppl::PipeStage::Neural, ppl::PipeReason::RuntimeMissing) && p.change(ppl::PipeStage::Upscale, ppl::PipeReason::FallbackVendorNeutral));
    CHECK(!p.experimental);
    // clean-1440 and enhanced-1440 keep the user's frame generation and never turn it on (orchestrator's
    // decision, 21 Nov): with it off they plan none; with the user's VFG 2x it is planned (or its stand-in).
    CHECK(base.fg == ppl::PipeFg::Off && !ppl::planPipeline(ppl::pipelineProfileRequest("clean-1440", base), caps).has(ppl::PipeStage::FrameGeneration));
    auto userFg = base; userFg.fg = ppl::PipeFg::Vfg; userFg.fgMultiplier = 2;
    CHECK(ppl::pipelineProfileRequest("clean-1440", userFg).fg == ppl::PipeFg::Vfg && ppl::pipelineProfileRequest("enhanced-1440", userFg).fgMultiplier == 2);
    auto userPf = base; userPf.fg = ppl::PipeFg::PushFlow; userPf.fgMultiplier = 3;
    CHECK(ppl::pipelineProfileRequest("enhanced-1440", userPf).fg == ppl::PipeFg::PushFlow && ppl::pipelineProfileRequest("clean-1440", userPf).fgMultiplier == 3);
    CHECK(ppl::pipelineProfileRequest("original", userFg).fg == ppl::PipeFg::Off && ppl::pipelineProfileRequest("max-fidelity", userPf).fg == ppl::PipeFg::Vfg);
    plain.fsrFg = true;
    p = ppl::planPipeline(ppl::pipelineProfileRequest("clean-1440", userFg), plain);
    CHECK(p.find(ppl::PipeStage::FrameGeneration)->detail == 2 && p.submittedFps > 119.8 && p.submittedFps < 119.9);
    CHECK(p.change(ppl::PipeStage::FrameGeneration, ppl::PipeReason::FallbackVendorNeutral) != nullptr);
    p = ppl::planPipeline(ppl::pipelineProfileRequest("max-fidelity", base), plain);
    CHECK(p.find(ppl::PipeStage::Upscale)->detail == std::uint32_t(ppl::PipeUpscaler::Fsr) && p.output.height == 2160);
}
TEST(pipeline_plan_names_and_parsers_round_trip){
    for (auto u : {ppl::PipeUpscaler::Off, ppl::PipeUpscaler::Dlss, ppl::PipeUpscaler::VsrStream, ppl::PipeUpscaler::Nis, ppl::PipeUpscaler::Fsr, ppl::PipeUpscaler::Vsr, ppl::PipeUpscaler::VsrHighBitrate})
        CHECK(ppl::parsePipeUpscaler(ppl::pipeUpscalerName(u)) == u);
    // Saved numbers keep their meaning: values are appended, never reordered.
    CHECK(std::uint32_t(ppl::PipeUpscaler::Vsr) == 5 && std::uint32_t(ppl::PipeUpscaler::VsrHighBitrate) == 6 && std::uint32_t(ppl::PipeStage::Sharpen) == 6);
    CHECK(std::uint32_t(ppl::PipeStage::Present) == 5 && std::uint32_t(ppl::PipeAction::Replaced) == 2 && std::uint32_t(ppl::PipeFg::Fsr) == 4);
    CHECK(ppl::pipeStageName(ppl::PipeStage::Sharpen) == "sharpen" && ppl::pipeActionName(ppl::PipeAction::Replaced) == "replaced");
    CHECK(ppl::pipeReasonName(ppl::PipeReason::FallbackVendorNeutral) == "fallback-vendor-neutral");
    CHECK(ppl::pipeReasonName(ppl::PipeReason::DoubleSharpen) == "double-sharpen" && ppl::pipeReasonName(ppl::PipeReason::DoubleClean) == "double-clean");
    for (auto f : {ppl::PipeFg::Off, ppl::PipeFg::Vfg, ppl::PipeFg::Dlss, ppl::PipeFg::PushFlow, ppl::PipeFg::Fsr})
        CHECK(ppl::parsePipeFg(ppl::pipeFgName(f)) == f);
    for (auto s : {ppl::PipeNrSize::P480, ppl::PipeNrSize::P720, ppl::PipeNrSize::P900, ppl::PipeNrSize::Realtime1080, ppl::PipeNrSize::P1440, ppl::PipeNrSize::Native})
        CHECK(ppl::parsePipeNrSize(ppl::pipeNrSizeName(s)) == s);
    for (auto l : {ppl::PipeNrLook::Conservative, ppl::PipeNrLook::Subtle, ppl::PipeNrLook::Balanced, ppl::PipeNrLook::Strong})
        CHECK(ppl::parsePipeNrLook(ppl::pipeNrLookName(l)) == l);
    for (auto t : {ppl::PipeSrTarget::Display, ppl::PipeSrTarget::Qhd, ppl::PipeSrTarget::Uhd4K})
        CHECK(ppl::parsePipeSrTarget(ppl::pipeSrTargetName(t)) == t);
    CHECK(!ppl::parsePipeFg("DLSS").has_value());
    CHECK(ppl::pipeReasonName(ppl::PipeReason::AboveDisplay) == "above-display");
    CHECK(ppl::pipeStageName(ppl::PipeStage::FrameGeneration) == "framegen");
    static_assert(ppl::planPipeline(ppl::PipelineRequest{}, ppl::PipelineCaps{}).stageCount == 2);
}
TEST(pipeline_idle_neural_flow_adaptation_is_skipped_only_when_nobody_reads_it){
    // Switch unset: the graph adapts as before, whatever runs.
    CHECK(!ppl::pipeIdleNrFlowSkippable(false, 0, false));
    // Switch set: skipped only with no neural layer and no inference-reuse pass.
    CHECK(ppl::pipeIdleNrFlowSkippable(true, 0, false));
    CHECK(!ppl::pipeIdleNrFlowSkippable(true, 1, false));
    CHECK(!ppl::pipeIdleNrFlowSkippable(true, 4, false));
    CHECK(!ppl::pipeIdleNrFlowSkippable(true, 0, true));
    static_assert(!ppl::pipeIdleNrFlowSkippable(true, 2, true));
}
TEST(pipeline_plan_worst_case_keeps_every_change){
    // The reviewer's case (21 Nov): as many rules at once as a request can trigger. Before, the 17th change was lost.
    ppl::PipelineRequest r; ppl::PipelineCaps c;
    r.displayHz = 60; r.upscaler = ppl::PipeUpscaler::Vsr; r.srTarget = ppl::PipeSrTarget::Uhd4K; r.supersample = true;
    r.secondUpscaler = ppl::PipeUpscaler::Nis; r.clean = 100; r.sharpen = 50; r.nr = true; r.nrSize = ppl::PipeNrSize::Native;
    r.fg = ppl::PipeFg::Dlss; r.fgMultiplier = 6; r.fgExperimental180 = true; r.hdr = true;
    c.vsr = true; c.nis = true; c.nrRuntime = true; c.nrMaxHeight = 0; c.pushFlow = true;
    c.gpuBudgetMs = 1; c.cleanMs = 5; c.nrMs = 50; c.srMs = 20;
    auto p = ppl::planPipeline(r, c);
    CHECK(!p.changesTruncated && p.changeCount > 16 && p.changeCount < ppl::kPipeMaxChanges);
    CHECK(p.change(ppl::PipeStage::Clean, ppl::PipeReason::GpuBudget) != nullptr);
    // The neural layer is the dearest stage: the budget drops it, so no HDR note is left for it.
    CHECK(!p.has(ppl::PipeStage::Neural) && p.change(ppl::PipeStage::Neural, ppl::PipeReason::NeuralHdrUnverified) == nullptr);
    // Many random-free variations of it: nothing is ever truncated.
    for (int up = 0; up <= 6; ++up)
        for (int budget = 0; budget < 3; ++budget) {
            auto v = r; v.upscaler = ppl::PipeUpscaler(up);
            auto w = c; w.gpuBudgetMs = budget == 0 ? -1.0 : budget == 1 ? 0.0 : 1.0; w.dlssSr = budget == 2;
            const auto q = ppl::planPipeline(v, w);
            CHECK(!q.changesTruncated && q.changeCount <= ppl::kPipeMaxChanges);
        }
    // The flag itself: a change that does not fit is counted as lost, never written past the end.
    ppl::PipelinePlan full{};
    for (std::size_t i = 0; i < ppl::kPipeMaxChanges; ++i) ppl::detail_pipe::addChange(full, ppl::PipeStage::Clean, ppl::PipeAction::Noted, ppl::PipeReason::DebandRequantised);
    CHECK(!full.changesTruncated && full.changeCount == ppl::kPipeMaxChanges);
    ppl::detail_pipe::addChange(full, ppl::PipeStage::Clean, ppl::PipeAction::Noted, ppl::PipeReason::DebandRequantised);
    CHECK(full.changesTruncated && full.changeCount == ppl::kPipeMaxChanges);
}
TEST(pipeline_plan_no_empty_reduction_before_scale_one){
    // A 2160-line stream on a 1440p display: the upscaler would not enlarge. One change (scale-one), no
    // "reduced 2160 -> 2160 above-display" before it.
    auto r = pplBase(); r.streamWidth = 3840; r.streamHeight = 2160; r.upscaler = ppl::PipeUpscaler::Nis;
    auto p = ppl::planPipeline(r, pplAllCaps());
    CHECK(!p.has(ppl::PipeStage::Upscale) && p.change(ppl::PipeStage::Upscale, ppl::PipeReason::ScaleOne) != nullptr);
    CHECK(p.change(ppl::PipeStage::Upscale, ppl::PipeReason::AboveDisplay) == nullptr && p.changeCount == 1);
    // A 1080p display with a 4K target: reduced to the stream's own size = scale one, reported once.
    r = pplBase(); r.displayWidth = 1920; r.displayHeight = 1080; r.upscaler = ppl::PipeUpscaler::Dlss; r.srTarget = ppl::PipeSrTarget::Uhd4K;
    p = ppl::planPipeline(r, pplAllCaps());
    CHECK(p.changeCount == 1 && p.change(ppl::PipeStage::Upscale, ppl::PipeReason::ScaleOne) != nullptr);
    // A real reduction is still reported.
    r = pplBase(); r.upscaler = ppl::PipeUpscaler::Dlss; r.srTarget = ppl::PipeSrTarget::Uhd4K;
    p = ppl::planPipeline(r, pplAllCaps());   // kept: change() points into the plan
    const auto* c = p.change(ppl::PipeStage::Upscale, ppl::PipeReason::AboveDisplay);
    CHECK(c != nullptr && c->from == 2160 && c->to == 1440);
}
