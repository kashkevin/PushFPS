// SPDX-License-Identifier: GPL-3.0-only
// The shared SmartLink state (veyra/remoteplay/SmartLinkShared.h): what the audio sink, the governor and the
// interface read, and what the audio sink reports back. Included by CoreTests.cpp directly above main().
#include "veyra/remoteplay/SmartLinkShared.h"
#include <thread>

TEST(smartlink_shared_nothing_published_means_not_fresh){
    namespace sl=veyra::remoteplay::smartlink;
    sl::clearShared();
    const sl::SharedState s=sl::latestShared();
    CHECK(s.sequence==0);CHECK(s.mode==sl::Mode::Off);CHECK(!sl::isFresh(s,0));CHECK(!sl::isFresh(s,100000));
    CHECK(sl::latestAudioFeed().sequence==0);CHECK(sl::latestAudioFeed().bufferedMs<0);
}

TEST(smartlink_shared_freshness_and_clear){
    namespace sl=veyra::remoteplay::smartlink;
    sl::clearShared();
    sl::Decision d;d.reorderWindowMs=8;d.audioTargetMs=40;d.networkLimited=true;
    sl::publishShared(d,sl::Mode::Apply,1000);
    sl::SharedState s=sl::latestShared();
    CHECK(s.sequence==1);CHECK(s.decision.reorderWindowMs==8);CHECK(s.decision.audioTargetMs==40);CHECK(s.decision.networkLimited);
    CHECK(sl::isFresh(s,1000));CHECK(sl::isFresh(s,1000+sl::kSharedFreshMs));
    CHECK(!sl::isFresh(s,1001+sl::kSharedFreshMs));   // stale: readers leave everything as configured
    CHECK(!sl::isFresh(s,999));                      // a reader's clock behind the publisher's: not trusted
    sl::publishShared(d,sl::Mode::Off,2000);
    CHECK(!sl::isFresh(sl::latestShared(),2000));    // Off is never acted on
    sl::publishAudioFeed(23.5,7);
    CHECK(sl::latestAudioFeed().sequence==1);CHECK(sl::latestAudioFeed().bufferedMs==23.5);CHECK(sl::latestAudioFeed().underruns==7);
    sl::clearShared();                               // stream end: nothing reaches the next stream
    CHECK(sl::latestShared().sequence==0);CHECK(sl::latestAudioFeed().sequence==0);
}

TEST(smartlink_shared_two_threads){
    namespace sl=veyra::remoteplay::smartlink;
    sl::clearShared();
    std::thread writer([]{
        for(int i=1;i<=20000;++i){sl::Decision d;d.audioTargetMs=static_cast<std::uint32_t>(i);d.reorderWindowMs=static_cast<std::uint32_t>(i);
            sl::publishShared(d,sl::Mode::Observe,i);sl::publishAudioFeed(static_cast<double>(i),i);}
    });
    bool torn=false;std::uint64_t last=0;bool backwards=false;
    for(int i=0;i<20000;++i){
        const sl::SharedState s=sl::latestShared();
        if(s.decision.audioTargetMs!=s.decision.reorderWindowMs)torn=true;   // both fields come from one publish
        if(s.sequence<last)backwards=true;
        last=s.sequence;
        const sl::AudioFeed a=sl::latestAudioFeed();
        if(a.sequence!=0&&a.bufferedMs!=static_cast<double>(a.underruns))torn=true;
    }
    writer.join();
    CHECK(!torn);CHECK(!backwards);CHECK(sl::latestShared().sequence==20000);
    sl::clearShared();
}
