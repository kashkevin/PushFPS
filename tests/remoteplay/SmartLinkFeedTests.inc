// SPDX-License-Identifier: GPL-3.0-only
// Sprint 4 (network, SmartLink phase 2): the console's CONNECTIONQUALITY counters
// (ConsoleQuality.h) and the 200 ms observation windows (SmartLinkFeed.h).
// Pure logic on Linux; the Windows glue that fills the inputs is not run here.
#include "veyra/remoteplay/SmartLinkFeed.h"
#include "veyra/remoteplay/SmartLinkRunner.h"
#include "veyra/engine/SlipstreamSettings.h"
#include <vector>
namespace smartlink_feed_tests {
using namespace veyra::remoteplay;
using veyra::remoteplay::smartlink::FeedInput;using veyra::remoteplay::smartlink::SampleFeed;
inline FeedInput input(std::int64_t nowMs){FeedInput in;in.nowMs=nowMs;in.requestedKbps=15000;in.nominalFps=60;in.route=smartlink::Route::Wired;return in;}
}

TEST(console_quality_unknown_without_a_message_or_a_reference){
    TransportCounters c;
    auto q=interpretConsoleQuality(c,5000000,15000);
    CHECK(q.reports==0&&q.rttMs<0&&q.targetKbps<0&&q.ageMs<0);
    c.consoleReports=1;c.consoleRttMilli=12000000;c.consoleLastUs=4000000;   // rtt 12.0 as sent (x 1e6)
    q=interpretConsoleQuality(c,5000000,15000);
    CHECK(q.reports==1&&q.ageMs==1000.0);
    CHECK(q.rttMs<0);                                   // no units given: unknown
    ConsoleUnits u;for(std::uint64_t r=1;r<=5;++r){c.consoleReports=r;u.learn(c,15000);}
    CHECK(u.rttScale==0&&interpretConsoleQuality(c,5000000,15000,&u).rttMs<0); // no Senkusha reference: unit unknown
    c.senkushaRttUs=1;ConsoleUnits v;for(std::uint64_t r=6;r<=10;++r){c.consoleReports=r;v.learn(c,15000);}
    CHECK(v.rttScale==0);                               // 1 = "measured as 0"
}
namespace smartlink_feed_tests {
// n new reports of the same values into units.
inline void reports(ConsoleUnits& u,TransportCounters& c,int n,double requestedKbps){for(int i=0;i<n;++i){++c.consoleReports;u.learn(c,requestedKbps);}}
}
TEST(console_quality_decides_the_unit_only_after_three_agreeing_reports){
    using namespace smartlink_feed_tests;
    TransportCounters c;c.consoleLastUs=1;c.senkushaRttUs=4000; // 4 ms at connect
    c.consoleRttMilli=12000000;                                  // 12.0: milliseconds
    ConsoleUnits ms;reports(ms,c,2,-1);CHECK(ms.rttScale==0&&interpretConsoleQuality(c,2,-1,&ms).rttMs<0);
    reports(ms,c,1,-1);CHECK(ms.rttScale==1.0&&std::fabs(interpretConsoleQuality(c,2,-1,&ms).rttMs-12.0)<1e-9);
    // The same report read twice (health line and feed) counts once.
    ConsoleUnits once;++c.consoleReports;for(int i=0;i<5;++i)once.learn(c,-1);CHECK(once.rttScale==0&&once.rtt.votes[0]==1);
    c.consoleRttMilli=12000;                                     // 0.012: seconds
    ConsoleUnits seconds;reports(seconds,c,3,-1);CHECK(seconds.rttScale==1000.0&&std::fabs(interpretConsoleQuality(c,2,-1,&seconds).rttMs-12.0)<1e-9);
    // Kept once decided: a later Wi-Fi spike 40x the connect value keeps the session's unit.
    c.consoleRttMilli=3000000;c.senkushaRttUs=2000;ConsoleUnits units;reports(units,c,3,-1);CHECK(units.rttScale==1.0);
    c.consoleRttMilli=79000000;reports(units,c,3,-1);
    auto q=interpretConsoleQuality(c,2,-1,&units);CHECK(q.rttScale==1.0&&std::fabs(q.rttMs-79.0)<1e-9);
    // Decided, but a converted value outside 0.1-2000 ms is unknown, never a guess.
    c.consoleRttMilli=2500000000ull;CHECK(interpretConsoleQuality(c,2,-1,&units).rttMs<0);
    c.consoleRttMilli=50000;CHECK(interpretConsoleQuality(c,2,-1,&units).rttMs<0);  // 0.05 ms
    // Bitrates against the request: kbit/s, or bit/s; three agreeing reports.
    c.consoleTargetBitrate=14000;c.consoleUpstreamBitrate=300;
    ConsoleUnits kbps;reports(kbps,c,2,15000);CHECK(kbps.bitrateScale==0&&interpretConsoleQuality(c,2,15000,&kbps).targetKbps<0);
    reports(kbps,c,1,15000);q=interpretConsoleQuality(c,2,15000,&kbps);CHECK(q.bitrateScale==1.0&&q.targetKbps==14000&&q.upstreamKbps==300);
    c.consoleTargetBitrate=4000000;                              // 4 Mbit/s sent as bit/s
    ConsoleUnits bps;reports(bps,c,3,15000);q=interpretConsoleQuality(c,2,15000,&bps);CHECK(q.bitrateScale==0.001&&std::fabs(q.targetKbps-4000)<1e-6);
    ConsoleUnits none;reports(none,c,5,0);CHECK(interpretConsoleQuality(c,2,0,&none).targetKbps<0); // no request known: unknown
    c.consoleUpstreamLossPpm=2500;CHECK(std::fabs(interpretConsoleQuality(c,2,15000).upstreamLossPercent-0.25)<1e-12);
}
// Review net2 item 1 (reviewer's reproductions): the old nearest-power rule read these 1000x off.
TEST(console_quality_review_misreads_end_unknown_or_right){
    using namespace smartlink_feed_tests;
    const auto never1000xOff=[](double rttMs,double truthMs){return rttMs<0||(rttMs>truthMs/31.6&&rttMs<truthMs*31.6);};
    // connect 1 ms, the console sends 0.040 (seconds: 40 ms on Wi-Fi).
    {TransportCounters c;c.consoleLastUs=1;c.senkushaRttUs=1000;c.consoleRttMilli=40000;
     ConsoleUnits u;reports(u,c,10,15000);const auto q=interpretConsoleQuality(c,2,15000,&u);
     CHECK(never1000xOff(q.rttMs,40.0));CHECK(q.rttMs<0||std::fabs(q.rttMs-40.0)<1e-9);}
    // connect 40 ms (a spike), the console sends 1.0 (milliseconds).
    {TransportCounters c;c.consoleLastUs=1;c.senkushaRttUs=40000;c.consoleRttMilli=1000000;
     ConsoleUnits u;reports(u,c,10,15000);const auto q=interpretConsoleQuality(c,2,15000,&u);
     CHECK(never1000xOff(q.rttMs,1.0));CHECK(q.rttMs<0||std::fabs(q.rttMs-1.0)<1e-9);}
    // Then the link settles and the reports agree with the connect value: decided, and right.
    {TransportCounters c;c.consoleLastUs=1;c.senkushaRttUs=1000;c.consoleRttMilli=40000;
     ConsoleUnits u;reports(u,c,10,15000);c.consoleRttMilli=2000;reports(u,c,3,15000); // 0.002 s
     CHECK(u.rttScale==1000.0&&std::fabs(interpretConsoleQuality(c,2,15000,&u).rttMs-2.0)<1e-9);}
    // Mixed evidence (each unit supported) waits for a margin of 3.
    {TransportCounters c;c.consoleLastUs=1;c.senkushaRttUs=5000;ConsoleUnits u;
     c.consoleRttMilli=5000000;reports(u,c,3,-1);CHECK(u.rttScale==1.0);
     ConsoleUnits w;c.consoleRttMilli=5000;reports(w,c,1,-1);c.consoleRttMilli=5000000;reports(w,c,3,-1);CHECK(w.rttScale==0);
     reports(w,c,1,-1);CHECK(w.rttScale==1.0);}
}
// Review net2 item 2: a sub-millisecond round trip sent in seconds survives the patch's x 1e6.
TEST(console_quality_sub_millisecond_round_trip_in_seconds){
    using namespace smartlink_feed_tests;
    TransportCounters c;c.consoleLastUs=1;c.senkushaRttUs=400;               // 0.4 ms at connect (wired)
    c.consoleRttMilli=static_cast<std::uint64_t>(0.0004*kConsoleRttCountsPerUnit+0.5); // 0.0004 s as the patch stores it
    CHECK(c.consoleRttMilli==400);
    ConsoleUnits u;reports(u,c,3,-1);
    const auto q=interpretConsoleQuality(c,2,-1,&u);CHECK(u.rttScale==1000.0&&std::fabs(q.rttMs-0.4)<1e-9);
}
TEST(console_units_shared_by_health_line_and_feed_and_logged_once){
    using namespace smartlink_feed_tests;
    ConsoleUnits shared;SampleFeed feed;feed.shareUnits(&shared);
    auto in=input(0);in.transport.senkushaRttUs=3000;in.transport.consoleRttMilli=5000000;in.transport.consoleTargetBitrate=15000;in.transport.consoleLastUs=1;
    (void)feed.poll(in);
    char text[256];CHECK(shared.takeLogLine(text,sizeof(text))==0);
    for(int w=1;w<=3;++w){
        in.nowMs=200*w;in.transport.consoleReports=static_cast<std::uint64_t>(w);
        shared.learn(in.transport,15000);                       // the health line reads the same report first
        const auto s=feed.poll(in);CHECK(s.has_value());
        if(w<3)CHECK(s->rttMs<0);else CHECK(std::fabs(s->rttMs-5.0)<1e-9&&s->consoleTargetKbps==15000);
    }
    CHECK(shared.seen==3&&shared.rtt.votes[0]==3);
    const auto healthSide=healthCounters(in.transport,SessionInbox::Snapshot{},0,15000,&shared);
    CHECK(std::fabs(healthSide.console.rttMs-5.0)<1e-9);          // both readers say the same
    const auto n=shared.takeLogLine(text,sizeof(text));
    CHECK(n>0&&std::string(text,n).rfind("consoleUnits rtt=ms rate=kbit/s basis=reports:3 ",0)==0);
    CHECK(shared.takeLogLine(text,sizeof(text))==0);             // once per session
    // Undecided after 20 reports: logged as such.
    ConsoleUnits lone;TransportCounters c;c.consoleRttMilli=1000000000;c.senkushaRttUs=3000; // 1000.0: 333x the connect value as ms, implausible as s
    reports(lone,c,19,-1);CHECK(lone.takeLogLine(text,sizeof(text))==0);
    reports(lone,c,1,-1);const auto m=lone.takeLogLine(text,sizeof(text));
    CHECK(m>0&&std::string(text,m).rfind("consoleUnits rtt=undecided rate=undecided ",0)==0);
}
// Review net2 item 3 (reviewer's case): the late packet's bucket counted in one window,
// the frame it failed in the next.
TEST(smartlink_feed_late_rescue_carried_across_windows){
    using namespace smartlink_feed_tests;
    SampleFeed f;auto a=input(0);a.transport.reorderTimeoutUs=4000;(void)f.poll(a);
    a.nowMs=200;a.transport.reorderLateness[4]=1;               // 4-6 ms
    auto s1=f.poll(a);CHECK(s1&&s1->lateRescueMs==0);           // no frame lost yet
    a.nowMs=400;a.transport.lateFramesFailed=1;a.transport.fecFailed=1;
    auto s2=f.poll(a);CHECK(s2&&s2->lateRescueMs==2.0);         // carried: 6 - 4
    a.nowMs=600;a.transport.lateFramesFailed=2;a.transport.fecFailed=2;
    auto s3=f.poll(a);CHECK(s3&&s3->lateRescueMs<0);            // carried once only
    f.newConnection();a.nowMs=800;(void)f.poll(a);              // a reconnect drops the carry
    a.nowMs=1000;a.transport.reorderLateness[4]=2;(void)f.poll(a);
    f.newConnection();a.nowMs=1200;(void)f.poll(a);a.nowMs=1400;a.transport.lateFramesFailed=3;
    auto s4=f.poll(a);CHECK(s4&&s4->lateRescueMs<0);
}
TEST(health_line_carries_the_console_round_trip){
    HealthSnapshot s;s.valid=true;
    const auto none=formatHealthLine(s);
    CHECK(none.find(" consoleRttMs=-1.0 consoleTargetKbps=-1 consoleMeasuredKbps=-1 consoleReports=0 consoleAgeMs=-1")!=std::string::npos);
    TransportCounters t;t.consoleReports=4;t.consoleRttMilli=23500000;t.senkushaRttUs=3000;t.consoleTargetBitrate=15000;
    t.consoleLastUs=1000;t.libraryNowUs=251000;t.consoleMeasuredKbps=4100;
    ConsoleUnits units;smartlink_feed_tests::reports(units,t,3,15000);   // reports 5, 6, 7
    CHECK(healthCounters(t,SessionInbox::Snapshot{},0,15000).console.rttMs<0); // no session units: unknown
    const auto c=healthCounters(t,SessionInbox::Snapshot{},0,15000,&units);
    CHECK(std::fabs(c.console.rttMs-23.5)<1e-9&&c.console.targetKbps==15000);
    ConnectionHealth h;h.begin(0);HealthCounters k=c;k.packetsReceived=10;
    const auto snap=h.sample(ConnectionHealth::kInterval100ns,k);
    CHECK(snap&&std::fabs(snap->console.rttMs-23.5)<1e-9);
    const auto line=formatHealthLine(*snap);
    CHECK(line.find(" consoleRttMs=23.5 consoleTargetKbps=15000 consoleMeasuredKbps=4100 consoleReports=7 consoleAgeMs=250")!=std::string::npos);
    CHECK(line.find(" rtt=3.00ms(at-connect) ")!=std::string::npos);   // the connect-time key is unchanged
}
TEST(smartlink_feed_windows_of_200_ms_from_cumulative_counters){
    using namespace smartlink_feed_tests;
    SampleFeed feed;
    auto in=input(1000);in.transport.reorderTimeoutUs=2000;
    CHECK(!feed.poll(in));                                  // the first call only takes the base
    in.nowMs=1100;CHECK(!feed.poll(in));                    // not a full window yet
    in.nowMs=1200;in.transport.congestionReceived=300;in.transport.congestionLost=2;in.transport.reorderGaps=5;in.transport.reorderTimeouts=1;
    in.transport.reorderLateArrivals=1;in.transport.fecFailed=1;in.transport.fecRecovered=2;in.framesComplete=11;in.receivedBytes=100000;in.videoQueueDepth=1;
    for(std::int64_t t=1000;t<=1190;t+=17)feed.observeFrameArrival(t);
    feed.observeFrameArrival(1195);
    const auto s=feed.poll(in);
    CHECK(s.has_value());
    CHECK(s->windowMs==200&&s->endMs==1200&&s->packetsReceived==300&&s->packetsLost==2&&s->packetsReordered==4&&s->packetsLate==1);
    CHECK(std::fabs(s->packetGapMeanMs-200.0/300.0)<1e-9&&s->packetGapMaxMs<0);
    CHECK(s->framesArrived==11&&s->framesIncomplete==1&&s->framesFecRecovered==2);
    CHECK(s->frameGapMaxMs==17.0&&std::fabs(s->frameGapExcessMs-(17.0-1000.0/60.0))<1e-9);
    CHECK(s->reorderWaitMs==2.0&&s->lateRescueMs==0);       // a lost frame, but none to a late packet
    CHECK(s->measuredKbps==4000.0&&s->requestedKbps==15000&&s->videoQueueDepth==1);
    CHECK(s->rttMs<0&&s->consoleTargetKbps<0&&s->audioBufferedMs<0&&s->audioUnderruns<0);
    CHECK(s->wifiSignalPercent<0&&s->wifiRssiDbm==0);        // wired: no Wi-Fi numbers
}
TEST(smartlink_feed_empty_window_open_gap_and_late_rescue){
    using namespace smartlink_feed_tests;
    SampleFeed feed;auto in=input(0);in.transport.reorderTimeoutUs=2000;
    (void)feed.poll(in);feed.observeFrameArrival(0);
    in.nowMs=200;
    auto s=feed.poll(in);                                    // nothing arrived: an empty window is still a window
    CHECK(s&&s->packetsReceived==0&&s->framesArrived==0&&s->packetGapMeanMs<0);
    CHECK(s->frameGapMaxMs==200.0);                          // the gap still open at its end
    // Two frames lost to late packets whose lateness reached the <= 6 ms bucket: 4 ms more would have saved them.
    in.nowMs=400;in.transport.fecFailed=2;in.transport.lateFramesFailed=2;in.transport.reorderLateArrivals=3;
    in.transport.reorderLateness[1]=1;in.transport.reorderLateness[4]=2;  // <= 2 ms (in time) and <= 6 ms
    s=feed.poll(in);CHECK(s&&s->lateRescueMs==4.0);
    in.nowMs=600;in.transport.lateFramesFailed=3;in.transport.fecFailed=3;in.transport.reorderLateness[8]=1; // beyond 16 ms
    s=feed.poll(in);CHECK(s&&s->lateRescueMs==14.0);
    in.nowMs=800;in.transport.lateFramesFailed=4;in.transport.fecFailed=4;in.transport.reorderTimeoutUs=0;
    s=feed.poll(in);CHECK(s&&s->lateRescueMs<0);             // the wait is unknown
}
TEST(smartlink_feed_counter_reset_reconnect_and_time_going_back){
    using namespace smartlink_feed_tests;
    SampleFeed feed;auto in=input(10000);in.transport.congestionReceived=5000;in.framesComplete=600;
    (void)feed.poll(in);
    // A reconnect without newConnection(): the counters restart; the window counts them from zero.
    in.nowMs=10200;in.transport.congestionReceived=40;in.framesComplete=3;
    auto s=feed.poll(in);CHECK(s&&s->packetsReceived==40&&s->framesArrived==3);
    // With newConnection(): no sample across the reconnect; the next window starts there.
    feed.newConnection();in.nowMs=10400;in.transport.congestionReceived=0;in.framesComplete=0;
    CHECK(!feed.poll(in));
    in.nowMs=10600;in.transport.congestionReceived=310;s=feed.poll(in);CHECK(s&&s->packetsReceived==310&&s->frameGapMaxMs<0);
    // The clock goes back: the window restarts, no sample, no negative window.
    in.nowMs=10500;CHECK(!feed.poll(in));
    in.nowMs=10650;CHECK(!feed.poll(in));
    in.nowMs=10700;in.transport.congestionReceived=400;s=feed.poll(in);CHECK(s&&s->windowMs==200&&s->packetsReceived==90);
    // Frame arrivals out of order or negative are ignored.
    feed.observeFrameArrival(-5);feed.observeFrameArrival(10690);feed.observeFrameArrival(10680);
    in.nowMs=10900;s=feed.poll(in);CHECK(s&&s->frameGapMaxMs==210.0);
    CHECK(!feed.poll(input(-1)));
}
TEST(smartlink_feed_audio_and_wifi_numbers){
    using namespace smartlink_feed_tests;
    SampleFeed feed;auto in=input(0);in.route=smartlink::Route::Wifi;in.audioUnderrunsTotal=4;
    (void)feed.poll(in);
    in.nowMs=200;in.audioBufferedMs=38.5;in.audioUnderrunsTotal=6;
    in.wifi.signalPercent=72;in.wifi.rssiDbm=-61;in.wifi.rxMbps=286;in.wifi.bandGhz=5;
    auto s=feed.poll(in);
    CHECK(s&&s->audioBufferedMs==38.5&&s->audioUnderruns==2);
    CHECK(s->wifiSignalPercent==72&&s->wifiRssiDbm==-61&&s->wifiLinkMbps==286&&s->wifiBandGhz==5&&s->route==smartlink::Route::Wifi);
    in.nowMs=400;in.audioUnderrunsTotal=-1;s=feed.poll(in);CHECK(s&&s->audioUnderruns<0);
    in.nowMs=600;in.audioUnderrunsTotal=1;s=feed.poll(in);CHECK(s&&s->audioUnderruns<0); // after an unknown: unknown
    in.nowMs=800;in.audioUnderrunsTotal=0;s=feed.poll(in);CHECK(s&&s->audioUnderruns==0); // the sink restarted from 0
}
// --- the session owner's runner (SmartLinkRunner.h) and the settings file key ---
namespace smartlink_feed_tests {
// Windows of 200 ms on a clean link; `rescue` puts two frames lost to late packets (<= 6 ms bucket) into one window.
inline std::vector<smartlink::Runner::Step> drive(smartlink::Runner& r,int windows,int rescueAt,std::uint32_t waitUs){
    std::vector<smartlink::Runner::Step> out;FeedInput in=input(0);in.transport.reorderTimeoutUs=waitUs;
    std::uint64_t received=0,frames=0;
    for(int w=0;w<=windows;++w){
        in.nowMs=static_cast<std::int64_t>(w)*200;received+=330;frames+=12;
        in.transport.congestionReceived=received;in.framesComplete=frames;in.receivedBytes=frames*30000;
        if(w==rescueAt){in.transport.fecFailed+=2;in.transport.lateFramesFailed+=2;in.transport.reorderLateArrivals+=3;in.transport.reorderSkipped+=3;in.transport.reorderLateness[4]+=3;}
        for(int f=0;f<12;++f)r.frameArrived(in.nowMs-200+f*16+16);
        out.push_back(r.step(in));
    }
    return out;
}
}
TEST(smartlink_runner_observe_changes_nothing_and_logs_at_most_every_5_s){
    using namespace smartlink_feed_tests;
    smartlink::Runner r(smartlink::Mode::Observe,smartlink::ModelKind::Statistics,2000,false);
    const auto steps=drive(r,100,50,2000);
    unsigned logs=0,decided=0;
    for(const auto& s:steps){CHECK(s.setReorderUs==0);logs+=s.log;decided+=s.decided;}
    CHECK(decided==100);                       // every window after the base
    CHECK(logs>=4&&logs<=5);                   // 20 s of windows: one line per 5 s
    CHECK(r.appliedReorderUs()==0);
}
TEST(smartlink_runner_apply_widens_on_rescuable_frames_and_explicit_wait_wins){
    using namespace smartlink_feed_tests;
    smartlink::Runner r(smartlink::Mode::Apply,smartlink::ModelKind::Statistics,2000,false);
    const auto steps=drive(r,20,5,2000);
    std::uint32_t set=0;for(const auto& s:steps)if(s.setReorderUs){set=s.setReorderUs;CHECK(s.log);}
    CHECK(set>=6000&&set<=16000);              // 2 ms + up to 6 ms of lateness: the next ladder step up
    CHECK(r.appliedReorderUs()==set);
    CHECK(r.newConnection()==set);              // a reconnect starts at the wait reached
    smartlink::Runner clean(smartlink::Mode::Apply,smartlink::ModelKind::Statistics,2000,false);
    for(const auto& s:drive(clean,100,-1,2000))CHECK(s.setReorderUs==0); // a clean link: nothing changes
    smartlink::Runner fixed(smartlink::Mode::Apply,smartlink::ModelKind::Statistics,4000,true);
    unsigned said=0;
    for(const auto& s:drive(fixed,20,5,4000)){CHECK(s.setReorderUs==0);said+=s.explicitWins;}
    CHECK(said==1&&!fixed.appliesReorder());
}
TEST(slipstream_ini_smartlink_key_passes_its_value){
    namespace en=veyra::engine;
    auto s=en::parseSlipstreamSettings("smartlink = apply\n");
    CHECK(s.valid&&s.switches.size()==1&&s.switches[0].name=="VEYRA_EXP_RP_SMARTLINK"&&s.switches[0].value=="apply");
    CHECK(smartlink::parseMode(std::string_view(s.switches[0].value))==smartlink::Mode::Apply);
    s=en::parseSlipstreamSettings("VEYRA_EXP_RP_SMARTLINK=observe\n");
    CHECK(s.valid&&s.switches.size()==1&&s.switches[0].value=="observe");
    s=en::parseSlipstreamSettings("SmartLink=\n");CHECK(s.valid&&s.switches.empty()&&s.heldOff.size()==1&&s.heldOff[0]=="VEYRA_EXP_RP_SMARTLINK");
    s=en::parseSlipstreamSettings("smartlink=1\nVEYRA_EXP_RP_SMARTLINK_TRACE=1\n");
    CHECK(s.valid&&s.switches.size()==2&&s.switches[0].value=="1");
    CHECK(smartlink::parseMode(std::string_view(s.switches[0].value))==smartlink::Mode::Observe);
}
