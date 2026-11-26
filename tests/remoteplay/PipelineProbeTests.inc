// SPDX-License-Identifier: GPL-3.0-or-later
// The autorun step probe:<seconds> (veyra/engine/PushAutorunPlan.h) and the probe's sampling plan
// (veyra/engine/PushImageMetrics.h). Included by CoreTests.cpp directly above main().
#include "veyra/engine/PushAutorunPlan.h"
#include "veyra/engine/PushImageMetrics.h"
namespace {
namespace ppr = veyra::engine::autorun;
struct PprHookGuard { ~PprHookGuard() { ppr::registerStep("probe", nullptr); } };
}  // namespace

TEST(pipeline_probe_step_parses_like_the_other_hook_steps){
    const auto p = ppr::parsePlan("settle:10;probe:12;exit");
    CHECK(p.valid() && p.steps.size() == 3);
    CHECK(p.steps[1].kind == ppr::Kind::Hook && p.steps[1].name == "probe" && p.steps[1].seconds == 12);
    CHECK(ppr::isHookName("probe") && ppr::isHookName("sr") && !ppr::isHookName("probes"));
    // It needs its seconds, within the hook steps' range; nothing else is accepted.
    for (const char* bad : {"probe", "probe:", "probe:0", "probe:-3", "probe:601", "probe:5:6", "probe:x"}) {
        const auto q = ppr::parsePlan(bad);
        CHECK(!q.valid() && q.steps.size() == 1 && q.steps[0].name == "probe" && q.steps[0].reason == "bad-argument");
    }
    CHECK(ppr::parsePlan("probe:1").valid() && ppr::parsePlan("probe:600").valid());
    // The other steps are untouched.
    const auto others = ppr::parsePlan("sr:30;fg:15;quality");
    CHECK(others.valid() && others.steps[0].name == "sr" && others.steps[1].name == "fg" && others.steps[2].name == "quality");
    // A long probe gets the hook steps' time limit.
    CHECK(ppr::stepTimeoutMs(p.steps[1]) >= 600000);
}
TEST(pipeline_probe_takes_a_bounded_number_of_frame_pairs){
    using veyra::engine::pushPipelineProbePairs;
    CHECK(pushPipelineProbePairs(0) == 2 && pushPipelineProbePairs(1) == 2 && pushPipelineProbePairs(4) == 2 && pushPipelineProbePairs(6) == 3);
    CHECK(pushPipelineProbePairs(10) == 5 && pushPipelineProbePairs(12) == 6 && pushPipelineProbePairs(600) == 6);
    static_assert(pushPipelineProbePairs(20) == 6);
}
TEST(pipeline_probe_step_runs_through_its_registered_handler){
    PprHookGuard guard;
    ppr::Inputs in; in.presenting = true;
    // Without a handler the step fails as unsupported, as every hook step does.
    ppr::registerStep("probe", nullptr);
    ppr::Runner none(ppr::parsePlan("probe:6"));
    auto o = none.tick(in);
    bool unsupported = false;
    for (const auto& l : o.lines) unsupported = unsupported || l == "step=1 name=probe state=failed reason=unsupported";
    CHECK(unsupported);
    // With one: Start once with the step's seconds, Poll until Done.
    std::vector<int> events;
    int polls = 0;
    ppr::registerStep("probe", [&](ppr::HookEvent e, int seconds) {
        events.push_back(int(e));
        CHECK(seconds == 6);
        if (e == ppr::HookEvent::Poll && ++polls == 3) return ppr::HookResult{ppr::HookState::Done, {}};
        return ppr::HookResult{};
    });
    ppr::Runner r(ppr::parsePlan("probe:6"));
    in = {}; in.presenting = true;
    bool done = false;
    for (int i = 0; i < 8 && !done; ++i) {
        o = r.tick(in);
        for (const auto& l : o.lines) done = done || l == "step=1 name=probe state=done reason=ok";
    }
    CHECK(done);
    CHECK((events == std::vector<int>{0, 1, 1, 1}));
    // A probe that fails names its reason.
    ppr::registerStep("probe", [](ppr::HookEvent, int) { return ppr::HookResult{ppr::HookState::Failed, "no-frames"}; });
    ppr::Runner f(ppr::parsePlan("probe:6"));
    in = {}; in.presenting = true;
    o = f.tick(in);
    bool failed = false;
    for (const auto& l : o.lines) failed = failed || l.find("name=probe state=failed reason=no") != std::string::npos;
    CHECK(failed);
}
