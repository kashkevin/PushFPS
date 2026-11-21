// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Types.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
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
// the console needs the latest analog value, not the history of it. Motion
// samples are the exception: the replaced entry's samples are carried into the
// new one, older first, at most 16 (the newest are kept). The queue
// therefore grows with button edges only and an owner stall (decoder open,
// reconnect) no longer overflows it into a forced release.
//
// Expiry (what take() answers when the oldest pending state has waited longer
// than kStale100ns while the interface still polls, i.e. the owner stalled):
// Neutral, the original rule and this class's default, drops the expired
// states and answers once with a neutral state, sticks centred and everything
// released, although fresh states stand right behind. DropExpired is the rule
// of Veyra 9ff43a77 (#161, after 6ebaab48) and of PushFPS cea4552c: an expired
// state is dropped alone and the fresh ones behind it are sent in order, so a
// held button or trigger is not let go by a busy decoder. Under both rules
// neutral is still sent when the interface stopped polling, when input was
// switched off (focus or device loss) and before the latest state on overflow.
class ControllerQueue {
public:
    static constexpr std::size_t kCapacity=16;
    static constexpr HostTime kStale100ns=1000000; // 100 ms without a poll: neutral
    enum class Policy {Tick,Immediate};
    enum class Expiry {Neutral,DropExpired};
    explicit ControllerQueue(Policy policy=Policy::Immediate):policy_(policy){}
    Policy policy()const noexcept{return policy_;}
    Expiry expiry()const noexcept{return expiry_;}
    void setExpiry(Expiry expiry)noexcept{expiry_=expiry;}
    void reset(){pending_.clear();current_={};stamp_=0;overflows_=0;expired_=0;}
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
                carryMotion(pending_.back().second,state);
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
    // The state to submit now. Without a fresh poll input fails neutral. A
    // backlog older than the stale bound is dropped; under Expiry::Neutral the
    // answer to that is one neutral state, under Expiry::DropExpired the next
    // fresh state (the newest pending state carries the last poll's stamp, so
    // while the interface polls there always is one).
    ControllerState take(HostTime now){
        if(now-stamp_>kStale100ns){pending_.clear();current_={};return current_;}
        if(!pending_.empty()&&now-pending_.front().first>kStale100ns){
            while(!pending_.empty()&&now-pending_.front().first>kStale100ns){pending_.pop_front();++expired_;}
            if(expiry_==Expiry::Neutral){current_={};return current_;}
        }
        if(!pending_.empty()){current_=std::move(pending_.front().second);pending_.pop_front();}
        return current_;
    }
    bool pending()const noexcept{return !pending_.empty();}
    std::size_t depth()const noexcept{return pending_.size();}
    std::uint64_t overflows()const noexcept{return overflows_;}
    // States dropped because they had waited longer than kStale100ns.
    std::uint64_t expired()const noexcept{return expired_;}
    // Everything the console treats as an event rather than a level.
    static bool sameEdges(const ControllerState& a,const ControllerState& b)noexcept{
        return a.inputActive==b.inputActive&&a.buttons==b.buttons&&
            (a.l2>0)==(b.l2>0)&&(a.r2>0)==(b.r2>0)&&
            a.touches[0].id==b.touches[0].id&&a.touches[1].id==b.touches[1].id;
    }
private:
    // The replaced state's motion samples have not been sent: put them in front of
    // the new ones (the backend integrates orientation from every sample). Over the
    // array's capacity the oldest go first.
    static void carryMotion(const ControllerState& older,ControllerState& newer)noexcept{
        const std::size_t cap=newer.motionSamples.size();
        const std::size_t oldCount=(std::min)(std::size_t(older.motionSampleCount),cap);
        const std::size_t newCount=(std::min)(std::size_t(newer.motionSampleCount),cap);
        if(oldCount==0)return;
        const std::size_t keepOld=(std::min)(oldCount,cap-newCount);
        if(keepOld==0)return;
        std::move_backward(newer.motionSamples.begin(),newer.motionSamples.begin()+std::ptrdiff_t(newCount),newer.motionSamples.begin()+std::ptrdiff_t(keepOld+newCount));
        std::copy(older.motionSamples.begin()+std::ptrdiff_t(oldCount-keepOld),older.motionSamples.begin()+std::ptrdiff_t(oldCount),newer.motionSamples.begin());
        newer.motionSampleCount=static_cast<std::uint8_t>(keepOld+newCount);
    }
    Policy policy_;
    Expiry expiry_=Expiry::Neutral;
    std::deque<std::pair<HostTime,ControllerState>> pending_;
    ControllerState current_{};
    HostTime stamp_=0;
    std::uint64_t overflows_=0;
    std::uint64_t expired_=0;
};
} // namespace veyra::remoteplay
