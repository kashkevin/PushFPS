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
class ReorderQueueModel {
public:
    static constexpr std::size_t kSlots=64; // TAKION_AV_VIDEO_REORDER_QUEUE_SIZE_EXP 6
    static constexpr std::size_t kSkipMemory=4; // CHIAKI_VEYRA_REORDER_SKIP_MEMORY
    struct Released {std::uint16_t seq=0;std::int64_t atUs=0;};
    struct Counters : ReorderEvidence {
        std::uint64_t dropped=0,overshootUs=0;
    };
    // timeoutUs: what the connection starts with (the library latches it at connect).
    explicit ReorderQueueModel(std::uint32_t timeoutUs)noexcept:timeoutUs_(timeoutUs){counters_.reorderTimeoutUs=timeoutUs;}
    // chiaki_veyra_transport_set_av_reorder_timeout_live: taken at the next look.
    void setTimeoutUs(std::uint32_t us)noexcept{live_=us>16000u?16000u:us;}
    void packet(std::int64_t nowUs,std::uint16_t seq,std::vector<Released>& out){
        now_=nowUs;++counters_.videoPackets;
        if(!initialized_){initialized_=true;begin_=seq;count_=0;waitStart_=0;waitSeq_=seq;}
        lateArrival(seq);
        push(seq);
        flush(nowUs,out);
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
    static Seq add(Seq a,Seq b)noexcept{return static_cast<Seq>(static_cast<std::uint32_t>(static_cast<std::uint16_t>(a))+static_cast<std::uint32_t>(static_cast<std::uint16_t>(b)));}
    bool& slot(Seq seq)noexcept{return set_[static_cast<std::size_t>(seq%kSlots)];}
    void dropped()noexcept{++counters_.dropped;}
    void lateness(std::int64_t us)noexcept{
        std::size_t bucket=0;
        while(bucket<kReorderLatenessEdgesUs.size()&&us>static_cast<std::int64_t>(kReorderLatenessEdgesUs[bucket]))++bucket;
        ++counters_.lateness[bucket];
    }
    // veyra_takion_reorder_late_arrival
    void lateArrival(Seq seq)noexcept{
        if(!lt(seq,begin_))return;
        for(const auto& skip:skips_){
            if(!skip.count||lt(seq,skip.begin)||!lt(seq,add(skip.begin,skip.count)))continue;
            ++counters_.lateArrivals;lateness(now_-skip.waitStart);
            return;
        }
    }
    // chiaki_reorder_queue_push with CHIAKI_REORDER_QUEUE_DROP_STRATEGY_BEGIN
    void push(Seq seq)noexcept{
        Seq end=add(begin_,count_);
        if(ge(seq,begin_)&&lt(seq,end)){
            if(slot(seq)){dropped();return;} // received twice
            slot(seq)=true;return;
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
        slot(seq)=true;
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
                if(waitStart_!=0&&seq==waitSeq_)lateness(now-waitStart_); // the awaited packet came
                out.push_back({static_cast<std::uint16_t>(seq),now});
            }
            if(progress)waitStart_=0;
            if(count_==0)break;
            if(waitStart_!=0&&begin_!=waitSeq_){
                if(gt(begin_,waitSeq_))waitSeq_=begin_; // the head moved on within the same burst: same budget
                else{waitStart_=now;waitSeq_=begin_;break;}
            }
            if(waitStart_==0){++counters_.gaps;waitStart_=now;waitSeq_=begin_;break;}
            if(now-waitStart_<=static_cast<std::int64_t>(timeoutUs_))break;
            std::uint64_t skipped=0;
            while(skipped<count_&&!slot(add(begin_,skipped)))++skipped;
            if(skipped>=count_)break;
            ++counters_.timeouts;counters_.skipped+=skipped;
            counters_.overshootUs+=static_cast<std::uint64_t>(now-waitStart_-static_cast<std::int64_t>(timeoutUs_));
            skips_[skipNext_]={begin_,skipped,waitStart_};skipNext_=(skipNext_+1)%kSkipMemory;
            begin_=add(begin_,skipped);count_-=skipped;
            waitStart_=0;progress=true;
        }
    }
    struct Skip {Seq begin=0;std::uint64_t count=0;std::int64_t waitStart=0;};
    std::array<bool,kSlots> set_{};
    std::array<Skip,kSkipMemory> skips_{};
    std::size_t skipNext_=0;
    std::uint64_t count_=0;
    std::int64_t waitStart_=0,now_=0;
    std::uint32_t timeoutUs_,live_=0;
    Seq begin_=0,waitSeq_=0;
    bool initialized_=false;
    Counters counters_{};
};
} // namespace veyra::remoteplay
