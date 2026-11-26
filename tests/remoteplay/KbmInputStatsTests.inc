// SPDX-License-Identifier: GPL-3.0-only
// The [slipstream-input] measurements and the fallback self-check (KbmInputStats.h), sprint 5.
// Included by CoreTests.cpp above main(); uses its TEST / CHECK.
#include "veyra/remoteplay/KbmInputStats.h"
#include "veyra/remoteplay/ControllerQueue.h"
#include <string>
#include <thread>
#include <vector>

TEST(kbm_stats_percentiles_use_nearest_rank_and_unknown_is_minus_one){
    KbmStageSamples s;
    CHECK(s.percentile(.5)==-1);CHECK(s.max()==-1);
    for(int i=1;i<=100;++i)s.add(i);
    CHECK(s.percentile(.5)==50);CHECK(s.percentile(.95)==95);CHECK(s.percentile(.99)==99);CHECK(s.percentile(1)==100);
    CHECK(s.max()==100);
    s.add(-3);s.add(std::nan(""));CHECK(s.count()==100);   // refused
    // Only the newest kCapacity count.
    KbmStageSamples r;for(std::size_t i=0;i<KbmStageSamples::kCapacity+10;++i)r.add(i<10?1e6:1.0);
    CHECK(r.percentile(1)==1.0);
}

TEST(kbm_stats_probe_pairs_each_handed_state_with_one_send){
    KbmInputProbe probe;KbmInputMeter meter;
    const std::int64_t f=1000000;                 // ticks per second: 1 tick = 1 us
    probe.sent(1000,f,true,0,0);                  // nothing handed yet: a report, no state-to-send sample
    probe.handed(2000,1500);
    probe.sent(2600,f,true,1,2000);               // 0.6 ms after the hand-off, 1.1 ms after the event, 1.6 ms after the last report
    probe.sent(6600,f,false,0,0);                 // the 4 ms repeat of an unchanged state: no packet, no sample, no pairing
    probe.handed(7000,0);                         // a state without new keyboard / mouse input (the pad moved)
    probe.sent(7100,f,true,2,6900);               // 4.5 ms after the last report
    probe.sent(307100,f,true,0,0);                // 300 ms later: the hands paused, that is not a gap
    meter.takeProbe(probe);
    CHECK(meter.stateToSend.count()==2);
    CHECK(std::abs(meter.stateToSend.percentile(0)-0.1)<1e-6);CHECK(std::abs(meter.stateToSend.percentile(1)-0.6)<1e-6);   // samples are kept as float
    CHECK(meter.eventToSend.count()==1);CHECK(std::abs(meter.eventToSend.max()-1.1)<1e-6);
    CHECK(meter.sendGap.count()==2);CHECK(std::abs(meter.sendGap.max()-4.5)<1e-6);
    CHECK(meter.queueDepthMax==2);CHECK(meter.reports==4);
    CHECK(meter.queueAge.count()==2);CHECK(std::abs(meter.queueAge.max()-0.6)<1e-6);
    // Drained every poll, the ring never loses a record however long the session.
    for(int i=0;i<100000;++i){probe.sent(400000+i*4000,f,true,1,400000+i*4000-100);meter.takeProbe(probe);}
    CHECK(probe.lost()==0);
}

TEST(kbm_stats_probe_is_safe_across_two_threads){
    // The interface thread hands and drains while the owner sends (ThreadSanitizer run in verify.sh).
    KbmInputProbe probe;KbmInputMeter meter;
    std::atomic<bool> done{false};
    std::thread owner([&]{
        for(std::int64_t t=1;t<=200000;t+=10)probe.sent(t*10,1000000,true,std::uint32_t(t%4),t*10-5);
        done=true;
    });
    std::int64_t t=1;
    while(!done.load()){probe.handed(t*10,t*10-3);++t;meter.takeProbe(probe);}
    owner.join();meter.takeProbe(probe);
    CHECK(meter.sendGap.count()>0);
    CHECK(meter.sendGap.max()<=0.2);   // every gap is 100 ticks = 0.1 ms; a torn record would show
}

TEST(kbm_stats_poll_line_shows_a_late_interface_thread){
    KbmInputMeter meter;
    for(int i=0;i<995;++i)meter.poll(4000+(i%5)*20,4);        // on time
    for(int i=0;i<5;++i)meter.poll(31000,4);                  // the interface was busy for 31 ms, five times
    meter.rawBacklogMax=37;
    CHECK(meter.polls==1000&&meter.latePolls==5);
    const std::string line=meter.pollLine(4);
    for(const char* key:{"polls=1000","pollMs=4","pollGapMsP50=4.0","pollGapMsP95=4.0","pollGapMsP99=4.0","pollGapMsMax=31.000","latePolls=5","rawBacklogMax=37"})
        CHECK(line.find(key)!=std::string::npos);
    meter.poll(8000,4);CHECK(meter.latePolls==5);            // exactly twice the interval is not yet late
    meter.clear();CHECK(meter.polls==0&&meter.latePolls==0&&meter.pollGap.count()==0);
}

TEST(kbm_stats_line_has_every_contract_key_in_order){
    KbmInputMeter meter;KbmInputLineInfo info;
    info.source="kbm";info.inputProfile="ultra-low-latency";info.context="aim";info.profile="slipstream-kbm-gta-6";info.mouseHz=1000;
    info.smoothingMs=0;info.saturatedPct=2.5;info.curve="linear";info.switches=4;
    meter.translate.add(12.5);meter.windowSeconds=5;meter.reports=1250;
    const std::string line=meter.line(info);
    const char* keys[]={"source=kbm","inputProfile=ultra-low-latency","context=aim","profile=slipstream-kbm-gta-6","mouseEvents=0","mouseHz=1000",
        "rawToTranslateUsP50=-1","rawToTranslateUsP95=","rawToTranslateUsP99=","translateUsP50=12.5","translateUsP95=","translateUsP99=",
        "stateToSendMsP50=","stateToSendMsP95=","stateToSendMsP99=","eventToSendMsP50=","eventToSendMsP95=","eventToSendMsP99=",
        "eventToSendMsMax=","queueAgeMsP95=","queueDepthMax=0","reports=1250","sendHz=250.0","sendGapMsP50=","sendGapMsP95=","sendGapMsMax=",
        "smoothingMs=0.0","saturatedPct=2.5","curve=linear","switches=4","stuckReleases=0","heldAtLoss=0","fallback=none"};
    std::size_t at=0;
    for(const char* k:keys){const auto found=line.find(k,at);CHECK(found!=std::string::npos);at=found;}
    CHECK(line.find('/')==std::string::npos&&line.find('\\')==std::string::npos);   // never a path
}

TEST(kbm_fallback_needs_three_bad_windows_and_names_the_reason){
    KbmFallbackCheck c;
    KbmFallbackWindow fine{40,20,0.5,0.4};
    CHECK(!c.update(fine));
    // Twice as slow but less than 250 us more: not worse beyond the margin.
    KbmFallbackWindow small{200,20,0.5,0.4};
    for(int i=0;i<5;++i)CHECK(!c.update(small));
    KbmFallbackWindow slow{400,20,0.5,0.4};
    CHECK(!c.update(slow));CHECK(!c.update(slow));
    CHECK(!c.update(fine));                     // the run is broken
    CHECK(!c.update(slow));CHECK(!c.update(slow));
    const char* r=c.update(slow);
    CHECK(r&&std::string(r)=="translate-slower");CHECK(c.fellBack());
    CHECK(!c.update(slow));                     // reported once
    c.reset();CHECK(std::string(c.reason())=="none");
    KbmFallbackWindow late{20,20,3.0,0.5};
    CHECK(!c.update(late));CHECK(!c.update(late));
    CHECK(std::string(c.update(late))=="queue-older");
    // Without a legacy baseline the queue rule cannot act.
    KbmFallbackCheck d;KbmFallbackWindow unknown{20,20,9.0,-1};
    for(int i=0;i<10;++i)CHECK(!d.update(unknown));
    CHECK(std::string(kbmInputSource(3,0))=="kbm");CHECK(std::string(kbmInputSource(3,2))=="mixed");
    CHECK(std::string(kbmInputSource(0,2))=="pad");CHECK(std::string(kbmInputSource(0,0))=="none");
}

TEST(kbm_owner_tap_reports_queue_and_changed_states_and_is_silent_when_off){
    // On: the same sequence the session owner runs (queue before the take, then the state sent).
    KbmInputMeter meter;meter.takeProbe(kbmInputProbe());meter.clear();        // whatever earlier tests left in the process-wide probe
    ControllerQueue queue;KbmOwnerTap tap(true);
    ControllerState fire;fire.inputActive=true;fire.r2=255;
    ControllerState aim=fire;aim.rightX=12000;
    const HostTime t0=50000000;                                               // 100 ns units
    CHECK(queue.oldest()==0);
    CHECK(queue.push(t0,fire));CHECK(queue.oldest()==t0);
    kbmInputProbe().handed(t0,t0-5000);                                       // the event was 0.5 ms before the hand-off
    tap.queued(queue.depth(),queue.oldest());
    auto state=queue.take(t0+3000);tap.sent(state,t0+3000);                   // sent 0.3 ms after the hand-off
    tap.queued(queue.depth(),queue.oldest());
    state=queue.take(t0+43000);tap.sent(state,t0+43000);                      // the 4 ms repeat: unchanged
    CHECK(queue.push(t0+50000,aim));
    kbmInputProbe().handed(t0+50000,0);
    tap.queued(queue.depth(),queue.oldest());
    state=queue.take(t0+51000);tap.sent(state,t0+51000);
    meter.takeProbe(kbmInputProbe());
    CHECK(meter.reports==2);CHECK(meter.queueDepthMax==1);
    CHECK(meter.stateToSend.count()==2);CHECK(std::abs(meter.stateToSend.max()-0.3)<1e-6);
    CHECK(meter.eventToSend.count()==1);CHECK(std::abs(meter.eventToSend.max()-0.8)<1e-6);
    CHECK(meter.sendGap.count()==1);CHECK(std::abs(meter.sendGap.max()-4.8)<1e-6);
    CHECK(meter.queueAge.count()==2);CHECK(std::abs(meter.queueAge.max()-0.3)<1e-6);
    // Off: nothing reaches the probe.
    meter.clear();
    KbmOwnerTap off(false);CHECK(!off.on());
    off.queued(3,t0);off.sent(aim,t0+60000);
    meter.takeProbe(kbmInputProbe());
    CHECK(meter.reports==0&&meter.queueDepthMax==0&&meter.sendGap.count()==0);
}
