// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// SmartLink on the session owner (VEYRA_EXP_RP_SMARTLINK=observe|apply): the
// sample feed (SmartLinkFeed.h) -> the engine (SmartLinkController.h) every
// 200 ms window, what to publish, what to log (at most every 5 s), and in apply
// mode the reorder wait to hand to the library. Portable, header-only, owner
// thread only, no allocation. The session source creates one only when the
// switch is set: with it unset nothing of this exists.
//
// The reorder wait in apply mode: Decision::reorderWindowMs (2/4/8/16; 0 =
// leave = the wait the session was configured with) goes to the library's live
// wait (chiaki_veyra_transport_set_av_reorder_timeout_live, the mechanism of
// VEYRA_EXP_RP_REORDER_ADAPTIVE). An explicit VEYRA_EXP_RP_REORDER_US wins:
// SmartLink then never changes the wait, and says so once. With SmartLink in
// apply mode the adaptive reorder controller (ReorderWindow.h) is not run: one
// owner of the wait at a time.
#include "SmartLinkController.h"
#include "SmartLinkFeed.h"
#include "SmartLinkTypes.h"
#include <cstdint>
#include <optional>
namespace veyra::remoteplay::smartlink {
class Runner {
public:
    static constexpr std::int64_t kLogEveryMs=5000;
    // configuredReorderUs: the session's wait (TransportTuning::effectiveReorderUs()).
    // explicitReorder: VEYRA_EXP_RP_REORDER_US was set and accepted.
    Runner(Mode runMode,ModelKind model,std::uint32_t configuredReorderUs,bool explicitReorder)noexcept
        :mode_(runMode),explicit_(explicitReorder),configuredUs_(configuredReorderUs),
         engine_(Config{static_cast<double>(configuredReorderUs)/1000.0,2,20,model}){}
    Mode mode()const noexcept{return mode_;}
    bool appliesReorder()const noexcept{return mode_==Mode::Apply&&!explicit_;}
    void frameArrived(std::int64_t arrivalMs)noexcept{feed_.observeFrameArrival(arrivalMs);}
    // The session's console units, shared with the health line (SampleFeed::shareUnits).
    void shareUnits(ConsoleUnits* units)noexcept{feed_.shareUnits(units);}
    // A new connection attempt: the library's counters restart; the engine and the
    // wait reached so far are kept (a reconnect is not evidence of a better link).
    // Returns the wait to set on the new connection in apply mode (0 = none).
    std::uint32_t newConnection()noexcept{feed_.newConnection();return appliesReorder()&&appliedUs_!=0?appliedUs_:0u;}
    struct Step {
        std::optional<Sample> sample;  // a window ended (for the trace)
        bool decided=false;            // decision is new: publish it
        Decision decision{};
        std::uint32_t setReorderUs=0;  // apply mode: pass to the library now (0 = nothing to do)
        bool log=false;                // write toLogLine(decision, mode) now
        bool explicitWins=false;       // write once: the explicit wait wins over SmartLink
    };
    Step step(const FeedInput& in)noexcept{
        Step out;
        out.sample=feed_.poll(in);
        if(!out.sample)return out;
        out.decision=engine_.update(*out.sample);out.decided=true;last_=out.decision;
        if(mode_==Mode::Apply){
            if(explicit_){if(!explicitLogged_){explicitLogged_=true;out.explicitWins=true;}}
            else{
                const std::uint32_t wanted=out.decision.reorderWindowMs?out.decision.reorderWindowMs*1000u:configuredUs_;
                const std::uint32_t current=appliedUs_?appliedUs_:configuredUs_;
                if(wanted!=0&&wanted!=current){out.setReorderUs=wanted;appliedUs_=wanted==configuredUs_?0u:wanted;}
            }
        }
        if(lastLogMs_<0||in.nowMs<lastLogMs_||in.nowMs-lastLogMs_>=kLogEveryMs||out.setReorderUs){out.log=true;lastLogMs_=in.nowMs;}
        return out;
    }
    const Decision& last()const noexcept{return last_;}
    std::uint32_t appliedReorderUs()const noexcept{return appliedUs_;} // 0 = the configured wait
private:
    Mode mode_;bool explicit_;std::uint32_t configuredUs_;
    Engine engine_;SampleFeed feed_{};Decision last_{};
    std::uint32_t appliedUs_=0;std::int64_t lastLogMs_=-1;bool explicitLogged_=false;
};
} // namespace veyra::remoteplay::smartlink
