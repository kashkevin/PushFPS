// SPDX-License-Identifier: GPL-3.0-only
// Included by CoreTests.cpp: the experimental same-LAN transport profile
// (TransportTuning.h). Portable logic only; nothing here talks to a console.
#include "veyra/remoteplay/TransportTuning.h"
#include "veyra/remoteplay/ConnectionHealth.h"
#include <cmath>
// --- Switches (VEYRA_EXP_RP_LAN_PROFILE, _REORDER_US, _RCVBUF, _THREAD_PRIORITY, _TIMER_1MS) ---
TEST(lan_switches_unset_change_nothing){
    const auto e=resolveLanExperiments({});
    CHECK(!e.any());CHECK(!e.usesTransport());CHECK(!e.lanProfile);
    CHECK(e.transport==TransportTuning{});
    // Zero is "library default": the patched library then behaves as the unpatched one.
    CHECK(e.transport.reorderTimeoutUs==0&&e.transport.receiveBufferBytes==0&&!e.transport.threadPriority);
    CHECK(e.transport.effectiveReorderUs()==16000);CHECK(e.transport.effectiveReceiveBuffer()==0x19000);
    CHECK(!e.reorderOverrideIgnored&&!e.receiveBufferOverrideIgnored);
}
TEST(lan_transport_counters_start_at_zero){
    // 0 everywhere means "no connection yet" / "did not happen", including the
    // receive buffer fallback of chiaki patch 0004 (review N2).
    const TransportCounters c;
    CHECK(c.receiveBufferRequested==0&&c.receiveBufferEffective==0&&c.receiveBufferFallbacks==0);
    CHECK(c.reorderTimeoutUs==0&&c.threadsRaised==0&&c.threadPriorityFailures==0&&c.avPackets==0);
}
TEST(lan_switches_empty_and_zero_are_off){
    TransportSwitches s;s.lanProfile="";s.threadPriority="0";
    auto e=resolveLanExperiments(s);CHECK(!e.any());CHECK(e.transport==TransportTuning{});
    s.lanProfile="0";e=resolveLanExperiments(s);CHECK(!e.lanProfile);
    s.lanProfile="1";s.threadPriority="yes";e=resolveLanExperiments(s);CHECK(e.lanProfile&&e.transport.threadPriority);
}
TEST(lan_profile_defaults_and_overrides){
    TransportSwitches s;s.lanProfile="1";
    auto e=resolveLanExperiments(s);
    CHECK(e.any()&&e.usesTransport());CHECK(e.transport.reorderTimeoutUs==2000);CHECK(e.transport.receiveBufferBytes==1024u*1024u);
    CHECK(!e.transport.threadPriority);
    s.reorderUs="500";s.receiveBuffer="102400";e=resolveLanExperiments(s);
    CHECK(e.transport.reorderTimeoutUs==500&&e.transport.receiveBufferBytes==102400);
    s.reorderUs="16000";s.receiveBuffer="8388608";e=resolveLanExperiments(s);
    CHECK(e.transport.reorderTimeoutUs==16000&&e.transport.receiveBufferBytes==8388608);
    CHECK(!e.reorderOverrideIgnored&&!e.receiveBufferOverrideIgnored);
}
TEST(lan_profile_refuses_overrides_outside_the_range){
    for(const char* bad:{"499","16001","0","","-2000","2000us"," 2000","2e3","99999999999"}){
        TransportSwitches s;s.lanProfile="1";s.reorderUs=bad;
        const auto e=resolveLanExperiments(s);
        CHECK(e.transport.reorderTimeoutUs==2000);CHECK(e.reorderOverrideIgnored);CHECK(!e.receiveBufferOverrideIgnored);
    }
    for(const char* bad:{"102399","8388609","0","1M","4294967296"}){
        TransportSwitches s;s.lanProfile="1";s.receiveBuffer=bad;
        const auto e=resolveLanExperiments(s);
        CHECK(e.transport.receiveBufferBytes==1024u*1024u);CHECK(e.receiveBufferOverrideIgnored);CHECK(!e.reorderOverrideIgnored);
    }
}
TEST(lan_overrides_without_the_profile_do_nothing){
    TransportSwitches s;s.reorderUs="1000";s.receiveBuffer="2097152";
    const auto e=resolveLanExperiments(s);
    CHECK(!e.any());CHECK(e.transport==TransportTuning{});
    CHECK(e.reorderOverrideIgnored&&e.receiveBufferOverrideIgnored);
}
TEST(lan_thread_priority_is_its_own_switch){
    TransportSwitches s;s.threadPriority="1";
    const auto e=resolveLanExperiments(s);
    CHECK(e.any()&&e.usesTransport()&&!e.lanProfile);
    CHECK(e.transport.threadPriority);CHECK(e.transport.reorderTimeoutUs==0&&e.transport.receiveBufferBytes==0);
}
TEST(lan_timer_resolution_is_its_own_switch){
    // Review N1: one switch per idea, so that an A/B can say which of the two did it.
    TransportSwitches s;s.timer1ms="1";
    auto e=resolveLanExperiments(s);
    CHECK(e.timer1ms&&e.any());CHECK(!e.usesTransport());                         // the owner's own call, nothing in the library
    CHECK(!e.lanProfile&&!e.transport.threadPriority);CHECK(e.transport==TransportTuning{});
    CHECK(e.describe().find("threadPriority=unchanged timer1ms=on")!=std::string::npos);
    QueueLimits l;e.applyTo(l);CHECK(l.backlogFrames==0&&l.maxFrames==4&&l.idrFirstRetry100ns==0);
    // Thread priority no longer raises the timer resolution.
    s={};s.threadPriority="1";e=resolveLanExperiments(s);
    CHECK(e.transport.threadPriority&&!e.timer1ms);
    CHECK(e.describe().find("threadPriority=above-normal timer1ms=off")!=std::string::npos);
    // The LAN profile implies neither of them.
    s={};s.lanProfile="1";e=resolveLanExperiments(s);
    CHECK(e.lanProfile&&!e.transport.threadPriority&&!e.timer1ms);
    CHECK(e.describe().find("threadPriority=unchanged timer1ms=off")!=std::string::npos);
    for(const char* off:{"","0"}){s={};s.timer1ms=off;e=resolveLanExperiments(s);CHECK(!e.timer1ms&&!e.any());}
    CHECK(!resolveLanExperiments({}).timer1ms);
}
TEST(lan_switch_numbers_are_strict){
    CHECK(switchNumber(std::string("0"))==0u);CHECK(switchNumber(std::string("4294967295"))==4294967295u);
    CHECK(switchNumber(std::string("9999999999"))==9999999999u);
    for(const char* bad:{"","+1","-1","1 ","0x10","1.5","12345678901"})CHECK(!switchNumber(std::string(bad)));
    CHECK(!switchNumber(std::nullopt));
}
TEST(lan_experiments_describe_effective_values){
    TransportSwitches s;s.lanProfile="1";s.reorderUs="3000";s.receiveBuffer="7";s.threadPriority="1";
    const auto text=resolveLanExperiments(s).describe();
    CHECK(text.find("lanProfile=on")!=text.npos);CHECK(text.find("reorderUs=3000")!=text.npos);
    CHECK(text.find("receiveBuffer=1048576")!=text.npos);CHECK(text.find("threadPriority=above-normal")!=text.npos);
    CHECK(text.find("VEYRA_EXP_RP_RCVBUF ignored")!=text.npos);CHECK(text.find("VEYRA_EXP_RP_REORDER_US ignored")==text.npos);
    const auto off=resolveLanExperiments({}).describe();
    CHECK(off.find("lanProfile=off reorderUs=16000 receiveBuffer=102400 threadPriority=unchanged timer1ms=off")==0);
}
TEST(lan_health_switch_observes_without_tuning){
    TransportSwitches s;s.health="1";
    const auto e=resolveLanExperiments(s);
    // The counters are needed, so the library is configured, but with its own values.
    CHECK(e.health&&e.any()&&e.usesTransport());CHECK(e.transport==TransportTuning{});CHECK(!e.lanProfile);
    CHECK(e.describe().find("health=on")!=std::string::npos);
    CHECK(!resolveLanExperiments({}).health);
}
// --- Connection health (VEYRA_EXP_RP_HEALTH, ConnectionHealth.h) ---
namespace {
bool closeTo(double value,double expected,double tolerance){return std::fabs(value-expected)<=tolerance;}
constexpr HostTime kSecond=10000000;
// Five seconds of a clean 60 fps, 15 Mbit/s stream, added to the totals.
void cleanInterval(HealthCounters& c){c.packetsReceived+=10000;c.framesReceived+=300;c.bytesReceived+=9375000;}
}
TEST(health_jitter_is_zero_on_an_even_cadence){
    FrameJitter j;
    for(std::uint64_t i=0;i<200;++i)j.observe(i,static_cast<HostTime>(i)*200000,50); // 50 fps: 20 ms exactly
    CHECK(j.samples()==199);CHECK(j.jitterMs()==0.0);CHECK(j.peakMs()==0.0);
}
TEST(health_jitter_converges_on_the_mean_deviation){
    // Every second frame 2 ms late: |D| is 2 ms each time, so RFC 3550's J tends to 2 ms.
    FrameJitter j;
    for(std::uint64_t i=0;i<400;++i)j.observe(i,static_cast<HostTime>(i)*200000+(i%2?20000:0),50);
    CHECK(closeTo(j.jitterMs(),2.0,0.01));CHECK(closeTo(j.peakMs(),2.0,1e-9));
    // One sample moves the estimate by a sixteenth of the difference.
    FrameJitter one;one.observe(0,0,50);one.observe(1,200000+16000,50);
    CHECK(closeTo(one.jitterMs(),0.1,1e-9));
}
TEST(health_jitter_follows_the_wire_index_across_wrap_loss_and_duplicates){
    FrameJitter j;HostTime t=0;
    const std::uint16_t order[]={65533,65534,65535,0,1,2};
    for(const auto index:order){j.observeWire(index,t,50);t+=200000;}
    CHECK(j.samples()==5);CHECK(j.jitterMs()==0.0);
    // Three frames lost: the next one is three intervals later, which is on time.
    j.observeWire(6,t+3*200000,50);CHECK(j.samples()==6);CHECK(j.jitterMs()==0.0);
    // A duplicate, an older frame and a clock that stepped back are not samples.
    j.observeWire(6,t+4*200000,50);j.observeWire(3,t+5*200000,50);j.observeWire(7,0,50);
    CHECK(j.samples()==6);CHECK(j.jitterMs()==0.0);
    // No frame rate, no measurement.
    FrameJitter none;none.observeWire(0,0,0);none.observeWire(1,999,0);CHECK(none.samples()==0);
}
TEST(health_jitter_peak_forgets_an_old_spike){
    FrameJitter j;HostTime t=0;std::uint64_t i=0;
    for(;i<10;++i,t+=200000)j.observe(i,t,50);
    j.observe(i++,t+50000,50);t+=200000;                      // one frame 5 ms late
    CHECK(closeTo(j.peakMs(),5.0,1e-9));
    for(int k=0;k<600;++k,++i,t+=200000)j.observe(i,t,50);
    CHECK(j.peakMs()==0.0);CHECK(j.jitterMs()<0.001);
    j.reset();CHECK(j.samples()==0&&j.jitterMs()==0.0);
}
TEST(health_ipv4_parsing_is_strict){
    CHECK(parseIpv4("192.168.1.20")==0xC0A80114u);CHECK(parseIpv4("0.0.0.0")==0u);CHECK(parseIpv4("255.255.255.255")==0xFFFFFFFFu);
    for(const char* bad:{"","192.168.1","192.168.1.20.","192.168.1.20.1","192.168.01.20","256.1.1.1","1.2.3.4:9295","ps5.local","1..2.3"," 1.2.3.4","1.2.3.4 ","1.2.3.-4"})
        CHECK(!parseIpv4(bad));
}
TEST(health_private_ranges){
    for(const char* yes:{"10.0.0.1","10.255.255.254","172.16.0.1","172.31.255.1","192.168.0.1","192.168.255.9","169.254.10.10"})CHECK(privateIpv4(*parseIpv4(yes)));
    for(const char* no:{"9.255.255.255","11.0.0.1","172.15.255.255","172.32.0.1","192.167.1.1","192.169.1.1","8.8.8.8","100.64.0.1","127.0.0.1"})CHECK(!privateIpv4(*parseIpv4(no)));
}
TEST(health_route_direct_lan_needs_private_and_on_link){
    const LocalInterface ethernet{*parseIpv4("192.168.1.10"),24,LinkKind::Ethernet,1000000000u};
    const LocalInterface wifi{*parseIpv4("192.168.50.7"),24,LinkKind::WiFi,866000000u};
    const LocalInterface both[]={ethernet,wifi};
    auto r=assessRoute("192.168.1.20",both);
    CHECK(r.directLan()&&!r.wifi());CHECK(r.link==LinkKind::Ethernet&&r.linkBitsPerSecond==1000000000u);
    CHECK(std::string_view(r.name())=="direct-lan"&&std::string_view(r.linkName())=="ethernet");
    r=assessRoute("192.168.50.20",both);CHECK(r.directLan()&&r.wifi());CHECK(std::string_view(r.linkName())=="wifi");
    // Private but behind a router: not on any local subnet.
    r=assessRoute("10.8.0.5",both);CHECK(r.addressKnown&&r.privateRange&&!r.onLink&&!r.directLan()&&!r.wifi());
    CHECK(std::string_view(r.name())=="private-routed"&&r.link==LinkKind::Unknown);
    r=assessRoute("93.184.216.34",both);CHECK(!r.directLan());CHECK(std::string_view(r.name())=="not-local");
    // A host name is not resolved here: nothing is claimed about it.
    r=assessRoute("ps5.local",both);CHECK(!r.addressKnown&&!r.directLan());CHECK(std::string_view(r.name())=="unknown-host-name");
    r=assessRoute("192.168.1.20",{});CHECK(r.privateRange&&!r.onLink&&!r.directLan());
}
TEST(health_route_longest_prefix_and_odd_prefixes){
    const LocalInterface wide{*parseIpv4("10.0.0.2"),8,LinkKind::WiFi,0};
    const LocalInterface narrow{*parseIpv4("10.1.2.3"),24,LinkKind::Ethernet,2500000000u};
    const LocalInterface a[]={wide,narrow},b[]={narrow,wide};
    CHECK(assessRoute("10.1.2.50",a).link==LinkKind::Ethernet);CHECK(assessRoute("10.1.2.50",b).link==LinkKind::Ethernet);
    CHECK(assessRoute("10.9.9.9",a).wifi());
    // Point-to-point and unknown prefixes never count as a shared subnet.
    for(const std::uint8_t prefix:{std::uint8_t{0},std::uint8_t{31},std::uint8_t{32},std::uint8_t{200}}){
        const LocalInterface odd[]={{*parseIpv4("192.168.1.10"),prefix,LinkKind::Ethernet,0}};
        CHECK(!assessRoute("192.168.1.10",odd).onLink);
    }
    // A public address on the local subnet is on-link but not called a direct LAN.
    const LocalInterface pub[]={{*parseIpv4("93.184.216.1"),24,LinkKind::Ethernet,0}};
    const auto r=assessRoute("93.184.216.34",pub);CHECK(r.onLink&&!r.directLan());CHECK(std::string_view(r.name())=="on-link-public-address");
}
TEST(health_interval_reports_loss_rates_and_occupancy){
    ConnectionHealth h;h.begin(100);
    HealthCounters c;c.packetsReceived=9900;c.packetsLost=100;c.framesReceived=300;c.bytesReceived=9375000;
    c.reorderGaps=7;c.reorderTimeouts=2;c.reorderSkipped=5;c.reorderDropped=3;c.fecRecovered=4;c.fecFailed=1;c.keyframeRequests=1;
    c.decoderResets=1;c.framesLost=2;c.ingressDropped=6;c.mailboxOverwrites=9;c.ingressDepth=1;c.frameJitterMs=0.4;c.frameJitterPeakMs=3.5;
    CHECK(!h.due(100+5*kSecond-1));CHECK(!h.sample(100+5*kSecond-1,c));CHECK(!h.latest().valid);
    h.observeIngressDepth(3);
    CHECK(h.due(100+5*kSecond));
    const auto s=h.sample(100+5*kSecond,c);
    CHECK(s&&s->valid&&s->sequence==1);CHECK(closeTo(s->seconds,5.0,1e-9));
    CHECK(s->packetsReceived==9900&&s->packetsLost==100);CHECK(closeTo(s->lossPercent,1.0,1e-9));
    CHECK(s->reordered==5&&s->reorderTimeouts==2&&s->reorderSkipped==5&&s->late==3);
    CHECK(s->fecRecovered==4&&s->fecFailed==1&&s->keyframeRequests==1&&s->decoderResets==1);CHECK(s->recoveryEvents()==6);
    CHECK(s->networkFrameDrops==8);CHECK(s->mailboxOverwrites==9);
    CHECK(s->ingressDepth==1&&s->ingressPeak==3);
    CHECK(closeTo(s->receivedMbps,15.0,1e-9));CHECK(closeTo(s->receivedFps,60.0,1e-9));
    CHECK(closeTo(s->frameJitterMs,0.4,1e-12)&&closeTo(s->frameJitterPeakMs,3.5,1e-12));CHECK(!s->rttMs);
    CHECK(h.latest().valid&&h.latest().sequence==1);
    // The next interval reports only what happened in it.
    cleanInterval(c);c.ingressDepth=0;c.rttMs=1.25;
    const auto next=h.sample(100+10*kSecond+kSecond/2,c);
    CHECK(next&&next->sequence==2);CHECK(closeTo(next->seconds,5.5,1e-9));
    CHECK(next->packetsReceived==10000&&next->packetsLost==0&&next->lossPercent==0.0);
    CHECK(next->reordered==0&&next->late==0&&next->fecRecovered==0&&next->networkFrameDrops==0&&next->mailboxOverwrites==0);
    CHECK(next->ingressPeak==1);                                // the depth the last interval ended with
    CHECK(next->rttMs&&closeTo(*next->rttMs,1.25,1e-12));
    // A clock that stepped back restarts the interval instead of reporting nonsense.
    CHECK(h.due(50));CHECK(!h.sample(50,c));CHECK(!h.due(50+kSecond));
}
TEST(health_grade_thresholds){
    HealthSnapshot s;
    CHECK(ConnectionHealth::gradeInterval(s)==HealthGrade::Unknown);              // no packets: nothing to judge
    s.packetsReceived=10000;CHECK(ConnectionHealth::gradeInterval(s)==HealthGrade::Stable);
    auto unsteady=s;unsteady.lossPercent=0.1;CHECK(ConnectionHealth::gradeInterval(unsteady)==HealthGrade::Unsteady);
    unsteady=s;unsteady.reorderTimeouts=1;CHECK(ConnectionHealth::gradeInterval(unsteady)==HealthGrade::Unsteady);
    unsteady=s;unsteady.networkFrameDrops=1;CHECK(ConnectionHealth::gradeInterval(unsteady)==HealthGrade::Unsteady);
    unsteady=s;unsteady.frameJitterMs=4.0;CHECK(ConnectionHealth::gradeInterval(unsteady)==HealthGrade::Unsteady);
    auto unstable=s;unstable.lossPercent=1.0;CHECK(ConnectionHealth::gradeInterval(unstable)==HealthGrade::Unstable);
    unstable=s;unstable.decoderResets=2;CHECK(ConnectionHealth::gradeInterval(unstable)==HealthGrade::Unstable);
    unstable=s;unstable.frameJitterMs=10.0;CHECK(ConnectionHealth::gradeInterval(unstable)==HealthGrade::Unstable);
    // FEC doing its job and packets that merely arrived out of order are not instability.
    auto fine=s;fine.fecRecovered=20;fine.reordered=50;CHECK(ConnectionHealth::gradeInterval(fine)==HealthGrade::Stable);
}
TEST(health_grade_falls_at_once_and_recovers_one_step_per_two_clean_intervals){
    ConnectionHealth h;h.begin(0);HealthCounters c;HostTime t=0;
    const auto step=[&](std::uint64_t lost){cleanInterval(c);c.packetsLost+=lost;t+=5*kSecond;const auto s=h.sample(t,c);CHECK(s);return *s;};
    CHECK(step(0).grade==HealthGrade::Stable);
    auto s=step(500);CHECK(s.intervalGrade==HealthGrade::Unstable&&s.grade==HealthGrade::Unstable);   // falls at once
    s=step(0);CHECK(s.intervalGrade==HealthGrade::Stable&&s.grade==HealthGrade::Unstable);            // one clean interval: held
    s=step(0);CHECK(s.grade==HealthGrade::Unsteady);                                                  // two: one step
    s=step(0);CHECK(s.grade==HealthGrade::Unsteady);
    s=step(0);CHECK(s.grade==HealthGrade::Stable);
    // A bad interval in the middle of the climb restarts the count.
    s=step(500);CHECK(s.grade==HealthGrade::Unstable);
    s=step(0);CHECK(s.grade==HealthGrade::Unstable);
    s=step(20);CHECK(s.intervalGrade==HealthGrade::Unsteady&&s.grade==HealthGrade::Unsteady);         // two better intervals: one step, no further
    s=step(20);CHECK(s.grade==HealthGrade::Unsteady);                                                 // equal to the grade: stays
    s=step(0);CHECK(s.grade==HealthGrade::Unsteady);
    s=step(20);CHECK(s.grade==HealthGrade::Unsteady);                                                 // not better: the count starts again
    s=step(0);CHECK(s.grade==HealthGrade::Unsteady);
    // An interval with no packets at all says nothing and changes nothing.
    t+=5*kSecond;const auto silent=h.sample(t,c);CHECK(silent&&silent->intervalGrade==HealthGrade::Unknown&&silent->grade==HealthGrade::Unsteady);
}
TEST(health_reconnect_restarts_counters_and_keeps_the_grade){
    ConnectionHealth h;h.begin(0);HealthCounters c;cleanInterval(c);c.packetsLost=900;
    const auto bad=h.sample(5*kSecond,c);CHECK(bad&&bad->grade==HealthGrade::Unstable);
    // New attempt: the library's counters start again from zero.
    RouteAssessment route;route.addressKnown=route.privateRange=route.onLink=true;route.link=LinkKind::WiFi;
    h.begin(20*kSecond,route);CHECK(!h.latest().valid);
    HealthCounters fresh;cleanInterval(fresh);
    const auto first=h.sample(25*kSecond,fresh);
    CHECK(first&&first->sequence==1&&first->packetsReceived==10000&&first->packetsLost==0);
    CHECK(first->intervalGrade==HealthGrade::Stable&&first->grade==HealthGrade::Unstable); // a reconnect is not a recovery
    CHECK(first->route.wifi()&&first->route==route);
    // A counter that went backwards without begin() is taken as a restart, not as a huge number.
    HealthCounters smaller;smaller.packetsReceived=40;
    const auto wrapped=h.sample(30*kSecond,smaller);CHECK(wrapped&&wrapped->packetsReceived==40);
}
TEST(health_counters_come_from_the_library_and_the_inbox){
    TransportCounters t;t.congestionReceived=5000;t.congestionLost=12;t.congestionLostReported=10;t.reorderGaps=4;t.reorderTimeouts=1;t.reorderSkipped=2;t.reorderDropped=3;
    t.fecRecovered=6;t.fecFailed=1;t.idrRequests=2;t.avPackets=9999;
    SessionInbox::Snapshot s;s.video.recoveries=3;s.video.dropped=8;s.video.accessUnits=600;s.video.depth=2;s.framesLost=5;s.receivedBytes=123456;
    s.frameJitterMs=0.7;s.frameJitterPeakMs=2.5;
    auto c=healthCounters(t,s,11);
    CHECK(c.packetsReceived==5000&&c.packetsLost==12);          // measured loss, not the capped figure the console is told
    CHECK(c.reorderGaps==4&&c.reorderTimeouts==1&&c.reorderSkipped==2&&c.reorderDropped==3);
    CHECK(c.fecRecovered==6&&c.fecFailed==1&&c.keyframeRequests==2);
    CHECK(c.decoderResets==3&&c.framesLost==5&&c.ingressDropped==8&&c.framesReceived==600&&c.bytesReceived==123456);
    CHECK(c.mailboxOverwrites==11&&c.ingressDepth==2);CHECK(c.frameJitterMs==0.7&&c.frameJitterPeakMs==2.5);
    CHECK(!c.rttMs);                                            // the library did not measure it: not invented
    t.senkushaRttUs=1250;c=healthCounters(t,s,0);CHECK(c.rttMs&&closeTo(*c.rttMs,1.25,1e-12));
}
TEST(health_log_line_has_fixed_keys_and_no_address){
    ConnectionHealth h;RouteAssessment route=assessRoute("192.168.1.20",std::span<const LocalInterface>{});
    const LocalInterface local[]={{*parseIpv4("192.168.1.10"),24,LinkKind::Ethernet,1000000000u}};
    route=assessRoute("192.168.1.20",local);
    h.begin(0,route);HealthCounters c;cleanInterval(c);c.packetsLost=3;c.fecRecovered=2;
    const auto s=h.sample(5*kSecond,c);CHECK(s);
    const auto line=formatHealthLine(*s);
    for(const char* key:{"grade=stable","interval=stable","seconds=5.0","lossPercent=0.030","lost=3","received=10000","frameJitterMs=0.00","reordered=0","late=0",
        "reorderTimeouts=0","fecRecovered=2","fecFailed=0","keyframeRequests=0","decoderResets=0","frameDrops=0","ingressDepth=0","ingressPeak=0",
        "mailboxOverwrites=0","mbps=15.00","fps=60.0","rtt=not-measured","route=direct-lan","link=ethernet","linkMbps=1000"})
        CHECK(line.find(key)!=line.npos);
    CHECK(line.find("192.168")==line.npos);
    c.rttMs=0.8;cleanInterval(c);
    const auto timed=h.sample(10*kSecond,c);CHECK(timed);CHECK(formatHealthLine(*timed).find("rtt=0.80ms(at-connect)")!=std::string::npos);
    CHECK(std::string_view(healthGradeName(HealthGrade::Unknown))=="unknown"&&std::string_view(healthGradeName(HealthGrade::Unsteady))=="unsteady"&&std::string_view(healthGradeName(HealthGrade::Unstable))=="unstable");
}
TEST(inbox_measures_frame_jitter_per_session){
    SessionInbox q;auto t=q.begin(monotonic100ns());
    const auto g=t.generation();
    // Arrival stamps in the past, 60 fps, every second frame 3 ms late.
    const HostTime base=monotonic100ns()-20*kSecond;
    CHECK(t.video(config(g,base)));
    for(std::uint16_t i=0;i<300;++i)(void)t.video(frame(i,i%4==0,base+HostTime(i)*166667+(i%2?30000:0),g));
    const auto s=q.snapshot();
    CHECK(closeTo(s.frameJitterMs,3.0,0.05));CHECK(closeTo(s.frameJitterPeakMs,3.0,0.001));
    q.invalidate();q.finishStop();
    auto next=q.begin(monotonic100ns());CHECK(q.snapshot().frameJitterMs==0.0&&q.snapshot().frameJitterPeakMs==0.0);
    (void)next;
}
// --- Bounded decode backlog (VEYRA_EXP_RP_BACKLOG, QueueLimits::backlogFrames) ---
namespace {
QueueLimits backlogLimits(){QueueLimits l;l.backlogFrames=16;return l;}
constexpr HostTime kMs=10000;
// The overflow, age and keyframe rules of VideoIngress as they were before the
// backlog experiment, written out again for access units with a stored
// configuration and consecutive frame numbers.
struct OriginalQueueRule {
    std::deque<HostTime> arrivals;bool waiting=true;std::uint64_t recoveries=0,dropped=0,epoch=1;
    std::size_t maxFrames=4;HostTime maxAge=1000000;
    void recover(){dropped+=arrivals.size();arrivals.clear();if(!waiting){++epoch;++recoveries;}waiting=true;}
    bool expired(HostTime now)const{return !arrivals.empty()&&(now<arrivals.front()||now-arrivals.front()>maxAge);}
    PushStatus push(bool idr,HostTime arrival,HostTime now){
        if(now-arrival>maxAge){++dropped;recover();return PushStatus::WaitingForIdr;}
        if(expired(now)||arrivals.size()>=maxFrames)recover();
        if(waiting&&!idr){++dropped;return PushStatus::WaitingForIdr;}
        arrivals.push_back(arrival);waiting=false;return PushStatus::Accepted;
    }
    bool pop(HostTime now){if(expired(now)){recover();return false;}if(arrivals.empty())return false;arrivals.pop_front();return true;}
};
}
TEST(backlog_switch_sets_the_queue_limit_and_nothing_else){
    QueueLimits l;resolveLanExperiments({}).applyTo(l);
    CHECK(l.backlogFrames==0&&l.maxFrames==4&&l.maxAge100ns==1000000&&l.maxBytes==8u*1024u*1024u);
    TransportSwitches s;s.backlog="0";resolveLanExperiments(s).applyTo(l);CHECK(l.backlogFrames==0);
    s.backlog="1";const auto e=resolveLanExperiments(s);
    CHECK(e.backlog&&e.any()&&!e.usesTransport());                              // no library call is needed for it
    CHECK(e.transport==TransportTuning{});
    e.applyTo(l);CHECK(l.backlogFrames==16&&l.maxFrames==4&&l.maxAge100ns==1000000);
    l.validate();
    CHECK(e.describe().find("backlogFrames=16")!=std::string::npos);
    // A queue already as deep as the backlog has nothing to gain and stays valid.
    QueueLimits deep;deep.maxFrames=16;e.applyTo(deep);CHECK(deep.backlogFrames==0);deep.validate();
}
TEST(backlog_limits_validation){
    QueueLimits l;CHECK(l.backlogFrames==0);                                      // off unless asked
    for(const std::size_t bad:{std::size_t{1},std::size_t{4},std::size_t{65}}){l={};l.backlogFrames=bad;THROWS(VideoIngress{l});}
    for(const std::size_t good:{std::size_t{0},std::size_t{5},std::size_t{16},std::size_t{64}}){l={};l.backlogFrames=good;VideoIngress q(l);(void)q;}
}
TEST(backlog_off_drops_a_burst_and_asks_for_a_keyframe){
    VideoIngress q;start(q);
    for(std::uint16_t i=1;i<4;++i)CHECK(q.push(frame(i,false),0)==PushStatus::Accepted);
    CHECK(q.push(frame(4,false),0)==PushStatus::WaitingForIdr);                   // the fifth frame of a burst
    const auto s=q.stats();
    CHECK(s.depth==0&&s.waitingForIdr&&s.recoveries==1&&s.dropped==5);CHECK(s.backlogEpisodes==0&&s.backlogFrames==0);
    CHECK(q.takeIdrRequest(0));
}
TEST(backlog_decodes_through_a_burst_within_the_ceiling){
    // Twelve frames at once: what a 200 ms network stall releases at 60 fps.
    VideoIngress q(backlogLimits());start(q);
    for(std::uint16_t i=1;i<12;++i)CHECK(q.push(frame(i,false),0)==PushStatus::Accepted);
    auto s=q.stats();
    CHECK(s.depth==12&&!s.waitingForIdr&&s.recoveries==0&&s.dropped==0);CHECK(s.backlogEpisodes==1&&s.backlogFrames==8);
    CHECK(!q.takeIdrRequest(0));                                                  // nothing is asked of the console
    for(std::uint16_t i=0;i<12;++i){
        const auto v=q.tryPop((i+1)*kMs);                                         // the decoder takes one per millisecond
        CHECK(v&&v->sample.wireFrameIndex==i);CHECK(v->resetDecoder==(i==0));CHECK(v->epoch==1);
    }
    s=q.stats();CHECK(s.depth==0&&s.consumed==12&&s.recoveries==0&&s.highWaterDepth==12);
}
TEST(backlog_beyond_the_frame_ceiling_takes_the_original_path){
    VideoIngress q(backlogLimits());start(q);
    for(std::uint16_t i=1;i<16;++i)CHECK(q.push(frame(i,false),0)==PushStatus::Accepted);
    CHECK(q.stats().depth==16);
    CHECK(q.push(frame(16,false),0)==PushStatus::WaitingForIdr);                  // the seventeenth
    const auto s=q.stats();CHECK(s.depth==0&&s.waitingForIdr&&s.recoveries==1&&s.dropped==17);
    CHECK(q.takeIdrRequest(0));
    CHECK(q.push(frame(17,false),0)==PushStatus::WaitingForIdr);
    CHECK(q.push(frame(18,true),0)==PushStatus::Accepted);
    const auto v=q.tryPop(0);CHECK(v&&v->resetDecoder&&v->configBefore&&v->epoch==2);
}
TEST(backlog_may_not_stand_longer_than_the_age_ceiling){
    // A decoder exactly as fast as the stream but six frames behind: every frame
    // waits only 30 ms, yet the delay would stand for ever. The backlog is given
    // 100 ms to drain and is then dropped like any other overflow.
    VideoIngress q(backlogLimits());start(q);
    const HostTime step=5*kMs;HostTime t=0;std::uint16_t index=1;
    for(;index<6;++index){t+=step;CHECK(q.push(frame(index,false,t),t)==PushStatus::Accepted);}
    CHECK(q.stats().depth==6);                                                    // past four since t = 20 ms
    HostTime longestWait=0,recoveredAt=-1;
    for(int i=0;i<40&&recoveredAt<0;++i){
        t+=step;
        const auto v=q.tryPop(t);
        if(!v){recoveredAt=t;break;}
        longestWait=(std::max)(longestWait,t-v->sample.arrival100ns);
        CHECK(q.push(frame(index++,false,t),t)==PushStatus::Accepted);
    }
    CHECK(longestWait==30*kMs);
    CHECK(recoveredAt==125*kMs);                                                  // first look later than 20 ms + 100 ms
    const auto s=q.stats();CHECK(s.depth==0&&s.waitingForIdr&&s.recoveries==1&&s.backlogEpisodes==1);
    CHECK(q.takeIdrRequest(t));
    CHECK(q.push(frame(index++,false,t),t)==PushStatus::WaitingForIdr);
}
TEST(backlog_overdue_is_also_caught_by_the_producer){
    // Nobody pops at all: the next arriving frame finds the head frame too old
    // (the age rule; the episode's own allowance is tested further down).
    VideoIngress q(backlogLimits());start(q);
    std::uint16_t index=1;
    for(;index<8;++index)CHECK(q.push(frame(index,false,90*kMs),90*kMs)==PushStatus::Accepted);   // first frame is 90 ms old, the rest fresh
    CHECK(q.stats().depth==8);
    CHECK(q.push(frame(index,false,101*kMs),101*kMs)==PushStatus::WaitingForIdr);                 // the oldest is past 100 ms
    CHECK(q.stats().depth==0&&q.stats().recoveries==1);
}
TEST(backlog_drains_and_can_be_used_again){
    VideoIngress q(backlogLimits());start(q);
    std::uint16_t index=1;
    for(;index<8;++index)CHECK(q.push(frame(index,false),0)==PushStatus::Accepted);
    for(int i=0;i<8;++i)CHECK(q.tryPop(2*kMs));
    CHECK(q.stats().depth==0&&q.stats().backlogEpisodes==1);
    // Long after the first burst: its start time must not count against the second.
    const HostTime later=3000*kMs;
    for(int i=0;i<8;++i,++index)CHECK(q.push(frame(index,false,later),later)==PushStatus::Accepted);
    CHECK(q.stats().depth==8&&q.stats().backlogEpisodes==2&&q.stats().recoveries==0);
    for(int i=0;i<8;++i)CHECK(q.tryPop(later+3*kMs));
    // One above the original limit does not end the episode.
    for(int i=0;i<6;++i,++index)CHECK(q.push(frame(index,false,later+10*kMs),later+10*kMs)==PushStatus::Accepted);
    CHECK(q.tryPop(later+11*kMs));CHECK(q.stats().depth==5);
    CHECK(!q.tryPop(later+10*kMs+1000001));                                       // still five, 100 ms later: dropped
    CHECK(q.stats().recoveries==1);
}
// Review finding S1. One burst leaves five frames waiting, and from then on the
// decoder takes exactly one frame for every frame that arrives: depth 5,4,5,4...
// A drain to exactly the original limit must not end the episode, or every
// arriving frame opens a new 100 ms allowance and the extra delay stands for ever.
namespace {
struct MatchedDecoderRun {
    HostTime worstBefore=0,worstTail=0,worstWait=0,recoveredAfter=-1;
    std::uint64_t tailFrames=0;VideoQueueStats stats;
};
// 60 fps. Frames 56..59 are held by the network and arrive together with frame
// 60; the decoder takes at most one frame per frame time, before or after the
// arrival of that interval. The console answers a keyframe request two frames later.
MatchedDecoderRun runMatchedDecoder(const QueueLimits& limits,bool popFirst,int framesAfterBurst){
    VideoIngress q(limits);start(q);(void)q.tryPop(0);
    const HostTime frameTime=166667;const int burst=60;
    MatchedDecoderRun r;HostTime t=0,burstAt=0;std::uint16_t index=1;int keyframeAt=-1;
    const auto take=[&](int i){
        if(q.takeIdrRequest(t))keyframeAt=i+2;
        const auto v=q.tryPop(t);
        if(!v){if(i>burst&&r.recoveredAfter<0&&q.stats().recoveries)r.recoveredAfter=t-burstAt;return;}
        const HostTime wait=t-v->sample.arrival100ns;
        r.worstWait=(std::max)(r.worstWait,wait);
        if(i<burst-4)r.worstBefore=(std::max)(r.worstBefore,wait);
        if(i>burst+framesAfterBurst-600){r.worstTail=(std::max)(r.worstTail,wait);++r.tailFrames;}
    };
    for(int i=1;i<=burst+framesAfterBurst;++i){
        t+=frameTime;
        if(popFirst)take(i);
        const int arriving=i==burst?5:(i>=burst-4&&i<burst)?0:1;
        if(i==burst)burstAt=t;
        for(int n=0;n<arriving;++n){
            const bool idr=keyframeAt>=0&&i>=keyframeAt;
            if(idr)keyframeAt=-1;
            const auto pushed=q.push(frame(index++,idr,t),t);
            if(i>burst&&r.recoveredAfter<0&&pushed==PushStatus::WaitingForIdr)r.recoveredAfter=t-burstAt;
        }
        if(!popFirst)take(i);
    }
    r.stats=q.stats();return r;
}
}
TEST(backlog_at_exactly_the_limit_does_not_restart_the_allowance){
    const auto limits=backlogLimits();const HostTime frameTime=166667;
    for(const bool popFirst:{false,true}){
        const auto r=runMatchedDecoder(limits,popFirst,720);                      // twelve seconds after the burst
        CHECK(r.stats.backlogEpisodes==1);                                        // one burst is one episode, not one per frame
        CHECK(r.stats.recoveries==1&&r.stats.idrRequests==1);
        CHECK(r.recoveredAfter>=0&&r.recoveredAfter<=limits.maxAge100ns+frameTime);   // ended about 100 ms after the burst
        CHECK(r.worstWait<=limits.maxAge100ns);
        CHECK(r.tailFrames>=590);
        CHECK(r.worstBefore==(popFirst?frameTime:0));
        CHECK(r.worstTail==r.worstBefore);                                        // waits are back at the level before the burst
        CHECK(r.stats.depth<=1);
    }
    // Backlog off: the fifth frame drops the queue as always; the same end state.
    const QueueLimits off;
    const auto r=runMatchedDecoder(off,false,720);
    CHECK(r.stats.backlogEpisodes==0&&r.stats.recoveries==1&&r.worstTail==r.worstBefore);
}
// Review finding S2. The owner reads the clock, the Takion thread pushes a frame
// a moment later, and the owner then calls tryPop with its older value. A clock
// value from before the event it is compared with says nothing about age.
TEST(backlog_stale_consumer_clock_is_not_an_overdue_backlog){
    VideoIngress q(backlogLimits());start(q);
    for(std::uint16_t i=1;i<4;++i)CHECK(q.push(frame(i,false,900),900)==PushStatus::Accepted);
    CHECK(q.push(frame(4,false,1001),1001)==PushStatus::Accepted);                // the fifth: the episode starts at 1001
    CHECK(q.stats().depth==5&&q.stats().backlogEpisodes==1);
    const auto v=q.tryPop(1000);                                                  // read before that push
    CHECK(v&&v->sample.wireFrameIndex==0);
    const auto s=q.stats();
    CHECK(s.recoveries==0&&s.dropped==0&&s.depth==4&&!s.waitingForIdr&&s.epoch==1);
    CHECK(!q.takeIdrRequest(1001));                                               // nothing is asked of the console
    // The allowance itself is unchanged: it runs out 100 ms after 1001. Frames
    // are taken and replaced on the way so that none of them is itself too old.
    std::uint16_t index=5;HostTime t=1001;
    for(int i=0;i<9;++i){t+=10*kMs;CHECK(q.push(frame(index++,false,t),t)==PushStatus::Accepted);CHECK(q.tryPop(t));}
    CHECK(q.stats().depth==4&&q.stats().recoveries==0&&q.stats().backlogEpisodes==1);
    CHECK(q.push(frame(index++,false,t),t)==PushStatus::Accepted);
    CHECK(q.tryPop(1001+100*kMs));                                                // exactly 100 ms: not yet
    CHECK(!q.tryPop(1001+100*kMs+1));CHECK(q.stats().recoveries==1);
}
TEST(stale_consumer_clock_and_the_age_rule){
    // The same race against the age rule that was there before this branch: the
    // head frame arrived after the owner read its clock. Found in review; the
    // default is deliberately left as it was (a recovery and a keyframe request)
    // until it has been looked at on hardware, and the backlog switch tolerates it.
    VideoIngress off;off.begin(1);CHECK(off.push(config(),0)==PushStatus::ConfigStored);
    CHECK(off.push(frame(0,true,1001),1001)==PushStatus::Accepted);
    CHECK(!off.tryPop(1000));
    CHECK(off.stats().recoveries==1&&off.stats().dropped==1&&off.stats().waitingForIdr);   // original behaviour, unchanged
    CHECK(off.takeIdrRequest(1001));
    VideoIngress on(backlogLimits());on.begin(1);CHECK(on.push(config(),0)==PushStatus::ConfigStored);
    CHECK(on.push(frame(0,true,1001),1001)==PushStatus::Accepted);
    const auto v=on.tryPop(1000);
    CHECK(v&&v->sample.wireFrameIndex==0&&v->resetDecoder);
    CHECK(on.stats().recoveries==0&&on.stats().dropped==0&&!on.stats().waitingForIdr);
    CHECK(!on.takeIdrRequest(1001));
    // A frame that really is too old is still dropped with the switch on.
    CHECK(on.push(frame(1,false,2000),2000)==PushStatus::Accepted);
    CHECK(!on.tryPop(2000+1000001));CHECK(on.stats().recoveries==1&&on.stats().dropped==1);
}
// Review N4: two changes to the code that no test noticed.
TEST(backlog_overdue_is_caught_by_the_producer_while_the_head_is_young){
    // The decoder keeps taking frames, so the oldest one is never old, but the
    // queue has been past the limit for more than 100 ms. With no tryPop in
    // between, the arriving frame itself must end it. (The older test of this
    // name is ended by the age of the head frame, not by this check.)
    VideoIngress q(backlogLimits());start(q);CHECK(q.tryPop(0));
    std::uint16_t index=1;HostTime t=0;
    for(int i=0;i<6;++i)CHECK(q.push(frame(index++,false,0),0)==PushStatus::Accepted);   // six at once: the episode starts at 0
    for(int i=0;i<10;++i){
        t+=10*kMs;
        CHECK(q.push(frame(index++,false,t),t)==PushStatus::Accepted);            // also at exactly 100 ms
        const auto v=q.tryPop(t);CHECK(v&&t-v->sample.arrival100ns<=60*kMs);
    }
    CHECK(q.stats().depth==6&&q.stats().recoveries==0&&q.stats().backlogEpisodes==1);
    t+=10*kMs;                                                                    // 110 ms; the head arrived at 50 ms
    CHECK(q.push(frame(index++,false,t),t)==PushStatus::WaitingForIdr);
    const auto s=q.stats();
    CHECK(s.depth==0&&s.waitingForIdr&&s.recoveries==1&&s.dropped==7&&s.epoch==2);
    CHECK(q.takeIdrRequest(t));
}
TEST(backlog_keeps_every_other_recovery_rule){
    VideoIngress q(backlogLimits());start(q);
    for(std::uint16_t i=1;i<8;++i)CHECK(q.push(frame(i,false),0)==PushStatus::Accepted);
    CHECK(q.push(frame(9,false),0)==PushStatus::WaitingForIdr);                   // frame 8 never came: references lost
    CHECK(q.stats().depth==0&&q.stats().recoveries==1);
    CHECK(q.push(frame(10,true),0)==PushStatus::Accepted);
    for(std::uint16_t i=11;i<18;++i)CHECK(q.push(frame(i,false),0)==PushStatus::Accepted);
    q.requireKeyframe();                                                          // decoder error
    CHECK(q.stats().depth==0&&q.stats().waitingForIdr&&q.stats().recoveries==2);
    CHECK(q.push(frame(18,true),0)==PushStatus::Accepted);
    auto lost=frame(19,false);lost.framesLost=1;CHECK(q.push(std::move(lost),0)==PushStatus::WaitingForIdr);
    CHECK(q.push(frame(20,true),0)==PushStatus::Accepted);
    CHECK(q.push(frame(21,false,0),1000001)==PushStatus::WaitingForIdr);          // a frame that is itself too old
    // The per-frame age ceiling is the original one: a young backlog does not protect an old head.
    CHECK(q.push(frame(22,true,2000*kMs),2000*kMs)==PushStatus::Accepted);
    for(std::uint16_t i=23;i<29;++i)CHECK(q.push(frame(i,false,2050*kMs),2050*kMs)==PushStatus::Accepted);
    CHECK(!q.tryPop(2101*kMs));CHECK(q.stats().depth==0);
}
TEST(backlog_steady_state_is_unchanged){
    // 60 fps, the decoder keeping up, a keyframe now and then: both queues do the same.
    VideoIngress off,on(backlogLimits());start(off);start(on);
    (void)off.tryPop(0);(void)on.tryPop(0);
    HostTime t=0;
    for(std::uint16_t i=1;i<=600;++i){
        t+=166667;const bool idr=i%120==0;
        const auto a=off.push(frame(i,idr,t),t),b=on.push(frame(i,idr,t),t);
        CHECK(a==PushStatus::Accepted&&b==a);
        if(i%3){                                                                  // every third frame is taken together with the next
            for(;;){const auto x=off.tryPop(t+2*kMs),y=on.tryPop(t+2*kMs);CHECK(static_cast<bool>(x)==static_cast<bool>(y));if(!x)break;CHECK(x->sample.wireFrameIndex==y->sample.wireFrameIndex&&x->epoch==y->epoch&&x->resetDecoder==y->resetDecoder);}
        }
    }
    const auto a=off.stats(),b=on.stats();
    CHECK(a.accepted==b.accepted&&a.consumed==b.consumed&&a.dropped==b.dropped&&a.recoveries==b.recoveries&&a.epoch==b.epoch&&a.highWaterDepth==b.highWaterDepth);
    CHECK(a.recoveries==0&&a.highWaterDepth==2);CHECK(b.backlogEpisodes==0&&b.backlogFrames==0);
}
TEST(backlog_switch_off_decides_exactly_like_the_original_rule){
    // 60000 random arrivals, bursts, stalls and pops against the rule as it was.
    VideoIngress q;OriginalQueueRule rule;q.begin(1);CHECK(q.push(config(),0)==PushStatus::ConfigStored);
    std::mt19937 rng(20261118);HostTime now=0;std::uint16_t index=0;unsigned pushes=0,pops=0,recoveriesSeen=0;
    for(int i=0;i<60000;++i){
        const auto roll=rng()%100;
        if(roll<8)now+=static_cast<HostTime>(rng()%2500)*kMs/10;                 // up to 250 ms: a stall
        else if(roll<60)now+=static_cast<HostTime>(rng()%200)*kMs/10;            // up to 20 ms
        if(rng()%100<55){
            const bool idr=rng()%10==0;
            const HostTime arrival=now-(rng()%20==0?static_cast<HostTime>(rng()%1500)*kMs/10:0);   // sometimes delivered late
            if(arrival<0)continue;
            CHECK(q.push(frame(index++,idr,arrival),now)==rule.push(idr,arrival,now));++pushes;
        }else{CHECK(static_cast<bool>(q.tryPop(now))==rule.pop(now));++pops;}
        const auto s=q.stats();
        CHECK(s.depth==rule.arrivals.size()&&s.waitingForIdr==rule.waiting&&s.recoveries==rule.recoveries&&s.dropped==rule.dropped&&s.epoch==rule.epoch);
        CHECK(s.depth<=4&&s.backlogEpisodes==0&&s.backlogFrames==0);
        recoveriesSeen=static_cast<unsigned>(s.recoveries);
    }
    CHECK(pushes>20000&&pops>20000&&recoveriesSeen>500);                           // the workload did exercise the rule
}
// --- No path keeps latency after a stall (200 ms, 2 s; switches on and off) ---
// A model, not a measurement: simulated time in 1 ms steps, a 60 fps stream, a
// decoder that takes 2 ms per frame and a console that answers a keyframe
// request 35 ms later. What is checked is the queueing rules, nothing else.
namespace {
enum class Stall {Network,Owner};
struct VideoStallRun {
    HostTime worstWait=0,worstBefore=0,worstTail=0;
    HostTime resumed=-1;   // first frame decoded after the stall, counted from its end
    HostTime settled=0;    // last frame that waited longer than before the stall, counted from its end
    std::size_t depthAtEnd=0,highWater=0;
    std::uint64_t recoveries=0,idrRequests=0,backlogEpisodes=0,popped=0;
};
// Network: frames the console made during the stall all arrive together when it
// ends (the worst case for the queue). Owner: frames arrive on time but the
// session owner (decoder) does not run during the stall.
VideoStallRun runVideoStall(const QueueLimits& limits,Stall kind,HostTime stall){
    VideoIngress q(limits);q.begin(1);(void)q.push(config(1,0),0);
    const HostTime frameTime=166667,stallStart=2*kSecond,stallEnd=stallStart+stall,finish=stallEnd+3*kSecond;
    VideoStallRun r;std::uint64_t made=0;HostTime keyframeFrom=0,busyUntil=0;
    for(HostTime now=0;now<=finish;now+=kMs){
        for(;;){
            const HostTime sent=static_cast<HostTime>(made)*frameTime;
            const HostTime arrives=kind==Stall::Network&&sent>=stallStart&&sent<stallEnd?stallEnd:sent;
            if(arrives>now)break;
            const bool idr=keyframeFrom>=0&&sent>=keyframeFrom;
            if(idr)keyframeFrom=-1;
            (void)q.push(frame(static_cast<std::uint16_t>(made),idr,arrives),arrives);
            ++made;
        }
        const bool ownerStalled=kind==Stall::Owner&&now>=stallStart&&now<stallEnd;
        if(ownerStalled||now<busyUntil)continue;
        if(q.takeIdrRequest(now))keyframeFrom=now+35*kMs;
        const auto v=q.tryPop(now);
        if(!v)continue;
        const HostTime wait=now-v->sample.arrival100ns;
        ++r.popped;busyUntil=now+2*kMs;
        r.worstWait=(std::max)(r.worstWait,wait);
        if(now<stallStart)r.worstBefore=(std::max)(r.worstBefore,wait);
        if(now>=stallEnd){
            if(r.resumed<0)r.resumed=now-stallEnd;
            if(wait>r.worstBefore)r.settled=now-stallEnd;
        }
        if(now>=finish-2*kSecond)r.worstTail=(std::max)(r.worstTail,wait);
    }
    const auto s=q.stats();
    r.depthAtEnd=s.depth;r.highWater=s.highWaterDepth;r.recoveries=s.recoveries;r.idrRequests=s.idrRequests;r.backlogEpisodes=s.backlogEpisodes;
    return r;
}
// What must hold for every stall, with the backlog switch on or off.
void checkNoStandingVideoDelay(const VideoStallRun& r,const QueueLimits& limits){
    CHECK(r.popped>200);
    CHECK(r.worstBefore<=kMs);                                    // steady state: taken within the model's 1 ms step
    CHECK(r.worstWait<=limits.maxAge100ns);                       // the hard age ceiling, at every moment
    CHECK(r.worstTail<=r.worstBefore);                            // afterwards exactly as before the stall
    CHECK(r.depthAtEnd<=1);
    CHECK(r.resumed>=0&&r.resumed<=200*kMs);                      // pictures again within 200 ms of the stall's end
    CHECK(r.settled<=200*kMs);                                    // and no frame waits longer than before, after that
    CHECK(r.highWater<=(limits.backlogFrames?limits.backlogFrames:limits.maxFrames));
}
struct PadStallRun {
    HostTime worstAge=0,worstBefore=0,worstTail=0,settled=0;
    std::size_t worstDepth=0,depthAtEnd=0;
    std::uint64_t overflows=0,delivered=0,neutral=0;
};
// 250 Hz polls with a moving stick and a button edge every 50 ms; the owner
// sends as RemotePlaySessionSource::run does and does not run during the stall.
PadStallRun runPadStall(ControllerQueue::Policy policy,HostTime stall){
    ControllerQueue q(policy);PadStallRun r;
    const bool immediate=policy==ControllerQueue::Policy::Immediate;
    const HostTime stallStart=2*kSecond,stallEnd=stallStart+stall,finish=stallEnd+3*kSecond;
    HostTime nextTick=0;
    for(HostTime now=0;now<=finish;now+=kMs){
        if(now%(4*kMs)==0){
            auto s=pad((now/(50*kMs))%2?std::uint32_t{ControllerState::Cross}:0u,static_cast<std::int16_t>((now/(4*kMs))%30000));
            s.motionTimestampUs=static_cast<std::uint64_t>(now)+1;   // carries the poll time through the queue
            q.push(now,s);
        }
        r.worstDepth=(std::max)(r.worstDepth,q.depth());
        if(now>=stallStart&&now<stallEnd)continue;
        bool due=now>=nextTick;
        for(unsigned sent=0;sent<ControllerQueue::kCapacity+1;++sent){
            if(!due&&!(immediate&&q.pending()))break;
            const auto state=q.take(now);const bool more=q.pending();
            due=false;nextTick=now+4*kMs;
            if(state.motionTimestampUs==0)++r.neutral;                // a release: nothing old is replayed
            else{
                const HostTime age=now-static_cast<HostTime>(state.motionTimestampUs-1);
                ++r.delivered;r.worstAge=(std::max)(r.worstAge,age);
                if(now<stallStart)r.worstBefore=(std::max)(r.worstBefore,age);
                else if(age>r.worstBefore)r.settled=now-stallEnd;
                if(now>=finish-2*kSecond)r.worstTail=(std::max)(r.worstTail,age);
            }
            if(!immediate||!more)break;
        }
    }
    r.depthAtEnd=q.depth();r.overflows=q.overflows();
    return r;
}
}
TEST(stall_network_200ms_backlog_off_recovers_with_a_keyframe){
    const QueueLimits limits;
    const auto r=runVideoStall(limits,Stall::Network,200*kMs);
    checkNoStandingVideoDelay(r,limits);
    CHECK(r.recoveries==1&&r.idrRequests==1&&r.backlogEpisodes==0);   // twelve frames at once: dropped, keyframe asked
    CHECK(r.worstWait<=kMs);                                          // nothing old is ever decoded
    CHECK(r.resumed>=35*kMs);                                         // the picture waits for the console's keyframe
}
TEST(stall_network_200ms_backlog_on_decodes_through){
    const auto limits=backlogLimits();
    const auto r=runVideoStall(limits,Stall::Network,200*kMs);
    checkNoStandingVideoDelay(r,limits);
    CHECK(r.recoveries==0&&r.idrRequests==0);                         // nothing asked of the console
    CHECK(r.backlogEpisodes>=1&&r.backlogEpisodes<=2);                // the burst, and once more as the next frame meets its tail
    CHECK(r.resumed==0);                                              // the first frame of the burst is decoded at once
    CHECK(r.worstWait<=30*kMs);                                       // twelve frames at 2 ms each
    CHECK(r.settled<=30*kMs);                                         // and the queue is empty again right after
    CHECK(r.highWater==12);
}
TEST(stall_network_2s_recovers_the_same_with_backlog_on_and_off){
    const QueueLimits offLimits;const auto onLimits=backlogLimits();
    const auto off=runVideoStall(offLimits,Stall::Network,2*kSecond),on=runVideoStall(onLimits,Stall::Network,2*kSecond);
    checkNoStandingVideoDelay(off,offLimits);checkNoStandingVideoDelay(on,onLimits);
    // 120 frames at once is past any backlog: both drop and ask for a keyframe once.
    CHECK(off.recoveries==1&&on.recoveries==1&&off.idrRequests==1&&on.idrRequests==1);
    CHECK(on.highWater==16&&off.highWater<=4);
    CHECK(on.worstWait<=kMs&&off.worstWait<=kMs);                     // no stale frame is decoded in either
    CHECK(on.resumed==off.resumed&&on.settled==off.settled);
}
TEST(stall_owner_200ms_and_2s_never_decode_a_stale_frame){
    for(const HostTime stall:{200*kMs,2*kSecond}){
        const QueueLimits offLimits;const auto onLimits=backlogLimits();
        const auto off=runVideoStall(offLimits,Stall::Owner,stall),on=runVideoStall(onLimits,Stall::Owner,stall);
        checkNoStandingVideoDelay(off,offLimits);checkNoStandingVideoDelay(on,onLimits);
        // Frames that waited through the stall are older than 100 ms: dropped, never shown late.
        CHECK(off.recoveries==1&&on.recoveries==1);
        CHECK(off.worstWait<=kMs&&on.worstWait<=kMs);
        CHECK(off.resumed>=35*kMs&&on.resumed>=35*kMs);
    }
}
TEST(stall_owner_90ms_backlog_on_catches_up_inside_the_ceiling){
    // Just inside the age ceiling: six frames wait. Off: the fifth drops the queue.
    const QueueLimits offLimits;const auto onLimits=backlogLimits();
    const auto off=runVideoStall(offLimits,Stall::Owner,90*kMs),on=runVideoStall(onLimits,Stall::Owner,90*kMs);
    checkNoStandingVideoDelay(off,offLimits);checkNoStandingVideoDelay(on,onLimits);
    CHECK(off.recoveries==1&&on.recoveries==0&&on.idrRequests==0);
    CHECK(on.worstWait>50*kMs&&on.worstWait<=100*kMs);                // old frames are decoded, but never past 100 ms
    CHECK(on.resumed==0&&on.settled<=20*kMs);                         // and the delay is gone 20 ms later
}
TEST(stall_pad_immediate_policy_returns_to_no_delay){
    for(const HostTime stall:{200*kMs,2*kSecond}){
        const auto r=runPadStall(ControllerQueue::Policy::Immediate,stall);
        CHECK(r.delivered>500);
        CHECK(r.worstBefore==0);                                      // a poll is sent in the step it arrives
        CHECK(r.worstAge<=ControllerQueue::kStale100ns);              // nothing older than 100 ms ever reaches the console
        CHECK(r.settled<=4*kMs);                                      // the backlog is sent at once, in order
        CHECK(r.worstTail==0&&r.depthAtEnd==0);
        CHECK(r.worstDepth<=ControllerQueue::kCapacity);
    }
    CHECK(runPadStall(ControllerQueue::Policy::Immediate,200*kMs).overflows==0);   // four button edges: no forced release
}
TEST(stall_pad_tick_policy_is_bounded_but_keeps_a_backlog){
    // VEYRA_TEST_RP_LEGACY_INPUT, the original one-state-per-tick rule, kept for
    // A/B runs. It does not grow without bound, but what queued up during the
    // stall is replayed one tick at a time while new polls keep arriving, so a
    // delay remains. Recorded here so that nobody mistakes it for the default.
    for(const HostTime stall:{200*kMs,2*kSecond}){
        const auto r=runPadStall(ControllerQueue::Policy::Tick,stall);
        CHECK(r.worstBefore==0);
        CHECK(r.worstAge<=ControllerQueue::kStale100ns);
        CHECK(r.worstDepth<=ControllerQueue::kCapacity&&r.depthAtEnd<=ControllerQueue::kCapacity);
        CHECK(r.worstTail<=static_cast<HostTime>(ControllerQueue::kCapacity)*4*kMs);
        CHECK(r.worstTail>0);                                         // the remaining delay this policy is known for
        CHECK(r.overflows>0);
    }
}
// --- Keyframe retry (VEYRA_EXP_RP_IDR_RETRY_MS) ---
TEST(idr_retry_default_is_the_original_second){
    StreamRecovery r;using A=StreamRecovery::Action;
    CHECK(r.keyframeRetryMs()==1000);
    r.beginAttempt(0);r.frame(0);
    CHECK(r.poll(999,false,false)==A::Wait);CHECK(r.poll(1000,false,false)==A::Keyframe);
    CHECK(r.poll(2999,false,false)==A::Wait);CHECK(r.poll(3000,false,false)==A::Keyframe);
    CHECK(r.poll(5999,false,false)==A::Wait);CHECK(r.poll(6000,false,false)==A::Reconnect);
    CHECK(resolveLanExperiments({}).idrRetryMs==1000);CHECK(!resolveLanExperiments({}).any());
    QueueLimits l;resolveLanExperiments({}).applyTo(l);CHECK(l.idrFirstRetry100ns==0);
}
TEST(idr_retry_earlier_first_request_same_rest){
    StreamRecovery r;using A=StreamRecovery::Action;r.setKeyframeRetryMs(250);
    r.beginAttempt(0);r.frame(0);
    CHECK(r.poll(249,false,false)==A::Wait);CHECK(r.poll(250,false,false)==A::Keyframe);
    // The second request keeps its 2 s distance and the reconnect its 6 s of silence.
    CHECK(r.poll(2249,false,false)==A::Wait);CHECK(r.poll(2250,false,false)==A::Keyframe);
    CHECK(r.poll(5999,false,false)==A::Wait);CHECK(r.poll(6000,false,false)==A::Reconnect);
    // Before the first frame of an attempt there is nothing to repair: no request.
    StreamRecovery fresh;fresh.setKeyframeRetryMs(250);fresh.beginAttempt(0);
    CHECK(fresh.poll(5000,false,false)==A::Wait);
    // A frame in time resets the count.
    StreamRecovery steady;steady.setKeyframeRetryMs(250);steady.beginAttempt(0);
    for(int t=0;t<3000;t+=17){steady.frame(t);CHECK(steady.poll(t+16,false,false)==A::Wait);}
}
TEST(idr_retry_is_clamped_to_250_and_1000){
    StreamRecovery r;
    r.setKeyframeRetryMs(1);CHECK(r.keyframeRetryMs()==250);
    r.setKeyframeRetryMs(249);CHECK(r.keyframeRetryMs()==250);
    r.setKeyframeRetryMs(600);CHECK(r.keyframeRetryMs()==600);
    r.setKeyframeRetryMs(60000);CHECK(r.keyframeRetryMs()==1000);     // never later than the original
    r.setKeyframeRetryMs(-5);CHECK(r.keyframeRetryMs()==250);
}
TEST(idr_retry_switch_parsing){
    const auto with=[](const char* value){TransportSwitches s;s.idrRetryMs=value;return resolveLanExperiments(s);};
    CHECK(with("250").idrRetryMs==250&&with("250").any()&&!with("250").usesTransport());
    CHECK(with("600").idrRetryMs==600);CHECK(with("100").idrRetryMs==250);CHECK(with("1").idrRetryMs==250);
    CHECK(with("1000").idrRetryMs==1000&&!with("1000").any());        // the default asked for explicitly: nothing changes
    CHECK(with("5000").idrRetryMs==1000);
    CHECK(with("0").idrRetryMs==1000&&!with("0").idrRetryIgnored);    // 0 is "off", as for every switch
    for(const char* bad:{"fast","250ms","-250","2.5"}){const auto e=with(bad);CHECK(e.idrRetryMs==1000&&e.idrRetryIgnored&&!e.any());}
    CHECK(with("300").describe().find("idrRetryMs=300")!=std::string::npos);
    CHECK(with("fast").describe().find("VEYRA_EXP_RP_IDR_RETRY_MS ignored")!=std::string::npos);
}
// Review N3: below 500 ms the switch used to lower the spacing of every keyframe
// request, doubling the sustained rate. Only the first repeat of an outage moves.
namespace {
QueueLimits idrLimitsFor(const char* value){TransportSwitches s;s.idrRetryMs=value;QueueLimits l;resolveLanExperiments(s).applyTo(l);l.validate();return l;}
// Times at which requests go out when one is wanted in every millisecond of `wanted`.
std::vector<HostTime> idrRequestTimes(const QueueLimits& limits,HostTime duration,const std::function<bool(HostTime)>& wanted){
    VideoIngress q(limits);q.begin(1);(void)q.push(config(),0);
    std::vector<HostTime> times;
    for(HostTime now=0;now<duration;now+=kMs){
        if(wanted(now))q.requireKeyframe();
        if(q.takeIdrRequest(now))times.push_back(now);
    }
    CHECK(q.stats().idrRequests==times.size());
    return times;
}
// No stretch of time may hold more requests than `extra` above one per 500 ms.
bool idrRateBounded(const std::vector<HostTime>& times,std::size_t extra){
    for(std::size_t i=0;i<times.size();++i)for(std::size_t j=i;j<times.size();++j)
        if(j-i+1>static_cast<std::size_t>((times[j]-times[i])/(500*kMs))+1+extra)return false;
    return true;
}
}
TEST(idr_retry_below_500_moves_only_the_first_repeat_request){
    CHECK(QueueLimits{}.idrFirstRetry100ns==0);CHECK(QueueLimits::kIdrSpacing100ns==5000000);
    CHECK(idrLimitsFor("250").idrFirstRetry100ns==2500000);CHECK(idrLimitsFor("400").idrFirstRetry100ns==4000000);
    CHECK(idrLimitsFor("100").idrFirstRetry100ns==2500000);
    CHECK(idrLimitsFor("500").idrFirstRetry100ns==0);CHECK(idrLimitsFor("800").idrFirstRetry100ns==0);   // 500 and above: the queue is untouched
    QueueLimits l;l.idrFirstRetry100ns=2499999;THROWS(VideoIngress{l});l.idrFirstRetry100ns=5000000;THROWS(VideoIngress{l});
    l.idrFirstRetry100ns=4999999;{VideoIngress ok(l);(void)ok;}
    VideoIngress original;original.begin(1);(void)original.push(config(),0);original.requireKeyframe();
    CHECK(original.takeIdrRequest(0));original.requireKeyframe();
    CHECK(!original.takeIdrRequest(2500000));CHECK(!original.takeIdrRequest(4999999));CHECK(original.takeIdrRequest(5000000));
    VideoIngress quick(idrLimitsFor("250"));quick.begin(1);(void)quick.push(config(),0);quick.requireKeyframe();
    CHECK(quick.takeIdrRequest(0));quick.requireKeyframe();                       // opens the outage
    CHECK(!quick.takeIdrRequest(2499999));CHECK(quick.takeIdrRequest(2500000));   // its first repeat: after 250 ms
    quick.requireKeyframe();
    CHECK(!quick.takeIdrRequest(5000000));                                        // 250 ms later again: no, that was the one
    CHECK(!quick.takeIdrRequest(7499999));CHECK(quick.takeIdrRequest(7500000));   // the 500 ms spacing from here on
    quick.requireKeyframe();
    CHECK(!quick.takeIdrRequest(10000000));CHECK(!quick.takeIdrRequest(12499999));CHECK(quick.takeIdrRequest(12500000));
    CHECK(quick.stats().idrRequests==4&&quick.stats().idrEarlyRequests==1);CHECK(original.stats().idrEarlyRequests==0);
}
TEST(idr_retry_sustained_rate_is_the_original){
    // A minute without a keyframe, a request wanted all the time.
    const auto always=[](HostTime){return true;};
    const auto original=idrRequestTimes(QueueLimits{},60*kSecond,always),quick=idrRequestTimes(idrLimitsFor("250"),60*kSecond,always);
    CHECK(original.size()==120);CHECK(quick.size()==121);                         // one more in the whole outage, not twice as many
    for(std::size_t i=1;i<original.size();++i)CHECK(original[i]-original[i-1]==500*kMs);
    CHECK(quick[0]==0&&quick[1]==250*kMs);
    for(std::size_t i=2;i<quick.size();++i)CHECK(quick[i]-quick[i-1]==500*kMs);
    CHECK(idrRateBounded(original,0));CHECK(idrRateBounded(quick,1));CHECK(!idrRateBounded(quick,0));
    // 400 ms: the same shape.
    const auto slower=idrRequestTimes(idrLimitsFor("400"),10*kSecond,always);
    CHECK(slower[1]==400*kMs&&slower[2]==900*kMs&&slower[3]==1400*kMs);
}
TEST(idr_retry_first_repeat_is_once_per_outage){
    // A request opens a new outage only when none was sent in the second before it.
    VideoIngress q(idrLimitsFor("250"));q.begin(1);(void)q.push(config(),0);
    const auto ask=[&](HostTime ms100ns){q.requireKeyframe();return q.takeIdrRequest(ms100ns);};
    CHECK(ask(0));CHECK(ask(250*kMs));CHECK(ask(750*kMs));
    // 999.9999 ms of quiet: still the same outage, so no early repeat after it.
    const HostTime again=750*kMs+10000000-1;
    CHECK(ask(again));CHECK(!ask(again+250*kMs));CHECK(!ask(again+500*kMs-1));CHECK(ask(again+500*kMs));
    // A full second of quiet: a new outage, with its one early repeat.
    const HostTime next=again+500*kMs+10000000;
    CHECK(ask(next));CHECK(!ask(next+250*kMs-1));CHECK(ask(next+250*kMs));CHECK(!ask(next+500*kMs));CHECK(ask(next+750*kMs));
    CHECK(q.stats().idrEarlyRequests==2);
    // A repeat that comes at 500 ms or later anyway uses nothing up and earns nothing.
    VideoIngress late(idrLimitsFor("250"));late.begin(1);(void)late.push(config(),0);
    const auto askLate=[&](HostTime t){late.requireKeyframe();return late.takeIdrRequest(t);};
    CHECK(askLate(0));CHECK(askLate(600*kMs));CHECK(!askLate(850*kMs));CHECK(askLate(1100*kMs));CHECK(late.stats().idrEarlyRequests==0);
    // A clock value from before the last request never lets one through.
    CHECK(!askLate(1100*kMs-1));CHECK(!askLate(-1));
}
TEST(idr_retry_never_exceeds_the_original_rate_by_more_than_one){
    // Requests wanted in random bursts with random pauses, for ten simulated minutes.
    for(const char* value:{"250","300","499"}){
        std::mt19937 rng(20261118);HostTime until=0;bool on=false;
        const auto wanted=[&](HostTime now){
            if(now>=until){on=!on;until=now+static_cast<HostTime>(rng()%(on?4000:2500))*kMs;}
            return on&&rng()%3!=0;
        };
        const auto times=idrRequestTimes(idrLimitsFor(value),600*kSecond,wanted);
        CHECK(times.size()>300);CHECK(idrRateBounded(times,1));
        const HostTime first=idrLimitsFor(value).idrFirstRetry100ns;std::size_t early=0;
        for(std::size_t i=1;i<times.size();++i){
            const HostTime gap=times[i]-times[i-1];
            CHECK(gap>=first);
            if(gap>=500*kMs)continue;
            ++early;
            CHECK(i==1||times[i-1]-times[i-2]>=1000*kMs);                         // only straight after a request that opened an outage
            if(i+1<times.size())CHECK(times[i+1]-times[i]>=500*kMs);              // and never twice in a row
        }
        CHECK(early>20);                                                          // the workload did use the allowance
    }
}
TEST(idr_retry_switch_off_requests_exactly_as_before){
    // The original rule, written out: a wanted request goes out at once unless
    // one went out less than 500 ms ago. Unset, off, 500 and above, and not a number.
    for(const char* value:{"","0","500","1000","fast"}){
        std::mt19937 rng(7);TransportSwitches s;s.idrRetryMs=value;QueueLimits l;resolveLanExperiments(s).applyTo(l);
        VideoIngress q(l);q.begin(1);(void)q.push(config(),0);
        bool pending=true;                                                        // the first configuration asks for a keyframe
        HostTime last=-1,now=0;unsigned granted=0;
        for(int i=0;i<200000;++i){
            now+=static_cast<HostTime>(rng()%4)*kMs;if(rng()%500==0)now+=static_cast<HostTime>(rng()%3000)*kMs;
            if(rng()%3==0){q.requireKeyframe();pending=true;}
            const bool expect=pending&&(last<0||now-last>=5000000);
            CHECK(q.takeIdrRequest(now)==expect);
            if(expect){pending=false;last=now;++granted;}
        }
        CHECK(granted>100);CHECK(q.stats().idrRequests==granted&&q.stats().idrEarlyRequests==0);
    }
}
// Review N4, the second change no test noticed: what begin() resets.
TEST(begin_carries_nothing_over_from_the_session_before){
    // A backlog episode open when the session ends must not be overdue in the
    // next one: the owner's first look would ask for a keyframe before anything
    // has arrived.
    VideoIngress q(backlogLimits());start(q);
    for(std::uint16_t i=1;i<6;++i)CHECK(q.push(frame(i,false),0)==PushStatus::Accepted);
    CHECK(q.stats().depth==6&&q.stats().backlogEpisodes==1);
    q.begin(2);
    const HostTime later=10*kSecond;
    CHECK(!q.tryPop(later));
    CHECK(!q.takeIdrRequest(later));
    auto s=q.stats();
    CHECK(s.depth==0&&s.recoveries==0&&s.dropped==0&&s.backlogEpisodes==0&&s.idrRequests==0&&s.epoch==1&&s.waitingForIdr);
    // The new session then starts like any other, and a burst gets its full allowance.
    CHECK(q.push(config(2,later),later)==PushStatus::ConfigStored);
    CHECK(q.push(frame(0,true,later,2),later)==PushStatus::Accepted);
    for(std::uint16_t i=1;i<6;++i)CHECK(q.push(frame(i,false,later,2),later)==PushStatus::Accepted);
    const auto v=q.tryPop(later+90*kMs);CHECK(v&&v->resetDecoder&&v->epoch==1);
    CHECK(q.stats().recoveries==0&&q.stats().backlogEpisodes==1);
    // Nor does the time of the last keyframe request carry over: a session that
    // starts 100 ms after the old one asked may ask at once.
    VideoIngress r;r.begin(1);(void)r.push(config(),0);
    CHECK(r.takeIdrRequest(1000*kMs));
    r.begin(2);(void)r.push(config(2,1100*kMs),1100*kMs);
    CHECK(r.takeIdrRequest(1100*kMs));CHECK(r.stats().idrRequests==1);
    // With the early repeat (VEYRA_EXP_RP_IDR_RETRY_MS below 500) the new session has its own outage.
    VideoIngress e(idrLimitsFor("250"));e.begin(1);(void)e.push(config(),0);
    CHECK(e.takeIdrRequest(0));e.requireKeyframe();CHECK(e.takeIdrRequest(250*kMs));   // the old session used its early repeat
    e.begin(2);(void)e.push(config(2,300*kMs),300*kMs);
    CHECK(e.takeIdrRequest(300*kMs));e.requireKeyframe();
    CHECK(!e.takeIdrRequest(549*kMs));CHECK(e.takeIdrRequest(550*kMs));CHECK(e.stats().idrEarlyRequests==1);
}
// Review N5: producer and consumer threads on one VideoIngress, switch on and off.
namespace {
struct IngressStress {
    const char* failure=nullptr;
    std::uint64_t pushed=0,accepted=0,popped=0;
    VideoQueueStats stats;
};
// The clock is a counter that only the producer advances, one tick per frame,
// and the consumer reads it before it looks into the queue (on every second look
// with a yield in between), so its value is often older than the frame it then
// finds: the order of events in the application, where the owner reads the
// clock before it takes the lock. room: the producer waits (yield, bounded)
// while that many frames or more are queued; 0 = it never waits and overflows
// the queue all the time. With room at or below the frame limit and a 1 ms tick
// the queue can neither overflow nor hold an old frame; with a 1 us tick the
// whole run is shorter than the 100 ms allowances, so not even a backlog can
// run out. In both nothing in the stream asks for a recovery.
// No sleep anywhere; both loops are bounded.
IngressStress runIngressStress(const QueueLimits& limits,std::size_t room,std::uint32_t frames,HostTime tick=kMs){
    VideoIngress q(limits);q.begin(1);(void)q.push(config(),0);
    const std::size_t ceiling=limits.backlogFrames?limits.backlogFrames:limits.maxFrames;
    std::atomic<HostTime> clock{0};std::atomic<bool> done{false};std::atomic<const char*> failure{nullptr};
    std::atomic<std::uint64_t> pushed{0},accepted{0};
    const auto fail=[&](const char* why){const char* none=nullptr;failure.compare_exchange_strong(none,why);};
    std::thread producer([&]{
        for(std::uint32_t i=0;i<frames&&!failure.load();++i){
            std::uint32_t spins=0;
            while(room&&q.stats().depth>=room){
                if(++spins>50000000u||failure.load()){fail("the consumer never made room");break;}
                std::this_thread::yield();
            }
            const HostTime t=clock.fetch_add(tick)+tick;
            const auto status=q.push(frame(static_cast<std::uint16_t>(i),i%32==0,t),t);
            ++pushed;
            if(status==PushStatus::Accepted)++accepted;
            else if(status!=PushStatus::WaitingForIdr)fail("a push was neither accepted nor waiting for a keyframe");
            const auto s=q.stats();
            if(s.depth>ceiling||s.bytes>limits.maxBytes)fail("the queue went past its bound");
        }
        done=true;
    });
    IngressStress r;
    std::int64_t lastIndex=-1;std::uint64_t lastEpoch=0;
    const std::uint64_t iterationLimit=std::uint64_t{frames}*5000u+1000000u;
    for(std::uint64_t iteration=0;;++iteration){
        if(iteration>iterationLimit){fail("the consumer loop did not finish");break;}
        if(failure.load())break;
        const bool finished=done.load();          // before the look: a frame pushed after this is found by the next one
        const HostTime now=clock.load();          // possibly older than the frame the look finds
        if(iteration%2)std::this_thread::yield(); // invites the producer in between; nothing depends on it
        (void)q.takeIdrRequest(now);
        const auto v=q.tryPop(now);
        if(!v){if(finished)break;std::this_thread::yield();continue;}
        ++r.popped;
        const std::int64_t index=v->sample.wireFrameIndex?static_cast<std::int64_t>(*v->sample.wireFrameIndex):-1;
        if(index<0||index<=lastIndex)fail("frames left the queue out of order");
        if(v->epoch<lastEpoch)fail("the epoch went backwards");
        const bool first=v->epoch!=lastEpoch;
        if(first!=v->resetDecoder)fail("a decoder reset is due on the first frame of an epoch and only there");
        if(first&&(!v->configBefore||index%32!=0))fail("an epoch must start with the configuration and a keyframe");
        if(!first&&index!=lastIndex+1)fail("a frame was missing inside an epoch");
        lastIndex=index;lastEpoch=v->epoch;
    }
    producer.join();
    r.failure=failure.load();r.pushed=pushed.load();r.accepted=accepted.load();r.stats=q.stats();
    return r;
}
// What must hold however the two threads interleave.
void checkIngressStress(const IngressStress& r,const QueueLimits& limits,std::uint32_t frames){
    const std::size_t ceiling=limits.backlogFrames?limits.backlogFrames:limits.maxFrames;
    CHECK(r.failure==nullptr);
    CHECK(r.pushed==frames&&r.stats.accessUnits==frames&&r.stats.rejected==0);
    CHECK(r.stats.accepted==r.accepted&&r.stats.consumed==r.popped);
    CHECK(r.stats.depth==0);                                                      // the consumer's last look came after the last push
    CHECK(r.stats.dropped==frames-r.popped);                                      // every frame was decoded or counted as dropped, none lost or doubled
    CHECK(r.stats.recoveries+1==r.stats.epoch);
    // A recovery costs at most what was queued and the frames up to the next keyframe (one in 32).
    CHECK(r.stats.dropped<=r.stats.recoveries*(ceiling+31));
    CHECK(r.stats.highWaterDepth<=ceiling);
    if(!limits.backlogFrames)CHECK(r.stats.backlogEpisodes==0&&r.stats.backlogFrames==0);
}
}
TEST(ingress_two_threads_backlog_off_keeps_its_invariants){
    // The default, in three regimes: room for one more frame only, filled to the
    // limit, and a producer that never waits. The original age rule may call a
    // frame that arrived after the consumer's clock read expired (documented
    // finding, not changed here), so recoveries are allowed in all three; the
    // accounting, the order and the bounds are not negotiable.
    const std::uint32_t frames=20000;const QueueLimits limits;
    for(const std::size_t room:{std::size_t{2},std::size_t{4},std::size_t{0}}){
        const auto r=runIngressStress(limits,room,frames);checkIngressStress(r,limits,frames);
        if(room)CHECK(r.stats.highWaterDepth<=room);
    }
}
TEST(ingress_two_threads_backlog_on_keeps_its_invariants){
    // The switch on: a producer that fills the queue into the backlog (seven
    // frames, so episodes start, drain and run out), and one that never waits.
    const std::uint32_t frames=20000;const auto limits=backlogLimits();
    for(const std::size_t room:{std::size_t{7},std::size_t{0}}){
        const auto r=runIngressStress(limits,room,frames);checkIngressStress(r,limits,frames);
        if(room)CHECK(r.stats.highWaterDepth<=room);
    }
}
TEST(ingress_two_threads_backlog_on_never_recovers_without_a_reason){
    // With the switch on, and its tolerance for a clock value read too early
    // (review S2), every frame must come out, in order, with no recovery and no
    // keyframe request beyond the one a new session starts with, whenever
    // nothing is lost, late or in excess:
    // at most four frames queued, 1 ms apart;
    const std::uint32_t frames=20000;const auto limits=backlogLimits();
    const auto clean=[&](const IngressStress& r){
        checkIngressStress(r,limits,frames);
        CHECK(r.popped==frames&&r.accepted==frames);
        CHECK(r.stats.recoveries==0&&r.stats.dropped==0&&r.stats.epoch==1&&r.stats.idrRequests<=1);
    };
    for(const std::size_t room:{std::size_t{2},std::size_t{4}}){
        const auto r=runIngressStress(limits,room,frames);clean(r);
        CHECK(r.stats.highWaterDepth<=room&&r.stats.backlogEpisodes==0);
    }
    // and into the backlog, up to its sixteen frames, in a run whose 20 ms of
    // clock time cannot use up a 100 ms allowance.
    for(const std::size_t room:{std::size_t{7},std::size_t{16}}){
        const auto r=runIngressStress(limits,room,frames,10);clean(r);
        CHECK(r.stats.highWaterDepth<=room);
    }
}
