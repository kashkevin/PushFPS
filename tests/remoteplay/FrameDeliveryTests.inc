// SPDX-License-Identifier: GPL-3.0-only
// Frame delivery (include/veyra/remoteplay/FrameDelivery.h): statistics for the
// "[slipstream-delivery]" line and the two-slot catch-up hand-off
// (VEYRA_EXP_RP_DELIVERY). Included from CoreTests.cpp.
#include "veyra/remoteplay/FrameDelivery.h"
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>
namespace {
constexpr HostTime kDeliveryMs=10000;
DeliveryContext deliveryContext(double hz,double fps=60,bool fg=false){DeliveryContext c;c.refreshHz=hz;c.sourceFps=fps;c.frameGeneration=fg;return c;}
struct Item{std::uint64_t seq=0;unsigned flags=0;};
auto mergeFlags=[](Item& dropped,Item& kept){kept.flags|=dropped.flags|1u;};
}

TEST(delivery_policy_parse){
    CHECK(!parseDeliveryPolicy(""));CHECK(!parseDeliveryPolicy("1"));CHECK(!parseDeliveryPolicy("CatchUp"));
    CHECK(parseDeliveryPolicy("newest")==DeliveryPolicy::Newest);
    CHECK(parseDeliveryPolicy("catchup")==DeliveryPolicy::CatchUp);CHECK(parseDeliveryPolicy("catch-up")==DeliveryPolicy::CatchUp);
    CHECK(std::string(deliveryPolicyName(DeliveryPolicy::CatchUp))=="catchup");CHECK(std::string(deliveryPolicyName(DeliveryPolicy::Newest))=="newest");
}

TEST(delivery_histogram_percentiles){
    IntervalHistogram h;CHECK(h.percentileMs(0.5)<0);CHECK(h.maxMs()<0);
    for(int i=1;i<=100;++i)h.add(i*kDeliveryMs/10);           // 0.1 .. 10.0 ms
    CHECK(h.count()==100);
    CHECK(std::abs(h.percentileMs(0.50)-5.1)<0.051);   // bin upper edge, 0.1 ms resolution
    CHECK(std::abs(h.percentileMs(0.95)-9.6)<0.051);
    CHECK(std::abs(h.maxMs()-10.0)<1e-9);
    h.add(500*kDeliveryMs);CHECK(std::abs(h.maxMs()-500.0)<1e-9);CHECK(std::abs(h.percentileMs(1.0)-500.0)<1e-9); // overflow bin -> exact max
    h.add(-5);CHECK(h.count()==101);                   // negative ignored
    h.reset();CHECK(h.count()==0);
}

TEST(delivery_spsc_ring_never_blocks){
    SpscRing<int,4> ring;
    for(int i=0;i<4;++i)CHECK(ring.push(i));
    CHECK(!ring.push(99));CHECK(ring.overflows()==1);
    std::vector<int> got;CHECK(ring.drain([&](int v){got.push_back(v);})==4);
    CHECK((got==std::vector<int>{0,1,2,3}));
    CHECK(ring.push(5));got.clear();ring.drain([&](int v){got.push_back(v);});CHECK((got==std::vector<int>{5}));
}

TEST(delivery_stats_window){
    DeliveryStats s;
    HostTime t=1000*kDeliveryMs;
    // 16.7 ms cadence, then a clump (1 ms), then a 40 ms gap.
    for(int i=0;i<10;++i){s.published(t,t+2*kDeliveryMs,false);t+=167*kDeliveryMs/10;}
    const HostTime clump=t-166*kDeliveryMs/10+kDeliveryMs; // 1.1 ms after the previous arrival
    s.published(clump,clump+kDeliveryMs,true);  // and it replaced one; decoded 1 ms after it arrived (a decoded gap of 0.1 ms)
    s.published(clump+40*kDeliveryMs,clump+42*kDeliveryMs,false);       // a 40 ms gap
    s.took(t,t-3*kDeliveryMs,false,false);s.took(t+17*kDeliveryMs,t+16*kDeliveryMs,true,false);s.took(t+50*kDeliveryMs,0,false,true);
    CHECK(!s.due(t+100*kDeliveryMs,DeliveryPolicy::Newest));   // first window started at the first pickup
    const auto line=s.due(t+5000*kDeliveryMs,DeliveryPolicy::CatchUp);
    CHECK(line);
    CHECK(line->arrived==12);CHECK(line->shown==3);CHECK(line->replaced==2);CHECK(line->caughtUp==1);
    CHECK(line->clumps==1);CHECK(line->longGaps==1);
    CHECK(std::abs(line->gapP50-16.8)<0.051);CHECK(std::abs(line->gapMax-40.0)<1e-9);
    CHECK(std::abs(line->shownGapMax-33.0)<1e-9);
    CHECK(line->decodedClumps==1);CHECK(line->decodedGapP95>0);
    CHECK(std::abs(line->pickupDelayMax-3.0)<1e-9);CHECK(std::abs(line->pickupDelayP50-1.1)<0.051); // 1 ms and 3 ms; frame 3 unknown
    const auto text=formatDeliveryLine(*line);
    for(const char* key:{"arrived=12 ","shown=3 ","replaced=2 ","gapP50=","gapP95=","gapMax=","clumps=1 ","longGaps=1 ","shownGapP95=","shownGapMax=","policy=catchup","decodedGapP95=","decodedClumps=1 ","pickupDelayP95=","pickupDelayMax=3.00 "})
        CHECK(text.find(key)!=std::string::npos);
    // The next window starts empty; nothing is due before 5 s.
    CHECK(!s.due(t+5001*kDeliveryMs,DeliveryPolicy::Newest));
    const auto empty=s.due(t+10000*kDeliveryMs,DeliveryPolicy::Newest);CHECK(empty);CHECK(empty->arrived==0&&empty->shown==0&&empty->gapP50<0);
}

TEST(delivery_stats_restart_has_no_gap){
    DeliveryStats s;s.published(100*kDeliveryMs,101*kDeliveryMs,false);s.producerRestart();s.published(5000*kDeliveryMs,5001*kDeliveryMs,false);
    s.took(5000*kDeliveryMs,0,false,false);const auto line=s.due(10000*kDeliveryMs+5000*kDeliveryMs,DeliveryPolicy::Newest);
    CHECK(line);CHECK(line->arrived==2);CHECK(line->longGaps==0);CHECK(line->gapMax<0);
}

TEST(delivery_catchup_decision){
    CatchUpInputs in;in.context=deliveryContext(180);in.olderAge100ns=5*kDeliveryMs;
    CHECK(catchUpDecision(in));
    auto x=in;x.context.frameGeneration=true;CHECK(!catchUpDecision(x));
    x=in;x.context=deliveryContext(60);CHECK(!catchUpDecision(x));          // one refresh per frame: never
    x=in;x.context=deliveryContext(100);CHECK(!catchUpDecision(x));         // under 2x
    x=in;x.context=deliveryContext(119.88,59.94);CHECK(!catchUpDecision(x)); // 2x: within 0.9 x the bound no more (delta2)
    x=in;x.context=deliveryContext(143.86,59.94);CHECK(catchUpDecision(x)); // NTSC rates, 144 Hz class
    x=in;x.context=deliveryContext(0);CHECK(!catchUpDecision(x));           // unknown display
    x=in;x.context=deliveryContext(180,0);CHECK(!catchUpDecision(x));       // unknown source
    x=in;x.previousWasCatchUp=true;CHECK(!catchUpDecision(x));
    x=in;x.guardAllows=false;CHECK(!catchUpDecision(x));
    x=in;x.olderAge100ns=17*kDeliveryMs;CHECK(!catchUpDecision(x));                 // older than one source interval
    x=in;x.olderAge100ns=-1;CHECK(!catchUpDecision(x));
    DeliveryContext unknown;CHECK(unknown.frameGeneration);                 // default: catch-up off
}

TEST(delivery_slots_newest_is_todays_mailbox){
    DeliverySlots<Item> slots(DeliveryPolicy::Newest);
    CHECK(!slots.take(0,deliveryContext(180),true,mergeFlags));
    CHECK(!slots.publish(Item{1,2u},10*kDeliveryMs,mergeFlags));
    CHECK(slots.publish(Item{2,4u},11*kDeliveryMs,mergeFlags));      // replaced, counted by the caller
    CHECK(slots.pending()==1);
    const auto t=slots.take(12*kDeliveryMs,deliveryContext(180),true,mergeFlags);
    CHECK(t);CHECK(t->value.seq==2);CHECK(t->value.flags==(1u|2u|4u));CHECK(!t->caughtUp&&!t->replaced&&!t->morePending);
    CHECK(slots.pending()==0);
}

TEST(delivery_slots_catchup_order_and_bound){
    DeliverySlots<Item> slots(DeliveryPolicy::CatchUp);
    const auto ctx=deliveryContext(180);
    CHECK(!slots.publish(Item{1,0},100*kDeliveryMs,mergeFlags));CHECK(!slots.publish(Item{2,0},101*kDeliveryMs,mergeFlags));
    auto a=slots.take(104*kDeliveryMs,ctx,true,mergeFlags);CHECK(a&&a->value.seq==1&&a->caughtUp&&a->morePending);
    // A third frame arrives before the held one is taken: two pending again,
    // but the previous pickup was a catch-up, so newest wins (no lag build-up).
    CHECK(!slots.publish(Item{3,0},106*kDeliveryMs,mergeFlags));
    auto b=slots.take(110*kDeliveryMs,ctx,true,mergeFlags);CHECK(b&&b->value.seq==3&&b->replaced&&!b->caughtUp&&b->value.flags==1u);
    // Three published before a pickup: the oldest is dropped at publish.
    CHECK(!slots.publish(Item{4,0},200*kDeliveryMs,mergeFlags));CHECK(!slots.publish(Item{5,0},201*kDeliveryMs,mergeFlags));
    CHECK(slots.publish(Item{6,0},202*kDeliveryMs,mergeFlags));CHECK(slots.pending()==2);
    auto c=slots.take(203*kDeliveryMs,ctx,true,mergeFlags);CHECK(c&&c->value.seq==5&&c->caughtUp&&c->value.flags==1u);
    auto d=slots.take(209*kDeliveryMs,ctx,true,mergeFlags);CHECK(d&&d->value.seq==6&&!d->morePending);
    // Never more than two waiting, whatever the producer does.
    for(std::uint64_t i=10;i<1000;++i){slots.publish(Item{i,0},HostTime(i)*kDeliveryMs,mergeFlags);CHECK(slots.pending()<=2);}
    // Unsafe contexts fall back to newest wins.
    slots.clear();slots.publish(Item{1,0},0,mergeFlags);slots.publish(Item{2,0},kDeliveryMs,mergeFlags);
    auto e=slots.take(2*kDeliveryMs,deliveryContext(60),true,mergeFlags);CHECK(e&&e->value.seq==2&&e->replaced);
    slots.publish(Item{3,0},0,mergeFlags);slots.publish(Item{4,0},kDeliveryMs,mergeFlags);
    auto f=slots.take(2*kDeliveryMs,deliveryContext(180,60,true),true,mergeFlags);CHECK(f&&f->value.seq==4&&f->replaced);
    slots.publish(Item{5,0},0,mergeFlags);slots.publish(Item{6,0},kDeliveryMs,mergeFlags);
    auto g=slots.take(2*kDeliveryMs,ctx,false,mergeFlags);CHECK(g&&g->value.seq==6&&g->replaced);
}

TEST(delivery_catchup_guard){
    CatchUpGuard g;CHECK(g.allows(0));
    g.caughtUp(100*kDeliveryMs);CHECK(g.awaitingFollow());g.followed(106*kDeliveryMs,180,60);CHECK(!g.awaitingFollow());CHECK(g.allows(106*kDeliveryMs)); // 6 ms < 8.5
    g.caughtUp(200*kDeliveryMs);g.followed(210*kDeliveryMs,180,60);CHECK(!g.allows(210*kDeliveryMs));CHECK(g.blocks()==1);                               // 10 ms > 8.5 (was allowed up to 11.1 before)
    CHECK(!g.allows(210*kDeliveryMs+CatchUpGuard::kHold100ns-1));CHECK(g.allows(210*kDeliveryMs+CatchUpGuard::kHold100ns));
    g.followed(300*kDeliveryMs,180,60);CHECK(g.blocks()==1);   // no catch-up pending: nothing measured
    g.reset();CHECK(g.allows(0));
    // Unknown source rate: only the 2-refresh half of the bound (11.3 ms at 180 Hz).
    CatchUpGuard u;u.caughtUp(kDeliveryMs);u.followed(12*kDeliveryMs,180,0);CHECK(u.allows(12*kDeliveryMs));
    u.caughtUp(20*kDeliveryMs);u.followed(32*kDeliveryMs,180,0);CHECK(!u.allows(32*kDeliveryMs));
    // Unknown refresh: nothing measured.
    CatchUpGuard r;r.caughtUp(kDeliveryMs);r.followed(500*kDeliveryMs,0,60);CHECK(r.allows(500*kDeliveryMs));CHECK(r.blocks()==0);
}

// The bound min(2 refreshes, half a source interval) (+2 %) and whether one
// refresh is at most 0.9 x it (delta2 review), for every row of the table in
// docs/FRAME_DELIVERY.md: 60 and 30 fps at 60, 100, 120, 144, 165, 180, 240,
// 360 Hz and 59.94, 119.88, 143.86 Hz.
TEST(delivery_catchup_cost_bound){
    struct Row{double hz,fps,boundMs;bool can;};
    const Row rows[]={
        {60,60,8.333,false},{59.94,60,8.333,false},{100,60,8.333,false},{119.88,60,8.333,false},{120,60,8.333,false},
        {143.86,60,8.333,true},{144,60,8.333,true},{165,60,8.333,true},{180,60,8.333,true},{240,60,8.333,true},
        {360,60,5.556,true},
        {60,30,16.667,false},{59.94,30,16.667,false},{100,30,16.667,true},{119.88,30,16.667,true},{120,30,16.667,true},
        {143.86,30,13.902,true},{144,30,13.889,true},{165,30,12.121,true},{180,30,11.111,true},{240,30,8.333,true},
        {360,30,5.556,true},
        {119.88,59.94,8.342,false},{50,30,16.667,false},
    };
    for(const auto& row:rows){
        const double bound=static_cast<double>(catchUpCostBound100ns(row.hz,row.fps))/1e4;
        CHECK(std::abs(bound-row.boundMs*kCatchUpTolerance)<0.001);
        CHECK(catchUpCanMeetBound(row.hz,row.fps)==row.can);
        CatchUpInputs in;in.context=deliveryContext(row.hz,row.fps);in.olderAge100ns=kDeliveryMs;
        CHECK(catchUpDecision(in)==row.can);
        // The guard trips just above the bound and not at it.
        CatchUpGuard g;const HostTime b=catchUpCostBound100ns(row.hz,row.fps);
        g.caughtUp(kDeliveryMs);g.followed(kDeliveryMs+b,row.hz,row.fps);CHECK(g.allows(kDeliveryMs+b));
        g.caughtUp(100*kDeliveryMs);g.followed(100*kDeliveryMs+b+1,row.hz,row.fps);CHECK(!g.allows(100*kDeliveryMs+b+1));
        // One refresh (the normal cost) never trips where catch-up is allowed.
        if(row.can){CatchUpGuard n;n.caughtUp(kDeliveryMs);n.followed(kDeliveryMs+static_cast<HostTime>(1e7/row.hz),row.hz,row.fps);CHECK(n.blocks()==0);}
    }
    // 120 Hz, 60 fps: one refresh (8.33 ms) is above 0.9 x 8.5 ms, so catch-up is off there now.
    {CatchUpInputs in;in.context=deliveryContext(120,60);in.olderAge100ns=kDeliveryMs;CHECK(!catchUpDecision(in));}
    // 120 Hz, 60 fps: a cost of two refreshes (16.7 ms) is a whole source interval and trips.
    CatchUpGuard g;g.caughtUp(kDeliveryMs);g.followed(kDeliveryMs+167*kDeliveryMs/10,120,60);CHECK(g.blocks()==1);
    CHECK(catchUpCostBound100ns(0,60)==0);CHECK(catchUpCostBound100ns(120,0)==0);CHECK(!catchUpCanMeetBound(0,60));CHECK(!catchUpCanMeetBound(120,0));
}

// A GPU that hiccups regularly: the hold doubles from 5 s up to 80 s while
// trips keep coming; after a quiet 80 s it starts at 5 s again; a reconnect
// ends the hold but keeps the level.
TEST(delivery_catchup_guard_backoff){
    CatchUpGuard g;HostTime now=10*1000*kDeliveryMs;
    const auto trip=[&]{g.caughtUp(now);now+=20*kDeliveryMs;g.followed(now,120,60);};
    const HostTime expect[]={5,10,20,40,80,80,80};
    std::uint64_t paid=0;
    for(const HostTime seconds:expect){
        CHECK(g.allows(now));trip();++paid;
        CHECK(g.currentHold()==seconds*1000*kDeliveryMs);
        CHECK(!g.allows(now+seconds*1000*kDeliveryMs-1));
        now+=seconds*1000*kDeliveryMs;               // re-armed exactly when the hold ends
        CHECK(g.allows(now));
    }
    CHECK(g.blocks()==paid);
    // A hiccup at every chance: 7 above-bound catch-ups in 315 s, then one per 80 s.
    // Quiet for longer than kQuiet: back to 5 s.
    now+=CatchUpGuard::kQuiet100ns+kDeliveryMs;trip();CHECK(g.currentHold()==CatchUpGuard::kHold100ns);
    // Reconnect during a hold: allowed at once, but the next trip doubles.
    g.reset();CHECK(g.allows(now));trip();CHECK(g.currentHold()==2*CatchUpGuard::kHold100ns);
}

// With DeliveryPolicy::Newest the two-slot code must hand out exactly what the
// one-slot mailbox latest_ did (RemotePlaySessionSource::read before the
// experiment): same frame, same flags, same skipped count, whatever the
// context or the guard say. Random publish/take interleavings against a
// reference written from the original code.
TEST(delivery_newest_matches_one_slot_mailbox){
    struct Reference{
        std::optional<Item> latest;std::uint64_t skipped=0;
        void publish(Item item){if(latest){item.flags|=latest->flags|1u;++skipped;}latest=item;}
        std::optional<Item> take(){auto out=latest;latest.reset();return out;}
    };
    const DeliveryContext contexts[]={deliveryContext(180),deliveryContext(240,30),deliveryContext(60),deliveryContext(120,60,true),DeliveryContext{}};
    for(std::uint32_t seed=1;seed<=20;++seed){
        std::mt19937 rng(seed);
        DeliverySlots<Item> slots(DeliveryPolicy::Newest);Reference ref;std::uint64_t skipped=0;
        HostTime now=0;
        for(std::uint64_t i=1;i<=5000;++i){
            now+=static_cast<HostTime>(rng()%200)*kDeliveryMs/10;
            if(rng()%3!=0){
                const Item item{i,(rng()%5==0)?2u:0u};
                skipped+=slots.publish(item,now,mergeFlags)?1u:0u;ref.publish(item);
            }else{
                const auto& ctx=contexts[rng()%5];
                auto got=slots.take(now,ctx,rng()%2==0,mergeFlags);auto want=ref.take();
                CHECK(got.has_value()==want.has_value());
                if(got&&want){CHECK(got->value.seq==want->seq);CHECK(got->value.flags==want->flags);
                    CHECK(!got->caughtUp&&!got->replaced&&!got->morePending);}
            }
            CHECK(slots.pending()<=1);
        }
        CHECK(skipped==ref.skipped);
    }
}

// Two threads as in RemotePlaySessionSource: the decoder owner publishes under
// the caller's lock and records statistics outside it; the engine takes under
// the same lock, waits on a wake with a timeout, and drains the statistics.
// Shutdown arrives during traffic. Run under ThreadSanitizer by verify.sh.
TEST(delivery_two_slot_handoff_threads){
    for(const auto policy:{DeliveryPolicy::Newest,DeliveryPolicy::CatchUp}){
        std::mutex mutex;std::condition_variable wake;bool closed=false,signalled=false;
        DeliverySlots<Item> slots(policy);DeliveryStats stats;
        std::atomic<std::uint64_t> published{0},dropped{0};std::atomic<bool> producerDone{false};
        const std::uint64_t total=20000;
        std::thread producer([&]{
            std::mt19937 rng(7);
            for(std::uint64_t i=1;i<=total;++i){
                bool replaced=false;
                {std::lock_guard lock(mutex);if(closed)return;
                 replaced=slots.publish(Item{i,0},HostTime(i)*kDeliveryMs,mergeFlags);CHECK(slots.pending()<=2);signalled=true;}
                wake.notify_one();                 // never notify under the lock
                stats.published(HostTime(i)*kDeliveryMs,HostTime(i)*kDeliveryMs+kDeliveryMs,replaced);
                published.fetch_add(1);dropped.fetch_add(replaced?1u:0u);
                if(rng()%4==0)std::this_thread::yield();
            }
            producerDone.store(true);
        });
        std::uint64_t last=0,taken=0,takeDrops=0;
        HostTime now=0;
        const auto ctx=deliveryContext(180);
        for(;;){
            std::optional<DeliverySlots<Item>::Taken> got;
            {std::unique_lock lock(mutex);
             if(!slots.pending())wake.wait_for(lock,std::chrono::milliseconds(2),[&]{return signalled;});
             signalled=false;
             got=slots.take(HostTime(last+1)*kDeliveryMs+kDeliveryMs,ctx,true,mergeFlags);
             // Shutdown during traffic (half taken), or once a fast producer has
             // finished and nothing waits (most frames may have been replaced).
             if(taken>=total/2||(producerDone.load()&&!slots.pending()))closed=true;
            }
            if(got){CHECK(got->value.seq>last);last=got->value.seq;++taken;takeDrops+=got->replaced?1u:0u;
                now+=kDeliveryMs;stats.took(now,0,got->replaced,got->caughtUp);}
            {std::lock_guard lock(mutex);if(closed)break;}
        }
        producer.join();
        {std::lock_guard lock(mutex);slots.clear();CHECK(slots.pending()==0);}
        const auto line=stats.due(now+DeliveryStats::kWindow100ns,policy);
        CHECK(line);CHECK(line->shown==taken);
        CHECK(line->arrived+line->ringOverflows>0);
        CHECK(taken+dropped.load()+takeDrops<=published.load());
    }
}

// The delta2 review's jitter model (build/review-delta2/advB/b.cpp): a
// catch-up every 0.1 s costing one refresh plus a pickup jitter uniform in
// [0, J). At 120 Hz with 60 fps 0.2 ms of jitter tripped the guard (margin
// 0.17 ms); that row is off now. Where catch-up stays on with a 60 fps
// source, the margin is at least 0.85 ms (143.86 Hz: 6.95 vs 8.5 ms), so 0.5 ms
// of jitter trips nothing in an hour.
TEST(delivery_catchup_jitter_margin){
    const auto tripsPerHour=[](double hz,double fps,double jitterMs){
        CatchUpGuard g;std::uint32_t s=1;int trips=0;
        for(HostTime now=10000000;now<36000LL*1000000;now+=1000000){
            if(!g.allows(now))continue;
            g.caughtUp(now);
            s=s*1664525u+1013904223u;
            const double j=static_cast<double>(s>>8)/double(1u<<24)*jitterMs;
            const auto before=g.blocks();
            g.followed(now+static_cast<HostTime>((1000.0/hz+j)*1e4),hz,fps);
            if(g.blocks()!=before)++trips;
        }
        return trips;
    };
    CHECK(tripsPerHour(120,60,0.2)>0);   // the old 120 Hz row: why it is off now
    CHECK(!catchUpCanMeetBound(120,60));
    for(const double hz:{143.86,144.0,165.0,180.0,240.0,360.0}){
        CHECK(catchUpCanMeetBound(hz,60));CHECK(tripsPerHour(hz,60,0.5)==0);
    }
    for(const double hz:{100.0,120.0,144.0,180.0,240.0}){
        CHECK(catchUpCanMeetBound(hz,30));CHECK(tripsPerHour(hz,30,0.5)==0);
    }
}
