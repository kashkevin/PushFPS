// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "sync" (Package 9: one decoder reset in each 30 Mbit/s run, 13 and 10 frames dropped, no network loss):
// VideoIngress names what opened each wait for a keyframe (RecoveryCause), and the two signatures of the evidence
// replayed on the real queue. Code-level tests with synthetic times, not a PS5 measurement.
namespace ingress_recovery_tests {
inline std::uint64_t cause(const VideoQueueStats& s,RecoveryCause c){return s.recoveryCauses[static_cast<std::size_t>(c)];}
inline std::uint64_t episodes(const VideoQueueStats& s){std::uint64_t n=0;for(auto v:s.recoveryCauses)n+=v;return n;}
constexpr HostTime kFrame=166833; // 59.94 frames a second, 100 ns
}

TEST(sync_ingress_package9_run_c30_132145_stale_clock_signature){
    using namespace ingress_recovery_tests;
    // c-30@132145 at 66 s: no loss, no FEC failure, no decoder error; the ingress dropped 10 frames, the video
    // callback refused 9, one frame waited when it happened. Old rule: the owner read its clock (T), a frame
    // stamped after T was pushed before the owner took the lock, and the owner's look called it expired.
    for(bool fix:{false,true}){
        QueueLimits l;l.expiryFix=fix;VideoIngress q(l);start(q);
        CHECK(q.tryPop(0));                                            // the first keyframe, decoded
        HostTime t=0;std::uint16_t index=1;
        for(;index<60;++index){t+=kFrame;CHECK(q.push(frame(index,false,t),t)==PushStatus::Accepted);CHECK(q.tryPop(t));}
        const HostTime ownerClock=t+kFrame-10;                          // read just before the next frame was stamped
        t+=kFrame;CHECK(q.push(frame(index++,false,t),t)==PushStatus::Accepted);
        const auto popped=q.tryPop(ownerClock);
        unsigned refused=0;
        for(int i=0;i<9;++i){t+=kFrame;if(q.push(frame(index++,false,t),t)==PushStatus::WaitingForIdr)++refused;else CHECK(q.tryPop(t));}
        t+=kFrame;CHECK(q.push(frame(index++,true,t),t)==PushStatus::Accepted);
        const auto s=q.stats();
        if(!fix){
            CHECK(!popped&&cause(s,RecoveryCause::StaleClock)==1&&episodes(s)==1&&s.lastRecoveryDepth==1);
            CHECK(refused==9&&s.refusedWaiting==9&&s.dropped==10);     // Package 9: callbackRejected 9, frameDrops 10
        }else{
            CHECK(popped&&episodes(s)==0&&s.staleClockTolerated==1&&refused==0&&s.dropped==0);
        }
    }
}

TEST(sync_ingress_package9_run_c30_131516_startup_overflow_signature){
    using namespace ingress_recovery_tests;
    // c-30@131516 at start: the first hardware output took 209 ms (38-41 ms in the other seven runs); 13 frames
    // dropped, 9 refused. The owner, inside the first decode, takes nothing while frames keep arriving: the
    // fifth waiting frame finds maxFrames (4) queued, those 4 are dropped and every frame refused until the
    // keyframe the owner can only ask for once it is back. The stale-clock rule plays no part (same either way).
    for(bool fix:{false,true}){
        QueueLimits l;l.expiryFix=fix;VideoIngress q(l);start(q);    // keyframe 0 waits (the decoder is opening)
        HostTime t=0;unsigned refused=0;
        for(std::uint16_t index=1;index<=12;++index){t+=kFrame;if(q.push(frame(index,false,t),t)==PushStatus::WaitingForIdr)++refused;}
        const auto s=q.stats();
        CHECK(cause(s,RecoveryCause::Overflow)==1&&episodes(s)==1&&s.lastRecoveryDepth==4);
        CHECK(refused==9&&s.refusedWaiting==9&&s.dropped==13);          // Package 9: frameDrops 13, callbackRejected 9
    }
}

TEST(sync_ingress_each_cause_is_counted_once_per_episode){
    using namespace ingress_recovery_tests;
    {   // a real age: the head frame older than 100 ms when the owner looks
        VideoIngress q;start(q);CHECK(!q.tryPop(QueueLimits{}.maxAge100ns+1));
        const auto s=q.stats();CHECK(cause(s,RecoveryCause::Expired)==1&&episodes(s)==1&&s.lastRecoveryDepth==1);
        // still waiting: more non-keyframes do not open another episode
        for(std::uint16_t i=1;i<5;++i)CHECK(q.push(frame(i,false,2000000+i),2000000+i)==PushStatus::WaitingForIdr);
        CHECK(episodes(q.stats())==1&&q.stats().refusedWaiting==4);
    }
    {   // a wire index gap
        VideoIngress q;start(q);CHECK(q.tryPop(0));CHECK(q.push(frame(3,false,10),10)==PushStatus::WaitingForIdr);
        CHECK(cause(q.stats(),RecoveryCause::Gap)==1&&episodes(q.stats())==1);
    }
    {   // the library reported a lost frame
        VideoIngress q;start(q);CHECK(q.tryPop(0));auto f=frame(1,false,10);f.framesLost=1;
        CHECK(q.push(std::move(f),10)==PushStatus::WaitingForIdr);CHECK(cause(q.stats(),RecoveryCause::Lost)==1);
    }
    {   // the decoder owner asked for a keyframe
        VideoIngress q;start(q);CHECK(q.tryPop(0));q.requireKeyframe();CHECK(cause(q.stats(),RecoveryCause::Decoder)==1);
        q.requireKeyframe();CHECK(cause(q.stats(),RecoveryCause::Decoder)==1&&q.stats().recoveries==1);
    }
    {   // a refused sample, and a different configuration
        VideoIngress q;start(q);CHECK(q.tryPop(0));auto bad=frame(1,false,10);bad.width=1000;
        CHECK(q.push(std::move(bad),10)==PushStatus::Malformed);CHECK(cause(q.stats(),RecoveryCause::Malformed)==1);
        VideoIngress r;start(r);CHECK(r.tryPop(0));auto other=config();other.height=720;other.width=1280;
        CHECK(r.push(std::move(other),10)==PushStatus::ConfigStored);CHECK(cause(r.stats(),RecoveryCause::Config)==1);
    }
    {   // a frame already too old when pushed
        VideoIngress q;start(q);CHECK(q.tryPop(0));
        CHECK(q.push(frame(1,false,10),10+QueueLimits{}.maxAge100ns+1)==PushStatus::WaitingForIdr);
        CHECK(cause(q.stats(),RecoveryCause::TooOld)==1&&q.stats().refusedWaiting==1);
    }
    // A new stream starts every count at zero.
    VideoIngress q;start(q);q.requireKeyframe();q.begin(2);CHECK(episodes(q.stats())==0&&q.stats().refusedWaiting==0);
}

TEST(sync_ingress_cause_line_only_when_a_count_changes){
    RecoveryCauseLog log;VideoQueueStats s;
    CHECK(log.update(s,0).empty());
    s.recoveries=1;s.recoveryCauses[static_cast<std::size_t>(RecoveryCause::StaleClock)]=1;s.lastRecoveryDepth=1;s.refusedWaiting=9;
    const auto line=log.update(s,9);
    CHECK(line.rfind("causes episodes=1 ",0)==0);
    for(const char* part:{" staleClock=1"," overflow=0"," expired=0"," depthAtLast=1"," refusedWaiting=9"," staleClockTolerated=0"," callbackRejected=9"})
        CHECK(line.find(part)!=std::string::npos);
    CHECK(log.update(s,9).empty());
    CHECK(!log.update(s,10).empty());                                    // a callback refusal the ingress did not make
    s.staleClockTolerated=1;CHECK(log.update(s,10).find(" staleClockTolerated=1")!=std::string::npos);
    CHECK(std::string(recoveryCauseName(RecoveryCause::Count))=="unknown");
}
