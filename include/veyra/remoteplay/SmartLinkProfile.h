// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// SmartLink profiles: Route + a few seconds of measurements -> Wired / Strong
// Wi-Fi / Unstable Wi-Fi, with hysteresis, and plain-language advice ids
// (SmartLinkTypes.h Advice; the interface turns them into translated
// sentences). Advice never changes a system setting. Portable, header-only,
// O(1) per sample, no allocation. Unverified on Windows and on a PS5.
//
// Wi-Fi is "unstable" when, over the last minute, the stream shows any of:
//   - 3 or more stall clusters (frame gap more than 25 ms beyond the interval);
//     BENCH1 (Wi-Fi, one session, software counter) had about 1 % of seconds
//     with 4+ frames missing, i.e. one every ~100 s: that link stays "strong";
//   - packet loss above 0.5 % (the library's congestion report starts to cap at 5 %);
//   - a signal below 40 % (Windows' percent scale; ~-80 dBm, where rate
//     adaptation usually drops to the lowest rates), or periodic spikes at
//     2 or more a minute (one every ~100 s, however regular, stays "strong").
// Hysteresis: "unstable" must hold for 10 s before Strong -> Unstable, and
// "strong" for 60 s before Unstable -> Strong (asymmetric: falsely calling a
// bad link good is the worse error). The profile never flaps faster than that.
#include "SmartLinkTypes.h"
#include <array>
namespace veyra::remoteplay::smartlink {

struct ProfileInput {
    Route route=Route::Unknown;
    double nowMs=0;
    unsigned spikesLastMinute=0;
    double lossRate=0;          // share, recent
    double signalPercent=-1;    // -1 unknown
    double signalTrendPerMin=0;
    Regime regime=Regime::Unknown;
    bool rateBelowRequest=false; // the console kept its rate clearly below the request
    bool raiseSuggested=false;   // headroom shown for minutes
};
struct ProfileResult {
    Profile profile=Profile::Unknown;
    std::array<Advice,2> advice{Advice::None,Advice::None};
    bool changed=false;
};

class ProfileTracker {
public:
    static constexpr double kToUnstableMs=10000,kToStrongMs=60000,kFirstVerdictMs=5000;
    static constexpr unsigned kUnstableSpikesPerMinute=3;
    static constexpr double kUnstableLoss=0.005,kWeakSignalPercent=40;

    void reset()noexcept{*this={};}
    ProfileResult update(const ProfileInput& in)noexcept{
        ProfileResult r;
        if(!started_){started_=true;start_=in.nowMs;}
        if(in.route!=route_){route_=in.route;profile_=Profile::Unknown;candidateSince_=in.nowMs;start_=in.nowMs;}
        const Profile before=profile_;
        if(route_==Route::Wired)profile_=Profile::Wired;
        else if(route_==Route::Wifi){
            const bool weak=in.signalPercent>=0&&in.signalPercent<kWeakSignalPercent;
            const bool bad=weak||in.spikesLastMinute>=kUnstableSpikesPerMinute||in.lossRate>kUnstableLoss||
                           (in.regime==Regime::PeriodicSpikes&&in.spikesLastMinute>=2)||in.regime==Regime::WeakSignal||in.regime==Regime::Interrupted;
            const Profile want=bad?Profile::UnstableWifi:Profile::StrongWifi;
            if(want!=candidate_){candidate_=want;candidateSince_=in.nowMs;}
            if(profile_==Profile::Unknown){
                if(in.nowMs-start_>=kFirstVerdictMs)profile_=candidate_;
            }else if(candidate_!=profile_){
                const double hold=candidate_==Profile::UnstableWifi?kToUnstableMs:kToStrongMs;
                if(in.nowMs-candidateSince_>=hold)profile_=candidate_;
            }
        }else profile_=Profile::Unknown;
        r.profile=profile_;r.changed=profile_!=before;
        // Advice: at most two, the most useful first.
        std::size_t n=0;
        const auto add=[&](Advice a){if(n<r.advice.size())r.advice[n++]=a;};
        switch(profile_){
        case Profile::Wired:
            if(in.lossRate>kUnstableLoss||in.rateBelowRequest)add(Advice::LowerPictureRateNextTime);
            else if(in.raiseSuggested)add(Advice::RaiseRateNextTime);
            else add(Advice::LinkIsFine);
            break;
        case Profile::StrongWifi:
            add(in.raiseSuggested?Advice::RaiseRateNextTime:Advice::LinkIsFine);
            break;
        case Profile::UnstableWifi:
            add(Advice::UseCable);
            if(in.signalPercent>=0&&(in.signalPercent<kWeakSignalPercent||in.signalTrendPerMin<-10))add(Advice::MoveCloserOrFasterBand);
            else if(in.regime==Regime::PeriodicSpikes)add(Advice::BackgroundActivity);
            else if(in.rateBelowRequest||in.lossRate>kUnstableLoss)add(Advice::LowerPictureRateNextTime);
            break;
        default:break;
        }
        return r;
    }
    Profile profile()const noexcept{return profile_;}
private:
    Route route_=Route::Unknown;
    Profile profile_=Profile::Unknown,candidate_=Profile::Unknown;
    double candidateSince_=0,start_=0;
    bool started_=false;
};
} // namespace veyra::remoteplay::smartlink
