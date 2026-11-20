// SPDX-License-Identifier: GPL-3.0-only
// Included by CoreTests.cpp: the [slipstream-latency] accumulator, the [slipstream-swapchain] line
// and the VEYRA_EXP_PRESENT_TEARING rule (include/veyra/engine/PushPresentTiming.h). Portable only;
// the Windows glue (VideoPresenter / PresentSink) is unverified on Windows.
#include "veyra/engine/PushPresentTiming.h"
#include <string>
namespace present_timing_tests {
using veyra::engine::PushPresentLatency;
using veyra::engine::PushPresentTimes;
constexpr int64_t ms(double v){return int64_t(v*10000.0);}
PushPresentTimes frame(double arrivalMs){
    PushPresentTimes t;
    t.arrival=ms(1000+arrivalMs);t.decoded=t.arrival+ms(2);t.pickup=t.decoded+ms(0.5);
    t.presentCall=t.pickup+ms(1);t.presentReturn=t.presentCall+ms(0.25);
    return t;
}
bool near(double a,double b){return a>b-0.002&&a<b+0.002;}
}

TEST(present_timing_stages_and_join){
    using namespace present_timing_tests;
    PushPresentLatency l;
    CHECK(!l.due(ms(1000)));
    // 10 frames, ids 1..10; the statistics name each one 9 ms after its Present call.
    for(unsigned i=1;i<=10;++i){
        const auto t=frame(i*16.7);
        l.presented(i,t);
        if(i>1)l.scanout(i-1,frame((i-1)*16.7).presentCall+ms(9)); // one present later, as DXGI does
    }
    l.scanout(10,frame(10*16.7).presentCall+ms(9));
    l.scanout(10,frame(10*16.7).presentCall+ms(9)); // a second report of the same id is ignored
    l.scanout(77,ms(5000));                         // an id never presented is ignored
    const auto s=l.take(ms(7000));
    CHECK(s.frames==10);CHECK(s.scanoutKnown==10);
    CHECK(near(s[PushPresentLatency::ArrivalToDecoded].p50,2));
    CHECK(near(s[PushPresentLatency::DecodedToPickup].p95,0.5));
    CHECK(near(s[PushPresentLatency::PickupToPresentCall].p50,1));
    CHECK(near(s[PushPresentLatency::PresentCall].p50,0.25));
    CHECK(near(s[PushPresentLatency::PresentToScanout].p50,9));
    CHECK(near(s[PushPresentLatency::ArrivalToScanout].max,12.5));
    // Cleared after take.
    const auto empty=l.take(ms(8000));
    CHECK(empty.frames==0&&empty.scanoutKnown==0&&empty[PushPresentLatency::ArrivalToScanout].p50<0);
}

TEST(present_timing_percentiles_nearest_rank){
    using namespace present_timing_tests;
    PushPresentLatency l;
    for(unsigned i=1;i<=100;++i){
        auto t=frame(i*16.7);
        t.decoded=t.arrival+ms(double(i)/10.0); // 0.1 .. 10 ms, shuffled order does not matter
        l.presented(0,t);
    }
    const auto s=l.take(ms(2000));
    const auto& p=s[PushPresentLatency::ArrivalToDecoded];
    CHECK(p.samples==100);CHECK(near(p.p50,5.0));CHECK(near(p.p95,9.5));CHECK(near(p.p99,9.9));CHECK(near(p.max,10.0));
    CHECK(s.scanoutKnown==0);CHECK(s[PushPresentLatency::PresentToScanout].samples==0);
}

TEST(present_timing_unknown_and_reversed_stamps_are_skipped){
    using namespace present_timing_tests;
    PushPresentLatency l;
    PushPresentTimes t=frame(0);t.arrival=0;t.pickup=t.decoded-ms(1); // unknown arrival, pickup before decode
    l.presented(5,t);
    l.scanout(5,t.presentCall+ms(4));
    const auto s=l.take(ms(3000));
    CHECK(s.frames==1);
    CHECK(s[PushPresentLatency::ArrivalToDecoded].samples==0);
    CHECK(s[PushPresentLatency::DecodedToPickup].samples==0);
    CHECK(s[PushPresentLatency::ArrivalToScanout].samples==0);
    CHECK(s[PushPresentLatency::PresentToScanout].samples==1);CHECK(s.scanoutKnown==1);
}

TEST(present_timing_pending_ring_evicts_oldest_and_window_is_5s){
    using namespace present_timing_tests;
    PushPresentLatency l;
    CHECK(!l.due(ms(100)));CHECK(!l.due(ms(4000)));CHECK(l.due(ms(5100)));
    for(unsigned i=1;i<=PushPresentLatency::kPending+8;++i)l.presented(i,frame(i));
    l.scanout(1,frame(1).presentCall+ms(3)); // evicted: never joined
    l.scanout(PushPresentLatency::kPending+8,frame(PushPresentLatency::kPending+8).presentCall+ms(3));
    const auto s=l.take(ms(5100));
    CHECK(s.scanoutKnown==1);
    CHECK(!l.due(ms(6000)));CHECK(l.due(ms(10100)));
    // A scanout arriving in the next window for a frame presented in the previous one still joins.
    l.scanout(PushPresentLatency::kPending+7,frame(PushPresentLatency::kPending+7).presentCall+ms(3));
    CHECK(l.take(ms(10200)).scanoutKnown==1);
}

TEST(present_timing_capacity_is_bounded){
    using namespace present_timing_tests;
    PushPresentLatency l;
    for(unsigned i=0;i<PushPresentLatency::kCapacity+50;++i)l.presented(0,frame(i));
    const auto s=l.take(ms(1));
    CHECK(s.frames==PushPresentLatency::kCapacity+50);
    CHECK(s[PushPresentLatency::ArrivalToDecoded].samples==PushPresentLatency::kCapacity);
}

TEST(present_timing_lines_follow_the_contract){
    using namespace present_timing_tests;
    PushPresentLatency l;
    l.presented(1,frame(0));l.scanout(1,frame(0).presentCall+ms(6));
    veyra::engine::PushPresentContract c;c.syncInterval=1;c.tearing=false;c.queueLatency=1;c.waitable=true;c.compositionMode=-1;
    const auto line=veyra::engine::formatSlipstreamLatency(l.take(ms(1)),c);
    for(const char* key:{"frames=1 ","arrivalToDecodedP50=2.000","arrivalToDecodedP95=","decodedToPickupP50=","decodedToPickupP95=",
        "pickupToPresentCallP50=","pickupToPresentCallP95=","presentCallP50=0.250","presentCallP95=","presentToScanoutP50=6.000",
        "presentToScanoutP95=","arrivalToScanoutP50=9.500","arrivalToScanoutP95=","arrivalToScanoutP99=","arrivalToScanoutMax=",
        "scanoutKnown=1","syncInterval=1","tearing=0","queueLatency=1","waitable=1","flip=unknown"})
        CHECK(line.find(key)!=std::string::npos);
    CHECK(line.find(' ',line.size()-1)==std::string::npos);
    const auto empty=veyra::engine::formatSlipstreamLatency(l.take(ms(2)),c);
    CHECK(empty.find("presentToScanoutP50=-1.000")!=std::string::npos);
    CHECK(std::string(veyra::engine::pushFlipName(0))=="composed");
    CHECK(std::string(veyra::engine::pushFlipName(1))=="independent");
    CHECK(std::string(veyra::engine::pushFlipName(2))=="independent");
    CHECK(std::string(veyra::engine::pushFlipName(3))=="unknown");
    veyra::engine::PushSwapchainFacts f;f.flipModel=true;f.buffers=3;f.format=28;f.waitable=true;f.tearingSupported=true;
    f.syncInterval=1;f.flags=0x840;f.fullscreen=false;f.windowCoversMonitor=false;
    CHECK(veyra::engine::formatSlipstreamSwapchain(f)=="flip=unknown buffers=3 format=28 waitable=1 tearingSupported=1 syncInterval=1 flags=0x840 fullscreen=0 windowCoversMonitor=0");
    f.compositionMode=0;
    CHECK(veyra::engine::formatSlipstreamSwapchain(f).rfind("flip=composed ",0)==0);
    CHECK(veyra::engine::pushCoversRect(0,0,2560,1440,0,0,2560,1440));
    CHECK(!veyra::engine::pushCoversRect(0,40,2560,1440,0,0,2560,1440));
    CHECK(veyra::engine::pushCoversRect(-8,-8,2568,1448,0,0,2560,1440));
}

TEST(present_timing_tearing_experiment_rule){
    using veyra::engine::PushTearingInputs;
    PushTearingInputs in;in.switchOn=true;in.vsync=true;in.tearingSupported=true;in.vrrLevel=4;
    CHECK(veyra::engine::pushTearingExperiment(in));
    auto off=in;off.switchOn=false;CHECK(!veyra::engine::pushTearingExperiment(off));CHECK(std::string(veyra::engine::pushTearingRefusal(off))=="off");
    auto fg=in;fg.frameGeneration=true;CHECK(std::string(veyra::engine::pushTearingRefusal(fg))=="frame-generation");
    auto cap=in;cap.outputCap=true;CHECK(std::string(veyra::engine::pushTearingRefusal(cap))=="output-cap");
    auto vrr=in;vrr.vrrLevel=3;CHECK(std::string(veyra::engine::pushTearingRefusal(vrr))=="vrr-not-active");
    auto sup=in;sup.tearingSupported=false;CHECK(std::string(veyra::engine::pushTearingRefusal(sup))=="no-tearing-support");
    auto automatic=in;automatic.vsync=false;CHECK(std::string(veyra::engine::pushTearingRefusal(automatic))=="not-vsync");
    auto xess=in;xess.provider=true;CHECK(std::string(veyra::engine::pushTearingRefusal(xess))=="provider");
}
