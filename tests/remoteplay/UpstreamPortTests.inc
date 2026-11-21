// SPDX-License-Identifier: GPL-3.0-only
// Included by CoreTests.cpp: fixes ported from upstream in sprint 5 (docs/UPSTREAM_REVIEW_2026-11-20.md).
// ControllerQueue::Expiry::DropExpired is the rule of Veyra 9ff43a77 (#161) and PushFPS cea4552c: a state
// that waited too long is dropped alone; fresh input behind it is never answered with a neutral state.
// TriggerFilter is Veyra 17e54e24 with 15adac8f's correction (off unless VEYRA_EXP_RP_TRIGGER_FILTER=1).
#include "veyra/remoteplay/ControllerQueue.h"
#include "veyra/remoteplay/TriggerFilter.h"
#include <algorithm>
#include <deque>
namespace upstream_port_tests {
ControllerState upHeld(std::uint32_t buttons,std::uint8_t r2=0,std::int16_t stick=0){
    ControllerState s;s.inputActive=true;s.buttons=buttons;s.r2=r2;s.rightX=stick;return s;
}
ControllerQueue upDropQueue(ControllerQueue::Policy policy=ControllerQueue::Policy::Immediate){
    ControllerQueue q(policy);q.setExpiry(ControllerQueue::Expiry::DropExpired);return q;
}
struct UpStallRun {std::uint64_t neutral=0,delivered=0,expired=0;HostTime worstAge=0;ControllerState last{};};
// 250 Hz polls of a held R2 with a moving stick and a Cross edge every 50 ms; the owner sends as
// RemotePlaySessionSource::run does (everything pending when it wakes, else the 4 ms tick) and
// does not run for `stall` (a decoder backlog).
UpStallRun upRunHeldStall(ControllerQueue::Expiry expiry,ControllerQueue::Policy policy,HostTime stall){
    constexpr HostTime ms=10000;
    ControllerQueue q(policy);q.setExpiry(expiry);UpStallRun r;
    const bool immediate=policy==ControllerQueue::Policy::Immediate;
    const HostTime stallStart=2000*ms,stallEnd=stallStart+stall,finish=stallEnd+1000*ms;
    HostTime nextTick=0;
    for(HostTime now=0;now<=finish;now+=ms){
        if(now%(4*ms)==0){
            auto s=upHeld((now/(50*ms))%2?std::uint32_t{ControllerState::Cross}:0u,255,static_cast<std::int16_t>((now/(4*ms))%30000));
            s.motionTimestampUs=static_cast<std::uint64_t>(now)+1;   // carries the poll time through the queue
            q.push(now,s);
        }
        if(now>=stallStart&&now<stallEnd)continue;
        bool due=now>=nextTick;
        for(unsigned sent=0;sent<ControllerQueue::kCapacity+1;++sent){
            if(!due&&!(immediate&&q.pending()))break;
            const auto state=q.take(now);
            due=false;nextTick=now+4*ms;
            if(!state.inputActive||state.r2!=255)++r.neutral;        // the held trigger was let go by the queue
            else{++r.delivered;r.worstAge=(std::max)(r.worstAge,now-static_cast<HostTime>(state.motionTimestampUs-1));}
            r.last=state;
        }
    }
    r.expired=q.expired();
    return r;
}
}
TEST(upstream_expiry_class_default_is_the_original_rule){
    // The class default stays the original rule, so callers that do not ask keep their behaviour;
    // RemotePlaySessionSource asks for DropExpired unless VEYRA_TEST_RP_LEGACY_INPUT_EXPIRY is set.
    ControllerQueue q;CHECK(q.expiry()==ControllerQueue::Expiry::Neutral);CHECK(q.expired()==0);
    q.setExpiry(ControllerQueue::Expiry::DropExpired);CHECK(q.expiry()==ControllerQueue::Expiry::DropExpired);
    CHECK(q.policy()==ControllerQueue::Policy::Immediate);
}
TEST(upstream_expiry_fresh_input_after_a_stale_edge_is_not_a_release){
    using namespace upstream_port_tests;
    // Veyra #161's case: an old Cross edge, then (110 ms later) Circle with a finger down, then the
    // held state. The old edge is dropped alone; the two fresh states arrive in order.
    const HostTime base=1000;
    auto old=upHeld(ControllerState::Cross);auto current=upHeld(0,255);auto pressed=current;
    pressed.buttons=ControllerState::Circle;pressed.touches[0].id=2;
    for(const auto expiry:{ControllerQueue::Expiry::DropExpired,ControllerQueue::Expiry::Neutral}){
        ControllerQueue q;q.setExpiry(expiry);
        CHECK(q.push(base,old));CHECK(q.push(base+1100000,pressed));CHECK(q.push(base+1200000,current));
        CHECK(q.depth()==3);
        if(expiry==ControllerQueue::Expiry::Neutral)CHECK(q.take(base+1200000)==ControllerState{});   // the original rule: a release first
        CHECK(q.take(base+1200000)==pressed);
        CHECK(q.take(base+1200000)==current);
        CHECK(!q.pending());CHECK(q.expired()==1);
        // The interface stopped reporting: neutral under either rule.
        CHECK(q.take(base+1200000+ControllerQueue::kStale100ns+1)==ControllerState{});
    }
}
TEST(upstream_expiry_focus_loss_and_overflow_still_release){
    using namespace upstream_port_tests;
    auto q=upDropQueue();
    q.push(10,upHeld(ControllerState::Cross,255));CHECK(q.take(10).buttons==ControllerState::Cross);
    q.push(11,upHeld(0,255));q.push(12,upHeld(ControllerState::Circle,255));
    // Focus or device loss cancels what was queued and releases at once.
    CHECK(q.push(13,ControllerState{}));CHECK(q.depth()==1);CHECK(q.take(13)==ControllerState{});
    // More edges than the queue holds: release before the latest state (the existing fail-safe, kept).
    for(int i=0;i<40;++i)q.push(20,upHeld(i%2?std::uint32_t{ControllerState::Cross}:0u));
    CHECK(q.depth()<=ControllerQueue::kCapacity);CHECK(q.overflows()>0);
    // Never polled again: neutral.
    CHECK(q.take(20+2*ControllerQueue::kStale100ns)==ControllerState{});CHECK(!q.pending());
}
TEST(upstream_expiry_slow_owner_never_pulses_a_held_trigger){
    using namespace upstream_port_tests;
    // Veyra 6ebaab48's field case: the interface polls at 125 Hz, the owner sends at 62.5 Hz.
    for(const auto policy:{ControllerQueue::Policy::Immediate,ControllerQueue::Policy::Tick}){
        auto q=upDropQueue(policy);unsigned neutral=0,lagged=0;std::size_t highWater=0;
        for(int tick=0;tick<625;++tick){
            const HostTime now=1000+HostTime(tick)*80000;
            const auto state=upHeld(0,255,static_cast<std::int16_t>(tick%100));
            q.push(now,state);highWater=(std::max)(highWater,q.depth());
            if(tick%2==0){const auto out=q.take(now);if(!out.inputActive||out.r2!=255)++neutral;if(out.rightX!=state.rightX)++lagged;}
        }
        CHECK(neutral==0);
        if(policy==ControllerQueue::Policy::Immediate){CHECK(lagged==0);CHECK(highWater==1);CHECK(q.expired()==0);}
        // The tick policy (VEYRA_TEST_RP_LEGACY_INPUT) queues every poll: it lags and overflows as
        // before, but an expired state alone no longer releases the trigger.
        else CHECK(highWater<=ControllerQueue::kCapacity);
    }
}
TEST(upstream_expiry_owner_stall_keeps_held_input_and_sends_nothing_old){
    using namespace upstream_port_tests;
    constexpr HostTime ms=10000;
    for(const HostTime stall:{150*ms,200*ms,2000*ms}){
        const auto fixed=upRunHeldStall(ControllerQueue::Expiry::DropExpired,ControllerQueue::Policy::Immediate,stall);
        const auto original=upRunHeldStall(ControllerQueue::Expiry::Neutral,ControllerQueue::Policy::Immediate,stall);
        CHECK(fixed.delivered>500&&original.delivered>500);
        // The original rule lets the held trigger go once per stall although fresh input waits.
        CHECK(original.neutral==1);
        CHECK(fixed.neutral==0);
        CHECK(fixed.expired>0&&fixed.expired==original.expired);         // the same old states are dropped
        CHECK(fixed.worstAge<=ControllerQueue::kStale100ns);              // nothing older than 100 ms reaches the console
        CHECK(original.worstAge<=ControllerQueue::kStale100ns);
        CHECK(fixed.last==original.last);                                 // both end on the pad as it is
        CHECK(fixed.delivered==original.delivered);                       // same sends, minus the one release
    }
    // A stall inside the 100 ms bound: nothing expires and the two rules are the same.
    const auto a=upRunHeldStall(ControllerQueue::Expiry::DropExpired,ControllerQueue::Policy::Immediate,80*ms);
    const auto b=upRunHeldStall(ControllerQueue::Expiry::Neutral,ControllerQueue::Policy::Immediate,80*ms);
    CHECK(a.expired==0&&b.expired==0&&a.neutral==0&&b.neutral==0&&a.delivered==b.delivered);
}
// Veyra's TriggerFilterTests.cpp (17e54e24, as changed by 15adac8f), in this file's macros. The model of Chiaki's history
// window is upstream's: each value change is one event, a packet repeats the newest 8, and 40 ms
// of packets are lost right after L2 is released while R2 keeps jittering.
namespace upstream_port_tests {
constexpr HostTime upTriggerMs(int v){return HostTime(v)*10000;}
template<class Source> bool upConsoleSeesL2Release(Source source){
    struct Event {int trigger;int value;};
    std::deque<Event> window;          // newest 8 events, as in every packet
    int consoleL2=0;std::uint8_t prevL2=0,prevR2=0;
    for(int tick=0;tick<150;++tick){   // 4 ms controller cadence, 600 ms
        const HostTime now=upTriggerMs(tick*4);
        const std::uint8_t l2=tick<50?std::uint8_t{200}:std::uint8_t{0};   // L2 released at 200 ms
        const auto r2=static_cast<std::uint8_t>(180+(tick%3)-1);            // R2 held, jittering 179..181
        const TriggerFilter::Values out=source(l2,r2,now);
        bool pushed=false;
        const auto push=[&](int t,int v){window.push_back({t,v});if(window.size()>8)window.pop_front();pushed=true;};
        if(out.l2!=prevL2)push(0,out.l2);
        if(out.r2!=prevR2)push(1,out.r2);
        prevL2=out.l2;prevR2=out.r2;
        const bool lost=tick>=50&&tick<60;   // 40 ms of lost packets
        // A packet goes out for each new event; with nothing new, the last packet is resent
        // (Chiaki's tail resend at 20/50/100/200 ms).
        const bool tailResend=!pushed&&(tick==55||tick==62||tick==75||tick==100);
        if((pushed||tailResend)&&!lost)for(const auto& e:window)if(e.trigger==0)consoleL2=e.value;
    }
    return consoleL2==0;
}
}
TEST(upstream_trigger_filter_rules){
    using namespace upstream_port_tests;
    {TriggerFilter f;const auto v=f.apply(3,252,0);CHECK(v.l2==0&&v.r2==255);}                 // ends snap to 0 and 255
    {TriggerFilter f;f.apply(100,0,0);const auto a=f.apply(102,0,upTriggerMs(4)),b=f.apply(104,0,upTriggerMs(8));
     CHECK(a.l2==100&&b.l2==104);}                                                             // below the step held, a full step sent
    {TriggerFilter f;f.apply(150,0,0);auto v=f.apply(0,0,upTriggerMs(4));CHECK(v.l2==0);         // a release is sent at once from any value
     v=f.apply(40,0,upTriggerMs(8));CHECK(v.l2==40);}                                            // and so is a first press
    {TriggerFilter f;f.apply(200,120,0);f.apply(0,120,upTriggerMs(4));                           // L2 released
     const auto heldValue=f.apply(0,125,upTriggerMs(40));CHECK(heldValue.r2==120);               // nearby noise is held after a release
     const auto changed=f.apply(0,160,upTriggerMs(50));CHECK(changed.r2==160);                   // real analog travel continues
     const auto full=f.apply(0,255,upTriggerMs(60));CHECK(full.r2==255);                         // reaching 255 goes through during the hold
     const auto after=f.apply(0,160,upTriggerMs(200));CHECK(after.r2==160);}                     // and it catches up after the hold
    {TriggerFilter f;f.apply(0,0,0);f.apply(30,0,upTriggerMs(4));const auto ramp=f.apply(90,0,upTriggerMs(8));CHECK(ramp.l2==90);} // the trigger being pressed is never held
    {TriggerFilter f;f.apply(200,200,0);f.reset();const auto v=f.apply(100,0,upTriggerMs(4));CHECK(v.l2==100&&v.r2==0);}        // reset starts from released triggers
}
TEST(upstream_trigger_filter_keeps_the_release_inside_the_history_window){
    using namespace upstream_port_tests;
    const auto raw=[](std::uint8_t l2,std::uint8_t r2,HostTime){return TriggerFilter::Values{l2,r2};};
    TriggerFilter f;
    const auto filtered=[&f](std::uint8_t l2,std::uint8_t r2,HostTime now){return f.apply(l2,r2,now);};
    CHECK(!upConsoleSeesL2Release(raw));        // the model reproduces the stuck L2 without the filter
    CHECK(upConsoleSeesL2Release(filtered));    // with it the console sees the release
}
TEST(upstream_trigger_filter_never_delays_an_edge_or_real_travel){
    using namespace upstream_port_tests;
    // What the filter costs. L2 is pulled and let go every 160 ms while R2 is held and moved in steps of 8
    // (2 * kStep): every value of both triggers is sent in the poll it is read, also inside the 120 ms after
    // each L2 release.
    {TriggerFilter f;std::uint64_t late=0;
     for(int tick=0;tick<5000;++tick){
         const std::uint8_t l2=(tick/40)%2?std::uint8_t{255}:std::uint8_t{0};
         const auto r2=static_cast<std::uint8_t>(60+((tick/7)%20)*8);
         const auto out=f.apply(l2,r2,upTriggerMs(tick*4));
         if(out.l2!=l2||out.r2!=r2)++late;
     }
     CHECK(late==0);}
    // A smaller movement is not sent until it has grown: the console's value stays within kStep - 1 of the
    // trigger, or 2 * kStep - 1 during the window after the other trigger's release, and never longer than
    // that window for a movement of kStep or more.
    {TriggerFilter f;int worstError=0,worstErrorOutside=0;HostTime worstWait=0,pendingSince=-1;
     std::uint8_t r2=100;std::uint32_t seed=12345;
     for(int tick=0;tick<20000;++tick){
         const HostTime now=upTriggerMs(tick*4);
         const std::uint8_t l2=(tick/40)%2?std::uint8_t{200}:std::uint8_t{0};
         seed=seed*1664525u+1013904223u;
         const int move=int((seed>>24)%7)-3;                                       // R2 wanders by -3..3 a poll
         r2=static_cast<std::uint8_t>((std::min)(240,(std::max)(20,int(r2)+move)));
         const auto out=f.apply(l2,r2,now);
         const int error=std::abs(int(out.r2)-int(r2));
         const bool window=(tick/40)%2==0&&tick>=40&&tick%40<30;                    // 120 ms after L2 went to 0
         worstError=(std::max)(worstError,error);
         if(!window)worstErrorOutside=(std::max)(worstErrorOutside,error);
         if(error>=TriggerFilter::kStep){if(pendingSince<0)pendingSince=now;worstWait=(std::max)(worstWait,now-pendingSince);}
         else pendingSince=-1;
         CHECK(out.l2==l2);
     }
     CHECK(worstErrorOutside<=TriggerFilter::kStep-1);
     CHECK(worstError<=2*TriggerFilter::kStep-1);
     CHECK(worstWait<=TriggerFilter::kEdgeHold);}
}
