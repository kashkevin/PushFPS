// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Types.h"
#include <deque>
#include <utility>
namespace veyra::remoteplay {
// Controller states on their way from the UI poll to the session owner.
// Not thread-safe: the session source calls it under its own mutex.
//
// Tick policy (the original behaviour, kept for A/B runs): every polled state
// is queued and the owner takes one per 4 ms tick, so a press waits for the
// next tick and a backlog replays old stick positions one tick at a time.
//
// Immediate policy: push() reports whether the owner has something new to
// send and the owner takes everything pending as soon as it wakes. Button,
// trigger-edge and touch-contact changes each keep their own entry, in order,
// so a tap is never merged away. A state that differs from the newest pending
// one only in sticks, trigger depth, touch position or motion replaces it:
// the console needs the latest analog value, not the history of it. The queue
// therefore grows with button edges only and an owner stall (decoder open,
// reconnect) no longer overflows it into a forced release.
class ControllerQueue {
public:
    static constexpr std::size_t kCapacity=16;
    static constexpr HostTime kStale100ns=1000000; // 100 ms without a poll: neutral
    enum class Policy {Tick,Immediate};
    explicit ControllerQueue(Policy policy=Policy::Immediate):policy_(policy){}
    Policy policy()const noexcept{return policy_;}
    void reset(){pending_.clear();current_={};stamp_=0;overflows_=0;}
    void clearPending(){pending_.clear();current_={};}
    // Returns true when a new entry was queued or the newest one changed, i.e.
    // the owner should be woken. Always refreshes the poll stamp.
    bool push(HostTime now,ControllerState state){
        stamp_=now;
        // A neutral input (focus/device loss) cancels queued actions immediately.
        if(!state.inputActive)pending_.clear();
        if(policy_==Policy::Immediate){
            if(pending_.empty()){if(state==current_)return false;}
            else if(sameEdges(pending_.back().second,state)){
                if(pending_.back().second==state){pending_.back().first=now;return false;}
                pending_.back()={now,std::move(state)};return true;
            }
        }
        if(pending_.size()>=kCapacity){
            // Release before the latest state rather than replay a stale backlog.
            pending_.clear();pending_.push_back({now,{}});++overflows_;
        }
        pending_.push_back({now,std::move(state)});
        return true;
    }
    // The state to submit now. Without a fresh poll, or with a backlog older
    // than the stale bound, input fails neutral.
    ControllerState take(HostTime now){
        if(now-stamp_>kStale100ns){pending_.clear();current_={};return current_;}
        if(!pending_.empty()&&now-pending_.front().first>kStale100ns){
            while(!pending_.empty()&&now-pending_.front().first>kStale100ns)pending_.pop_front();
            current_={};return current_;
        }
        if(!pending_.empty()){current_=std::move(pending_.front().second);pending_.pop_front();}
        return current_;
    }
    bool pending()const noexcept{return !pending_.empty();}
    std::size_t depth()const noexcept{return pending_.size();}
    std::uint64_t overflows()const noexcept{return overflows_;}
    // Everything the console treats as an event rather than a level.
    static bool sameEdges(const ControllerState& a,const ControllerState& b)noexcept{
        return a.inputActive==b.inputActive&&a.buttons==b.buttons&&
            (a.l2>0)==(b.l2>0)&&(a.r2>0)==(b.r2>0)&&
            a.touches[0].id==b.touches[0].id&&a.touches[1].id==b.touches[1].id;
    }
private:
    Policy policy_;
    std::deque<std::pair<HostTime,ControllerState>> pending_;
    ControllerState current_{};
    HostTime stamp_=0;
    std::uint64_t overflows_=0;
};
} // namespace veyra::remoteplay
