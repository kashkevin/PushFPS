// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "sync", item 3 (Package 9: "presents dated 100 %" beside 2-3 dated presents a second): the
// [slipstream-latency] line says which share of its presents its present-to-vblank values describe.
#include "veyra/engine/PushPresentTiming.h"
#include <string>

TEST(sync_present_latency_line_says_the_share_of_presents_dated){
    using veyra::engine::PushPresentLatency;using veyra::engine::PushPresentTimes;
    PushPresentLatency l;
    // Package 9 steady state: 295 presents in 5 s, the statistics dated 8 of them, each about 0.5 ms after its
    // Present call. The P50 of 0.5 ms describes those 8 (2.7 %), not the 295.
    for(std::uint32_t i=1;i<=295;++i){
        PushPresentTimes t;t.arrival=10000000+std::int64_t(i)*168000;t.decoded=t.arrival+3000;t.pickup=t.decoded+500;
        t.presentCall=t.pickup+10000;t.presentReturn=t.presentCall+3000;
        l.presented(i,t);
        if(i%37==0)l.scanout(i,t.presentCall+5000);
    }
    veyra::engine::PushPresentContract c;
    const auto s=l.take(70000000);
    CHECK(s.frames==295&&s.scanoutKnown==7);
    const auto line=veyra::engine::formatSlipstreamLatency(s,c);
    CHECK(line.find(" scanoutKnown=7 ")!=std::string::npos);
    CHECK(line.find(" scanoutKnownPct=2.4")!=std::string::npos);
    CHECK(line.find("presentToScanoutP50=0.500")!=std::string::npos);
    // A window without a present: unknown, never 0 % or 100 %.
    const auto empty=veyra::engine::formatSlipstreamLatency(l.take(130000000),c);
    CHECK(empty.find(" scanoutKnownPct=-1.0")!=std::string::npos);
}
