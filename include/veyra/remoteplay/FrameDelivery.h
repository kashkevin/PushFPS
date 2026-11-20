// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Frame delivery between the Remote Play decoder owner thread and the engine
// thread (docs/FRAME_DELIVERY.md). Portable, header-only, no Windows types.
//
// Three pieces, all off unless an experiment switch asks for them:
// - DeliveryStats: the aggregated "[slipstream-delivery]" line
//   (VEYRA_EXP_RP_STATS=1). The producer (decoder owner thread) writes arrival
//   records into a fixed single-producer/single-consumer ring; the consumer
//   (engine thread, at pickup) drains it and owns every window counter. No
//   lock, no allocation after construction, no per-frame logging.
// - DeliverySlots: at most two decoded frames waiting for the engine, under
//   the caller's existing lock. With DeliveryPolicy::Newest it behaves exactly
//   like the one-slot mailbox (newest wins, the replaced frame counted).
// - catchUpDecision: VEYRA_EXP_RP_DELIVERY=catchup. When two frames wait at
//   pickup and the display refreshes at least twice per source frame, the
//   older one is handed out first (shown for about one refresh) and the newer
//   one at the next pickup, instead of the older one never being shown.
#include "Types.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace veyra::remoteplay {

enum class DeliveryPolicy : std::uint8_t { Newest, CatchUp };
inline const char* deliveryPolicyName(DeliveryPolicy p) noexcept { return p==DeliveryPolicy::CatchUp?"catchup":"newest"; }
// VEYRA_EXP_RP_DELIVERY value -> policy. Unset or unknown: no experiment
// (today's one-slot mailbox); "newest" selects the two-slot path with
// today's rule (for an A/B of the path itself), "catchup" the catch-up rule.
inline std::optional<DeliveryPolicy> parseDeliveryPolicy(std::string_view v) noexcept {
    if(v=="newest")return DeliveryPolicy::Newest;
    if(v=="catchup"||v=="catch-up")return DeliveryPolicy::CatchUp;
    return std::nullopt;
}

// Fixed histogram of intervals: 0.1 ms bins up to 100 ms, one overflow bin.
// Percentiles are bin upper edges (resolution 0.1 ms), max is exact.
class IntervalHistogram {
public:
    static constexpr int kBins=1000;           // 0.1 ms each
    static constexpr HostTime kBin100ns=1000;  // 0.1 ms in 100 ns units
    void add(HostTime interval100ns) noexcept {
        if(interval100ns<0)return;
        const HostTime bin=interval100ns/kBin100ns;
        ++bins_[static_cast<std::size_t>(std::min<HostTime>(bin,kBins))];
        ++count_;max_=std::max(max_,interval100ns);
    }
    std::uint64_t count() const noexcept { return count_; }
    // -1 with no samples. p in (0,1].
    double percentileMs(double p) const noexcept {
        if(!count_)return -1;
        const auto rank=static_cast<std::uint64_t>(std::max(1.0,static_cast<double>(count_)*p+0.999999));
        std::uint64_t seen=0;
        for(int i=0;i<=kBins;++i){
            seen+=bins_[static_cast<std::size_t>(i)];
            if(seen>=rank)return i==kBins?maxMs():std::min(maxMs(),static_cast<double>(i+1)*0.1);
        }
        return maxMs();
    }
    double maxMs() const noexcept { return count_?static_cast<double>(max_)/10000.0:-1; }
    void reset() noexcept { bins_.fill(0);count_=0;max_=0; }
private:
    std::array<std::uint32_t,kBins+1> bins_{};
    std::uint64_t count_=0;HostTime max_=0;
};

// Bounded single-producer/single-consumer ring of small records. push() never
// blocks: a full ring counts an overflow and drops the record (statistics
// only). Producer: push(). Consumer: drain(). One thread each.
template<class T,std::size_t N> class SpscRing {
    static_assert(N>=2&&(N&(N-1))==0,"power of two");
public:
    bool push(const T& value) noexcept {
        const auto head=head_.load(std::memory_order_relaxed);
        if(head-tail_.load(std::memory_order_acquire)>=N){overflows_.fetch_add(1,std::memory_order_relaxed);return false;}
        items_[head&(N-1)]=value;
        head_.store(head+1,std::memory_order_release);
        return true;
    }
    template<class F> std::size_t drain(F&& f) {
        auto tail=tail_.load(std::memory_order_relaxed);
        const auto head=head_.load(std::memory_order_acquire);
        std::size_t n=0;
        for(;tail!=head;++tail,++n)f(items_[tail&(N-1)]);
        tail_.store(tail,std::memory_order_release);
        return n;
    }
    std::uint64_t overflows() const noexcept { return overflows_.load(std::memory_order_relaxed); }
private:
    std::array<T,N> items_{};
    std::atomic<std::uint64_t> head_{0},tail_{0},overflows_{0};
};

// One "[slipstream-delivery]" window.
// Extras that separate the causes of a replaced frame (docs/FRAME_DELIVERY.md):
// arrival clumps (network: gap*, clumps), decode bursts (decodedGapP95,
// decodedClumps: decoder output closer than the arrivals were) and a blind
// engine (pickupDelay*: decoded -> taken by the engine).
struct DeliveryLine {
    std::uint64_t arrived=0,shown=0,replaced=0,clumps=0,longGaps=0,caughtUp=0,ringOverflows=0,decodedClumps=0;
    double gapP50=-1,gapP95=-1,gapMax=-1,shownGapP95=-1,shownGapMax=-1,windowS=0;
    double decodedGapP95=-1,pickupDelayP50=-1,pickupDelayP95=-1,pickupDelayMax=-1;
    DeliveryPolicy policy=DeliveryPolicy::Newest;
};
// Fixed contract keys first (S4_CONTRACT.md), then this stream's extras.
inline std::string formatDeliveryLine(const DeliveryLine& l) {
    char text[512];
    std::snprintf(text,sizeof text,"arrived=%llu shown=%llu replaced=%llu gapP50=%.2f gapP95=%.2f gapMax=%.2f clumps=%llu longGaps=%llu shownGapP95=%.2f shownGapMax=%.2f policy=%s caughtUp=%llu decodedGapP95=%.2f decodedClumps=%llu pickupDelayP50=%.2f pickupDelayP95=%.2f pickupDelayMax=%.2f windowS=%.1f ringOverflows=%llu",
        static_cast<unsigned long long>(l.arrived),static_cast<unsigned long long>(l.shown),static_cast<unsigned long long>(l.replaced),
        l.gapP50,l.gapP95,l.gapMax,static_cast<unsigned long long>(l.clumps),static_cast<unsigned long long>(l.longGaps),
        l.shownGapP95,l.shownGapMax,deliveryPolicyName(l.policy),static_cast<unsigned long long>(l.caughtUp),
        l.decodedGapP95,static_cast<unsigned long long>(l.decodedClumps),l.pickupDelayP50,l.pickupDelayP95,l.pickupDelayMax,l.windowS,
        static_cast<unsigned long long>(l.ringOverflows));
    return text;
}

// Arrival = the frame's takion-callback time (FramePacket::arrivalHost100ns),
// recorded when the decoded frame is published. "shown" = the engine took the
// frame from the mailbox (pickup); its scanout belongs to stream "present".
class DeliveryStats {
public:
    static constexpr HostTime kClump100ns=40000;      // arrivals under 4 ms after the previous
    static constexpr HostTime kLongGap100ns=250000;   // gaps over 25 ms
    static constexpr HostTime kWindow100ns=50000000;  // 5 s
    // ---- producer (decoder owner thread) ----
    // `decoded100ns`: when the decoder handed the frame out (0 = unknown).
    void published(HostTime arrival100ns,HostTime decoded100ns,bool replacedOne) noexcept {
        Record r{lastArrival_>0&&arrival100ns>=lastArrival_?arrival100ns-lastArrival_:-1,
                 lastDecoded_>0&&decoded100ns>=lastDecoded_?decoded100ns-lastDecoded_:-1,replacedOne};
        if(arrival100ns>0)lastArrival_=arrival100ns;
        if(decoded100ns>0)lastDecoded_=decoded100ns;
        ring_.push(r);
    }
    // A reconnect: the next arrival's gap means nothing.
    void producerRestart() noexcept { lastArrival_=0;lastDecoded_=0; }
    // ---- consumer (engine thread) ----
    // The engine took a frame: `now` is the pickup time. `replacedHere`: an
    // older frame was dropped at this pickup (two-slot newest-wins);
    // `caughtUp`: the older of two was handed out first.
    // `decoded100ns`: the frame's decode time (0 = unknown, no pickup delay).
    void took(HostTime now,HostTime decoded100ns,bool replacedHere,bool caughtUp) noexcept {
        drainArrivals();
        if(!windowStart_)windowStart_=now;
        if(decoded100ns>0&&now>=decoded100ns)pickupDelays_.add(now-decoded100ns);
        ++line_.shown;line_.replaced+=replacedHere?1u:0u;line_.caughtUp+=caughtUp?1u:0u;
        if(lastShown_>0&&now>=lastShown_)shownGaps_.add(now-lastShown_);
        lastShown_=now;
    }
    void consumerRestart() noexcept { lastShown_=0; }
    // Consumer: the finished window when 5 s have passed since it started,
    // and a new window begins. At most one line per 5 s by construction.
    std::optional<DeliveryLine> due(HostTime now,DeliveryPolicy policy) {
        if(!windowStart_||now-windowStart_<kWindow100ns)return std::nullopt;
        drainArrivals();
        DeliveryLine out=line_;
        out.gapP50=gaps_.percentileMs(0.50);out.gapP95=gaps_.percentileMs(0.95);out.gapMax=gaps_.maxMs();
        out.shownGapP95=shownGaps_.percentileMs(0.95);out.shownGapMax=shownGaps_.maxMs();
        out.decodedGapP95=decodedGaps_.percentileMs(0.95);
        out.pickupDelayP50=pickupDelays_.percentileMs(0.50);out.pickupDelayP95=pickupDelays_.percentileMs(0.95);out.pickupDelayMax=pickupDelays_.maxMs();
        out.windowS=static_cast<double>(now-windowStart_)/1e7;out.policy=policy;
        const auto overflows=ring_.overflows();out.ringOverflows=overflows-overflowBase_;overflowBase_=overflows;
        line_={};gaps_.reset();shownGaps_.reset();decodedGaps_.reset();pickupDelays_.reset();windowStart_=now;
        return out;
    }
private:
    struct Record{HostTime gap=-1,decodedGap=-1;bool replaced=false;};
    void drainArrivals() {
        ring_.drain([&](const Record& r){
            ++line_.arrived;line_.replaced+=r.replaced?1u:0u;
            if(r.gap>=0){gaps_.add(r.gap);if(r.gap<kClump100ns)++line_.clumps;if(r.gap>kLongGap100ns)++line_.longGaps;}
            if(r.decodedGap>=0){decodedGaps_.add(r.decodedGap);if(r.decodedGap<kClump100ns)++line_.decodedClumps;}
        });
    }
    SpscRing<Record,512> ring_;
    HostTime lastArrival_=0,lastDecoded_=0;                    // producer only
    DeliveryLine line_;IntervalHistogram gaps_,shownGaps_,decodedGaps_,pickupDelays_; // consumer only
    HostTime windowStart_=0,lastShown_=0;std::uint64_t overflowBase_=0; // consumer only
};

// What the engine knows and the mailbox does not. Written by the engine
// thread, read by the engine thread at pickup: plain values, the caller keeps
// them in atomics if another thread writes them.
struct DeliveryContext {
    double refreshHz=0;      // display refresh; 0 = unknown
    double sourceFps=0;      // nominal stream rate; 0 = unknown
    bool frameGeneration=true; // unknown counts as on: catch-up stays off
};

// Catch-up rule, pure. Two frames are waiting at pickup; hand out the older
// one first only when all of these hold, else newest wins (today's rule):
// - frame generation off (its pacing and history assume today's hand-off);
// - refresh and source rate known, refresh >= 2 x source rate (with 2 %
//   tolerance for 119.88 vs 59.94): the older frame then costs the newer one
//   about one refresh, never a whole source interval;
// - the previous pickup was not itself a catch-up (at most one in a row, so a
//   lag never builds up: after a catch-up the next double pickup takes the
//   newest);
// - the older frame is not older than one source interval at pickup (an old
//   frame shown late adds age, not smoothness);
// - the measured cost (time from a catch-up pickup to the next pickup) has
//   stayed within two refreshes (see CatchUpGuard).
struct CatchUpInputs {
    DeliveryContext context;HostTime olderAge100ns=0;bool previousWasCatchUp=false;bool guardAllows=true;
};
inline bool catchUpDecision(const CatchUpInputs& in) noexcept {
    const auto& c=in.context;
    if(c.frameGeneration||!(c.refreshHz>=1.0)||!(c.sourceFps>=1.0))return false;
    if(c.refreshHz<2.0*c.sourceFps*0.98)return false;
    if(in.previousWasCatchUp||!in.guardAllows)return false;
    const double interval100ns=1e7/c.sourceFps;
    return in.olderAge100ns>=0&&static_cast<double>(in.olderAge100ns)<=interval100ns;
}

// Measures what a catch-up cost: the time from the catch-up pickup to the
// pickup of the frame held back. With a queue-1 vsync presenter that is the
// processing time rounded up to the next vblank; above two refreshes the
// engine is too slow for the rule to cost only one refresh, so it is switched
// off for kHold100ns and then tried again. Consumer thread only.
class CatchUpGuard {
public:
    static constexpr HostTime kHold100ns=50000000; // 5 s
    bool allows(HostTime now) const noexcept { return now>=blockedUntil_; }
    void caughtUp(HostTime now) noexcept { pendingSince_=now; }
    // The pickup that followed a catch-up.
    void followed(HostTime now,double refreshHz) noexcept {
        if(!pendingSince_)return;
        const HostTime cost=now-pendingSince_;pendingSince_=0;
        if(refreshHz>=1.0&&static_cast<double>(cost)>2.0*1e7/refreshHz){blockedUntil_=now+kHold100ns;++blocks_;}
    }
    bool awaitingFollow() const noexcept { return pendingSince_!=0; }
    std::uint64_t blocks() const noexcept { return blocks_; }
    void reset() noexcept { pendingSince_=0;blockedUntil_=0; }
private:
    HostTime pendingSince_=0,blockedUntil_=0;std::uint64_t blocks_=0;
};

// At most two decoded frames, oldest first. Not thread-safe: the caller holds
// its own lock around every call (RemotePlaySessionSource::mutex_), exactly as
// it did around the one-slot latest_. Strict order: a frame is never handed
// out after a newer one. A third frame replaces the oldest.
// `merge(dropped,kept)` is called for every frame dropped unseen, so the
// caller can carry its flags (discontinuity, resize, drop) to the kept one.
template<class T> class DeliverySlots {
public:
    struct Taken {T value;HostTime arrival=0;bool replaced=false,caughtUp=false,morePending=false;};
    explicit DeliverySlots(DeliveryPolicy policy=DeliveryPolicy::Newest):policy_(policy){}
    DeliveryPolicy policy() const noexcept { return policy_; }
    std::size_t pending() const noexcept { return count_; }
    // Returns true when a waiting frame was dropped to make room.
    template<class Merge> bool publish(T value,HostTime arrival,Merge&& merge) {
        bool dropped=false;
        const std::size_t capacity=policy_==DeliveryPolicy::CatchUp?2u:1u;
        if(count_==capacity){
            // Newest: the one waiting frame is replaced (today's mailbox).
            // CatchUp: the oldest of two goes; the two newest stay.
            // The dropped frame's flags go to the next frame handed out.
            if(capacity==2){merge(slots_[0].value,slots_[1].value);slots_[0]=std::move(slots_[1]);}
            else merge(slots_[0].value,value);
            dropped=true;
            --count_;
        }
        slots_[count_]=Slot{std::move(value),arrival};++count_;
        return dropped;
    }
    template<class Merge> std::optional<Taken> take(HostTime now,const DeliveryContext& context,bool guardAllows,Merge&& merge) {
        if(!count_)return std::nullopt;
        Taken out;
        if(count_==2){
            const bool catchUp=catchUpDecision({context,now-slots_[0].arrival,lastWasCatchUp_,guardAllows});
            if(catchUp){
                out.value=std::move(slots_[0].value);out.arrival=slots_[0].arrival;out.caughtUp=true;out.morePending=true;
                slots_[0]=std::move(slots_[1]);slots_[1]=Slot{};count_=1;lastWasCatchUp_=true;
                return out;
            }
            merge(slots_[0].value,slots_[1].value);out.replaced=true;
            slots_[0]=std::move(slots_[1]);slots_[1]=Slot{};count_=1;
        }
        out.value=std::move(slots_[0].value);out.arrival=slots_[0].arrival;
        slots_[0]=Slot{};count_=0;lastWasCatchUp_=false;
        return out;
    }
    void clear() { slots_[0]=Slot{};slots_[1]=Slot{};count_=0;lastWasCatchUp_=false; }
private:
    struct Slot{T value{};HostTime arrival=0;};
    std::array<Slot,2> slots_{};std::size_t count_=0;bool lastWasCatchUp_=false;DeliveryPolicy policy_;
};
} // namespace veyra::remoteplay
