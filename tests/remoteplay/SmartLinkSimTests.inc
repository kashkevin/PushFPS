// SPDX-License-Identifier: GPL-3.0-only
// SmartLink acceptance checks in the Linux simulator (veyra/remoteplay/SmartLinkSim.h): model results, not measurements.
// Included by CoreTests.cpp directly above main(). One seed per scenario to keep the sanitizer builds short;
// tools/smartlink_sim runs three.
#include "veyra/remoteplay/SmartLinkSim.h"
#include "veyra/remoteplay/SmartLinkTrace.h"
namespace smartlink_sim_tests {
namespace sl=veyra::remoteplay::smartlink;namespace sim=sl::sim;
inline bool smart(sim::PolicyKind k){return k==sim::PolicyKind::SmartLink||k==sim::PolicyKind::SmartLinkLan||k==sim::PolicyKind::SmartLinkLearned;}
inline const sim::Scenario& scenario(const char* name){
    static const auto all=sim::scenarios();
    for(const auto& c:all)if(std::string_view(c.name)==name)return c;
    throw std::runtime_error(std::string("no scenario ")+name);
}
}

TEST(smartlink_sim_wired_clean_adds_zero_delay_and_makes_zero_changes){
    namespace t=smartlink_sim_tests;namespace sim=t::sim;
    const auto& c=t::scenario("wired_clean");
    const auto path=sim::makePath(c,1);
    const auto base=sim::run(c,path,sim::PolicyKind::Rc4Default,{});
    for(const auto& p:sim::kPolicies){
        if(!t::smart(p.kind))continue;
        auto r=sim::run(c,path,p.kind,{});
        CHECK(r.metrics.settingChanges==0);
        CHECK(r.metrics.delivered==base.metrics.delivered&&r.metrics.givenUp==0);
        float worst=0;for(float a:r.metrics.added)worst=(std::max)(worst,a);
        CHECK(worst==0.0f);
        CHECK(r.metrics.audioMeanMs==base.metrics.audioMeanMs);
        CHECK(r.metrics.leaveAfterS==0.0);
    }
}

TEST(smartlink_sim_every_scenario_bounded_changes_and_back_to_leave){
    namespace t=smartlink_sim_tests;namespace sim=t::sim;namespace sl=t::sl;
    for(const auto& c:sim::scenarios()){
        const auto path=sim::makePath(c,1);
        const auto base=sim::run(c,path,sim::PolicyKind::Rc4Default,{});
        auto bAdded=base.metrics.added;const double bp95=sim::detail::percentile(bAdded,0.95);
        for(auto kind:{sim::PolicyKind::SmartLink,sim::PolicyKind::SmartLinkLan}){
            auto r=sim::run(c,path,kind,{});
            CHECK(r.metrics.maxChangesPerMinute<=static_cast<double>(sl::Engine::kReorderChangesPerMinute+sl::Engine::kAudioChangesPerMinute));
            CHECK(r.metrics.leaveAfterS>=0&&r.metrics.leaveAfterS<=sl::Engine::kReturnBoundMs/1000.0);
            // Never: more added delay than slipstream_rc4 by more than the largest step AND fewer frames.
            const double p95=sim::detail::percentile(r.metrics.added,0.95);
            CHECK(!(p95>bp95+16.0&&r.metrics.delivered<base.metrics.delivered));
        }
    }
}

TEST(smartlink_sim_governor_case_is_network_limited){
    namespace t=smartlink_sim_tests;namespace sim=t::sim;
    const auto& c=t::scenario("governor_case");
    const auto r=sim::run(c,sim::makePath(c,3),sim::PolicyKind::Rc4Default,{true,3});
    std::size_t in=0,limited=0;double jitter=0;std::size_t n=0;
    for(std::size_t i=0;i<r.windows.size();++i){
        const double at=static_cast<double>(r.windows[i].sample.endMs)/1000.0-1.0;
        if(at<c.fromS+10||at>c.untilS)continue;
        ++in;limited+=r.decisions[i].networkLimited;jitter+=r.windows[i].features[1]*20.0;++n; // features[1] = mean excess / 20
    }
    CHECK(jitter/static_cast<double>(n)<3.0);         // mean jitter under 3 ms
    CHECK(limited*100>=in*90);                        // and still network-limited
    // A clean wired link never is.
    const auto& w=t::scenario("wired_clean");
    const auto clean=sim::run(w,sim::makePath(w,3),sim::PolicyKind::Rc4Default,{true,3});
    for(const auto& d:clean.decisions)CHECK(!d.networkLimited);
}

TEST(smartlink_sim_trace_replay_gives_the_same_decisions){
    namespace t=smartlink_sim_tests;namespace sim=t::sim;namespace sl=t::sl;
    const auto& c=t::scenario("wifi_random_spikes");
    const auto r=sim::run(c,sim::makePath(c,2),sim::PolicyKind::Rc4Default,{true,2});
    sl::Config cfg;cfg.configuredReorderMs=16;cfg.audioFloorMs=sim::kAudioFloorMs;
    sl::Engine e(cfg);
    std::array<char,sl::trace::kMaxLine> line{};
    for(std::size_t i=0;i<r.windows.size();++i){
        const auto n=sl::trace::formatTraceLine(r.windows[i].sample,line.data(),line.size());
        sl::Sample s;CHECK(n>0&&sl::trace::parseTraceLine(std::string_view(line.data(),n),s));
        const auto d=e.update(s);const auto& o=r.decisions[i];
        CHECK(d.reorderWindowMs==o.reorderWindowMs&&d.audioTargetMs==o.audioTargetMs&&d.networkLimited==o.networkLimited);
        CHECK(std::abs(d.stallRisk-o.stallRisk)<0.02); // the CSV keeps 3 decimals
    }
}
