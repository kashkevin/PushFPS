// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include "veyra/pipeline/ResolutionPlan.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace veyra::engine {
// Enhancement pipeline planner (sprint 5, Package 8, stream "pipeline"). Portable, header-only, pure:
// tests in tests/remoteplay/PipelinePlanTests.inc; the interface contract is
// /home/claude/out/agents/S5_PIPELINE_API.md (bridge `pushPipelinePlan(request)` forwards here).
//
// Given what the user asked for (a request) and what this PC reports it can run (capabilities: runtimes
// present and supported, measured GPU time and video memory; never a card name), it returns the ordered
// stages and every stage it dropped or reduced, each with one reason from a fixed set. It decides nothing
// by GPU model, and it does not decide which order looks best: the neural layer's position (before or after
// the upscaler) is the request's, because only measurements on the owner's PC can settle it.
//
// Rules (each tested):
//   - clean-up runs first, on the decoded picture, at the stream's size;
//   - the neural layer never runs above its size: min(policy lines, runtime cap, its input), 16:9 fitted
//     like pipeline::ResolutionPlan; the policy maps to pipeline::NrSizePolicy;
//   - an upscaler target larger than the display is reduced to the display unless supersampling was asked
//     for explicitly (docs/SUPER_RESOLUTION.md: a 3840x2160 target shrunk onto a 2560x1440 screen = 2.25x
//     the pixels); an upscaler that would not enlarge (scale 1) is dropped; a second upscaler is dropped;
//   - a stage whose runtime is missing falls back to a vendor-neutral one (upscaler: FSR or NIS; frame
//     generation: FSR, then PushFlow) and says so, or is dropped when there is none (the neural layer has
//     no vendor-neutral equivalent); frame generation is reduced so its output stays within the display
//     refresh and the 120 FPS target (180 only when explicitly experimental), and dropped when nothing is
//     left to generate;
//   - never two sharpening stages in a row: a separate sharpening stage (or NIS as a second step) after an
//     upscaler that sharpens by itself is dropped;
//   - never clean-up twice: beside an upscaler family that already reduces compression artifacts, our own
//     clean-up stage is weakened (standard RTX Video SR) or dropped (the streaming family, which cleans
//     hardest); the family's clean-up cannot be switched off, ours can, so ours gives way;
//   - a note, nothing changed: clean-up kept in front of an upscaler that takes an 8-bit picture loses
//     part of its debanding again when that picture is quantised, unless the final write is dithered;
//   - a note, nothing changed: the neural layer on an HDR stream is never treated as verified (NVIDIA
//     documents its model for an LDR picture with motion vectors, depth and a mask from a game engine; on
//     decoded HDR video its behaviour and memory need are undocumented: S5_NR_RESEARCH.md);
//   - headroom: with measured costs and a measured budget, the most expensive optional IMAGE stage is
//     reduced first (neural layer: a smaller inference size, then off; upscaler: no supersampling, then NIS,
//     then off; clean-up: off) until the plan fits. Frame generation's multiplier and frame pacing are
//     never touched by this rule: the planner has no pacing field at all;
//   - no image stage waits for a later frame (pipeStageLookahead is 0 for each: tested); frame generation
//     by interpolation holds each real frame for its successor, which the plan reports (interpolationHold)
//     rather than hides.
// The double-clean and double-sharpen rules encode what the code and NVIDIA's descriptions say the families
// do (docs/NEURAL_RENDERING.md); whether the weakened combination looks better than either stage alone is
// for the kit's arms to show. Nothing here has run on a GPU. Unverified on Windows.

// Values are appended, never reordered: the bridge and the kit use the names below.
enum class PipeUpscaler : std::uint8_t { Off, Dlss, VsrStream, Nis, Fsr, Vsr, VsrHighBitrate };
enum class PipeFg : std::uint8_t { Off, Vfg, Dlss, PushFlow, Fsr };
enum class PipeNrLook : std::uint8_t { Conservative, Subtle, Balanced, Strong };
enum class PipeNrSize : std::uint8_t { P480, P720, P900, Realtime1080, P1440, Native };
enum class PipeNrPlace : std::uint8_t { AfterUpscale, BeforeUpscale };
enum class PipeSrTarget : std::uint8_t { Display, Qhd, Uhd4K };

struct PipelineRequest {
    std::uint32_t streamWidth = 1920, streamHeight = 1080;
    double streamFps = 60.0;
    double contentFps = 0.0;  // the game's own rate when known; 0 = the stream rate
    bool hdr = false;
    std::uint32_t displayWidth = 2560, displayHeight = 1440;
    double displayHz = 60.0;
    std::uint8_t clean = 0;   // restoration strength 0..100, 0 = off
    PipeUpscaler upscaler = PipeUpscaler::Off;
    // EnhancementSettings::dlssSrModel: 0 K, 1 M (DLSS 4.5), 2 L. An INPUT: the planner never chooses it. NVIDIA
    // recommends M for half resolution per axis and K for the higher-resolution modes; 1080p -> 1440p is 0.75
    // per axis, so which is better there is open (kit arms `v-dlss` with M against `v-dlss-k`). The profiles
    // keep the caller's model; only max-fidelity sets M, because that is the owner's reference.
    std::uint32_t srModel = 1;
    PipeSrTarget srTarget = PipeSrTarget::Display;
    bool supersample = false;
    PipeUpscaler secondUpscaler = PipeUpscaler::Off;
    bool nr = false;
    PipeNrLook nrLook = PipeNrLook::Conservative;
    PipeNrSize nrSize = PipeNrSize::Realtime1080;
    PipeNrPlace nrPlace = PipeNrPlace::AfterUpscale;
    PipeFg fg = PipeFg::Off;
    std::uint32_t fgMultiplier = 2;
    double fgTargetFps = 120.0;
    bool fgExperimental180 = false;
    // Added 20 Nov 22:40 UTC (defaults keep every earlier request's meaning):
    std::uint8_t sharpen = 0;   // a separate sharpening stage after the upscaler, 0..100, 0 = none
    int srSharpness = -1;       // the upscaler's own sharpening where the family has a control (NIS): 0..100, -1 = its default
    bool ditheredOutput = false; // the final write to the display format is dithered (the restore stream's dither)
};

// Costs and the budget are GPU milliseconds per REAL (source) frame, as `[slipstream-nr]` reports them
// (nrMsP95, srMsP95, fgMsP95 = the whole generated batch of one pair, `[slipstream-clean]` gpuMsP95).
struct PipelineCaps {
    bool dlssSr = false, vsr = false, nis = true, fsr = true;
    bool nrRuntime = false;
    std::uint32_t nrMaxHeight = 1440;  // 0 = no cap known
    bool vfg = false, dlssFg = false, pushFlow = false, fsrFg = false;
    double gpuBudgetMs = -1.0;
    double cleanMs = -1.0, nrMs = -1.0, srMs = -1.0, fgMs = -1.0;
    std::int64_t vramFreeMiB = -1, nrVramMiB = -1;
};

enum class PipeStage : std::uint8_t { Decode, Clean, Neural, Upscale, FrameGeneration, Present, Sharpen };
enum class PipeAction : std::uint8_t { Dropped, Reduced, Replaced, Noted };
enum class PipeReason : std::uint8_t {
    RuntimeMissing, ScaleOne, AboveDisplay, DoubleUpscaler, NrSizeCap, NrAboveInput, FgAboveRefresh, FgAboveTarget,
    FgMultiplierOne, FgMultiplierUnsupported, GpuBudget, VramBudget, InvalidRequest,
    FallbackVendorNeutral, DoubleSharpen, DoubleClean, DebandRequantised, NeuralHdrUnverified
};
struct PipeExtent {
    std::uint32_t width = 0, height = 0;
    constexpr bool operator==(const PipeExtent&) const = default;
};
struct PlannedStage {
    PipeStage stage = PipeStage::Decode;
    PipeExtent input{}, output{};
    PipeExtent internal{};      // neural: inference size; others: equal to output
    std::uint32_t detail = 0;   // clean / sharpen: strength; neural: PipeNrLook; upscale: PipeUpscaler; framegen: multiplier
};
struct PipeChange {
    PipeStage stage = PipeStage::Decode;
    PipeAction action = PipeAction::Dropped;
    PipeReason reason = PipeReason::InvalidRequest;
    // Heights for size changes, multipliers for frame generation, strengths for clean / sharpen, and for
    // Replaced the family before and after (PipeUpscaler or PipeFg value).
    std::uint32_t from = 0, to = 0;
};
// 32 changes: more than the rules can produce in one plan (worst case tested); `changesTruncated` says if one was lost.
inline constexpr std::size_t kPipeMaxStages = 8, kPipeMaxChanges = 32;
struct PipelinePlan {
    std::array<PlannedStage, kPipeMaxStages> stages{};
    std::size_t stageCount = 0;
    std::array<PipeChange, kPipeMaxChanges> changes{};
    std::size_t changeCount = 0;
    bool changesTruncated = false;  // a change did not fit (never expected: kPipeMaxChanges is above the worst case)
    PipeExtent output{};
    // The upscaler's own sharpening to apply (NIS: 0..100; -1 = the family's default or no control). A NIS that
    // stands in for an upscaler that does not sharpen (DLSS SR, FSR: missing runtime or the budget rule) gets 0, so
    // a stand-in never brings sharpening halos nobody asked for; a NIS the user chose keeps the request's value.
    int srSharpness = -1;
    pipeline::NrSizePolicy nrPolicy = pipeline::NrSizePolicy::Realtime;
    double submittedFps = 0.0;
    double uniqueMotionFpsMax = 0.0;
    bool experimental = false;
    bool overBudget = false;      // measured costs still exceed the measured budget after every image stage gave way
    // Sum of the planned stages' known costs per real frame (-1 without a budget). After a budget change it
    // contains ESTIMATES (cost taken as proportional to pixels; a replaced stage counted as unknown = 0).
    double plannedCostMs = -1.0;
    bool costEstimated = false;
    std::uint32_t imageDelayFrames = 0;  // frames an image stage waits for: always 0 (no stage looks ahead)
    bool interpolationHold = false;      // frame generation is planned: each real frame waits for its successor

    constexpr const PlannedStage* find(PipeStage s) const {
        for (std::size_t i = 0; i < stageCount; ++i)
            if (stages[i].stage == s) return &stages[i];
        return nullptr;
    }
    constexpr bool has(PipeStage s) const { return find(s) != nullptr; }
    constexpr const PipeChange* change(PipeStage s, PipeReason r) const {
        for (std::size_t i = 0; i < changeCount; ++i)
            if (changes[i].stage == s && changes[i].reason == r) return &changes[i];
        return nullptr;
    }
};

constexpr std::string_view pipeStageName(PipeStage s) {
    switch (s) {
    case PipeStage::Decode: return "decode";
    case PipeStage::Clean: return "clean";
    case PipeStage::Neural: return "neural";
    case PipeStage::Upscale: return "upscale";
    case PipeStage::FrameGeneration: return "framegen";
    case PipeStage::Present: return "present";
    case PipeStage::Sharpen: return "sharpen";
    }
    return "invalid";
}
constexpr std::string_view pipeActionName(PipeAction a) {
    return a == PipeAction::Dropped ? "dropped" : a == PipeAction::Reduced ? "reduced" : a == PipeAction::Replaced ? "replaced" : "noted";
}
constexpr std::string_view pipeReasonName(PipeReason r) {
    switch (r) {
    case PipeReason::RuntimeMissing: return "runtime-missing";
    case PipeReason::ScaleOne: return "scale-one";
    case PipeReason::AboveDisplay: return "above-display";
    case PipeReason::DoubleUpscaler: return "double-upscaler";
    case PipeReason::NrSizeCap: return "nr-size-cap";
    case PipeReason::NrAboveInput: return "nr-above-input";
    case PipeReason::FgAboveRefresh: return "fg-above-refresh";
    case PipeReason::FgAboveTarget: return "fg-above-target";
    case PipeReason::FgMultiplierOne: return "fg-multiplier-one";
    case PipeReason::FgMultiplierUnsupported: return "fg-multiplier-unsupported";
    case PipeReason::GpuBudget: return "gpu-budget";
    case PipeReason::VramBudget: return "vram-budget";
    case PipeReason::InvalidRequest: return "invalid-request";
    case PipeReason::FallbackVendorNeutral: return "fallback-vendor-neutral";
    case PipeReason::DoubleSharpen: return "double-sharpen";
    case PipeReason::DoubleClean: return "double-clean";
    case PipeReason::DebandRequantised: return "deband-requantised";
    case PipeReason::NeuralHdrUnverified: return "neural-hdr-unverified";
    }
    return "invalid";
}
constexpr std::string_view pipeUpscalerName(PipeUpscaler u) {
    switch (u) {
    case PipeUpscaler::Off: return "off";
    case PipeUpscaler::Dlss: return "dlss";
    case PipeUpscaler::VsrStream: return "vsr-stream";
    case PipeUpscaler::Nis: return "nis";
    case PipeUpscaler::Fsr: return "fsr";
    case PipeUpscaler::Vsr: return "vsr";
    case PipeUpscaler::VsrHighBitrate: return "vsr-hb";
    }
    return "invalid";
}
constexpr std::string_view pipeFgName(PipeFg f) {
    switch (f) {
    case PipeFg::Off: return "off";
    case PipeFg::Vfg: return "vfg";
    case PipeFg::Dlss: return "dlss";
    case PipeFg::PushFlow: return "pushflow";
    case PipeFg::Fsr: return "fsr";
    }
    return "invalid";
}
constexpr std::string_view pipeNrLookName(PipeNrLook l) {
    switch (l) {
    case PipeNrLook::Conservative: return "conservative";
    case PipeNrLook::Subtle: return "subtle";
    case PipeNrLook::Balanced: return "balanced";
    case PipeNrLook::Strong: return "strong";
    }
    return "invalid";
}
constexpr std::string_view pipeNrSizeName(PipeNrSize s) {
    switch (s) {
    case PipeNrSize::P480: return "480";
    case PipeNrSize::P720: return "720";
    case PipeNrSize::P900: return "900";
    case PipeNrSize::Realtime1080: return "1080";
    case PipeNrSize::P1440: return "1440";
    case PipeNrSize::Native: return "native";
    }
    return "invalid";
}
constexpr std::string_view pipeSrTargetName(PipeSrTarget t) {
    return t == PipeSrTarget::Qhd ? "qhd" : t == PipeSrTarget::Uhd4K ? "4k" : "display";
}
// Parsers for the bridge and the test hook: exact lower-case names as above.
constexpr std::optional<PipeUpscaler> parsePipeUpscaler(std::string_view v) {
    for (auto u : {PipeUpscaler::Off, PipeUpscaler::Dlss, PipeUpscaler::VsrStream, PipeUpscaler::Nis, PipeUpscaler::Fsr, PipeUpscaler::Vsr,
                   PipeUpscaler::VsrHighBitrate})
        if (pipeUpscalerName(u) == v) return u;
    return std::nullopt;
}
constexpr std::optional<PipeFg> parsePipeFg(std::string_view v) {
    for (auto f : {PipeFg::Off, PipeFg::Vfg, PipeFg::Dlss, PipeFg::PushFlow, PipeFg::Fsr})
        if (pipeFgName(f) == v) return f;
    return std::nullopt;
}
constexpr std::optional<PipeNrLook> parsePipeNrLook(std::string_view v) {
    for (auto l : {PipeNrLook::Conservative, PipeNrLook::Subtle, PipeNrLook::Balanced, PipeNrLook::Strong})
        if (pipeNrLookName(l) == v) return l;
    return std::nullopt;
}
constexpr std::optional<PipeNrSize> parsePipeNrSize(std::string_view v) {
    for (auto s : {PipeNrSize::P480, PipeNrSize::P720, PipeNrSize::P900, PipeNrSize::Realtime1080, PipeNrSize::P1440, PipeNrSize::Native})
        if (pipeNrSizeName(s) == v) return s;
    return std::nullopt;
}
constexpr std::optional<PipeSrTarget> parsePipeSrTarget(std::string_view v) {
    for (auto t : {PipeSrTarget::Display, PipeSrTarget::Qhd, PipeSrTarget::Uhd4K})
        if (pipeSrTargetName(t) == v) return t;
    return std::nullopt;
}

// What each upscaler family does besides enlarging, by reading the code and the families' descriptions
// (include/veyra/engine/EnhancementSettings.h; docs/NEURAL_RENDERING.md gives the lines):
//   - runs on any DirectX 12 adapter: FSR, NIS;
//   - reduces compression artifacts by design: RTX Video SR standard ("trained on compressed video") and
//     streaming ("cleans hardest"); the high-bitrate family "skips the compression clean-up";
//   - sharpens by itself: NIS (adaptive sharpening, unless its sharpness is 0) and the RTX Video SR
//     families (part of the model, no control). DLSS SR is evaluated with sharpness 0 and FSR with
//     sharpening off in this app (EnhanceGraph.cpp `EvalDesc ed{}`, FsrSrBackend.cpp `enableSharpening=false`).
constexpr bool pipeUpscalerVendorNeutral(PipeUpscaler u) { return u == PipeUpscaler::Nis || u == PipeUpscaler::Fsr; }
constexpr bool pipeUpscalerCleans(PipeUpscaler u) { return u == PipeUpscaler::Vsr || u == PipeUpscaler::VsrStream; }
constexpr bool pipeUpscalerSharpens(PipeUpscaler u, int srSharpness = -1) {
    switch (u) {
    case PipeUpscaler::Nis: return srSharpness != 0;
    case PipeUpscaler::Vsr: case PipeUpscaler::VsrStream: case PipeUpscaler::VsrHighBitrate: return true;
    case PipeUpscaler::Off: case PipeUpscaler::Dlss: case PipeUpscaler::Fsr: return false;
    }
    return false;
}
// Takes an encoded 8-bit picture (R8G8B8A8): every RTX Video SR family (VideoSrBackend.cpp refuses any other
// format; VfxSrBackend.cpp creates its textures as such). DLSS SR, FSR and NIS read the graph's FP16 colour.
constexpr bool pipeUpscalerEightBitInput(PipeUpscaler u) {
    return u == PipeUpscaler::Vsr || u == PipeUpscaler::VsrStream || u == PipeUpscaler::VsrHighBitrate;
}
constexpr bool pipeFgVendorNeutral(PipeFg f) { return f == PipeFg::Fsr || f == PipeFg::PushFlow; }
// Our clean-up strength beside an upscaler that cleans by itself (standard RTX Video SR): at most this.
inline constexpr std::uint32_t kPipeCleanBesideCleaningUpscaler = 25;
// Frames a stage waits for before it can show the current one. Every image stage here works on the
// current frame (and, for the temporal ones, on PAST frames only). A stage that needed a later frame
// would have to return non-zero here, and the plan would report it in imageDelayFrames.
constexpr std::uint32_t pipeStageLookahead(PipeStage) { return 0; }

// NR size policy -> lines (0 = the layer's input) and the pipeline::NrSizePolicy it stands for.
constexpr std::uint32_t pipeNrLines(PipeNrSize s) {
    switch (s) {
    case PipeNrSize::P480: return 480;
    case PipeNrSize::P720: return 720;
    case PipeNrSize::P900: return 900;
    case PipeNrSize::Realtime1080: return 1080;
    case PipeNrSize::P1440: return 1440;
    case PipeNrSize::Native: return 0;
    }
    return 0;
}
constexpr pipeline::NrSizePolicy pipeNrPolicyForLines(std::uint32_t lines) {
    return lines == 0 ? pipeline::NrSizePolicy::Native : lines <= 480 ? pipeline::NrSizePolicy::P480
        : lines <= 720 ? pipeline::NrSizePolicy::P720 : lines <= 900 ? pipeline::NrSizePolicy::P900
        : lines <= 1080 ? pipeline::NrSizePolicy::Realtime : pipeline::NrSizePolicy::P1440;
}
constexpr pipeline::NrSizePolicy pipeNrPolicy(PipeNrSize s) { return pipeNrPolicyForLines(pipeNrLines(s)); }
// The headroom rule steps the neural layer's inference size down through the sizes the owner asked to
// benchmark (1440, 1080, 900, 720) and switches it off below 720 lines.
inline constexpr std::uint32_t kPipeNrBudgetFloorLines = 720;
constexpr std::uint32_t pipeNrStepDown(std::uint32_t lines) {
    return lines > 1440 ? 1440u : lines > 1080 ? 1080u : lines > 900 ? 900u : lines > 720 ? 720u : 0u;
}

// Frame generation: the largest multiplier each backend offers (EnhancementSettings.h choices) and whether
// a value is one it offers.
constexpr std::uint32_t pipeFgMaxMultiplier(PipeFg f) {
    switch (f) {
    case PipeFg::Vfg: return 8;
    case PipeFg::Dlss: return 6;
    case PipeFg::PushFlow: return 4;
    case PipeFg::Fsr: return 2;
    case PipeFg::Off: return 1;
    }
    return 1;
}
constexpr bool pipeFgOffers(PipeFg f, std::uint32_t m) {
    if (m < 2 || m > pipeFgMaxMultiplier(f)) return false;
    return f != PipeFg::Dlss || m != 5;  // DLSS exposes 2/3/4/6
}
// Same tolerance as the performance modes (PushPerformanceMode.h kPushModeDisplayFit): 60.00 x 3 fits 179.82 Hz.
inline constexpr double kPipeRateFit = 1.02;
inline constexpr double kPipeFgTargetFps = 120.0, kPipeFgExperimentalFps = 180.0;

namespace detail_pipe {
constexpr bool validExtent(std::uint32_t w, std::uint32_t h) { return w >= 16 && h >= 16 && w <= 16384 && h <= 16384; }
constexpr bool validRate(double r) { return r > 0.0 && r <= 1000.0; }
constexpr double pixels(PipeExtent e) { return double(e.width) * double(e.height); }
// Fit `from` into `box` keeping its shape, even sizes, enlarging only (the single-step SR rule of
// pipeline::srStageExtents).
constexpr PipeExtent enlargeInto(PipeExtent from, PipeExtent box) {
    const double scale = std::min(double(box.width) / from.width, double(box.height) / from.height);
    if (scale <= 1.0) return from;
    return {std::max(2u, std::uint32_t(from.width * scale + 1e-6) & ~1u), std::max(2u, std::uint32_t(from.height * scale + 1e-6) & ~1u)};
}
// The neural layer's inference size: a 16:9 box of `lines`, never larger than the input (ResolutionPlan.h).
constexpr PipeExtent nrInternal(PipeExtent in, std::uint32_t lines) {
    if (lines == 0) return in;
    const double scale = std::min({1.0, (lines * 16.0 / 9.0) / in.width, double(lines) / in.height});
    if (scale >= 1.0) return in;
    return {std::max(1u, std::uint32_t(in.width * scale) & ~1u), std::max(1u, std::uint32_t(in.height * scale) & ~1u)};
}
constexpr void addChange(PipelinePlan& p, PipeStage s, PipeAction a, PipeReason r, std::uint32_t from = 0, std::uint32_t to = 0) {
    if (p.changeCount < p.changes.size()) p.changes[p.changeCount++] = PipeChange{s, a, r, from, to};
    else p.changesTruncated = true;
}
constexpr void addStage(PipelinePlan& p, PipeStage s, PipeExtent in, PipeExtent out, PipeExtent internal, std::uint32_t detail) {
    if (p.stageCount < p.stages.size()) p.stages[p.stageCount++] = PlannedStage{s, in, out, internal, detail};
}
constexpr bool upscalerPresent(PipeUpscaler u, const PipelineCaps& c) {
    switch (u) {
    case PipeUpscaler::Dlss: return c.dlssSr;
    case PipeUpscaler::VsrStream: case PipeUpscaler::Vsr: case PipeUpscaler::VsrHighBitrate: return c.vsr;
    case PipeUpscaler::Nis: return c.nis;
    case PipeUpscaler::Fsr: return c.fsr;
    case PipeUpscaler::Off: return false;
    }
    return false;
}
// The vendor-neutral stand-in for an upscaler whose runtime is missing: the temporal one (FSR) for DLSS SR,
// the spatial one (NIS) for the spatial RTX Video SR families; then the other. Off = none.
constexpr PipeUpscaler upscalerFallback(PipeUpscaler u, const PipelineCaps& c) {
    const bool temporal = u == PipeUpscaler::Dlss;
    const PipeUpscaler first = temporal ? PipeUpscaler::Fsr : PipeUpscaler::Nis;
    const PipeUpscaler second = temporal ? PipeUpscaler::Nis : PipeUpscaler::Fsr;
    if (first != u && upscalerPresent(first, c)) return first;
    if (second != u && upscalerPresent(second, c)) return second;
    return PipeUpscaler::Off;
}
constexpr bool fgPresent(PipeFg f, const PipelineCaps& c) {
    switch (f) {
    case PipeFg::Vfg: return c.vfg;
    case PipeFg::Dlss: return c.dlssFg;
    case PipeFg::PushFlow: return c.pushFlow;
    case PipeFg::Fsr: return c.fsrFg;
    case PipeFg::Off: return false;
    }
    return false;
}
constexpr PipeFg fgFallback(PipeFg f, const PipelineCaps& c) {
    if (f != PipeFg::Fsr && c.fsrFg) return PipeFg::Fsr;
    if (f != PipeFg::PushFlow && c.pushFlow) return PipeFg::PushFlow;
    return PipeFg::Off;
}
}  // namespace detail_pipe

constexpr PipelinePlan planPipeline(const PipelineRequest& r, const PipelineCaps& c) {
    using namespace detail_pipe;
    PipelinePlan p{};
    const PipeExtent stream{r.streamWidth, r.streamHeight};
    if (!validExtent(r.streamWidth, r.streamHeight) || !validExtent(r.displayWidth, r.displayHeight) || !validRate(r.streamFps) ||
        !validRate(r.displayHz) || r.contentFps < 0.0 || r.contentFps > 1000.0 || r.fgTargetFps < 0.0) {
        addChange(p, PipeStage::Decode, PipeAction::Reduced, PipeReason::InvalidRequest);
        addStage(p, PipeStage::Decode, stream, stream, stream, 0);
        addStage(p, PipeStage::Present, stream, stream, stream, 0);
        p.output = stream;
        return p;
    }
    const PipeExtent display{r.displayWidth, r.displayHeight};

    // 1. Upscaler: runtime (with the vendor-neutral fallback), target, redundancy. Its input is always the
    //    stream's size: clean-up and a neural layer placed before it both return their input size.
    PipeUpscaler up = r.upscaler;
    int srSharp = r.srSharpness < 0 ? -1 : std::min(r.srSharpness, 100);  // out-of-range values: -1 = default, at most 100
    if (up != PipeUpscaler::Off && !upscalerPresent(up, c)) {
        const PipeUpscaler fallback = upscalerFallback(up, c);
        if (fallback == PipeUpscaler::Off) addChange(p, PipeStage::Upscale, PipeAction::Dropped, PipeReason::RuntimeMissing);
        else addChange(p, PipeStage::Upscale, PipeAction::Replaced, PipeReason::FallbackVendorNeutral, std::uint32_t(up), std::uint32_t(fallback));
        if (fallback == PipeUpscaler::Nis && !pipeUpscalerSharpens(up, srSharp)) srSharp = 0;
        up = fallback;
    }
    PipeExtent srOut = stream;
    if (up != PipeUpscaler::Off) {
        const PipeExtent box = r.srTarget == PipeSrTarget::Qhd ? PipeExtent{2560, 1440} : r.srTarget == PipeSrTarget::Uhd4K ? PipeExtent{3840, 2160} : display;
        PipeExtent out = enlargeInto(stream, box);
        if ((out.width > display.width || out.height > display.height) && !r.supersample) {
            const PipeExtent reduced = enlargeInto(stream, display);
            // A reduction that ends at the stream's size is reported once, as scale-one below.
            if (!(reduced == out) && !(reduced == stream))
                addChange(p, PipeStage::Upscale, PipeAction::Reduced, PipeReason::AboveDisplay, out.height, reduced.height);
            out = reduced;
        }
        if (out == stream) {
            addChange(p, PipeStage::Upscale, PipeAction::Dropped, PipeReason::ScaleOne, stream.height, out.height);
            up = PipeUpscaler::Off;
        } else {
            srOut = out;
        }
    }
    if (r.secondUpscaler != PipeUpscaler::Off) {
        // NIS as a second step after an upscaler that already sharpens is a second sharpening stage.
        const bool sharpenTwice = r.secondUpscaler == PipeUpscaler::Nis && pipeUpscalerSharpens(up, srSharp);
        addChange(p, PipeStage::Upscale, PipeAction::Dropped, sharpenTwice ? PipeReason::DoubleSharpen : PipeReason::DoubleUpscaler);
    }

    // 2. Clean-up: never twice. The family's own clean-up cannot be switched off, so ours gives way.
    std::uint32_t clean = std::min<std::uint32_t>(r.clean, 100u);
    if (clean > 0 && pipeUpscalerCleans(up)) {
        const std::uint32_t kept = up == PipeUpscaler::VsrStream ? 0u : std::min(clean, kPipeCleanBesideCleaningUpscaler);
        if (kept < clean) {
            addChange(p, PipeStage::Clean, kept == 0 ? PipeAction::Dropped : PipeAction::Reduced, PipeReason::DoubleClean, clean, kept);
            clean = kept;
        }
    }

    // What the clean-up stage's debanding smoothed below one 8-bit step is quantised again by an upscaler
    // that takes an 8-bit picture. Nothing is changed for it: the plan says so, unless the final write is
    // dithered (S5_SRC_VIDEO.md: libplacebo debands first and dithers last).
    if (clean > 0 && pipeUpscalerEightBitInput(up) && !r.ditheredOutput)
        addChange(p, PipeStage::Clean, PipeAction::Noted, PipeReason::DebandRequantised, clean, clean);

    // 3. A separate sharpening stage: never after an upscaler that sharpens by itself.
    std::uint32_t sharpen = std::min<std::uint32_t>(r.sharpen, 100u);
    if (sharpen > 0 && pipeUpscalerSharpens(up, srSharp)) {
        addChange(p, PipeStage::Sharpen, PipeAction::Dropped, PipeReason::DoubleSharpen, sharpen, 0);
        sharpen = 0;
    }

    // 4. Neural layer: runtime, video memory, size (its position is the request's). No vendor-neutral
    //    stand-in exists for it: without its runtime it is dropped and the clean-up stage remains.
    bool neural = r.nr;
    if (neural && !c.nrRuntime) {
        addChange(p, PipeStage::Neural, PipeAction::Dropped, PipeReason::RuntimeMissing);
        neural = false;
    }
    if (neural && c.vramFreeMiB >= 0 && c.nrVramMiB >= 0 && c.nrVramMiB > c.vramFreeMiB) {
        addChange(p, PipeStage::Neural, PipeAction::Dropped, PipeReason::VramBudget,
                  std::uint32_t(std::min<std::int64_t>(c.nrVramMiB, 0xffffffffLL)), std::uint32_t(std::min<std::int64_t>(c.vramFreeMiB, 0xffffffffLL)));
        neural = false;
    }
    const bool neuralBefore = r.nrPlace == PipeNrPlace::BeforeUpscale;
    std::uint32_t nrLines = 0;  // inference lines; decided against the layer's input as first planned
    if (neural) {
        const PipeExtent in = neuralBefore ? stream : srOut;
        nrLines = pipeNrLines(r.nrSize);
        if (nrLines == 0) nrLines = in.height;
        if (c.nrMaxHeight > 0 && nrLines > c.nrMaxHeight) {
            addChange(p, PipeStage::Neural, PipeAction::Reduced, PipeReason::NrSizeCap, nrLines, c.nrMaxHeight);
            nrLines = c.nrMaxHeight;
        }
        if (nrLines > in.height) {
            if (r.nrSize != PipeNrSize::Native) addChange(p, PipeStage::Neural, PipeAction::Reduced, PipeReason::NrAboveInput, nrLines, in.height);
            nrLines = in.height;
        }
    }

    // 5. Frame generation: runtime (vendor-neutral fallback), the backend's multipliers, target, refresh.
    PipeFg fg = r.fg;
    std::uint32_t multiplier = 1;
    if (fg != PipeFg::Off && !fgPresent(fg, c)) {
        const PipeFg fallback = fgFallback(fg, c);
        if (fallback == PipeFg::Off) addChange(p, PipeStage::FrameGeneration, PipeAction::Dropped, PipeReason::RuntimeMissing);
        else addChange(p, PipeStage::FrameGeneration, PipeAction::Replaced, PipeReason::FallbackVendorNeutral, std::uint32_t(fg), std::uint32_t(fallback));
        fg = fallback;
    }
    if (fg != PipeFg::Off) {
        std::uint32_t m = std::max<std::uint32_t>(r.fgMultiplier, 1u);
        const std::uint32_t asked = m;
        if (m > 1 && !pipeFgOffers(fg, m)) {
            std::uint32_t n = std::min(m, pipeFgMaxMultiplier(fg));
            while (n > 1 && !pipeFgOffers(fg, n)) --n;
            addChange(p, PipeStage::FrameGeneration, PipeAction::Reduced, PipeReason::FgMultiplierUnsupported, m, n);
            m = n;
        }
        double target = r.fgTargetFps > 0.0 ? r.fgTargetFps : kPipeFgTargetFps;
        target = std::min(target, r.fgExperimental180 ? kPipeFgExperimentalFps : kPipeFgTargetFps);
        const auto stepDown = [&](double limit) {
            std::uint32_t n = m;
            while (n > 1 && r.streamFps * n > limit * kPipeRateFit) {
                --n;
                while (n > 1 && !pipeFgOffers(fg, n)) --n;
            }
            return n;
        };
        const std::uint32_t byTarget = stepDown(target);
        if (byTarget < m) {
            addChange(p, PipeStage::FrameGeneration, PipeAction::Reduced, PipeReason::FgAboveTarget, m, byTarget);
            m = byTarget;
        }
        const std::uint32_t byRefresh = stepDown(r.displayHz);
        if (byRefresh < m) {
            addChange(p, PipeStage::FrameGeneration, PipeAction::Reduced, PipeReason::FgAboveRefresh, m, byRefresh);
            m = byRefresh;
        }
        if (m <= 1) {
            addChange(p, PipeStage::FrameGeneration, PipeAction::Dropped, PipeReason::FgMultiplierOne, asked, 1);
            fg = PipeFg::Off;
        } else {
            multiplier = m;
        }
    }

    // 6. Headroom: with a measured budget, the most expensive optional IMAGE stage gives way first, one
    //    step at a time, until the planned stages fit. Frame generation and pacing are not candidates.
    //    A stage whose cost is unknown is never touched. Costs after a step are estimates (see the plan).
    if (c.gpuBudgetMs >= 0.0) {
        double cleanCost = clean > 0 && c.cleanMs > 0.0 ? c.cleanMs : 0.0;
        double nrCost = neural && c.nrMs > 0.0 ? c.nrMs : 0.0;
        // A measured upscaler cost belongs to the family that was asked for, not to its stand-in.
        double srCost = up != PipeUpscaler::Off && up == r.upscaler && c.srMs > 0.0 ? c.srMs : 0.0;
        const double fgCost = fg != PipeFg::Off && fg == r.fg && c.fgMs > 0.0 ? c.fgMs : 0.0;
        for (int step = 0; step < 16 && cleanCost + nrCost + srCost + fgCost > c.gpuBudgetMs; ++step) {
            if (nrCost > 0.0 && nrCost >= srCost && nrCost >= cleanCost) {
                const PipeExtent in = neuralBefore ? stream : srOut;
                const PipeExtent before = nrInternal(in, nrLines);
                const std::uint32_t lower = pipeNrStepDown(before.height);
                if (lower >= kPipeNrBudgetFloorLines) {
                    addChange(p, PipeStage::Neural, PipeAction::Reduced, PipeReason::GpuBudget, before.height, lower);
                    nrCost *= pixels(nrInternal(in, lower)) / pixels(before);
                    nrLines = lower;
                } else {
                    addChange(p, PipeStage::Neural, PipeAction::Dropped, PipeReason::GpuBudget, before.height, 0);
                    neural = false;
                    nrCost = 0.0;
                }
            } else if (srCost > 0.0 && srCost >= cleanCost) {
                const PipeExtent fitted = enlargeInto(stream, display);
                if (pixels(srOut) > pixels(fitted) && !(fitted == stream)) {
                    // Supersampling is the first thing to go: fewer pixels for this stage and every later one.
                    addChange(p, PipeStage::Upscale, PipeAction::Reduced, PipeReason::GpuBudget, srOut.height, fitted.height);
                    srCost *= pixels(fitted) / pixels(srOut);
                    srOut = fitted;
                } else if (up != PipeUpscaler::Nis && c.nis) {
                    addChange(p, PipeStage::Upscale, PipeAction::Replaced, PipeReason::GpuBudget, std::uint32_t(up), std::uint32_t(PipeUpscaler::Nis));
                    if (!pipeUpscalerSharpens(up, srSharp)) srSharp = 0;  // the stand-in does not sharpen either
                    up = PipeUpscaler::Nis;
                    srCost = 0.0;  // not measured for the stand-in
                    if (sharpen > 0 && pipeUpscalerSharpens(up, srSharp)) {
                        addChange(p, PipeStage::Sharpen, PipeAction::Dropped, PipeReason::DoubleSharpen, sharpen, 0);
                        sharpen = 0;
                    }
                } else {
                    addChange(p, PipeStage::Upscale, PipeAction::Dropped, PipeReason::GpuBudget, srOut.height, stream.height);
                    up = PipeUpscaler::Off;
                    srOut = stream;
                    srCost = 0.0;
                }
                p.costEstimated = true;
                continue;
            } else if (cleanCost > 0.0) {
                addChange(p, PipeStage::Clean, PipeAction::Dropped, PipeReason::GpuBudget, clean, 0);
                clean = 0;
                cleanCost = 0.0;
            } else {
                break;  // only frame generation, or stages of unknown cost, are left
            }
            p.costEstimated = true;
        }
        p.plannedCostMs = cleanCost + nrCost + srCost + fgCost;
        p.overBudget = p.plannedCostMs > c.gpuBudgetMs;
    }

    if (neural && r.hdr) addChange(p, PipeStage::Neural, PipeAction::Noted, PipeReason::NeuralHdrUnverified);
    p.srSharpness = up == PipeUpscaler::Off ? -1 : srSharp;

    // 7. The ordered stages.
    addStage(p, PipeStage::Decode, stream, stream, stream, 0);
    if (clean > 0) addStage(p, PipeStage::Clean, stream, stream, stream, clean);
    PipeExtent cur = stream;
    const auto placeNeural = [&](PipeExtent in) {
        const PipeExtent internal = nrInternal(in, nrLines);
        p.nrPolicy = r.nrSize == PipeNrSize::Native && internal == in ? pipeline::NrSizePolicy::Native : pipeNrPolicyForLines(internal.height);
        addStage(p, PipeStage::Neural, in, in, internal, std::uint32_t(r.nrLook));
        p.experimental = true;
    };
    if (neural && neuralBefore) placeNeural(cur);
    if (up != PipeUpscaler::Off) {
        addStage(p, PipeStage::Upscale, cur, srOut, srOut, std::uint32_t(up));
        cur = srOut;
    }
    if (neural && !neuralBefore) placeNeural(cur);
    if (sharpen > 0) addStage(p, PipeStage::Sharpen, cur, cur, cur, sharpen);
    p.submittedFps = r.streamFps;
    const double content = r.contentFps > 0.0 ? std::min(r.contentFps, r.streamFps) : r.streamFps;
    p.uniqueMotionFpsMax = content;
    if (fg != PipeFg::Off) {
        addStage(p, PipeStage::FrameGeneration, cur, cur, cur, multiplier);
        p.submittedFps = r.streamFps * multiplier;
        p.uniqueMotionFpsMax = std::min(p.submittedFps, content * multiplier);
        if (r.streamFps * multiplier > kPipeFgTargetFps * kPipeRateFit) p.experimental = true;
        p.interpolationHold = true;
    }
    addStage(p, PipeStage::Present, cur, cur, cur, 0);
    p.output = cur;
    for (std::size_t i = 0; i < p.stageCount; ++i)
        if (p.stages[i].stage != PipeStage::FrameGeneration) p.imageDelayFrames += pipeStageLookahead(p.stages[i].stage);
    return p;
}

// The five profile ids of S5_CONTRACT.md. `base` carries stream, display and HDR; everything the profile
// decides is overwritten. An unknown id returns `base` unchanged. The clean strength is a placeholder for
// the restore stream's default; max-fidelity is the owner's own reference configuration (DLSS SR model M,
// 4K target shrunk to the display, NVIDIA VFG 2x) until something stronger is verified. On a PC without
// the NVIDIA runtimes the same requests plan FSR / NIS and FSR frame generation, each with its reason.
inline constexpr std::uint8_t kPipeProfileCleanStrength = 50;
inline constexpr std::string_view kPipeProfileIds[] = {"original", "clean-1440", "enhanced-1440", "max-fidelity", "ultra-low-latency"};
constexpr bool pipeProfileKnown(std::string_view id) {
    for (auto known : kPipeProfileIds)
        if (known == id) return true;
    return false;
}
namespace detail_pipe {
constexpr PipelineRequest cleared(const PipelineRequest& base) {
    PipelineRequest r = base;
    r.clean = 0; r.upscaler = PipeUpscaler::Off; r.srTarget = PipeSrTarget::Display; r.supersample = false;  // srModel: the caller's
    r.secondUpscaler = PipeUpscaler::Off; r.nr = false; r.nrLook = PipeNrLook::Conservative; r.nrSize = PipeNrSize::Realtime1080;
    r.nrPlace = PipeNrPlace::AfterUpscale; r.fg = PipeFg::Off; r.fgMultiplier = 2; r.fgTargetFps = kPipeFgTargetFps;
    r.fgExperimental180 = false; r.sharpen = 0; r.srSharpness = -1;  // ditheredOutput is a fact of the build: kept
    return r;
}
}  // namespace detail_pipe
constexpr PipelineRequest pipelineProfileRequest(std::string_view id, const PipelineRequest& base) {
    if (!pipeProfileKnown(id)) return base;
    PipelineRequest r = detail_pipe::cleared(base);
    if (id == "clean-1440" || id == "enhanced-1440") {
        r.clean = kPipeProfileCleanStrength; r.upscaler = PipeUpscaler::Dlss; r.srTarget = PipeSrTarget::Qhd;
        // The user's frame generation is kept, never turned on (orchestrator's decision, 21 Nov 02:20 UTC):
        // interpolation holds each real frame for about one content interval, and the owner's Original-versus-
        // processed comparison must not mix frame generation into the picture stages.
        r.fg = base.fg; r.fgMultiplier = base.fgMultiplier; r.fgTargetFps = base.fgTargetFps; r.fgExperimental180 = base.fgExperimental180;
        r.nr = id == "enhanced-1440";
    } else if (id == "max-fidelity") {
        r.upscaler = PipeUpscaler::Dlss; r.srModel = 1; r.srTarget = PipeSrTarget::Uhd4K; r.supersample = true; r.fg = PipeFg::Vfg;
    }
    return r;
}
// The owner's five benchmark arms (second directive, section 3), without frame generation so that the
// image stages are compared alone: A original stream; B DLSS upscaling alone; C clean-up + DLSS upscaling;
// D neural layer alone; E clean-up + neural layer + DLSS upscaling. Another letter returns `base` unchanged.
constexpr PipelineRequest pipelineArmRequest(char arm, const PipelineRequest& base) {
    if (arm < 'A' || arm > 'E') return base;
    PipelineRequest r = detail_pipe::cleared(base);
    const bool cleanArm = arm == 'C' || arm == 'E', upscaleArm = arm == 'B' || arm == 'C' || arm == 'E', neuralArm = arm == 'D' || arm == 'E';
    if (cleanArm) r.clean = kPipeProfileCleanStrength;
    if (upscaleArm) { r.upscaler = PipeUpscaler::Dlss; r.srTarget = PipeSrTarget::Qhd; }
    r.nr = neuralArm;
    return r;
}
// Experiment VEYRA_EXP_PIPE_IDLE_NR_FLOW (docs/NEURAL_RENDERING.md section 9, item 7; off by default). After the
// optical flow the graph adapts the field to the neural layer's size (`nrFlow_`) on every frame with flow, also
// when no neural layer exists. The only readers of that field are the neural layers (their motion input) and the
// neural inference-reuse pass (src/pipeline/EnhanceGraph.cpp, read 21 Nov 2026), so with no layer and no reuse pass
// nobody reads it and its adaptation pass may be skipped. Everything else the flow feeds (`baseFlow_`, the per-layer,
// double-SR and DLAA fields, frame generation's refined field) is adapted as before.
constexpr bool pipeIdleNrFlowSkippable(bool switchOn, std::size_t neuralLayers, bool reusePass) {
    return switchOn && neuralLayers == 0 && !reusePass;
}
}  // namespace veyra::engine
