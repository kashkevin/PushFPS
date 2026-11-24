// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <cmath>

namespace veyra::engine {
// Adaptive neural layer under frame generation (stream "nrperf", experiment VEYRA_EXP_RP_NR_ADAPT=1; off by default).
// Portable, header-only, pure: tests in tests/remoteplay/NrAdaptTests.inc. Why: docs/drafts/NR_STUTTER.md, cause C1.
//
// A live source is processed one pair at a time (EngineController `enhancementPending`): when the GPU work of one
// pair (the real frame's base, the neural layer included, plus its generated frames) is longer than the pair's
// interval, decoded frames are replaced in the one-slot mailbox and generated frames of late frames are skipped.
// The cheapest way to shorten that work without a rebuild is the inference reuse that already exists
// (pipeline/NrReuse.h, engine/AutoNrReuseController.h, docs/NR_INFERENCE_REUSE_2026-11-07.md): a reused frame skips
// the inference and carries the previous inference's change along the optical flow, and the next frame always infers
// again. Its Auto controller steps up after 0.5 s of mean processing over the budget and back down after 3 s with
// 15 % to spare (hysteresis), and logs every change (`[nr-reuse] auto level=`).
//
// What this experiment changes, and nothing else:
//   1. Mode: in a live session with the neural layer AND graph frame generation on, a reuse mode the user left Off
//      runs as Auto. A mode the user chose (Auto, 1/2, 1/3, 1/4) is never changed. Unset switch: never.
//   2. Budget: for that Auto, the controller's budget is 80 % of the PAIR interval (the content rate once duplicate
//      removal has measured it, else the transport rate), where the upstream rule always takes the transport rate
//      (too strict for a 30 fps game whose pairs are 33 ms apart). The share (0.8) is the upstream controller's.
// It adds no buffer and no wait. A reused frame does less GPU work, but while the reuse pass exists its motion pass (one
// compute pass at the flow size, NrReusePass::compose) runs on every real frame in series with the base, also at level 0:
// it adds its GPU time to each real frame and so to the pair phase (Remote Play: T + P95 of the base; capture cards: the
// learned readiness P95). Small and unmeasured. Nothing here has run on a GPU.

inline constexpr double kPushNrAdaptBudgetShare = 0.8;  // AutoNrReuseController's budget: 80 % of the interval

struct PushNrAdaptInput {
    bool switchOn = false;        // VEYRA_EXP_RP_NR_ADAPT=1 (read once by the engine)
    bool live = false;            // capture card, PS5 / Moonlight / Xbox stream, screen or frame input
    bool image = false;           // still image: never
    bool neuralLayer = false;     // the graph runs a neural layer (EnhanceGraphDesc::enableNr)
    bool frameGeneration = false; // the graph generates frames (EnhanceGraphDesc::enableFg)
    bool presentSinkFg = false;   // XeSS generates inside the presenter: no graph pair, no pair budget here
    bool ngxRuntime = false;      // NVIDIA's runtime on an NVIDIA card (the reuse pass serves NGX layers only)
    bool userReuseOff = false;    // the user's NR inference reuse is Off
};

// Why the experiment does or does not set Auto; the first rule that applies wins (each one tested).
enum class PushNrAdaptReason : unsigned char {
    SwitchOff, NotLive, NoNeuralLayer, NoFrameGeneration, PresentSinkFg, NotNgx, UserChoice, PairBudget
};
constexpr PushNrAdaptReason pushNrAdaptReason(const PushNrAdaptInput& in) {
    if (!in.switchOn) return PushNrAdaptReason::SwitchOff;
    if (!in.live || in.image) return PushNrAdaptReason::NotLive;
    if (!in.neuralLayer) return PushNrAdaptReason::NoNeuralLayer;
    if (!in.frameGeneration) return PushNrAdaptReason::NoFrameGeneration;
    if (in.presentSinkFg) return PushNrAdaptReason::PresentSinkFg;
    if (!in.ngxRuntime) return PushNrAdaptReason::NotNgx;
    if (!in.userReuseOff) return PushNrAdaptReason::UserChoice;
    return PushNrAdaptReason::PairBudget;
}
constexpr bool pushNrAdaptForcesAuto(const PushNrAdaptInput& in) {
    return pushNrAdaptReason(in) == PushNrAdaptReason::PairBudget;
}
constexpr const char* pushNrAdaptReasonName(PushNrAdaptReason r) {
    switch (r) {
    case PushNrAdaptReason::SwitchOff: return "switch-off";
    case PushNrAdaptReason::NotLive: return "not-live";
    case PushNrAdaptReason::NoNeuralLayer: return "no-neural-layer";
    case PushNrAdaptReason::NoFrameGeneration: return "no-frame-generation";
    case PushNrAdaptReason::PresentSinkFg: return "present-sink-fg";
    case PushNrAdaptReason::NotNgx: return "not-ngx";
    case PushNrAdaptReason::UserChoice: return "user-choice";
    case PushNrAdaptReason::PairBudget: return "pair-budget";
    }
    return "unknown";
}

// The pairs' rate: the measured content rate when it is a usable number at or under the transport rate (duplicate
// removal keeps one picture of each repeat, so the content rate can never exceed what arrives), else the transport
// rate. 0 when neither is usable.
inline double pushNrAdaptPairFps(double transportFps, double contentFps) {
    const bool transportOk = std::isfinite(transportFps) && transportFps > 0.0;
    const bool contentOk = std::isfinite(contentFps) && contentFps >= 1.0 && (!transportOk || contentFps <= transportFps * 1.01);
    if (contentOk) return contentFps;
    return transportOk ? transportFps : 0.0;
}
// The budget handed to AutoNrReuseController::observe, in ms: 80 % of the pair interval; 0 (the controller then
// does nothing) when no rate is known.
inline double pushNrAdaptBudgetMs(double transportFps, double contentFps) {
    const double fps = pushNrAdaptPairFps(transportFps, contentFps);
    return fps > 0.0 ? kPushNrAdaptBudgetShare * 1000.0 / fps : 0.0;
}
}  // namespace veyra::engine
