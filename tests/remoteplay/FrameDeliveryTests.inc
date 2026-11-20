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
    x=in;x.context=deliveryContext(119.88,59.94);CHECK(catchUpDecision(x)); // 2x with NTSC rates
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
    g.caughtUp(100*kDeliveryMs);CHECK(g.awaitingFollow());g.followed(106*kDeliveryMs,180);CHECK(!g.awaitingFollow());CHECK(g.allows(106*kDeliveryMs)); // 6 ms < 2 x 5.56
    g.caughtUp(200*kDeliveryMs);g.followed(215*kDeliveryMs,180);CHECK(!g.allows(215*kDeliveryMs));CHECK(g.blocks()==1);                               // 15 ms > 11.1
    CHECK(!g.allows(215*kDeliveryMs+CatchUpGuard::kHold100ns-1));CHECK(g.allows(215*kDeliveryMs+CatchUpGuard::kHold100ns));
    g.followed(300*kDeliveryMs,180);CHECK(g.blocks()==1);   // no catch-up pending: nothing measured
    g.reset();CHECK(g.allows(0));
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
