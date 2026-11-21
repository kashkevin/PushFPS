// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experimental (VEYRA_EXP_RP_KBM): the report rate of the mouse in use, measured from when Raw
// Input reports are read, and what it means for the pad poll and the smoothing window.
// Portable and unit-tested; the Win32 capture passes the steady clock and the number of reports
// with movement each time it reads Raw Input (include/veyra/ui/RemotePlayKeyboardMouse.h).
//
// Raw Input carries no time per report. Reports are counted in 100 ms bins of the reading time
// instead. A mouse sends a report only when it moved, so only bins inside a continuous movement
// count (a bin with reports between two others with reports: the first and last bin of a movement
// are partial), and a slow hand gives fewer reports than the mouse can send: the rate is the
// median of the five busiest such bins of the last 64, snapped to the nearest standard class
// (125, 250, 500, 1000, 2000, 4000, 8000 Hz) on a log scale. A brisk movement of a second or two
// settles it. Reads that lag behind (the interface thread was busy) move reports between bins but
// do not change their sum, which the median of several bins absorbs.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace veyra::remoteplay {

inline constexpr std::array<int,7> kKbmMouseRateClasses{125,250,500,1000,2000,4000,8000};

// What a report rate means for the pad.
struct KbmAccumulation {
    double reportIntervalMs=0;   // one report every this long while the hand moves
    int shortestUsefulPollMs=1;  // a poll shorter than this finds no new report some of the time (wasted work)
    double windowFloorMs=0;      // smoothing window that always holds a report: two report intervals (nothing read as a stop)
    bool pollWastes=false;       // the poll in use is shorter than shortestUsefulPollMs
    bool floorRaises=false;      // the floor is longer than both the profile's window and one poll: it changes the output
};
inline KbmAccumulation kbmAccumulationFor(int rateHz,int pollMs,double profileWindowMs)noexcept{
    KbmAccumulation out;
    if(rateHz<=0)return out;
    out.reportIntervalMs=1000.0/rateHz;
    out.shortestUsefulPollMs=std::clamp(int(std::ceil(out.reportIntervalMs-1e-9)),1,8);
    out.windowFloorMs=std::min(50.0,2*out.reportIntervalMs);
    out.pollWastes=pollMs<out.shortestUsefulPollMs;
    out.floorRaises=out.windowFloorMs>std::max(profileWindowMs,double(pollMs))+1e-9;
    return out;
}

class KbmMouseRate {
public:
    static constexpr std::uint64_t kBinUs=100000;
    static constexpr std::size_t kKept=64,kTop=5;
    // `reports`: reports with movement read at `nowUs` (a steady clock in microseconds).
    void add(std::uint64_t nowUs,unsigned reports)noexcept{
        if(reports==0)return;
        const std::uint64_t bin=nowUs/kBinUs;
        if(!started_){started_=true;bin_=bin;}
        if(bin<bin_)return;                         // a clock going backwards: ignore
        while(bin_<bin)close();
        count_+=reports;
    }
    // A standard class once at least kTop full bins were seen, else 0.
    int classHz()const noexcept{return class_;}
    double measuredPerSecond()const noexcept{return measured_;}
    std::size_t busyBins()const noexcept{return kept_;}
    // True once after the class changed.
    bool takeChange()noexcept{const bool c=changed_;changed_=false;return c;}
    static int snap(double perSecond)noexcept{
        if(!(perSecond>0))return 0;
        int best=kKbmMouseRateClasses[0];double distance=1e9;
        for(const int c:kKbmMouseRateClasses){
            const double d=std::abs(std::log2(perSecond/c));
            if(d<distance){distance=d;best=c;}
        }
        return best;
    }
private:
    // The current bin ends; the one before it is judged now that both neighbours are known.
    void close()noexcept{
        if(prev_>0&&prevPrev_>0&&count_>0)keep(prev_);
        prevPrev_=prev_;prev_=count_;count_=0;++bin_;
    }
    void keep(unsigned count)noexcept{
        bins_[next_]=double(count)*1e6/double(kBinUs);
        next_=(next_+1)%kKept;kept_=std::min(kKept,kept_+1);
        if(kept_<kTop)return;
        std::array<double,kKept> sorted{};
        std::copy(bins_.begin(),bins_.begin()+std::ptrdiff_t(kept_),sorted.begin());
        std::sort(sorted.begin(),sorted.begin()+std::ptrdiff_t(kept_),[](double a,double b){return a>b;});
        measured_=sorted[kTop/2];
        const int c=snap(measured_);
        if(c!=class_){class_=c;changed_=true;}
    }
    std::array<double,kKept> bins_{};
    std::size_t next_=0,kept_=0;
    std::uint64_t bin_=0;
    unsigned count_=0,prev_=0,prevPrev_=0;
    bool started_=false,changed_=false;
    int class_=0;
    double measured_=0;
};
} // namespace veyra::remoteplay
