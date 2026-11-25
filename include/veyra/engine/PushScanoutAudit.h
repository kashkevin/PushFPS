// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Slipstream scan-out audit (sprint 5, stream "present"; line [slipstream-scanout], VEYRA_EXP_RP_STATS=1).
//
// Why: BENCH2 showed that once the driver runs variable refresh only about one present in six gets a
// scan-out date and `replacedAtLeast` counts hundreds although every present was issued with sync
// interval 1 (which DXGI never replaces). PresentSink::sampleDisplayLatency reads the DXGI frame
// statistics once per present and infers "replaced" from `presents shown - refreshes counted`, an
// inference that only holds on a fixed refresh. This audit does not guess what the statistics do under
// variable refresh: it records what they DID (how often they failed, stood still, skipped presents, how old
// they were when read) and counts as "may have been replaced" only presents that could be replaced at all
// (sync interval 0). The figures decide the next step on real hardware; nothing here changes presentation.
//
// Portable and allocation-free; one owner thread (the presentation owner).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace veyra::engine {

// One reading of IDXGISwapChain::GetFrameStatistics, taken just before a Present.
struct PushScanoutSample {
    bool ok=false;            // the call succeeded
    bool disjoint=false;      // it failed with DXGI_ERROR_FRAME_STATISTICS_DISJOINT
    bool hasSyncTime=false;   // SyncQPCTime was not zero
    std::uint32_t presentCount=0,presentRefreshCount=0,syncRefreshCount=0;
    double statsAgeMs=-1;     // read time minus SyncQPCTime; < 0 unknown
};

class PushScanoutAudit {
public:
    static constexpr std::size_t kIssued=64;     // presents remembered until the statistics pass them
    static constexpr std::size_t kValues=1024;   // per window
    struct Summary {
        std::uint32_t presents=0;      // presents issued whose scan-out could be dated
        std::uint32_t samples=0;       // statistics readings
        std::uint32_t failed=0,disjoint=0,noSync=0;
        std::uint32_t unchanged=0;     // readings that named the same present as the reading before
        std::uint32_t named=0;         // readings that named a present of ours for the first time
        std::uint32_t exact=0;         // ... whose own vblank was the newest one (no period needed to date it)
        std::uint32_t gapRejected=0;   // ... but with refresh counters that cannot date it (behind, or over 16 apart)
        std::uint32_t passedVsync=0;   // presents the statistics skipped, issued with sync interval >= 1: shown, undated
        std::uint32_t passedSync0=0;   // presents the statistics skipped, issued with sync interval 0: may have been replaced
        std::uint32_t refreshShort=0;  // sum of (presents shown - refreshes counted) where positive: the old "replacedAtLeast"
        std::uint32_t unsettled=0;     // presents forgotten before the statistics reached them
        double stepP50=-1,stepMax=-1;              // presents covered by one change of the statistics
        double statsAgeMsP50=-1,statsAgeMsMax=-1;  // how old the statistics were when read
        double namedPct()const{return presents?100.0*double(named)/double(presents):-1.0;}
    };

    // A present went out. id: its DXGI present id (GetLastPresentCount right after it), never 0.
    void presentIssued(std::uint32_t id,bool replaceable){
        if(!id)return;
        auto& slot=issued_[next_++%kIssued];
        if(slot.used&&!slot.settled)++s_.unsettled;
        slot.id=id;slot.replaceable=replaceable;slot.used=true;slot.settled=false;
        ++s_.presents;
    }

    void sample(const PushScanoutSample& r){
        ++s_.samples;
        if(!r.ok){++s_.failed;if(r.disjoint)++s_.disjoint;based_=false;return;}   // the next reading is a new baseline
        if(!r.hasSyncTime){++s_.noSync;return;}
        if(r.statsAgeMs>=0&&ages_<kValues)age_[ages_++]=float(std::min(r.statsAgeMs,1.0e6));
        if(!based_){based_=true;lastPresent_=r.presentCount;lastRefresh_=r.presentRefreshCount;if(Slot* slot=find(r.presentCount))slot->settled=true;settleThrough(r.presentCount,false);return;}
        if(r.presentCount==lastPresent_){++s_.unchanged;return;}
        const std::uint32_t step=r.presentCount-lastPresent_;
        if(step>0x7fffffffu){lastPresent_=r.presentCount;lastRefresh_=r.presentRefreshCount;return;}   // counters restarted
        if(steps_<kValues)step_[steps_++]=float(std::min<std::uint32_t>(step,1000000u));
        const std::uint32_t refreshStep=r.presentRefreshCount-lastRefresh_;
        if(refreshStep<=0x7fffffffu&&step>refreshStep)s_.refreshShort+=step-refreshStep;
        if(Slot* slot=find(r.presentCount);slot&&!slot->settled){
            slot->settled=true;++s_.named;
            const bool behind=r.syncRefreshCount<r.presentRefreshCount;
            if(behind||r.syncRefreshCount-r.presentRefreshCount>16u)++s_.gapRejected;
            else if(r.syncRefreshCount==r.presentRefreshCount)++s_.exact;
        }
        settleThrough(r.presentCount,true);
        lastPresent_=r.presentCount;lastRefresh_=r.presentRefreshCount;
    }

    // Summarises and clears the window; presents not yet reached by the statistics stay.
    Summary take(){
        Summary out=s_;
        percentiles(step_,steps_,out.stepP50,out.stepMax);
        percentiles(age_,ages_,out.statsAgeMsP50,out.statsAgeMsMax);
        s_={};steps_=ages_=0;
        return out;
    }
    void reset(){*this={};}

private:
    struct Slot {std::uint32_t id=0;bool replaceable=false,used=false,settled=false;};
    Slot* find(std::uint32_t id){for(auto& slot:issued_)if(slot.used&&slot.id==id)return &slot;return nullptr;}
    // Every remembered present older than `shown` was passed by the statistics without being named.
    void settleThrough(std::uint32_t shown,bool count){
        for(auto& slot:issued_){
            if(!slot.used||slot.settled)continue;
            const std::uint32_t behind=shown-slot.id;
            if(behind==0u||behind>0x7fffffffu)continue;   // the named one, or issued after it
            slot.settled=true;
            if(count)++(slot.replaceable?s_.passedSync0:s_.passedVsync);
        }
    }
    static void percentiles(std::array<float,kValues>& values,std::size_t n,double& p50,double& max){
        if(!n)return;
        std::sort(values.begin(),values.begin()+static_cast<std::ptrdiff_t>(n));
        p50=double(values[(n-1)/2]);max=double(values[n-1]);
    }
    std::array<Slot,kIssued> issued_{};
    std::size_t next_=0;
    Summary s_{};
    std::array<float,kValues> step_{},age_{};
    std::size_t steps_=0,ages_=0;
    std::uint32_t lastPresent_=0,lastRefresh_=0;
    bool based_=false;
};

inline std::string formatSlipstreamScanout(const PushScanoutAudit::Summary& s,int syncInterval){
    char line[640];
    std::snprintf(line,sizeof(line),
        "presents=%u samples=%u failed=%u disjoint=%u noSync=%u unchanged=%u named=%u exact=%u gapRejected=%u "
        "passedVsync=%u passedSync0=%u refreshShort=%u unsettled=%u stepP50=%.0f stepMax=%.0f statsAgeMsP50=%.3f "
        "statsAgeMsMax=%.3f namedPct=%.1f syncInterval=%d",
        s.presents,s.samples,s.failed,s.disjoint,s.noSync,s.unchanged,s.named,s.exact,s.gapRejected,
        s.passedVsync,s.passedSync0,s.refreshShort,s.unsettled,s.stepP50,s.stepMax,s.statsAgeMsP50,
        s.statsAgeMsMax,s.namedPct(),syncInterval);
    return line;
}

} // namespace veyra::engine
