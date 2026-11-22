// SPDX-License-Identifier: GPL-3.0-only
// Latency governor and GPU budget with SmartLink's view of the link (sprint 4, phase 2): PushControlSample's
// linkKnown / linkLimited / linkHeadroom, pushGpuOnlySample. Included by CoreTests.cpp directly above main().
// LINUX MODEL: every number is invented (PushGovernorSim.h); nothing here ran on a GPU, a PC or a PS5.
#include "veyra/engine/PushGovernorSim.h"
#include <cstdint>
#include <string>
#include <vector>
namespace governor_link_tests {
namespace gl=veyra::engine;
using Fg=gl::PushFgRequest;
constexpr gl::PushFgMethod kFlow=gl::PushFgMethod::PushFlow;
enum class Link{Unknown,Limited,Headroom,Neutral};
gl::PushControlSample at(int second,const Fg& level,double unplanned,double baseLoad,double decodeMs,Link link){
    gl::PushControlSample s;s.nowMs=static_cast<std::int64_t>(second)*1000;s.sourceFps=30;
    s.latencyP95Ms=gl::pushPlannedDelayMs(level.multiplier,30)+unplanned;s.networkJitterMs=0.3;s.lossPercent=0;
    auto& b=s.budget;b.sourceIntervalMs=1000.0/30;b.outputIntervalMs=1000.0/90;b.gpuFrameMs=baseLoad*b.sourceIntervalMs;
    b.fgMs=level.multiplier>=2?static_cast<double>(level.multiplier-1)*(2.0+static_cast<double>(level.tier)):0.0;
    b.decodeQueueDelayMs=decodeMs;b.presented=90;b.missedDeadlines=0;b.skippedPairShare=0;b.power=gl::PushPowerSource::Mains;
    s.linkKnown=link!=Link::Unknown;s.linkLimited=link==Link::Limited;s.linkHeadroom=link==Link::Headroom;
    return s;
}
struct Loop{
    gl::PushGovernorControl control;Fg user,current;std::vector<gl::PushSimRecord> trace;
    explicit Loop(Fg request):user(request),current(request){}
    gl::PushControlTick tick(const gl::PushControlSample& s){
        gl::PushSimRecord r;r.nowMs=s.nowMs;r.before=gl::PushGpuLevel{current.tier,current.multiplier};r.jitterMs=s.networkJitterMs;r.sourceFps=s.sourceFps;
        r.latencyMs=s.latencyP95Ms;r.latencyKnown=true;r.linkHeadroom=s.linkKnown&&s.linkHeadroom;
        const auto t=control.update(current,s);r.beforeIndex=t.startIndex;r.tick=t;if(t.apply)current=t.target;
        r.after=gl::PushGpuLevel{current.tier,current.multiplier};r.changed=!(r.after.multiplier==r.before.multiplier&&r.after.tier==r.before.tier);
        r.userRequest=user;trace.push_back(r);return t;
    }
    std::string violation()const{return gl::pushGovernorInvariantViolation(trace,{});}
};
// First second at which the loop changed a setting, or -1.
template<typename World> int firstChange(Link link,World world,int seconds=120){
    Loop loop(Fg{kFlow,3,2});
    for(int s=0;s<seconds;++s)if(loop.tick(world(s,loop.current,link)).apply)return s;
    return -1;
}
}

TEST(governor_link_limited_makes_no_reduction_for_what_the_link_explains){
    namespace t=governor_link_tests;
    // Decode waits of 25 ms that GPU work of 28 ms COULD explain: without a SmartLink decision (phase 1), or with
    // one that says nothing, they are answered; with "the link limits the stream" they are not, and it says so.
    const auto decode=[](int s,const t::Fg& level,t::Link link){return t::at(s,level,2,level.multiplier==3?0.6:0.3,level.multiplier==3?25.0:1.0,link);};
    CHECK(t::firstChange(t::Link::Unknown,decode)>=0);CHECK(t::firstChange(t::Link::Neutral,decode)>=0);
    CHECK(t::firstChange(t::Link::Limited,decode)<0);
    t::Loop loop(t::Fg{t::kFlow,3,2});
    for(int s=0;s<30;++s)loop.tick(decode(s,loop.current,t::Link::Limited));
    CHECK(loop.trace.back().tick.linkLimited);CHECK(loop.trace.back().tick.reason=="link-limited-no-reduction");
    CHECK(loop.trace.back().tick.budgetReason=="link-limited-gpu-only");
    CHECK(loop.violation().empty());
    // A latency excess longer than all the GPU work is not stepped (as in phase 1); one that differs by level
    // (18 ms at 3x, 2 ms at 2x, within the work) is the GPU's and is still stepped while the link is limited.
    const auto stall=[](int s,const t::Fg& level,t::Link link){return t::at(s,level,45,0.3,1,link);};
    const auto gpu=[](int s,const t::Fg& level,t::Link link){return t::at(s,level,level.multiplier==3?18.0:2.0,0.3,1,link);};
    CHECK(t::firstChange(t::Link::Limited,stall)<0);CHECK(t::firstChange(t::Link::Unknown,stall)<0);
    CHECK(t::firstChange(t::Link::Limited,gpu)>=0);
}

TEST(governor_link_limited_still_answers_an_overloaded_gpu){
    namespace t=governor_link_tests;namespace gl=t::gl;
    // Link stalls (SmartLink: limited, decode waits 40 ms) AND a GPU at 1.0 + generation: the GPU evidence stands on
    // its own, the step comes, and it is a GPU-load step.
    t::Loop loop(t::Fg{t::kFlow,3,2});
    int first=-1;gl::PushControlCause cause=gl::PushControlCause::None;
    for(int s=0;s<60;++s){
        const auto tick=loop.tick(t::at(s,loop.current,s%3==0?25.0:3.0,loop.current.multiplier==3?1.0:0.3,s%3==0?40.0:1.0,t::Link::Limited));
        if(tick.apply&&first<0){first=s;cause=tick.cause;}
    }
    CHECK(first>=0&&first<10);CHECK(cause==gl::PushControlCause::GpuLoad);
    CHECK(loop.violation().empty());
    // The standalone GPU budget (VEYRA_EXP_RP_GPU_BUDGET) is fed the same way: pushGpuOnlySample keeps GPU time.
    gl::PushGpuBudgetSample in;in.sourceIntervalMs=33.3;in.gpuFrameMs=40;in.fgMs=0;in.decodeQueueDelayMs=40;in.presented=60;in.missedDeadlines=30;in.skippedPairShare=0.9;
    const auto only=gl::pushGpuOnlySample(in);
    CHECK(only.gpuFrameMs==40);CHECK(only.decodeQueueDelayMs<0);CHECK(only.presented==0&&only.missedDeadlines==0);CHECK(only.skippedPairShare<0);
    gl::PushGpuBudget budget(gl::pushGpuBudgetLadder(gl::PushFgMethod::PushFlow,3,2));
    bool stepped=false;
    for(int s=0;s<10;++s){auto x=only;x.now100ns=static_cast<std::int64_t>(s)*10000000;stepped=budget.update(x).changed||stepped;}
    CHECK(stepped);   // GPU load 1.2: the GPU's own
    gl::PushGpuBudget calm(gl::pushGpuBudgetLadder(gl::PushFgMethod::PushFlow,3,2));
    auto quiet=in;quiet.gpuFrameMs=8;
    bool calmStepped=false;
    for(int s=0;s<10;++s){auto x=gl::pushGpuOnlySample(quiet);x.now100ns=static_cast<std::int64_t>(s)*10000000;calmStepped=calm.update(x).changed||calmStepped;}
    CHECK(!calmStepped);   // decode waits and misses withheld: nothing left to step for
}

TEST(governor_link_headroom_returns_sooner_only_with_the_gpu_healthy){
    namespace t=governor_link_tests;
    // A busy GPU (0.6 + generation) until 30 s with 25 ms decode waits at 3x (a decode-protection step), then a
    // calm GPU. With SmartLink's headroom the return comes after half a hold; without it after the full hold.
    const auto run=[](t::Link link,double calmLoad){
        t::Loop loop(t::Fg{t::kFlow,3,2});
        int down=-1,up=-1;
        for(int s=0;s<300&&up<0;++s){
            const double load=s<30?0.6:calmLoad;
            const auto tick=loop.tick(t::at(s,loop.current,2,load,s<30&&loop.current.multiplier==3?25.0:1.0,link));
            if(tick.apply&&down<0)down=s;else if(tick.apply&&down>=0)up=s;
        }
        CHECK(loop.violation().empty());
        return up;
    };
    const int unknown=run(t::Link::Unknown,0.3),headroom=run(t::Link::Headroom,0.3),busy=run(t::Link::Headroom,0.55);
    CHECK(unknown>0);CHECK(headroom>0);CHECK(headroom<unknown);CHECK(unknown-headroom>=8);
    // load 0.55 + generation is over the budget's healthy line: the headroom of the link alone does not shorten it
    CHECK(busy<0||busy>=headroom+8);
}

TEST(governor_link_unknown_is_exactly_phase_one){
    namespace t=governor_link_tests;namespace gl=t::gl;
    // One controller fed the link fields set but not known, one fed the defaults: identical decisions.
    gl::PushGovernorControl x,y;t::Fg cx{t::kFlow,3,2},cy=cx;
    for(int s=0;s<600;++s){
        const double load=s>=60&&s<200?0.95:0.3;const double unpl=s%40<6?18.0:2.0;
        auto sx=t::at(s,cx,unpl,load,s%17==0?25.0:1.0,t::Link::Unknown);sx.linkLimited=true;sx.linkHeadroom=true;
        auto sy=t::at(s,cy,unpl,load,s%17==0?25.0:1.0,t::Link::Unknown);
        const auto tx=x.update(cx,sx),ty=y.update(cy,sy);
        CHECK(tx.apply==ty.apply);CHECK(tx.reason==ty.reason);CHECK(tx.index==ty.index);
        if(tx.apply)cx=tx.target;
        if(ty.apply)cy=ty.target;
    }
}

TEST(governor_link_randomized_traces_keep_every_invariant){
    namespace gl=governor_link_tests::gl;
    unsigned changes=0;
    for(std::uint64_t seed=2000;seed<2080;++seed){
        auto trace=gl::pushSimRandomTrace(seed,600);
        gl::PushSimRandom r(seed*31u);
        bool known=false,limited=false,headroom=false;
        for(auto& w:trace.worlds){
            if(r.below(15)==0){known=r.below(4)!=0;limited=r.below(3)==0;headroom=!limited&&r.below(2)==0;}
            w.linkKnown=known;w.linkLimited=limited;w.linkHeadroom=headroom;
        }
        const auto s=gl::pushRunGovernorSim("random-link",600,[&trace](int t){return trace.worlds[static_cast<std::size_t>(t)];},seed,{},trace.request,trace.actions);
        if(!s.firstViolation.empty())throw std::runtime_error("seed "+std::to_string(seed)+": "+s.firstViolation);
        changes+=s.changes;
    }
    CHECK(changes>50);
}

TEST(governor_link_limited_wifi_stalls_with_slow_gpu_still_reduce){
    namespace gl=governor_link_tests::gl;
    // The review's Wi-Fi stalls on a slow GPU, with SmartLink saying "link-limited" all the time: the GPU's own
    // trouble is still answered; the same stalls on a healthy GPU change nothing.
    const auto run=[](double scale){
        std::vector<double> stall(7200,0);gl::PushSimRandom q(99);
        for(int t=0;t<7200;){stall[static_cast<std::size_t>(t)]=20+q.below(61);t+=2+static_cast<int>(q.below(7));}
        return gl::pushRunGovernorSim("wifi-link-limited",7200,[&stall,scale](int t){
            gl::PushSimWorld w;w.jitterMs=2.5;w.decodeStallMs=stall[static_cast<std::size_t>(t)];w.arrivalStallMs=w.decodeStallMs;w.gpuScale=scale;
            w.linkKnown=true;w.linkLimited=true;return w;},7u,{});
    };
    const auto slow=run(2.2),fine=run(1.0);
    CHECK(slow.firstViolation.empty());CHECK(fine.firstViolation.empty());
    // At least the GPU-load step (the latency the link may explain is then left alone: model, over budget less
    // than without the governor but not gone).
    CHECK(slow.changes>=1);CHECK(slow.overBudgetGovernor<slow.overBudgetNone);
    bool gpuStep=false;for(const auto& r:slow.trace)gpuStep=gpuStep||(r.changed&&r.tick.cause==gl::PushControlCause::GpuLoad);
    CHECK(gpuStep);
    CHECK(fine.changes==0);
}
