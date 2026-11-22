// SPDX-License-Identifier: GPL-3.0-only
// SmartLink predictor, controller, profiles and trace format (veyra/remoteplay/SmartLink*.h).
// Included by CoreTests.cpp directly above main(). Every number below is made up, not a measurement.
#include "veyra/remoteplay/SmartLinkController.h"
#include "veyra/remoteplay/SmartLinkTrace.h"
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
namespace smartlink_tests {
namespace sl=veyra::remoteplay::smartlink;
// A clean 200 ms window of a 60 fps stream at time `ms`.
inline sl::Sample clean(std::int64_t ms,sl::Route route=sl::Route::Wired){
    sl::Sample s;s.endMs=ms;s.windowMs=200;s.route=route;
    s.packetsReceived=324;s.packetsLost=0;s.packetsReordered=0;s.packetsLate=0;s.packetGapMeanMs=0.6;s.packetGapMaxMs=3;
    s.framesArrived=12;s.framesIncomplete=0;s.framesFecRecovered=0;s.frameGapMaxMs=17.5;s.frameGapExcessMs=0.8;s.lateRescueMs=0;
    s.rttMs=4;s.consoleTargetKbps=15000;s.measuredKbps=9000;s.requestedKbps=15000;s.videoQueueDepth=0;s.audioBufferedMs=20;s.audioUnderruns=0;
    if(route==sl::Route::Wifi){s.wifiSignalPercent=80;s.wifiRssiDbm=-55;s.wifiLinkMbps=866;s.wifiBandGhz=5;}
    return s;
}
inline std::uint64_t mix(std::uint64_t x){x+=0x9E3779B97F4A7C15ull;x=(x^(x>>30))*0xBF58476D1CE4E5B9ull;x=(x^(x>>27))*0x94D049BB133111EBull;return x^(x>>31);}
}

TEST(smartlink_switch_values){
    namespace sl=smartlink_tests::sl;
    CHECK(sl::parseMode(std::nullopt)==sl::Mode::Off);CHECK(sl::parseMode(std::string_view(""))==sl::Mode::Off);
    CHECK(sl::parseMode(std::string_view("0"))==sl::Mode::Off);CHECK(sl::parseMode(std::string_view("OFF"))==sl::Mode::Off);
    CHECK(sl::parseMode(std::string_view("1"))==sl::Mode::Observe);CHECK(sl::parseMode(std::string_view("observe"))==sl::Mode::Observe);
    CHECK(sl::parseMode(std::string_view("Apply"))==sl::Mode::Apply);CHECK(sl::parseMode(std::string_view("applyx"))==sl::Mode::Observe);
    CHECK(sl::parseModel(std::nullopt)==sl::ModelKind::Statistics);CHECK(sl::parseModel(std::string_view("stat"))==sl::ModelKind::Statistics);
    CHECK(sl::parseModel(std::string_view("Learned"))==sl::ModelKind::Statistics);CHECK(sl::parseModel(std::string_view("learned"))==sl::ModelKind::Learned);
}

TEST(smartlink_log_line_has_the_brief_fields_in_order){
    namespace sl=smartlink_tests::sl;
    sl::Decision d;d.profile=sl::Profile::UnstableWifi;d.regime=sl::Regime::PeriodicSpikes;d.stallRisk=0.456;d.rttMs=12.25;d.rttMaxMs=79;
    d.targetKbps=15000;d.measuredKbps=4012.4;d.gapP95Ms=25;d.gapMaxMs=61.5;d.late=3;d.lost=0;d.reorderWindowMs=8;d.audioTargetMs=40;
    d.networkLimited=true;d.signalPercent=std::numeric_limits<double>::quiet_NaN();d.reasons=5;
    const auto line=sl::toLogLine(d,sl::Mode::Apply);
    CHECK(line.view()=="[slipstream-smartlink] profile=unstable-wifi regime=periodic-spikes risk=0.46 rttMs=12.2 rttMaxMs=79.0 targetKbps=15000 "
                       "measuredKbps=4012 gapP95Ms=25.0 gapMaxMs=61.5 late=3 lost=0 reorderMs=8 audioTargetMs=40 netLimited=1 headroom=0 signal=-1 mode=apply reasons=5");
    // Huge and negative values never overflow the fixed buffer.
    d.rttMs=1e300;d.gapMaxMs=-5;d.late=std::numeric_limits<std::int64_t>::max();
    const auto big=sl::toLogLine(d,sl::Mode::Observe);
    CHECK(big.size<big.text.size());CHECK(big.view().find("gapMaxMs=-1")!=std::string_view::npos);CHECK(big.view().find("mode=observe")!=std::string_view::npos);
}

TEST(smartlink_wired_clean_leaves_everything_for_an_hour){
    namespace t=smartlink_tests;namespace sl=t::sl;
    sl::Engine e;
    std::uint64_t changes=0;
    for(std::int64_t i=1;i<=18000;++i){
        auto s=t::clean(i*200);s.frameGapExcessMs=static_cast<double>(t::mix(static_cast<std::uint64_t>(i))%3000)/1000.0; // 0-3 ms of normal noise
        const auto d=e.update(s);
        CHECK(d.reorderWindowMs==0&&d.audioTargetMs==0&&!d.networkLimited);
        CHECK(d.stallRisk<0.5);
        if(d.changed)++changes;
        if(i>100)CHECK(d.profile==sl::Profile::Wired);
    }
    CHECK(changes==0);CHECK(e.changes()==0);
}

TEST(smartlink_widens_within_one_window_and_never_beyond_the_evidence){
    namespace t=smartlink_tests;namespace sl=t::sl;
    sl::Config c;c.configuredReorderMs=2;
    sl::Engine e(c);
    for(int i=1;i<=50;++i)CHECK(e.update(t::clean(i*200)).reorderWindowMs==0);
    auto s=t::clean(51*200);s.lateRescueMs=1.5;s.framesIncomplete=1;s.packetsLate=2;s.reorderWaitMs=2;
    auto d=e.update(s);
    CHECK(d.reorderWindowMs==4);CHECK(d.changed);CHECK((d.reasons&sl::reason::kLateFramesRescuable)!=0); // 2 + 1.5 -> 4, not 8
    s=t::clean(52*200);s.lateRescueMs=3;s.reorderWaitMs=4;
    d=e.update(s);CHECK(d.reorderWindowMs==8); // 4 + 3 -> 8
    s=t::clean(53*200);s.lateRescueMs=40;s.reorderWaitMs=8;
    d=e.update(s);CHECK(d.reorderWindowMs==16); // capped at the ladder's top
    // Late packets alone (no frame given up) never widen.
    sl::Engine quiet(c);
    for(int i=1;i<=600;++i){auto x=t::clean(i*200);x.packetsLate=3;x.packetsReordered=20;CHECK(quiet.update(x).reorderWindowMs==0);}
}

TEST(smartlink_reorder_returns_to_leave_after_a_long_clean_period){
    namespace t=smartlink_tests;namespace sl=t::sl;
    sl::Config c;c.configuredReorderMs=2;
    sl::Engine e(c);
    std::int64_t ms=0;
    for(int i=0;i<20;++i){ms+=200;e.update(t::clean(ms));}
    ms+=200;auto s=t::clean(ms);s.lateRescueMs=10;s.reorderWaitMs=2;
    CHECK(e.update(s).reorderWindowMs==16);
    const std::int64_t evidenceEnd=ms;
    std::int64_t leaveAt=-1;std::uint32_t previous=16;
    for(int i=0;i<1500;++i){
        ms+=200;const auto d=e.update(t::clean(ms));
        CHECK(d.reorderWindowMs<=previous); // only narrows on a clean link
        previous=d.reorderWindowMs;
        if(d.reorderWindowMs==0&&leaveAt<0)leaveAt=ms;
    }
    // 16 -> 8 -> 4 -> 2 (= configured): three relax dwells of 30 s.
    CHECK(leaveAt>0&&leaveAt-evidenceEnd<=3*30000+1000);
}

TEST(smartlink_trims_a_wait_that_only_buys_delay_and_restores_it){
    namespace t=smartlink_tests;namespace sl=t::sl;
    sl::Engine e; // configured: the library's 16 ms
    std::int64_t ms=0;std::uint32_t lowest=0;bool any=false;
    for(int i=0;i<1500;++i){
        ms+=200;auto s=t::clean(ms);
        if(i%10==0){s.packetsLost=1;}               // single losses every 2 s, never late
        const auto d=e.update(s);
        if(d.reorderWindowMs){any=true;lowest=lowest?(std::min)(lowest,d.reorderWindowMs):d.reorderWindowMs;}
    }
    CHECK(any&&lowest==2);
    for(int i=0;i<1500;++i){ms+=200;(void)e.update(t::clean(ms));}
    CHECK(e.update(t::clean(ms+200)).reorderWindowMs==0); // back to the configured wait
}

TEST(smartlink_reorder_changes_are_bounded_per_minute){
    namespace t=smartlink_tests;namespace sl=t::sl;
    sl::Config c;c.configuredReorderMs=2;
    sl::Engine e(c);
    std::int64_t ms=0;std::array<std::int64_t,64> at{};std::size_t n=0;std::uint32_t previous=0;
    for(int i=0;i<30000;++i){
        ms+=200;auto s=t::clean(ms);
        // Adversarial: rescue evidence that alternates in size, and clean stretches in between.
        if((i/400)%2==0&&i%7==0){s.lateRescueMs=static_cast<double>(1+t::mix(static_cast<std::uint64_t>(i))%14);s.reorderWaitMs=previous?previous:2;}
        const auto d=e.update(s);
        if(d.reorderWindowMs!=previous){at[n%at.size()]=ms;++n;previous=d.reorderWindowMs;}
        std::size_t inMinute=0;for(std::size_t k=0;k<(std::min)(n,at.size());++k)if(ms-at[k]<60000)++inMinute;
        CHECK(inMinute<=sl::Engine::kReorderChangesPerMinute);
    }
}

TEST(smartlink_audio_target_rises_after_underruns_and_returns_to_the_floor){
    namespace t=smartlink_tests;namespace sl=t::sl;
    sl::Engine e(sl::ModelKind::Statistics);
    std::int64_t ms=0;double sum=0;int count=0;bool raised=false;
    for(int i=0;i<1500;++i){ // 5 minutes of a stall every 4 s with an audio gap
        ms+=200;auto s=t::clean(ms,sl::Route::Wifi);
        if(i%20==0){s.frameGapExcessMs=60;s.frameGapMaxMs=77;s.audioUnderruns=1;}
        const auto d=e.update(s);
        if(d.audioTargetMs>0){raised=true;CHECK(d.audioTargetMs>=30&&d.audioTargetMs<=100);}
    }
    CHECK(raised);
    std::int64_t floorAt=-1;
    for(int i=0;i<9000;++i){ // 30 minutes clean
        ms+=200;const auto d=e.update(t::clean(ms,sl::Route::Wifi));
        if(d.audioTargetMs==0&&floorAt<0)floorAt=ms;
        if(i>=3000){sum+=d.audioTargetMs;++count;}
    }
    CHECK(floorAt>0);
    CHECK(sum/count==0.0); // the long-run target is the floor again: no accumulation
}

TEST(smartlink_network_limited_for_stalls_with_small_mean_jitter){
    namespace t=smartlink_tests;namespace sl=t::sl;
    // The governor review's case: 20-80 ms stalls every 2-8 s, mean jitter under 3 ms.
    sl::Engine e;
    std::int64_t ms=0,next=3000;int limited=0,windows=0;double jitter=0;int jitterCount=0;
    std::uint64_t seed=7;
    for(int i=0;i<3000;++i){
        ms+=200;auto s=t::clean(ms,sl::Route::Wifi);
        if(ms>=next){seed=t::mix(seed);s.frameGapExcessMs=20+static_cast<double>(seed%61);s.frameGapMaxMs=s.frameGapExcessMs+16.7;
            seed=t::mix(seed);next=ms+2000+static_cast<std::int64_t>(seed%6001);}
        const auto d=e.update(s);
        if(i>=150){++windows;if(d.networkLimited)++limited;CHECK(e.prediction().jitterMeanMs<6);jitter+=e.prediction().jitterMeanMs;++jitterCount;}
    }
    CHECK(jitter/jitterCount<3.0);
    CHECK(limited*100>=windows*95); // true (practically) all the time
    // A clean link: false within the hold.
    bool cleared=false;
    for(int i=0;i<200;++i){ms+=200;if(!e.update(t::clean(ms,sl::Route::Wifi)).networkLimited)cleared=true;}
    CHECK(cleared);
    // A local decoder backlog is not the network.
    sl::Engine local;ms=0;next=3000;
    for(int i=0;i<500;++i){ms+=200;auto s=t::clean(ms,sl::Route::Wifi);s.videoQueueDepth=5;if(ms>=next){s.frameGapExcessMs=50;next=ms+3000;}CHECK(!local.update(s).networkLimited);}
}

TEST(smartlink_adversarial_inputs_never_misbehave){
    namespace t=smartlink_tests;namespace sl=t::sl;
    const double nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();
    sl::Engine e;
    // All unknown: nothing is decided.
    for(int i=0;i<500;++i){const auto d=e.update(sl::Sample{});CHECK(d.reorderWindowMs==0&&d.audioTargetMs==0&&!d.networkLimited&&!d.linkHeadroom&&d.suggestedStartKbps==0);}
    // NaN, infinities, negative and huge values in every field.
    std::int64_t ms=0;
    for(int i=0;i<3000;++i){
        ms+=200;sl::Sample s=t::clean(ms);
        const std::uint64_t r=t::mix(static_cast<std::uint64_t>(i));
        const double pick[]={nan,inf,-inf,-5,1e300,0,1e-300};
        const double v=pick[r%7];
        s.windowMs=(r>>3)%5==0?v:200;s.frameGapExcessMs=v;s.frameGapMaxMs=v;s.lateRescueMs=v;s.rttMs=v;s.consoleTargetKbps=v;
        s.measuredKbps=v;s.requestedKbps=v;s.audioBufferedMs=v;s.wifiSignalPercent=v;s.wifiRssiDbm=v;s.reorderWaitMs=v;
        s.packetsLost=static_cast<std::int64_t>(r%3)-1;s.packetsLate=(r>>5)%2?std::numeric_limits<std::int64_t>::max():-7;
        s.audioUnderruns=(r>>7)%2?std::numeric_limits<std::int64_t>::max():-1;s.framesArrived=(r>>9)%4==0?0:12;
        if((r>>11)%13==0)s.endMs=ms-100000; // time going backwards
        if((r>>13)%17==0)s.windowMs=0;      // an empty window
        const auto d=e.update(s);
        CHECK(std::isfinite(d.stallRisk)&&d.stallRisk>=0&&d.stallRisk<=1);
        CHECK(d.reorderWindowMs==0||d.reorderWindowMs==2||d.reorderWindowMs==4||d.reorderWindowMs==8||d.reorderWindowMs==16);
        CHECK(d.audioTargetMs<=100);
        CHECK(d.suggestedStartKbps<=1000000u);
        CHECK(toLogLine(d,sl::Mode::Observe).size>0);
    }
    // Time going backwards for good (a reset clock): taken after a while, decisions continue.
    sl::Engine back;ms=10000000;
    for(int i=0;i<100;++i){ms+=200;(void)back.update(t::clean(ms));}
    ms=1000;bool usable=false;
    for(int i=0;i<100;++i){ms+=200;(void)back.update(t::clean(ms));if(back.prediction().usable)usable=true;}
    CHECK(usable);
}

TEST(smartlink_twelve_hours_without_drift){
    namespace t=smartlink_tests;namespace sl=t::sl;
    sl::Engine e;
    std::int64_t ms=0;
    // 12 h of windows; one 50 ms spike every 97 s on Wi-Fi.
    for(std::int64_t i=0;i<216000;++i){
        ms+=200;auto s=t::clean(ms,sl::Route::Wifi);
        if(i%485==0){s.frameGapExcessMs=50;}
        const auto d=e.update(s);
        CHECK(std::isfinite(d.stallRisk));
        if(i>216000-100){CHECK(d.reorderWindowMs==0);CHECK(d.audioTargetMs==0||d.audioTargetMs<=40);}
    }
    CHECK(e.prediction().jitterMeanMs<2);
    CHECK(e.profile().profile==sl::Profile::StrongWifi); // one stall every ~100 s is not "unstable"
}

TEST(smartlink_periodic_spikes_are_found_and_raise_the_risk_before_the_next){
    namespace t=smartlink_tests;namespace sl=t::sl;
    sl::Predictor p;
    std::int64_t ms=0;double riskBefore=0,riskBetween=0;int before=0,between=0;
    for(int i=0;i<3000;++i){ // a 40 ms spike every 10 s
        ms+=200;auto s=t::clean(ms,sl::Route::Wifi);
        const std::int64_t phase=ms%10000;
        if(phase==0)s.frameGapExcessMs=40;
        const auto r=p.update(s);
        if(i<600)continue;
        if(phase>=8400&&phase<=9800){riskBefore+=r.stallRisk;++before;}
        if(phase>=2000&&phase<=6000){riskBetween+=r.stallRisk;++between;}
        if(i>1000)CHECK(r.regime==sl::Regime::PeriodicSpikes);
    }
    CHECK(riskBefore/before>=0.5);
    CHECK(riskBetween/between<riskBefore/before-0.2);
    CHECK(p.update(t::clean(ms+200,sl::Route::Wifi)).spikePeriodMs>9500);
}

TEST(smartlink_profile_hysteresis_never_flaps){
    namespace sl=smartlink_tests::sl;
    sl::ProfileTracker tracker;
    sl::ProfileInput in;in.route=sl::Route::Wifi;
    unsigned changes=0;double lastChange=-1e9,minGap=1e9;
    for(int i=0;i<36000;++i){ // 2 hours, the condition toggling every 0.2-8 s
        in.nowMs=i*200.0;
        in.spikesLastMinute=((i/(1+(i%40)))%2)?5:0;
        const auto r=tracker.update(in);
        if(r.changed&&i>0){++changes;minGap=(std::min)(minGap,in.nowMs-lastChange);lastChange=in.nowMs;}
    }
    CHECK(minGap>=sl::ProfileTracker::kToUnstableMs);
    CHECK(changes<=2*36000*200/static_cast<unsigned>(sl::ProfileTracker::kToUnstableMs+sl::ProfileTracker::kToStrongMs)+2);
    // Strong -> Unstable needs 10 s, back needs 60 s.
    sl::ProfileTracker p2;in={};in.route=sl::Route::Wifi;in.signalPercent=80;
    for(int i=0;i<100;++i){in.nowMs=i*200.0;(void)p2.update(in);}
    CHECK(p2.profile()==sl::Profile::StrongWifi);
    in.signalPercent=20;
    for(int i=100;i<149;++i){in.nowMs=i*200.0;CHECK(p2.update(in).profile==sl::Profile::StrongWifi);}
    in.nowMs=151*200.0;auto r=p2.update(in);CHECK(r.profile==sl::Profile::UnstableWifi);
    CHECK(r.advice[0]==sl::Advice::UseCable&&r.advice[1]==sl::Advice::MoveCloserOrFasterBand);
    in.signalPercent=80;
    for(int i=152;i<450;++i){in.nowMs=i*200.0;CHECK(p2.update(in).profile==sl::Profile::UnstableWifi);}
    in.nowMs=460*200.0;CHECK(p2.update(in).profile==sl::Profile::StrongWifi);
    // Wired is wired; advice never names a setting change.
    sl::ProfileTracker w;in={};in.route=sl::Route::Wired;in.nowMs=1;CHECK(w.update(in).profile==sl::Profile::Wired);CHECK(w.update(in).advice[0]==sl::Advice::LinkIsFine);
}

TEST(smartlink_headroom_needs_full_rate_delivery){
    namespace t=smartlink_tests;namespace sl=t::sl;
    // A roomy link that DELIVERS the request (measured 14000 of 15000): headroom after 2 min, a raise after 5, at most 1.25 x.
    sl::Engine e;std::int64_t ms=0;sl::Decision d;
    const auto full=[&](std::int64_t at){auto s=t::clean(at);s.measuredKbps=14000;return s;};
    for(int i=0;i<300;++i){ms+=200;d=e.update(full(ms));}
    CHECK(!d.linkHeadroom&&d.suggestedStartKbps==0); // 60 s is not enough
    for(int i=0;i<400;++i){ms+=200;d=e.update(full(ms));}
    CHECK(d.linkHeadroom&&d.suggestedStartKbps==0);  // headroom, but not yet minutes of it for a raise
    for(int i=0;i<1000;++i){ms+=200;d=e.update(full(ms));CHECK(d.suggestedStartKbps<=18750);}
    CHECK(d.suggestedStartKbps==18500);              // 15000 * 1.25 rounded down to 500: the stated ceiling
    // The absolute ceiling: a 48 Mbit/s request is never raised above 50 Mbit/s.
    sl::Engine big;ms=0;
    for(int i=0;i<2000;++i){ms+=200;auto s=full(ms);s.requestedKbps=48000;s.consoleTargetKbps=48000;s.measuredKbps=46000;d=big.update(s);CHECK(d.suggestedStartKbps<=50000);}
    CHECK(d.suggestedStartKbps==50000);
    // Never above the request without headroom.
    sl::Engine lossy;ms=0;
    for(int i=0;i<3000;++i){ms+=200;auto s=full(ms);if(i%50==0)s.packetsLost=1;d=lossy.update(s);CHECK(d.suggestedStartKbps<=15000);}
}

// Review blocker 2: a calm picture (the console sends 25-60 % of the request on a menu, BENCH1) shows nothing about
// the link: no headroom, no raise, no "start lower", advice None or LinkIsFine; on wired and on Wi-Fi.
TEST(smartlink_calm_menu_gives_no_rate_advice){
    namespace t=smartlink_tests;namespace sl=t::sl;
    for(auto route:{sl::Route::Wired,sl::Route::Wifi})for(double target:{15000.0,4500.0,-1.0})for(double share:{0.25,0.3,0.6}){
        sl::Engine e;std::int64_t ms=0;sl::Decision d;
        for(int i=0;i<18000;++i){ // one hour
            ms+=200;auto s=t::clean(ms,route);s.measuredKbps=15000*share;s.consoleTargetKbps=target;
            d=e.update(s);
            CHECK(!d.linkHeadroom);CHECK(d.suggestedStartKbps==0);CHECK((d.reasons&sl::reason::kRateBelowRequest)==0);
        }
        for(auto a:e.profile().advice)CHECK(a==sl::Advice::None||a==sl::Advice::LinkIsFine);
    }
}

// A genuinely constrained link: the console holds its target at 9 Mbit/s AND packets are lost: suggest lower.
TEST(smartlink_constrained_link_suggests_lower){
    namespace t=smartlink_tests;namespace sl=t::sl;
    sl::Engine low;std::int64_t ms=0;sl::Decision d;
    for(int i=0;i<600;++i){ms+=200;auto s=t::clean(ms,sl::Route::Wifi);s.consoleTargetKbps=9000;s.measuredKbps=8500;if(i%40==0)s.packetsLost=2;d=low.update(s);}
    CHECK(!d.linkHeadroom);CHECK(d.suggestedStartKbps>=8500&&d.suggestedStartKbps<=9000);
    CHECK((d.reasons&sl::reason::kRateBelowRequest)!=0);
    // The same low target with a clean link: no suggestion (the console chose a low rate; nothing blames the link).
    sl::Engine calm;ms=0;
    for(int i=0;i<600;++i){ms+=200;auto s=t::clean(ms,sl::Route::Wifi);s.consoleTargetKbps=9000;s.measuredKbps=8500;d=calm.update(s);}
    CHECK(d.suggestedStartKbps==0);
}

TEST(smartlink_trace_lines_round_trip){
    namespace t=smartlink_tests;namespace sl=t::sl;namespace tr=sl::trace;
    std::array<char,tr::kMaxLine> buffer{};
    CHECK(tr::writeTraceHeader(buffer.data(),buffer.size())==tr::kHeader.size()+1);
    std::size_t commas=0;for(char c:tr::kHeader)if(c==',')++commas;CHECK(commas+1==tr::kColumns);
    sl::Sample parsed;CHECK(!tr::parseTraceLine(std::string_view(buffer.data()),parsed));
    for(int i=0;i<2000;++i){
        auto s=t::clean(1000+i*200,i%2?sl::Route::Wifi:sl::Route::Wired);
        s.frameGapExcessMs=static_cast<double>(t::mix(static_cast<std::uint64_t>(i))%100000)/1000.0;s.rttMs=i%3?-1:12.5;
        if(i%5==0){s=sl::Sample{};s.endMs=i;} // mostly unknown
        const auto n=tr::formatTraceLine(s,buffer.data(),buffer.size());
        CHECK(n>0&&n<tr::kMaxLine&&buffer[n-1]=='\n');
        CHECK(tr::parseTraceLine(std::string_view(buffer.data(),n),parsed));
        CHECK(parsed.endMs==s.endMs&&parsed.route==s.route&&parsed.packetsLost==s.packetsLost&&parsed.framesArrived==s.framesArrived);
        CHECK(std::abs(parsed.frameGapExcessMs-s.frameGapExcessMs)<0.001&&parsed.rttMs==s.rttMs);
        CHECK(parsed.wifiRssiDbm==s.wifiRssiDbm&&parsed.wifiSignalPercent==s.wifiSignalPercent);
    }
    // A too-small buffer writes nothing; bad fields become unknown; endMs is required.
    CHECK(tr::formatTraceLine(t::clean(1),buffer.data(),10)==0);
    CHECK(tr::parseTraceLine("5,200,1,x,3",parsed)&&parsed.packetsReceived==-1&&parsed.packetsLost==3&&parsed.framesArrived==-1);
    CHECK(!tr::parseTraceLine("abc,200",parsed));CHECK(!tr::parseTraceLine("# comment",parsed));CHECK(!tr::parseTraceLine("",parsed));
}

// Review blocker 1: the library applies a new reorder wait ~250 ms late, so the windows after a change still report
// lateRescueMs against the OLD wait. With 0, 1 and 2 windows of lag the wait never exceeds the smallest step that
// covers the lateness (3 ms late -> 4, 5 -> 8, 9 -> 16; before the fix 3 -> 8 and 5 -> 16).
TEST(smartlink_reorder_wait_with_apply_lag_never_overshoots){
    namespace t=smartlink_tests;namespace sl=t::sl;
    for(int lag=0;lag<=2;++lag)for(double late:{1.5,3.0,5.0,9.0}){
        sl::Config c;c.configuredReorderMs=2;sl::Engine e(c);
        std::vector<double> applied(static_cast<std::size_t>(lag)+1,2.0);
        std::uint64_t seed=static_cast<std::uint64_t>(lag*100)+static_cast<std::uint64_t>(late*10);
        unsigned widest=0;
        for(int i=1;i<=3000;++i){
            auto s=t::clean(i*200,sl::Route::Wifi);const double w=applied.front();s.reorderWaitMs=w;
            seed=t::mix(seed);
            if(i>100&&i<400&&seed%10<3&&late>w){s.lateRescueMs=late-w+0.001;s.framesIncomplete=1;s.packetsLate=1;}
            const auto d=e.update(s);widest=(std::max)(widest,d.reorderWindowMs);
            applied.erase(applied.begin());applied.push_back(d.reorderWindowMs?d.reorderWindowMs:2.0);
        }
        const unsigned need=late<=2?2u:late<=4?4u:late<=8?8u:16u;
        CHECK((widest?widest:2u)==need); // 0 = the configured 2 ms
    }
}

// Review should-fix 5: networkLimited must tell a link stall from a frame the console skipped. The app does not know
// packetGapMaxMs; the engine judges a gap window with the next one (frames short and nothing lost = the console).
TEST(smartlink_console_skipped_frames_are_not_network_limited){
    namespace t=smartlink_tests;namespace sl=t::sl;
    // A 30 fps game in a 60 fps stream (6 frames per window, 33 ms gaps) with one frame dropped every 8 s (gap 66.7 ms,
    // that window one frame short), packets flowing.
    sl::Engine e;sl::Decision d;long limited=0,raised=0;
    for(int i=1;i<=18000;++i){
        auto s=t::clean(i*200,sl::Route::Wifi);s.framesArrived=6;s.frameGapMaxMs=33.4;s.frameGapExcessMs=16.7;s.packetsReceived=162;
        if(i%40==0){s.framesArrived=5;s.frameGapMaxMs=66.7;s.frameGapExcessMs=50;s.packetsReceived=135;}
        d=e.update(s);limited+=d.networkLimited;raised+=d.audioTargetMs>0;
    }
    CHECK(limited==0);CHECK(raised==0);
    // BENCH1-like: the console delivers ~57 of 60 fps (a window with a skipped frame has 11 frames and one 33 ms gap).
    sl::Engine b;limited=0;std::uint64_t seed=7;
    for(int i=1;i<=18000;++i){
        auto s=t::clean(i*200,sl::Route::Wifi);seed=t::mix(seed);
        if(seed%10<6){s.framesArrived=11;s.frameGapMaxMs=33.4;s.frameGapExcessMs=16.7;}
        d=b.update(s);limited+=d.networkLimited;
    }
    CHECK(limited==0);
}

TEST(smartlink_link_stalls_are_network_limited_unless_a_backlog_drains){
    namespace t=smartlink_tests;namespace sl=t::sl;
    // The governor case: drought gaps of 20-80 ms every 2-8 s; the late frames arrive in a burst (the count recovers
    // within the next window); mean jitter stays under 3 ms. Also the short ones (gap 20-41 ms) alone.
    for(int variant=0;variant<2;++variant){
        sl::Engine e;sl::Decision d;std::uint64_t seed=11+static_cast<std::uint64_t>(variant);long next=10,limited=0,n=0;bool owe=false;
        for(int i=1;i<=18000;++i){
            auto s=t::clean(i*200,sl::Route::Wifi);
            if(owe){s.framesArrived=13;owe=false;}
            if(i>=next){
                seed=t::mix(seed);const double gap=variant==0?37+static_cast<double>(seed%64):37+static_cast<double>(seed%5); // stall 20-80 / 20-24 ms
                s.frameGapMaxMs=gap;s.frameGapExcessMs=gap-16.7;s.framesArrived=11;owe=true;
                seed=t::mix(seed);next=i+10+static_cast<long>(seed%31);
            }
            d=e.update(s);if(i>200){++n;limited+=d.networkLimited;}
        }
        CHECK(limited*10>=n*9); // flagged nearly all the time
    }
    // Spikes while a local decoder backlog drains (queue >= 3 in the window BEFORE the gap): never the link.
    sl::Engine q;long limited=0;
    for(int i=1;i<=18000;++i){
        auto s=t::clean(i*200,sl::Route::Wifi);
        if(i%15==14)s.videoQueueDepth=4;
        if(i%15==0){s.frameGapMaxMs=76;s.frameGapExcessMs=60;s.framesArrived=11;s.videoQueueDepth=2;}
        if(i%15==1)s.framesArrived=13;
        limited+=q.update(s).networkLimited;
    }
    CHECK(limited==0);
}

// Review should-fix 4: under sustained stalls (excess 20-80 ms every 2-8 s, some underruns) for an hour the audio
// reserve stays bounded by kAudioMaxMs (50 ms) and its mean stays near the floor; 12 h without ratchet.
TEST(smartlink_audio_reserve_capped_and_no_ratchet){
    namespace t=smartlink_tests;namespace sl=t::sl;
    sl::Engine e;std::uint64_t seed=3;long next=10;double hour[12]{};unsigned most=0;
    for(long i=1;i<=216000;++i){
        auto s=t::clean(i*200,sl::Route::Wifi);
        if(i>=next){seed=t::mix(seed);s.frameGapExcessMs=20+static_cast<double>(seed%61);s.frameGapMaxMs=s.frameGapExcessMs+16.7;s.audioUnderruns=static_cast<std::int64_t>(seed%2);
            seed=t::mix(seed);next=i+10+static_cast<long>(seed%31);}
        const auto d=e.update(s);most=(std::max)(most,d.audioTargetMs);
        hour[(i-1)/18000]+=d.audioTargetMs?d.audioTargetMs:20.0;
    }
    CHECK(most<=50);
    double worst=0;for(double h:hour)worst=(std::max)(worst,h/18000);
    CHECK(worst<=50);CHECK(hour[11]/18000<=hour[0]/18000+1.0);
}

// Review should-fix 7: loss pairs every 35 s on the 16 ms configuration swung trim/relax 203 times an hour. Bound:
// a relax undone within a minute doubles the next dwell (to 240 s), so such a link makes at most 3 trims (16 -> 8 ->
// 4 -> 2) and a few relaxes: at most 12 changes per hour, and never more than 4 per minute.
TEST(smartlink_trim_relax_swing_is_bounded){
    namespace t=smartlink_tests;namespace sl=t::sl;
    for(int period:{175,150,250}){
        sl::Config c;c.configuredReorderMs=16;sl::Engine e(c);
        for(int i=1;i<=18000;++i){auto s=t::clean(i*200);if(i%period==0||i%period==3)s.packetsLost=1;(void)e.update(s);}
        CHECK(e.changes()<=12);
    }
}

// Small: a round trip that swings 2-79 ms with every frame on time and nothing lost is not "congested".
TEST(smartlink_rtt_swing_alone_is_not_congestion){
    namespace t=smartlink_tests;namespace sl=t::sl;
    sl::Engine e;std::uint64_t seed=5;double rtt=4;long congested=0;
    for(int i=1;i<=18000;++i){
        auto s=t::clean(i*200,sl::Route::Wifi);if(i%5==0){seed=t::mix(seed);rtt=2+static_cast<double>(seed%78);}s.rttMs=rtt;s.measuredKbps=4000;
        congested+=e.update(s).regime==sl::Regime::Congested;
    }
    CHECK(congested<=180); // at most 1 % of windows (it was about half)
}

// Decision::advice: every value reachable from a plausible sequence; a clean wired link shows only None / LinkIsFine;
// at most one change a minute; LinkIsFine only after clean minutes; rate advice only under the rate rules.
TEST(smartlink_advice_in_decision){
    namespace t=smartlink_tests;namespace sl=t::sl;
    using Gen=void(*)(sl::Sample&,int);
    struct Case {sl::Route route;Gen gen;sl::Advice expect;};
    const Case cases[]={
        {sl::Route::Wired,[](sl::Sample&,int){},sl::Advice::LinkIsFine},                                        // clean, measured 9000 of 15000
        {sl::Route::Wired,[](sl::Sample& s,int){s.measuredKbps=14000;},sl::Advice::RaiseRateNextTime},            // full delivery for minutes
        {sl::Route::Wired,[](sl::Sample& s,int i){s.consoleTargetKbps=9000;s.measuredKbps=8500;if(i%40==0)s.packetsLost=2;},sl::Advice::LowerPictureRateNextTime},
        {sl::Route::Wifi,[](sl::Sample& s,int i){const auto u=static_cast<std::uint64_t>(i);
            if(smartlink_tests::mix(u)%20==0){s.frameGapMaxMs=76;s.frameGapExcessMs=60;s.framesArrived=11;}if(smartlink_tests::mix(u-1)%20==0)s.framesArrived=13;},sl::Advice::UseCable}, // stalls at random, ~one every 4 s
        {sl::Route::Wifi,[](sl::Sample& s,int i){s.wifiSignalPercent=30;s.wifiRssiDbm=-85;if(i%25==0){s.frameGapMaxMs=76;s.frameGapExcessMs=60;s.framesArrived=11;}if(i%25==1)s.framesArrived=13;},sl::Advice::MoveCloserOrFasterBand},
        {sl::Route::Wifi,[](sl::Sample& s,int i){if(i%50==0){s.frameGapMaxMs=76;s.frameGapExcessMs=60;s.framesArrived=11;}if(i%50==1)s.framesArrived=13;},sl::Advice::BackgroundActivity}, // every 10 s exactly
    };
    for(const auto& c:cases){
        sl::Engine e;sl::Decision d;double lastChange=-1e18;sl::Advice shown=sl::Advice::None;bool seen=false;
        for(int i=1;i<=9000;++i){ // 30 minutes
            auto s=t::clean(i*200,c.route);c.gen(s,i);d=e.update(s);
            if(i<300)CHECK(d.advice==sl::Advice::None);  // nothing in the first minute
            if(d.advice!=shown){CHECK(i*200.0-lastChange>=60000);lastChange=i*200.0;shown=d.advice;}
            if(c.route==sl::Route::Wired&&c.expect==sl::Advice::LinkIsFine)CHECK(d.advice==sl::Advice::None||d.advice==sl::Advice::LinkIsFine);
            seen|=d.advice==c.expect;
        }
        CHECK(seen);
    }
    // A calm menu (a third of the request) on wired never gets rate advice.
    sl::Engine calm;
    for(int i=1;i<=9000;++i){auto s=t::clean(i*200);s.measuredKbps=4500;s.consoleTargetKbps=4500;const auto a=calm.update(s).advice;CHECK(a==sl::Advice::None||a==sl::Advice::LinkIsFine);}
}

// Small: trace integers are read back through a double; values above 2^53 are clamped there on writing, so a
// written line always reads back to the same line.
TEST(smartlink_trace_integers_clamped_to_2_pow_53){
    namespace t=smartlink_tests;namespace sl=t::sl;namespace tr=sl::trace;
    std::array<char,tr::kMaxLine> a{},b{};
    auto s=t::clean(1000);s.packetsReceived=std::numeric_limits<std::int64_t>::max();s.endMs=(std::int64_t{1}<<53)+12345;
    const auto n=tr::formatTraceLine(s,a.data(),a.size());CHECK(n>0);
    sl::Sample p;CHECK(tr::parseTraceLine(std::string_view(a.data(),n),p));
    CHECK(p.endMs==(std::int64_t{1}<<53));CHECK(p.packetsReceived==(std::int64_t{1}<<53));
    const auto m=tr::formatTraceLine(p,b.data(),b.size());CHECK(m==n&&std::memcmp(a.data(),b.data(),n)==0);
}
