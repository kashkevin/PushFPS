// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// include/veyra/engine/PushScanoutAudit.h: what the DXGI frame statistics did, per 5 s window. The statistics
// sequences below are made up to exercise the bookkeeping; how DXGI really behaves under variable refresh is
// exactly what the line is there to record on the owner's PC.
#include "veyra/engine/PushScanoutAudit.h"
#include <string>
namespace scanout_audit_tests {
inline veyra::engine::PushScanoutSample reading(std::uint32_t present,std::uint32_t presentRefresh,std::uint32_t syncRefresh,double ageMs=1.0){
    veyra::engine::PushScanoutSample r;r.ok=true;r.hasSyncTime=true;r.presentCount=present;r.presentRefreshCount=presentRefresh;r.syncRefreshCount=syncRefresh;r.statsAgeMs=ageMs;return r;
}
}

TEST(scanout_audit_fixed_refresh_names_every_present){
    using namespace scanout_audit_tests;
    veyra::engine::PushScanoutAudit audit;
    // 60 presents a second on a 180 Hz panel, V-sync: before each Present the statistics name the previous one.
    for(std::uint32_t i=1;i<=300;++i){
        if(i>1)audit.sample(reading(i-1,(i-1)*3,(i-1)*3+2,11.0));
        audit.presentIssued(i,false);
    }
    const auto s=audit.take();
    CHECK(s.presents==300&&s.samples==299&&s.failed==0&&s.unchanged==0);
    CHECK(s.named==298&&s.exact==0&&s.gapRejected==0);   // the first reading is the baseline
    CHECK(s.passedVsync==0&&s.passedSync0==0&&s.refreshShort==0&&s.unsettled==0);
    CHECK(s.stepP50==1&&s.stepMax==1&&s.statsAgeMsP50==11&&s.namedPct()>99);
}

TEST(scanout_audit_sparse_statistics_are_undated_presents_not_replaced_ones){
    using namespace scanout_audit_tests;
    veyra::engine::PushScanoutAudit audit;
    // The BENCH2 shape: the statistics move only every sixth present and their refresh counter advances by
    // less than the presents shown. With sync interval 1 nothing can have been replaced.
    std::uint32_t shown=0,refresh=0;
    for(std::uint32_t i=1;i<=301;++i){
        if(i>1&&(i-1)%6==0){shown=i-1;refresh+=2;}
        if(i>1)audit.sample(reading(shown,refresh,refresh,i%6==1?0.5:40.0));
        audit.presentIssued(i,false);
    }
    const auto s=audit.take();
    CHECK(s.presents==301&&s.samples==300);
    CHECK(s.named==50&&s.exact==50);                      // one present in six gets a date
    CHECK(s.passedVsync==50*5&&s.passedSync0==0);         // shown, only not dated
    CHECK(s.refreshShort==50*4);                          // what the old counter calls "replaced"
    CHECK(s.unchanged==249&&s.stepP50==6&&s.stepMax==6);
    CHECK(s.namedPct()>16&&s.namedPct()<17&&s.statsAgeMsMax==40);
}

TEST(scanout_audit_sync_interval_zero_presents_passed_over_may_have_been_replaced){
    using namespace scanout_audit_tests;
    veyra::engine::PushScanoutAudit audit;
    // Present(0): three presents inside one refresh, the statistics name only the last of each three.
    audit.presentIssued(1,true);audit.sample(reading(1,10,10));
    for(std::uint32_t i=2;i<=31;++i){
        audit.presentIssued(i,true);
        if((i-1)%3==0)audit.sample(reading(i,10+(i-1)/3,10+(i-1)/3));
    }
    const auto s=audit.take();
    CHECK(s.named==10&&s.passedSync0==20&&s.passedVsync==0&&s.refreshShort==20);
}

TEST(scanout_audit_failures_restart_the_baseline_and_old_presents_are_counted_once){
    using namespace scanout_audit_tests;
    veyra::engine::PushScanoutAudit audit;
    veyra::engine::PushScanoutSample bad;bad.disjoint=true;
    veyra::engine::PushScanoutSample zero;zero.ok=true;
    audit.presentIssued(1,false);audit.sample(reading(1,5,5));
    audit.presentIssued(2,false);audit.sample(bad);
    audit.presentIssued(3,false);audit.sample(zero);
    audit.presentIssued(4,false);audit.sample(reading(3,90,91));   // new baseline: names nothing, counts nothing
    audit.presentIssued(5,false);audit.sample(reading(4,93,120));  // refresh counters too far apart to date it
    audit.presentIssued(6,false);audit.sample(reading(4,93,121));
    audit.presentIssued(0,false);                                  // an undatable present is not remembered
    const auto s=audit.take();
    CHECK(s.presents==6&&s.samples==6&&s.failed==1&&s.disjoint==1&&s.noSync==1&&s.unchanged==1);
    CHECK(s.named==1&&s.gapRejected==1&&s.exact==0&&s.passedVsync==0&&s.refreshShort==0);
    // More presents than the audit remembers without the statistics ever moving: reported, not invented.
    for(std::uint32_t i=7;i<7+80;++i)audit.presentIssued(i,false);
    const auto later=audit.take();
    CHECK(later.unsettled==80-64+2&&later.named==0&&later.stepP50==-1&&later.namedPct()==0);
    // A restarted counter (new swap chain) is a baseline, not four thousand million presents.
    audit.sample(reading(2,1,1));
    const auto restarted=audit.take();
    CHECK(restarted.named==0&&restarted.passedVsync==0&&restarted.refreshShort==0);
}

TEST(scanout_audit_line_has_every_field_once){
    veyra::engine::PushScanoutAudit audit;
    const std::string line=veyra::engine::formatSlipstreamScanout(audit.take(),1);
    for(const char* key:{"presents=0 ","samples=0 ","failed=0 ","disjoint=0 ","noSync=0 ","unchanged=0 ","named=0 ","exact=0 ","gapRejected=0 ",
            "passedVsync=0 ","passedSync0=0 ","refreshShort=0 ","unsettled=0 ","stepP50=-1 ","stepMax=-1 ","statsAgeMsP50=-1.000 ","statsAgeMsMax=-1.000 ","namedPct=-1.0 ","syncInterval=1"}){
        const auto at=line.find(key);
        CHECK(at!=std::string::npos&&line.find(key,at+1)==std::string::npos);
    }
}
