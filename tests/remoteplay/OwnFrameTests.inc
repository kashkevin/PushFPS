// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/pipeline/PushOwnFrame.h (stream "ownfg", step 2): who makes a Lower delay pair's I (NVIDIA VFG or
// PushWarp), how that changes the pair's eligibility, and the evidence text. Values are examples.
#include "veyra/pipeline/PushOwnFrame.h"
#include <string>
#include <string_view>

namespace ownframe_tests {
using namespace veyra::pipeline;

TEST(ownframe_source_from_preference) {
    CHECK(pushOwnFrameSourceFrom(std::string_view("nvidia")) == PushOwnFrameSource::Nvidia);
    CHECK(pushOwnFrameSourceFrom(std::u16string_view(u"pushfps")) == PushOwnFrameSource::PushFps);
    CHECK(pushOwnFrameSourceFrom(std::string_view("auto")) == PushOwnFrameSource::Auto);
    CHECK(pushOwnFrameSourceFrom(std::string_view("")) == PushOwnFrameSource::Auto);           // unset
    CHECK(pushOwnFrameSourceFrom(std::string_view("PushFPS")) == PushOwnFrameSource::Auto);    // exact values only
    CHECK(std::string(pushOwnFrameSourceName(PushOwnFrameSource::PushFps)) == "pushfps");
}

TEST(ownframe_use_by_generator_and_source_only) {
    using S = PushOwnFrameSource; using G = PushOwnFrameGenerator; using U = PushOwnFrameUse;
    // VFG: NVIDIA's frame unless the user chose PushFPS (c). A refusal by VFG never brings PushWarp in (review 1, MUST 3):
    // the mode is then unavailable with that reason, as before; the function has no refusal input any more.
    CHECK(pushOwnFrameUse(S::Auto, G::Vfg, true, true) == U::None);
    CHECK(pushOwnFrameUse(S::Nvidia, G::Vfg, true, true) == U::None);
    CHECK(pushOwnFrameUse(S::PushFps, G::Vfg, true, true) == U::Chosen);
    // PushFlow: PushWarp is the only source (b); the NVIDIA choice cannot apply there
    CHECK(pushOwnFrameUse(S::Auto, G::PushFlow, true, true) == U::NotVfg);
    CHECK(pushOwnFrameUse(S::PushFps, G::PushFlow, true, true) == U::NotVfg);
    CHECK(pushOwnFrameUse(S::Nvidia, G::PushFlow, true, true) == U::None);
    // not served: DLSS path / ANR / FSR / AMD FRC
    CHECK(pushOwnFrameUse(S::PushFps, G::Other, true, true) == U::None);
    // PushWarp not ready (its self-check failed, was blocked or never ran), or the inputs missing: never
    CHECK(pushOwnFrameUse(S::PushFps, G::Vfg, false, true) == U::None);
    CHECK(pushOwnFrameUse(S::Auto, G::PushFlow, true, false) == U::None);
    CHECK(pushOwnFrameServes(G::PushFlow) && !pushOwnFrameServes(G::Vfg) && !pushOwnFrameServes(G::Other));
}

TEST(ownframe_created_only_when_chosen_or_pushflow) {
    using G = PushOwnFrameGenerator;
    CHECK(!pushOwnFrameCreate(G::Vfg, false));   // the owner's case (source NVIDIA): nothing created, nothing self-checked
    CHECK(pushOwnFrameCreate(G::Vfg, true));
    CHECK(pushOwnFrameCreate(G::PushFlow, false));
    CHECK(!pushOwnFrameCreate(G::Other, true));
}

inline PushPredictEligibility ownPair() {
    PushPredictEligibility e;
    e.requested = PushPairSchedule::OnePredicted; e.vfg = true; e.multiplier = 3; e.historyValid = true;
    e.formatSupported = true; e.passReady = true;
    return e;
}

TEST(ownframe_eligibility_replaces_only_the_generator_reasons) {
    auto e = ownPair(); e.vfgRefused = true;
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::VfgRefused);
    pushOwnFrameEligibility(e, PushOwnFrameUse::Chosen);   // the user chose PushFPS: no VFG request, so its refusal does not apply
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::Ok);
    e = ownPair(); e.vfg = false;   // PushFlow
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::NotVfg);
    pushOwnFrameEligibility(e, PushOwnFrameUse::NotVfg);
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::Ok);
    // every other reason stays
    e = ownPair(); e.vfg = false; e.multiplier = 2;
    pushOwnFrameEligibility(e, PushOwnFrameUse::NotVfg);
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::NotThreeX);
    e = ownPair(); e.historyValid = false;
    pushOwnFrameEligibility(e, PushOwnFrameUse::Chosen);
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::NoHistory);
    e = ownPair(); e.formatSupported = false;
    pushOwnFrameEligibility(e, PushOwnFrameUse::Chosen);
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::UnsupportedFormat);
    e = ownPair(); e.presentSinkFg = true;
    pushOwnFrameEligibility(e, PushOwnFrameUse::Chosen);
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::PresentSinkFg);
    // None changes nothing: NVIDIA's pair exactly as before
    e = ownPair(); e.vfgRefused = true;
    pushOwnFrameEligibility(e, PushOwnFrameUse::None);
    CHECK(pushPredictPairRefusal(e) == PushPredictRefusal::VfgRefused);
}

TEST(ownframe_evidence_text) {
    PushOwnFrameEvidence e;
    PushWarpCostWindow w;
    CHECK(pushOwnFrameTimingText(e, w).empty());   // PushWarp never created: the lines are unchanged
    e.warp = 1; e.lastUse = PushOwnFrameUse::Chosen; e.ownPairs = 12; e.vfgPairs = 3; e.measured = 2; e.holeShareSum = 0.1;
    PushWarpStageMs ms{}; ms.fill(0.5); ms[static_cast<unsigned>(PushWarpStage::Total)] = 2.0;
    w.add(1000, ms);
    const auto t = pushOwnFrameTimingText(e, w);
    CHECK(t.find(" fgSource=pushwarp ownI=chosen") == 0);
    CHECK(t.find("ownIPairs=12 vfgIPairs=3") != std::string::npos);
    CHECK(t.find("ownIHoleShare=0.050") != std::string::npos);
    CHECK(t.find("ownIGpuMs=samples:1,trust:0.50/0.50/0.50") != std::string::npos && t.find("total:2.00/2.00/2.00") != std::string::npos);
    e.lastUse = PushOwnFrameUse::None; e.warp = 2;
    const auto u = pushOwnFrameTimingText(e, w);
    CHECK(u.find(" fgSource=nvidia ownI=none") == 0 && u.find("ownIWarp=check-failed") != std::string::npos);
    // the window keeps 10 s
    w.add(1000 + PushWarpCostWindow::kWindow100ns + 1, ms);
    CHECK(w.size() == 1);
}

}  // namespace ownframe_tests
