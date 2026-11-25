// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// A model of the pinned chiaki-ng video reorder queue with chiaki patch 0004
// (lib/src/reorderqueue.c, takion_av_queue_flush_with_timeout and the Takion
// thread's timed wait in lib/src/takion.c), in virtual time. For the unit
// tests of ReorderWindow.h and the Linux simulator in
// tools/remoteplay_latency_bench/reorder_sim.cpp only: the application never
// uses it. Written from the C source line by line (the queue is the library's
// AGPL-3.0 code restated; this file is GPL-3.0-only like the rest of the
// project's Remote Play code, and both licences bind the combined program).
// It is a model: nothing here has run against a console.
//
// Time is microseconds, as in the library. packet() is a video packet read
// from the socket; timer() is the socket wait running out. The caller plays
// the Takion thread: after every call, nextWakeUs() says when timer() is due
// if no packet arrives first.
#include "ReorderWindow.h"
#include <vector>
namespace veyra::remoteplay {
// The frame fields of a video packet (ChiakiTakionAVPacket); all 0 when the
// caller does not model frames (then the early loss decision never cuts).
struct ReorderUnit {std::uint16_t frame=0,unit=0,total=0,fec=0;};
// Chiaki patch 0004 options (CHIAKI_VEYRA_OPTION_*), latched at connect.
struct ReorderOptions {bool wrapFix=false,earlyLoss=false;};
class ReorderQueueModel {
public:
    static constexpr std::size_t kSlots=64; // TAKION_AV_VIDEO_REORDER_QUEUE_SIZE_EXP 6
    static constexpr std::size_t kSkipMemory=4; // CHIAKI_VEYRA_REORDER_SKIP_MEMORY
    static constexpr std::size_t kLateSeenMemory=32; // CHIAKI_VEYRA_LATE_SEEN_MEMORY
    static constexpr std::int64_t kEarlyLossCooldownUs=300000000; // CHIAKI_VEYRA_EARLY_LOSS_COOLDOWN_US
    struct Released {std::uint16_t seq=0;std::int64_t atUs=0;};
    struct Counters : ReorderEvidence {
        std::uint64_t dropped=0,overshootUs=0;
        std::uint64_t earlyCuts=0,earlyWrong=0; // CHIAKI_VEYRA_STAT_EARLY_LOSS_CUTS / _WRONG
    };
    using Unit=ReorderUnit;
    using Options=ReorderOptions;
    // timeoutUs: what the connection starts with (the library latches it at connect).
    explicit ReorderQueueModel(std::uint32_t timeoutUs)noexcept:timeoutUs_(timeoutUs){counters_.reorderTimeoutUs=timeoutUs;}
    ReorderQueueModel(std::uint32_t timeoutUs,Options options)noexcept:timeoutUs_(timeoutUs){counters_.reorderTimeoutUs=timeoutUs;options_=options;}
    // chiaki_veyra_transport_set_av_reorder_timeout_live: taken at the next look.
    void setTimeoutUs(std::uint32_t us)noexcept{live_=us>16000u?16000u:us;}
    // True when the packet was one a timeout had given up on (a late arrival),
    // the first time it came (a repeat is counted as a late arrival but is not
    // new evidence for its frame).
    bool packet(std::int64_t nowUs,std::uint16_t seq,std::vector<Released>& out,Unit unit={}){
        now_=nowUs;++counters_.videoPackets;
        if(!initialized_){
            initialized_=true;
            const std::uint16_t first=unit.unit>0?static_cast<std::uint16_t>(seq-unit.unit):seq; // takion_handle_packet_av
            begin_=first;count_=0;waitStart_=0;waitSeq_=first;
        }
        pendingUnit_=unit;
        const bool late=lateArrival(seq);
        push(seq,unit);
        flush(nowUs,out);
        return late;
    }
    // The wait ran out. Exactly at the limit the library does not give up yet
    // (`<=`) and its loop comes straight back; real time has moved on by then.
    void timer(std::int64_t nowUs,std::vector<Released>& out){
        now_=nowUs;
        if(!initialized_)return;
        flush(nowUs,out);
        if(waitStart_!=0&&static_cast<std::int64_t>(timeoutUs_)-(nowUs-waitStart_)<=0){now_=nowUs+1;flush(nowUs+1,out);}
    }
    // takion_av_queues_next_timeout_ms from `nowUs`: whole milliseconds, rounded up.
    std::optional<std::int64_t> nextWakeUs(std::int64_t nowUs)const noexcept{
        if(!initialized_||waitStart_==0)return {};
        const std::int64_t remaining=static_cast<std::int64_t>(timeoutUs_)-(nowUs-waitStart_);
        if(remaining<=0)return nowUs;
        return nowUs+((remaining+999)/1000)*1000;
    }
    std::int64_t nowUs()const noexcept{return now_;}
    bool waiting()const noexcept{return waitStart_!=0;}
    const Counters& counters()const noexcept{return counters_;}
private:
    // The library keeps packet indexes in 64-bit variables: comparisons look at
    // the low 16 bits (RFC 1982, chiaki_seq_num_16_lt / _gt), "equal" compares
    // all 64, and adding does not wrap at 16 bits (two uint16_t promoted to int).
    // Kept exactly so: see the note on the index wrap in the experiments doc.
    using Seq=std::uint64_t;
    static bool lt(Seq x,Seq y)noexcept{
        const auto a=static_cast<std::uint16_t>(x),b=static_cast<std::uint16_t>(y);
        if(a==b)return false;
        const std::int32_t d=static_cast<std::int32_t>(b)-static_cast<std::int32_t>(a);
        return (a<b&&d<32768)||(a>b&&-d>32768);
    }
    static bool gt(Seq x,Seq y)noexcept{
        const auto a=static_cast<std::uint16_t>(x),b=static_cast<std::uint16_t>(y);
        if(a==b)return false;
        const std::int32_t d=static_cast<std::int32_t>(b)-static_cast<std::int32_t>(a);
        return (a<b&&d>32768)||(a>b&&-d<32768);
    }
    static bool ge(Seq a,Seq b)noexcept{return a==b||gt(a,b);}
    Seq add(Seq a,Seq b)const noexcept{
        const std::uint32_t sum=static_cast<std::uint32_t>(static_cast<std::uint16_t>(a))+static_cast<std::uint32_t>(static_cast<std::uint16_t>(b));
        return options_.wrapFix?static_cast<Seq>(static_cast<std::uint16_t>(sum)):static_cast<Seq>(sum); // CHIAKI_VEYRA_OPTION_REORDER_WRAP_FIX
    }
    bool& slot(Seq seq)noexcept{return set_[static_cast<std::size_t>(seq%kSlots)];}
    Unit& unitAt(Seq seq)noexcept{return units_[static_cast<std::size_t>(seq%kSlots)];}
    void dropped()noexcept{++counters_.dropped;}
    void lateness(std::int64_t us)noexcept{
        std::size_t bucket=0;
        while(bucket<kReorderLatenessEdgesUs.size()&&us>static_cast<std::int64_t>(kReorderLatenessEdgesUs[bucket]))++bucket;
        ++counters_.lateness[bucket];
    }
    // veyra_takion_reorder_late_arrival
    bool lateArrival(Seq seq)noexcept{
        if(!lt(seq,begin_))return false;
        for(const auto& skip:skips_){
            if(!skip.count||lt(seq,skip.begin)||!lt(seq,add(skip.begin,skip.count)))continue;
            ++counters_.lateArrivals;lateness(now_-skip.waitStart);
            outOfOrder_=now_?now_:1;
            if(skip.early)++counters_.earlyWrong;
            for(const auto v:lateSeen_)if(v==seq+1)return false;
            lateSeen_[lateSeenNext_]=seq+1;lateSeenNext_=(lateSeenNext_+1)%kLateSeenMemory;
            if(releasedValid_&&pendingUnit_.frame==released_.frame)++releasedSeen_;
            return true;
        }
        return false;
    }
    // veyra_takion_early_loss
    bool earlyLoss(std::int64_t now)noexcept{
        if(!options_.earlyLoss||!releasedValid_)return false;
        if(outOfOrder_!=0&&now-outOfOrder_<kEarlyLossCooldownUs)return false;
        std::uint64_t missing=0;
        while(missing<count_&&!slot(add(begin_,missing)))++missing;
        if(missing==0||missing>=count_)return false;
        const Unit& next=unitAt(add(begin_,missing));
        if(next.frame!=released_.frame)return false;
        if(static_cast<std::uint64_t>(static_cast<std::uint16_t>(next.unit-released_.unit-1))!=missing)return false;
        const std::uint64_t below=next.unit,absent=below>releasedSeen_?below-releasedSeen_:0;
        return absent>next.fec;
    }
    // chiaki_reorder_queue_push with CHIAKI_REORDER_QUEUE_DROP_STRATEGY_BEGIN
    void push(Seq seq,Unit unit)noexcept{
        Seq end=add(begin_,count_);
        if(ge(seq,begin_)&&lt(seq,end)){
            if(slot(seq)){dropped();return;} // received twice
            slot(seq)=true;unitAt(seq)=unit;return;
        }
        if(lt(seq,begin_)){dropped();return;}
        if(!ge(seq,end)){
            if(count_==0){begin_=seq;end=seq;}
            else{dropped();return;}
        }
        std::uint64_t freeSlots=kSlots-count_;
        Seq totalEnd=add(end,freeSlots);
        const Seq newEnd=add(seq,1);
        if(lt(totalEnd,newEnd)){
            while(count_>0&&lt(totalEnd,newEnd)){
                if(slot(begin_))dropped();
                begin_=add(begin_,1);--count_;
                freeSlots=kSlots-count_;totalEnd=add(end,freeSlots);
            }
            if(count_==0)begin_=seq;
        }
        end=add(begin_,count_);
        while(lt(end,newEnd)){++count_;slot(end)=false;end=add(begin_,count_);}
        slot(seq)=true;unitAt(seq)=unit;
    }
    bool pull(Seq& seq)noexcept{
        if(count_==0||!slot(begin_))return false;
        seq=begin_;begin_=add(begin_,1);--count_;return true;
    }
    // takion_av_queue_flush_with_timeout
    void flush(std::int64_t now,std::vector<Released>& out){
        if(live_&&live_!=timeoutUs_){timeoutUs_=live_;counters_.reorderTimeoutUs=live_;} // veyra_takion_reorder_refresh
        bool progress=true;
        while(progress){
            progress=false;
            Seq seq=0;
            while(pull(seq)){
                progress=true;
                if(waitStart_!=0&&seq==waitSeq_){lateness(now-waitStart_);outOfOrder_=now?now:1;} // the awaited packet came
                if(!releasedValid_||unitAt(seq).frame!=released_.frame)releasedSeen_=0;
                ++releasedSeen_;released_=unitAt(seq);releasedValid_=true;
                out.push_back({static_cast<std::uint16_t>(seq),now});
            }
            if(progress)waitStart_=0;
            if(count_==0)break;
            if(waitStart_!=0&&begin_!=waitSeq_){
                if(gt(begin_,waitSeq_))waitSeq_=begin_; // the head moved on within the same burst: same budget
                else{waitStart_=now;waitSeq_=begin_;break;}
            }
            if(waitStart_==0){++counters_.gaps;waitStart_=now;waitSeq_=begin_;if(!earlyLoss(now))break;}
            else if(now-waitStart_<=static_cast<std::int64_t>(timeoutUs_)&&!earlyLoss(now))break;
            const bool early=now-waitStart_<=static_cast<std::int64_t>(timeoutUs_);
            std::uint64_t skipped=0;
            while(skipped<count_&&!slot(add(begin_,skipped)))++skipped;
            if(skipped>=count_)break;
            ++counters_.timeouts;counters_.skipped+=skipped;
            if(early)++counters_.earlyCuts;
            else counters_.overshootUs+=static_cast<std::uint64_t>(now-waitStart_-static_cast<std::int64_t>(timeoutUs_));
            skips_[skipNext_]={begin_,skipped,waitStart_,early};skipNext_=(skipNext_+1)%kSkipMemory;
            begin_=add(begin_,skipped);count_-=skipped;
            waitStart_=0;progress=true;
        }
    }
    struct Skip {Seq begin=0;std::uint64_t count=0;std::int64_t waitStart=0;bool early=false;};
    std::array<bool,kSlots> set_{};
    std::array<Unit,kSlots> units_{};
    std::array<Seq,kLateSeenMemory> lateSeen_{};std::size_t lateSeenNext_=0;
    Options options_{};Unit released_{},pendingUnit_{};bool releasedValid_=false;std::int64_t outOfOrder_=0;std::uint64_t releasedSeen_=0;
    std::array<Skip,kSkipMemory> skips_{};
    std::size_t skipNext_=0;
    std::uint64_t count_=0;
    std::int64_t waitStart_=0,now_=0;
    std::uint32_t timeoutUs_,live_=0;
    Seq begin_=0,waitSeq_=0;
    bool initialized_=false;
    Counters counters_{};
};
// Per-frame late-packet evidence as chiaki patch 0004 keeps it on the Takion
// thread (veyra_frame_evidence*, chiaki_veyra_takion_video_frame_failed in
// lib/src/takion.c): the last kFrames video frames by 16-bit frame index; a
// frame counts once when FEC failed on it, one of its given-up packets arrived
// afterwards, and the library saw (released in order, or late) at least as
// many of its packets as it has source units. Compared with the C source by
// reorder_library_check.sh.
class LateFrameEvidenceModel {
public:
    static constexpr std::size_t kFrames=16; // CHIAKI_VEYRA_FRAME_MEMORY
    // A packet released in order (late=false) or arriving after it was given up.
    void unit(std::uint16_t frame,std::uint16_t total,std::uint16_t fec,bool late)noexcept{
        Slot* e=slot(frame);
        if(!e)return;
        if(total>fec)e->needed=static_cast<std::uint16_t>(total-fec);
        ++e->seen;
        if(late)e->late=true;
        check(*e);
    }
    void failed(std::uint16_t frame)noexcept{
        Slot* e=slot(frame);
        if(!e)return;
        e->failed=true;
        check(*e);
    }
    std::uint64_t lateFramesFailed()const noexcept{return count_;}
private:
    struct Slot {std::uint16_t frame=0,needed=0;std::uint32_t seen=0;bool used=false,failed=false,late=false,counted=false;};
    static bool gt16(std::uint16_t a,std::uint16_t b)noexcept{
        if(a==b)return false;
        const std::int32_t d=static_cast<std::int32_t>(b)-static_cast<std::int32_t>(a);
        return (a<b&&d>32768)||(a>b&&-d<32768);
    }
    Slot* slot(std::uint16_t frame)noexcept{
        Slot& e=slots_[frame%kFrames];
        if(e.used&&e.frame==frame)return &e;
        if(e.used&&!gt16(frame,e.frame))return nullptr;
        e=Slot{};e.used=true;e.frame=frame;
        return &e;
    }
    void check(Slot& e)noexcept{
        if(!e.failed||!e.late||e.counted||e.needed==0||e.seen<e.needed)return;
        e.counted=true;++count_;
    }
    std::array<Slot,kFrames> slots_{};
    std::uint64_t count_=0;
};
} // namespace veyra::remoteplay
