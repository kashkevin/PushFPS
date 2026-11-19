// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// StreamCadence: picture numbers for a stream that shows every game picture twice, and the
// portable half of the GPU measurement that feeds it (pipeline/StreamChange.h).
// Included by CoreTests.cpp (same TEST()/CHECK() macros). The error that matters is a false
// repeat: a new game picture called a repeat is a dropped frame. A kept repeat costs nothing.
#include "veyra/pipeline/StreamChange.h"
#include "veyra/engine/PushStreamDedup.h"
#include "veyra/engine/ContentDedup.h"   // the engine's duplicate removal: what a wrong number costs
#include <cmath>
#include <vector>
namespace cadence_tests {
using Input=StreamCadence::Input;
// A picture with its largest and mean block change. Repeats of a console picture change the mean
// far less than the largest block: the encoder refines a few blocks, a new picture moves all of them.
Input in(float change,float mean,std::int64_t pts=0){Input v;v.change=change;v.meanChange=mean;v.pts100ns=pts;return v;}
Input fresh(float change){return in(change,change*0.25f);}       // a new picture
Input copy(float change){return in(change,change*0.08f);}        // a repeat (encoder refinement)
// Runs a locked 30-in-60 stream: n pictures, high/low alternating, first is a boundary.
void doubled(StreamCadence& c,int n,float high=60.0f,float low=3.0f){
    for(int i=0;i<n;++i){auto v=i%2?copy(low):fresh(high);v.boundary=i==0;c.observe(v);}
}
}
TEST(stream_cadence_locks_on_a_doubled_stream_and_numbers_pairs){
    StreamCadence c;std::uint64_t last=0;unsigned repeats=0;
    for(int i=0;i<40;++i){
        const bool second=i%2==1;const auto r=c.observe(second?3.0f:60.0f,i==0);
        if(i>=8){CHECK(r.locked);CHECK(r.repeat==second);CHECK(second?r.picture==last:r.picture==last+1);}
        if(r.repeat)++repeats;
        last=r.picture;
    }
    CHECK(repeats>=16);CHECK(c.stats().locks==1);CHECK(c.stats().unlocks==0);
}
TEST(stream_cadence_never_repeats_on_sixty_fps_content){
    StreamCadence c;std::uint64_t last=0;
    const float changes[]={60.0f,55.0f,70.0f,48.0f,66.0f,52.0f,58.0f,61.0f};
    for(int i=0;i<200;++i){const auto r=c.observe(changes[i%8],i==0);CHECK(!r.repeat);CHECK(!r.locked);CHECK(r.picture==last+1);last=r.picture;}
    CHECK(c.state()==StreamCadenceState::Unique);
}
TEST(stream_cadence_keeps_a_slow_scene_whose_changes_are_small_but_even){
    StreamCadence c;
    for(int i=0;i<100;++i){const auto r=c.observe(6.0f+float(i%3),i==0);CHECK(!r.repeat);}
}
TEST(stream_cadence_an_unchanged_picture_is_not_a_repeat_without_the_rhythm){
    // The first version called every picture whose largest block moved by less than 0.6 a repeat,
    // rhythm or not. A block mean cannot see what moves inside a block, so "the blocks did not
    // move" is not "nothing changed": real 60 fps pictures were dropped (independent review).
    StreamCadence c;
    const auto first=c.observe(255.0f,true);const auto moved=c.observe(40.0f,false);const auto still=c.observe(0.1f,false);
    CHECK(!first.repeat);CHECK(!moved.repeat);CHECK(!still.repeat);CHECK(!still.locked);CHECK(still.picture==moved.picture+1);
    // 600 pictures whose blocks never move: not one repeat, every picture its own number.
    StreamCadence d;std::uint64_t last=0;
    for(int i=0;i<600;++i){const auto r=d.observe(0.02f+0.01f*float(i%5),i==0);CHECK(!r.repeat);CHECK(!r.locked);CHECK(r.picture==last+1);last=r.picture;}
    CHECK(d.stats().repeats==0);
}
TEST(stream_cadence_small_changes_inside_a_trusted_rhythm_are_judged_by_the_rhythm_alone){
    using namespace cadence_tests;
    // Locked on a doubled stream; then the picture fades slowly: every new picture moves every
    // block by half a step (below the old "identical" threshold), its repeat by nearly nothing.
    // The new pictures are new.
    StreamCadence c;doubled(c,40);CHECK(c.locked());unsigned falseRepeats=0;
    for(int i=0;i<200;++i){const bool isNew=i%2==0;const auto r=c.observe(isNew?in(0.5f,0.5f):in(0.05f,0.02f));if(isNew&&r.repeat)++falseRepeats;}
    CHECK(falseRepeats==0);
    // The same when the game goes to 60 fps during the fade: every picture moves by a quarter step.
    StreamCadence d;doubled(d,40);
    for(int i=0;i<200;++i)CHECK(!d.observe(in(0.25f+0.01f*float(i%3),0.25f)).repeat);
    CHECK(!d.locked());
}
TEST(stream_cadence_follows_a_late_game_frame){
    // A A B B B C C: the third B is a second repeat; the rhythm continues on the new phase.
    StreamCadence c;int i=0;
    for(;i<20;++i)c.observe(i%2?3.0f:60.0f,i==0);
    CHECK(c.locked());
    const auto extra=c.observe(2.5f,false);          // third copy: neither a clear fall nor a rise
    CHECK(!extra.repeat||extra.picture>0);            // either answer is safe; it must not unlock at once
    CHECK(c.locked());
    const auto next=c.observe(60.0f,false);CHECK(!next.repeat);
    const auto copy=c.observe(3.0f,false);CHECK(copy.repeat);CHECK(copy.picture==next.picture);
}
TEST(stream_cadence_unlocks_when_the_game_goes_to_sixty){
    StreamCadence c;
    for(int i=0;i<20;++i)c.observe(i%2?3.0f:60.0f,i==0);
    CHECK(c.locked());
    unsigned falseRepeats=0,lockedFor=0;
    for(int i=0;i<60;++i){const auto r=c.observe(50.0f+float(i%4),false);if(r.repeat)++falseRepeats;if(r.locked)++lockedFor;}
    CHECK(falseRepeats==0);CHECK(!c.locked());CHECK(c.stats().unlocks==1);
    CHECK(lockedFor<=3);                              // three new pictures in a row end the rhythm
    CHECK(c.state()==StreamCadenceState::Unique);
}
TEST(stream_cadence_restarts_at_a_boundary_and_rejects_bad_input){
    StreamCadence c;
    for(int i=0;i<20;++i)c.observe(i%2?3.0f:60.0f,i==0);
    const auto before=c.observe(60.0f,false).picture;
    const auto reset=c.observe(1.0f,true);            // would be a repeat by size, but nothing to compare with
    CHECK(!reset.repeat);CHECK(!reset.locked);CHECK(reset.picture==before+1);CHECK(reset.confidence==0.0f);
    const auto bad=c.observe(std::numeric_limits<float>::quiet_NaN(),false);
    CHECK(!bad.repeat);CHECK(bad.picture==reset.picture+1);
    const auto negative=c.observe(-1.0f,false);CHECK(!negative.repeat);
    const auto infinite=c.observe(std::numeric_limits<float>::infinity(),false);CHECK(!infinite.repeat);CHECK(!infinite.locked);
    // The measurement after a bad one is a comparison again; without a rhythm even an unchanged picture is new.
    const auto after=c.observe(0.0f,false);CHECK(!after.repeat);CHECK(!after.locked);
    CHECK(c.stats().boundaries>=4);
}

// ---- The two sequences an independent check found against the first version.
TEST(stream_cadence_adversarial_scene_cut_exactly_at_a_thirty_to_sixty_switch){
    using namespace cadence_tests;
    // Locked on 60/3. A cut (250) and from that picture on the game runs at 60: the picture after
    // the cut changes by 60, far below 0.4 x 250. The first version called it a repeat.
    for(const bool withMean:{false,true})for(const bool flagged:{false,true}){
        StreamCadence c;
        for(int i=0;i<24;++i){Input v=i%2?copy(3.0f):fresh(60.0f);if(!withMean)v.meanChange=-1.0f;v.boundary=i==0;c.observe(v);}
        CHECK(c.locked());
        Input cut=in(250.0f,withMean?120.0f:-1.0f);cut.sceneChange=flagged;
        const auto atCut=c.observe(cut);CHECK(!atCut.repeat);CHECK(atCut.sceneChange);
        unsigned repeats=0;
        for(int i=0;i<40;++i){Input v=fresh(60.0f+float(i%5));if(!withMean)v.meanChange=-1.0f;if(c.observe(v).repeat)++repeats;}
        CHECK(repeats==0);CHECK(!c.locked());
    }
}
TEST(stream_cadence_adversarial_cut_into_a_slow_sixty_fps_scene){
    using namespace cadence_tests;
    // After the cut the new scene barely moves: every new picture changes less than a repeat did
    // before the cut. Nothing may be dropped on the strength of the old scene's levels.
    for(const bool flagged:{false,true}){
        StreamCadence c;doubled(c,24);
        Input cut=in(250.0f,120.0f);cut.sceneChange=flagged;c.observe(cut);
        unsigned repeats=0;
        for(int i=0;i<40;++i)if(c.observe(in(2.0f+0.1f*float(i%3),1.2f)).repeat)++repeats;
        CHECK(repeats==0);
    }
}
TEST(stream_cadence_adversarial_early_frame_while_motion_slows){
    using namespace cadence_tests;
    // Locked on 60/3. Motion slows (40) and the next game picture comes one refresh early (15):
    // 15 < 0.4 x 40, in the slot where a repeat was expected. The first version dropped it.
    for(const bool withMean:{false,true}){
        StreamCadence c;
        for(int i=0;i<24;++i){Input v=i%2?copy(3.0f):fresh(60.0f);if(!withMean)v.meanChange=-1.0f;v.boundary=i==0;c.observe(v);}
        Input b=fresh(40.0f),early=fresh(15.0f),second=copy(3.0f),next=fresh(30.0f),nextCopy=copy(2.5f);
        if(!withMean)for(Input* v:{&b,&early,&second,&next,&nextCopy})v->meanChange=-1.0f;
        CHECK(!c.observe(b).repeat);
        const auto r=c.observe(early);CHECK(!r.repeat);CHECK(r.fallback||withMean);   // by the largest block alone it looked like a repeat: kept as new
        c.observe(second);                                                  // its real repeat: either answer is safe
        CHECK(!c.observe(next).repeat);
        const auto again=c.observe(nextCopy);CHECK(again.repeat);CHECK(c.locked());   // the rhythm carries on, on the new phase
    }
}
TEST(stream_cadence_adversarial_sixty_fps_slowing_to_a_stop_after_thirty){
    using namespace cadence_tests;
    // 30 -> 60 while the camera decelerates: each new picture changes well under 0.4 of the one before.
    for(const bool withMean:{false,true}){
        StreamCadence c;
        for(int i=0;i<24;++i){Input v=i%2?copy(3.0f):fresh(60.0f);if(!withMean)v.meanChange=-1.0f;v.boundary=i==0;c.observe(v);}
        unsigned repeats=0;float change=130.0f;
        for(int i=0;i<12;++i){
            change*=0.38f;if(change<0.7f)change=0.7f;       // never below "nothing changed"
            Input v=fresh(change);if(!withMean)v.meanChange=-1.0f;
            const auto r=c.observe(v);
            // A picture within the band the repeats themselves filled cannot be told apart; anything above it must be kept.
            if(r.repeat&&change>3.0f*2.5f)++repeats;
        }
        CHECK(repeats==0);
    }
}
TEST(stream_cadence_adversarial_rapid_camera_never_drops_a_picture){
    using namespace cadence_tests;
    // 60 fps content under a whipping camera: changes jump between 20 and 120 at random. By chance
    // they alternate now and then; the mean change of a new picture is never as small as a repeat's.
    StreamCadence c;std::mt19937 rng(20261118);std::uniform_real_distribution<float> level(20.0f,120.0f);
    std::uint64_t last=0;unsigned locked=0;
    for(int i=0;i<200000;++i){
        Input v=fresh(level(rng));v.boundary=i==0;
        const auto r=c.observe(v);CHECK(!r.repeat);if(i)CHECK(r.picture==last+1);last=r.picture;if(r.locked)++locked;
    }
    // And the worst case built by hand: a strict big/small alternation of new pictures (mouse input
    // sampled at half the frame rate). The small pictures still move the whole picture.
    StreamCadence d;
    for(int i=0;i<400;++i){Input v=i%2?in(24.0f,7.0f):in(90.0f,26.0f);v.boundary=i==0;CHECK(!d.observe(v).repeat);}
    CHECK(d.stats().repeats==0);
}
TEST(stream_cadence_adversarial_half_rate_animation_in_a_sixty_fps_game){
    using namespace cadence_tests;
    // The camera moves every picture, a large object only on every second one: the largest block
    // alternates 40 / 6, the mean hardly at all. Every picture is new.
    StreamCadence c;
    for(int i=0;i<400;++i){Input v=i%2?in(6.0f,1.8f):in(40.0f,2.1f);v.boundary=i==0;const auto r=c.observe(v);CHECK(!r.repeat);CHECK(!r.locked);}
}
TEST(stream_cadence_adversarial_camera_moving_on_every_second_picture_at_sixty){
    using namespace cadence_tests;
    // A 60 fps game whose camera moves a whole pixel on every second picture (a slow scroll) while
    // its objects move on every picture: the mean alternates like a doubled stream, the largest
    // block does not. The mean alone never starts the rhythm.
    StreamCadence c;
    for(int i=0;i<600;++i){Input v=i%2?in(40.0f+float(i%5),0.2f):in(42.0f+float(i%3),3.0f);v.boundary=i==0;const auto r=c.observe(v);CHECK(!r.repeat);CHECK(!r.locked);}
    // After a real doubled stream it does not keep the rhythm alive for long either.
    StreamCadence d;doubled(d,40);CHECK(d.locked());unsigned lockedFor=0;
    for(int i=0;i<200;++i){const auto r=d.observe(i%2?in(40.0f+float(i%5),0.2f):in(42.0f+float(i%3),3.0f));CHECK(!r.repeat);if(r.locked)++lockedFor;}
    CHECK(!d.locked());CHECK(lockedFor<=16);
}
TEST(stream_cadence_keeps_the_rhythm_through_a_rewritten_block){
    using namespace cadence_tests;
    // libx265 now and then rewrites one block of a repeat: 50 steps in the largest block, the mean
    // still a hundredth of a new picture's. Kept (it is not provably a repeat), but the next
    // ordinary repeat is found at once.
    StreamCadence c;doubled(c,40,100.0f,3.0f);
    CHECK(!c.observe(fresh(100.0f)).repeat);
    const auto rewritten=c.observe(in(50.0f,0.2f));CHECK(!rewritten.repeat);CHECK(rewritten.fallback);CHECK(rewritten.locked);
    CHECK(!c.observe(fresh(100.0f)).repeat);
    const auto next=c.observe(copy(3.0f));CHECK(next.repeat);CHECK(c.stats().unlocks==0);
    // Without the mean the same picture costs the rhythm nothing worse than a restart.
    StreamCadence d;for(int i=0;i<40;++i)d.observe(i%2?3.0f:100.0f,i==0);
    d.observe(100.0f,false);CHECK(!d.observe(50.0f,false).repeat);CHECK(!d.observe(100.0f,false).repeat);
}
TEST(stream_cadence_adversarial_slow_pan_with_half_rate_animation_at_sixty){
    using namespace cadence_tests;
    // Found on the tuning streams (a photograph under a slow sub-pixel pan, a small object moving
    // on every second picture): the largest block alternates 100 / 10, the mean stays at 0.7 on
    // every picture, far below one step. Every picture is new.
    StreamCadence c;
    for(int i=0;i<600;++i){Input v=i%2?in(10.0f+float(i%4),0.70f):in(100.0f+float(i%9),0.72f);v.changedShare=0.2f;v.boundary=i==0;const auto r=c.observe(v);CHECK(!r.repeat);CHECK(!r.locked);}
    // The same after a real doubled stream: the rhythm in the largest block goes on, but no picture is dropped.
    StreamCadence d;doubled(d,40,100.0f,3.0f);
    for(int i=0;i<200;++i){Input v=i%2?in(10.0f+float(i%4),0.70f):in(100.0f+float(i%9),0.72f);CHECK(!d.observe(v).repeat);}
    CHECK(!d.locked());
}
TEST(stream_cadence_adversarial_forty_and_twenty_fps_patterns_stay_unlocked){
    for(const int pattern:{0,1}){
        StreamCadence c;
        for(int i=0;i<300;++i){
            // 40 fps in 60 Hz: new, repeat, new. 20 fps: new, repeat, repeat.
            const bool isNew=pattern==0?i%3!=1:i%3==0;
            const auto r=c.observe(isNew?55.0f:3.0f,i==0);
            if(isNew)CHECK(!r.repeat);
            CHECK(!r.locked);
        }
    }
}
TEST(stream_cadence_recovers_quickly_through_rate_flips){
    using namespace cadence_tests;
    // One second at 30, one at 60, again and again: nothing new is dropped, and each 30 fps second
    // is found again within lockSteps pictures.
    StreamCadence c;unsigned found=0,total=0;
    for(int i=0;i<1200;++i){
        const bool thirty=(i/60)%2==0;const bool isNew=!thirty||i%2==0;
        Input v=isNew?fresh(60.0f+float(i%7)):copy(3.0f);v.boundary=i==0;
        const auto r=c.observe(v);
        if(isNew)CHECK(!r.repeat);
        else{++total;if(r.repeat)++found;}
    }
    CHECK(total==300);CHECK(found>=300-10*4);          // at most 4 kept repeats per 30 fps second
    CHECK(c.stats().locks>=10);
}
TEST(stream_cadence_static_scene_then_motion_resumes_on_the_known_phase){
    using namespace cadence_tests;
    StreamCadence c;doubled(c,40);
    CHECK(c.locked());
    // The game pauses: only encoder refinement is left, small and even.
    for(int i=0;i<90;++i){const auto r=c.observe(copy(1.5f+0.2f*float(i%3)));CHECK(r.confidence<=1.0f);}
    CHECK(!c.locked());CHECK(c.state()==StreamCadenceState::Still);
    // Motion resumes on the same phase (an even number of paused pictures): found again faster than from cold.
    unsigned kept=0;
    for(int i=0;i<20;++i){const bool isNew=i%2==0;const auto r=c.observe(isNew?fresh(60.0f):copy(3.0f));if(isNew)CHECK(!r.repeat);else if(!r.repeat)++kept;}
    CHECK(c.locked());CHECK(kept<=2);CHECK(c.stats().fastLocks==1);
    // A pause of an odd length moves the phase: no shortcut, the full count again.
    StreamCadence d;doubled(d,40);
    for(int i=0;i<91;++i)d.observe(copy(1.5f+0.2f*float(i%3)));
    for(int i=0;i<20;++i)d.observe(i%2==0?fresh(60.0f):copy(3.0f));
    CHECK(d.locked());CHECK(d.stats().fastLocks==0);
}
TEST(stream_cadence_safe_fallback_below_the_confidence_floor){
    using namespace cadence_tests;
    // The contract: a repeat is only ever reported with confidence at or above the floor.
    std::mt19937 rng(7);std::uniform_real_distribution<float> unit(0.0f,1.0f);
    for(const float floor:{0.0f,0.3f,0.5f,0.9f,1.0f}){
        StreamCadenceConfig config;config.confidenceFloor=floor;StreamCadence c(config);
        for(int i=0;i<60000;++i){
            // A doubled stream with noisy levels and now and then garbage.
            const bool isNew=i%2==0;Input v=isNew?fresh(10.0f+90.0f*unit(rng)):copy(0.2f+12.0f*unit(rng));
            if(unit(rng)<0.002f)v.change=std::numeric_limits<float>::quiet_NaN();
            if(unit(rng)<0.002f)v.boundary=true;
            if(unit(rng)<0.003f)v.sceneChange=true;
            const auto r=c.observe(v);
            CHECK(r.confidence>=0.0f);CHECK(r.confidence<=1.0f);CHECK(r.picture>0);
            if(r.repeat){CHECK(r.confidence>=floor);CHECK(!r.fallback);}
            if(r.fallback)CHECK(!r.repeat);
        }
    }
    // A floor nothing can reach: every picture is new, even an unchanged one.
    StreamCadenceConfig never;never.confidenceFloor=1.5f;StreamCadence c(never);std::uint64_t last=0;
    for(int i=0;i<200;++i){const auto r=c.observe(i%2?0.0f:60.0f,i==0);CHECK(!r.repeat);CHECK(r.picture==last+1);last=r.picture;}
}
TEST(stream_cadence_confidence_grows_with_the_rhythm_and_falls_with_a_thin_margin){
    using namespace cadence_tests;
    StreamCadence c;float firstLocked=-1.0f,settled=0.0f;
    for(int i=0;i<60;++i){
        Input v=i%2?copy(3.0f):fresh(60.0f);v.boundary=i==0;const auto r=c.observe(v);
        if(!r.locked){CHECK(r.confidence==0.0f);CHECK(!r.repeat);}
        if(r.locked&&r.repeat){if(firstLocked<0.0f)firstLocked=r.confidence;settled=r.confidence;}
    }
    CHECK(firstLocked>=c.config().confidenceFloor);CHECK(settled>=firstLocked);CHECK(settled>0.95f);
    // A repeat that changed almost as much as the limit allows: lower confidence than a clear one.
    c.observe(fresh(60.0f));const auto clear=c.observe(copy(3.0f));
    c.observe(fresh(60.0f));const auto thin=c.observe(in(60.0f*0.4f*0.98f,0.3f));
    CHECK(clear.repeat);CHECK(!thin.repeat);CHECK(thin.fallback);CHECK(thin.confidence<clear.confidence);
    CHECK(c.stats().fallbacks>=1);
}
TEST(stream_cadence_source_rate_from_timestamps_and_from_the_frame_count){
    using namespace cadence_tests;
    // 59.94 Hz stream times; a 30 fps game, then 60.
    StreamCadence c;std::int64_t pts=1000000;float atThirty=0.0f,atSixty=0.0f;
    for(int i=0;i<240;++i){Input v=i%2?copy(3.0f):fresh(60.0f);v.boundary=i==0;v.pts100ns=pts;pts+=166833;atThirty=c.observe(v).sourceFps;}
    for(int i=0;i<240;++i){Input v=fresh(55.0f+float(i%5));v.pts100ns=pts;pts+=166833;atSixty=c.observe(v).sourceFps;}
    CHECK(atThirty>29.0f);CHECK(atThirty<31.0f);CHECK(atSixty>58.9f);CHECK(atSixty<61.0f);
    // Without timestamps the nominal stream period counts the time.
    StreamCadence d;float rate=0.0f;
    for(int i=0;i<240;++i)rate=d.observe(i%2?3.0f:60.0f,i==0).sourceFps;
    CHECK(rate>29.0f);CHECK(rate<31.0f);
    // A boundary empties the window: no rate until it has filled again.
    Input cut=fresh(60.0f);cut.boundary=true;CHECK(d.observe(cut).sourceFps==0.0f);
    // Timestamps that stand still or run backwards never divide by zero or go negative.
    StreamCadence e;
    for(int i=0;i<200;++i){Input v=fresh(50.0f);v.pts100ns=i<100?5000000:5000000-i;const auto r=e.observe(v);CHECK(std::isfinite(r.sourceFps));CHECK(r.sourceFps>=0.0f);}
}
TEST(stream_cadence_scene_cut_inside_a_doubled_stream_keeps_the_rhythm){
    using namespace cadence_tests;
    StreamCadence c;doubled(c,40);
    Input cut=in(240.0f,110.0f);cut.sceneChange=true;
    const auto atCut=c.observe(cut);CHECK(!atCut.repeat);CHECK(atCut.sceneChange);CHECK(c.locked());
    const auto copyOfCut=c.observe(copy(3.0f));CHECK(!copyOfCut.repeat);CHECK(copyOfCut.fallback);   // one kept repeat per cut
    unsigned kept=0;
    for(int i=0;i<40;++i){const bool isNew=i%2==0;const auto r=c.observe(isNew?fresh(45.0f):copy(2.0f));if(isNew)CHECK(!r.repeat);else if(!r.repeat)++kept;}
    CHECK(kept==0);CHECK(c.locked());CHECK(c.stats().cuts==1);
    // Found without being told, from the size of the jump.
    StreamCadence d;doubled(d,40);
    CHECK(d.observe(in(240.0f,110.0f)).sceneChange);
}
TEST(stream_cadence_repeat_level_ceiling_follows_a_starved_stream){
    using namespace cadence_tests;
    // A starved encoder: repeats change by up to 28, new pictures by 80. Still doubled, still found.
    StreamCadence c;std::mt19937 rng(3);std::uniform_real_distribution<float> low(8.0f,28.0f);unsigned found=0;
    for(int i=0;i<400;++i){
        const bool isNew=i%2==0;Input v=isNew?in(80.0f,18.0f):in(low(rng),1.4f);v.boundary=i==0;
        const auto r=c.observe(v);if(isNew)CHECK(!r.repeat);else if(r.repeat)++found;
    }
    CHECK(found>=130);
}
// ---- pipeline/StreamChange.h: the numbers the GPU pass writes, and its CPU reference.
TEST(stream_change_decodes_the_gpu_result_words){
    using veyra::pipeline::decodeStreamChange;using veyra::pipeline::StreamChangeWords;
    // A 400x160 picture: 25 x 10 blocks, 64000 samples.
    StreamChangeWords words{};words.maxFixed=20u*256u+128u;words.sumFixed=250u*256u;words.changed=50;words.blocks=250;
    words.pixelMaxFixed=37u*256u;words.pixelsOver=1234;words.pixels=64000;
    const auto m=decodeStreamChange(words,400,160,true);
    CHECK(m.valid);CHECK(m.comparable);CHECK(std::abs(m.maxBlock-20.5f)<1e-4f);CHECK(std::abs(m.meanBlock-1.0f)<1e-4f);CHECK(std::abs(m.changedShare-0.2f)<1e-4f);
    CHECK(m.pixelMax==37.0f);CHECK(m.pixelsOver==1234.0f);
    // Not every block or sample reported in: the pass did not finish or wrote somewhere else. Not a measurement.
    auto bad=words;bad.blocks=249;CHECK(!decodeStreamChange(bad,400,160,true).valid);
    bad=words;bad.pixels=63999;CHECK(!decodeStreamChange(bad,400,160,true).valid);
    bad=words;bad.pixels=64001;CHECK(!decodeStreamChange(bad,400,160,true).valid);
    bad=words;bad.changed=251;CHECK(!decodeStreamChange(bad,400,160,true).valid);
    bad=words;bad.pixelsOver=64001;CHECK(!decodeStreamChange(bad,400,160,true).valid);
    bad=words;bad.maxFixed=255u*256u+1u;CHECK(!decodeStreamChange(bad,400,160,true).valid);
    bad=words;bad.pixelMaxFixed=255u*256u+1u;CHECK(!decodeStreamChange(bad,400,160,true).valid);
    CHECK(!decodeStreamChange(words,416,160,true).valid);            // the words of another picture size
    CHECK(!decodeStreamChange(StreamChangeWords{},0,0,true).valid);
    CHECK(!decodeStreamChange(words,8,8,true).valid);CHECK(!decodeStreamChange(words,8000,160,true).valid);
    // The first picture after a reset has nothing to be compared with: valid, not comparable, no numbers.
    StreamChangeWords first{};first.blocks=250;first.pixels=64000;first.pixelMaxFixed=999;
    const auto f=decodeStreamChange(first,400,160,false);CHECK(f.valid);CHECK(!f.comparable);CHECK(f.pixelMax==0.0f);CHECK(f.maxBlock==0.0f);
}
TEST(stream_change_reference_block_means_match_the_definition){
    using namespace veyra::pipeline;
    // 40x35 luma with a 48-byte pitch: 3x3 blocks, the right ones 8 wide and the bottom ones 3 high.
    const unsigned width=40,height=35,pitch=48;
    std::vector<std::uint8_t> a(pitch*height,0),b(pitch*height,0);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<pitch;++x){a[y*pitch+x]=std::uint8_t((x*7+y*3)%200);b[y*pitch+x]=a[y*pitch+x];}
    CHECK(streamChangeBlocks(width,height).x==3);CHECK(streamChangeBlocks(width,height).y==3);CHECK(streamChangeBlocks(1920,1080).count()==120u*68u);
    CHECK(streamChangeBlocks(16,16).count()==1);CHECK(streamChangeBlocks(17,16).count()==2);
    std::vector<float> meansA,meansB;
    CHECK(streamChangeBlockMeans(a.data(),pitch,width,height,1,meansA));CHECK(meansA.size()==9);
    double whole=0;for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)whole+=a[y*pitch+x];
    CHECK(std::abs(double(meansA[0])-whole/256.0)<1e-3);
    double corner=0;for(unsigned y=32;y<35;++y)for(unsigned x=32;x<40;++x)corner+=a[y*pitch+x];
    CHECK(std::abs(double(meansA[8])-corner/24.0)<1e-3);            // the corner block: 8 x 3 samples
    // Change one whole block by exactly 5 steps; the row padding beyond the picture changes wildly.
    for(unsigned y=16;y<32;++y)for(unsigned x=16;x<32;++x)b[y*pitch+x]=std::uint8_t(a[y*pitch+x]+5);
    for(unsigned y=0;y<height;++y)for(unsigned x=width;x<pitch;++x)b[y*pitch+x]=std::uint8_t(255-a[y*pitch+x]);
    CHECK(streamChangeBlockMeans(b.data(),pitch,width,height,1,meansB));
    const auto m=streamChangeCompare(meansA,meansB);
    CHECK(m.valid);CHECK(m.comparable);CHECK(std::abs(m.maxBlock-5.0f)<0.01f);CHECK(std::abs(m.changedShare-1.0f/9.0f)<1e-6f);
    CHECK(std::abs(m.meanBlock-5.0f/9.0f)<0.01f);
    // The fixed-point words the shader would write decode to the same numbers.
    const auto words=streamChangeReference(a.data(),b.data(),pitch,width,height,1);
    const auto g=decodeStreamChange(words,width,height,true);
    CHECK(g.valid);CHECK(std::abs(g.maxBlock-m.maxBlock)<1.0f/256.0f);CHECK(std::abs(g.meanBlock-m.meanBlock)<1.0f/256.0f);CHECK(g.changedShare==m.changedShare);
    CHECK(words.pixels==width*height);CHECK(words.blocks==9);CHECK(g.pixelMax==5.0f);CHECK(g.pixelsOver==256.0f);   // 5 > 4: all 256 samples of the block count
    // A change in the edge blocks is measured too: the last column and the last row belong to the picture.
    auto edge=a;edge[10*pitch+39]=std::uint8_t(a[10*pitch+39]+60);edge[34*pitch+5]=std::uint8_t(a[34*pitch+5]+90);
    const auto e=decodeStreamChange(streamChangeReference(a.data(),edge.data(),pitch,width,height,1),width,height,true);
    CHECK(e.valid);CHECK(e.pixelMax==90.0f);CHECK(e.pixelsOver==2.0f);CHECK(std::abs(e.maxBlock-90.0f/48.0f)<0.01f);   // the bottom block: 16 x 3 samples
    // 16-bit samples (P010: ten bits in the top of the word) give the same 8-bit steps.
    std::vector<std::uint8_t> wide(std::size_t(pitch)*2*height,0),wideB;
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const unsigned v=unsigned(a[y*pitch+x])<<8;wide[(y*pitch+x)*2]=std::uint8_t(v&255);wide[(y*pitch+x)*2+1]=std::uint8_t(v>>8);}
    std::vector<float> meansWide;
    CHECK(streamChangeBlockMeans(wide.data(),pitch*2,width,height,2,meansWide));
    for(std::size_t i=0;i<9;++i)CHECK(std::abs(meansWide[i]-meansA[i]*(65280.0f/65535.0f))<0.01f);
    wideB=wide;wideB[(7*pitch+9)*2+1]=std::uint8_t(wideB[(7*pitch+9)*2+1]+30);   // one sample 30 steps brighter
    const auto w=decodeStreamChange(streamChangeReference(wide.data(),wideB.data(),pitch*2,width,height,2),width,height,true);
    CHECK(w.valid);CHECK(std::abs(w.pixelMax-30.0f*(65280.0f/65535.0f))<0.01f);CHECK(w.pixelsOver==1.0f);
    CHECK(!streamChangeBlockMeans(nullptr,pitch,width,height,1,meansA));CHECK(!streamChangeBlockMeans(a.data(),8,width,height,1,meansA));
    CHECK(!streamChangeCompare(meansA,std::vector<float>(3)).valid);
    CHECK(streamChangeReference(a.data(),b.data(),8,width,height,1).blocks==0);   // a pitch too small: no words, so no measurement
}
TEST(stream_change_sees_what_a_block_mean_cannot){
    using namespace veyra::pipeline;
    // The reviewer's point in one picture pair: a 6x6 object moves one sample to the right inside a
    // 16x16 block of a flat picture. The block's mean is the same; twelve samples are not.
    const unsigned width=64,height=48;
    std::vector<std::uint8_t> a(width*height,100),b(width*height,100);
    for(unsigned y=20;y<26;++y)for(unsigned x=35;x<41;++x)a[y*width+x]=220;
    for(unsigned y=20;y<26;++y)for(unsigned x=36;x<42;++x)b[y*width+x]=220;
    const auto m=decodeStreamChange(streamChangeReference(a.data(),b.data(),width,width,height,1),width,height,true);
    CHECK(m.valid);CHECK(m.maxBlock==0.0f);CHECK(m.meanBlock==0.0f);CHECK(m.changedShare==0.0f);   // blind
    CHECK(m.pixelMax==120.0f);CHECK(m.pixelsOver==12.0f);
    // A 2x2 particle drifting diagonally, and a fade of one step on a quarter of the samples.
    std::vector<std::uint8_t> c(width*height,100),d(width*height,100),f(width*height,100);
    c[5*width+5]=c[5*width+6]=c[6*width+5]=c[6*width+6]=250;d[6*width+6]=d[6*width+7]=d[7*width+6]=d[7*width+7]=250;
    const auto p=decodeStreamChange(streamChangeReference(c.data(),d.data(),width,width,height,1),width,height,true);
    CHECK(p.maxBlock==0.0f);CHECK(p.pixelMax==150.0f);CHECK(p.pixelsOver==6.0f);
    for(unsigned i=0;i<width*height;i+=4)f[i]=101;
    const auto fade=decodeStreamChange(streamChangeReference(a.data(),a.data(),width,width,height,1),width,height,true);
    CHECK(fade.pixelMax==0.0f);CHECK(fade.pixelsOver==0.0f);       // the same picture: nothing, exactly
    const auto step=decodeStreamChange(streamChangeReference(std::vector<std::uint8_t>(width*height,100).data(),f.data(),width,width,height,1),width,height,true);
    CHECK(step.pixelMax==1.0f);CHECK(step.pixelsOver==0.0f);CHECK(std::abs(step.meanBlock-0.25f)<0.01f);   // a fade is small everywhere and moves every block
}
TEST(stream_change_a_removed_device_is_a_failure_not_a_completion){
    using veyra::pipeline::StreamChangeFence;using veyra::pipeline::streamChangeFence;
    // ID3D12Fence::GetCompletedValue() answers UINT64_MAX once the device is removed. Compared with
    // ">= target" that reads as "finished"; the result buffer then holds whatever it held before.
    CHECK(streamChangeFence(4,5)==StreamChangeFence::Running);
    CHECK(streamChangeFence(5,5)==StreamChangeFence::Done);CHECK(streamChangeFence(9,5)==StreamChangeFence::Done);
    CHECK(streamChangeFence(0,0)==StreamChangeFence::Done);
    CHECK(streamChangeFence(std::numeric_limits<std::uint64_t>::max(),5)==StreamChangeFence::Lost);
    CHECK(streamChangeFence(std::numeric_limits<std::uint64_t>::max(),0)==StreamChangeFence::Lost);
    CHECK(streamChangeFence(std::numeric_limits<std::uint64_t>::max()-1,5)==StreamChangeFence::Done);
}
TEST(stream_change_wait_never_passes_the_next_presentation){
    using veyra::pipeline::kStreamChangeWait100ns;using veyra::pipeline::streamChangeWaitBudget;
    // The engine thread also presents frames at exact times. It waits for a measurement at most
    // 4 ms, and never past the moment its scheduler has to wake for the next presentation.
    CHECK(kStreamChangeWait100ns==40000);
    const std::int64_t now=5000000000ll;
    CHECK(streamChangeWaitBudget(now,0)==40000);                 // nothing scheduled
    CHECK(streamChangeWaitBudget(now,-7)==40000);
    CHECK(streamChangeWaitBudget(now,now+100000)==40000);        // the next presentation is 10 ms away
    CHECK(streamChangeWaitBudget(now,now+40000)==40000);
    CHECK(streamChangeWaitBudget(now,now+39999)==39999);
    CHECK(streamChangeWaitBudget(now,now+12345)==12345);         // 1.2 ms away: wait no longer than that
    CHECK(streamChangeWaitBudget(now,now+1)==1);
    CHECK(streamChangeWaitBudget(now,now)==0);                   // due now, or overdue: do not wait at all
    CHECK(streamChangeWaitBudget(now,now-1)==0);
    CHECK(streamChangeWaitBudget(now,1)==0);
    // A clock that has not started, and the far ends, do not overflow.
    CHECK(streamChangeWaitBudget(0,0)==40000);CHECK(streamChangeWaitBudget(0,25)==25);
    CHECK(streamChangeWaitBudget(std::numeric_limits<std::int64_t>::min()+1,std::numeric_limits<std::int64_t>::max())==40000);
    CHECK(streamChangeWaitBudget(std::numeric_limits<std::int64_t>::max(),1)==0);
    // What the runner does with a budget it is handed: never more than the 4 ms, never negative.
    using veyra::pipeline::streamChangeWaitClamp;
    CHECK(streamChangeWaitClamp(-5)==0);CHECK(streamChangeWaitClamp(0)==0);CHECK(streamChangeWaitClamp(777)==777);
    CHECK(streamChangeWaitClamp(40001)==40000);CHECK(streamChangeWaitClamp(std::numeric_limits<std::int64_t>::max())==40000);
}
// ---- engine/PushStreamDedup.h: measurement -> classifier -> picture number for ContentDedup.
namespace cadence_tests {
veyra::pipeline::StreamChangeOutcome measured(float change,float mean,float share=0.5f,float pixelMax=0.0f,float pixelsOver=0.0f){
    veyra::pipeline::StreamChangeOutcome o;o.now.valid=true;o.now.comparable=true;o.now.maxBlock=change;o.now.meanBlock=mean;o.now.changedShare=share;
    o.now.pixelMax=pixelMax;o.now.pixelsOver=pixelsOver;return o;
}
// A new picture moves samples everywhere and far; its repeat is refined: a few samples, by little.
veyra::pipeline::StreamChangeOutcome newPicture(){return measured(60.0f,3.0f,0.6f,140.0f,60000.0f);}
veyra::pipeline::StreamChangeOutcome repeatPicture(){return measured(3.0f,0.2f,0.02f,5.0f,40.0f);}
}
TEST(push_stream_dedup_numbers_only_while_the_rhythm_is_trusted){
    using namespace cadence_tests;using veyra::engine::PushStreamDedup;
    PushStreamDedup d;std::uint64_t sequence=0,last=0;unsigned unnumberedAfterLock=0,dropped=0;bool locked=false;
    for(int i=0;i<60;++i){
        const bool second=i%2==1;
        const auto r=d.observe(second?repeatPicture():newPicture(),++sequence,1000000+166833ll*i,i==0);
        if(!locked){CHECK(!r.numbered||r.cadence.locked);locked=r.cadence.locked;last=r.numbered?r.picture:0;continue;}
        if(!r.numbered){++unnumberedAfterLock;continue;}
        CHECK((r.picture>>56)==3);CHECK(r.repeat==second);
        if(second){CHECK(r.picture==last);++dropped;}else CHECK(r.picture!=last);
        last=r.picture;
    }
    CHECK(locked);CHECK(unnumberedAfterLock==0);CHECK(dropped>=24);CHECK(d.stats().repeats>=dropped);CHECK(d.stats().repeats<=dropped+1);   // the picture the rhythm locked on may be one
    // A 60 fps game: never a number, exactly as before the experiment.
    PushStreamDedup sixty;
    for(int i=0;i<300;++i)CHECK(!sixty.observe(measured(50.0f+float(i%7),3.0f+0.1f*float(i%5),0.6f),std::uint64_t(i+1),0,i==0).numbered);
    CHECK(sixty.stats().numbered==0);CHECK(sixty.stats().measured==300);
}
TEST(push_stream_dedup_gpu_errors_and_gaps_keep_the_frame_and_restart_the_rhythm){
    using namespace cadence_tests;using veyra::engine::PushStreamDedup;
    PushStreamDedup d;std::uint64_t sequence=0;
    for(int i=0;i<40;++i)d.observe(i%2?repeatPicture():newPicture(),++sequence,0,i==0);
    CHECK(d.cadence().locked());
    // No valid measurement (GPU error): no number, and what follows starts again, so the next
    // repeat is not dropped on the strength of a rhythm that has a hole in it.
    CHECK(!d.observe(veyra::pipeline::StreamChangeOutcome{},++sequence,0,false).numbered);
    const auto after=d.observe(repeatPicture(),++sequence,0,false);
    CHECK(!after.numbered);CHECK(!d.cadence().locked());CHECK(d.stats().unmeasured==1);
    // A picture the engine never saw (sequence gap) restarts it too.
    PushStreamDedup g;sequence=0;
    for(int i=0;i<40;++i)g.observe(i%2?repeatPicture():newPicture(),++sequence,0,i==0);
    ++sequence;
    CHECK(!g.observe(repeatPicture(),++sequence,0,false).numbered);CHECK(!g.cadence().locked());
    // The first measurement after a graph rebuild has nothing to be compared with.
    PushStreamDedup r;sequence=0;
    for(int i=0;i<40;++i)r.observe(i%2?repeatPicture():newPicture(),++sequence,0,i==0);
    auto first=repeatPicture();first.now.comparable=false;
    CHECK(!r.observe(first,++sequence,0,false).numbered);CHECK(!r.cadence().locked());
    // A boundary from the engine (seek, reset) likewise.
    PushStreamDedup b;sequence=0;
    for(int i=0;i<40;++i)b.observe(i%2?repeatPicture():newPicture(),++sequence,0,i==0);
    CHECK(!b.observe(repeatPicture(),++sequence,0,true).numbered);CHECK(!b.cadence().locked());
}
TEST(push_stream_dedup_a_late_measurement_keeps_the_rhythm){
    using namespace cadence_tests;using veyra::engine::PushStreamDedup;
    PushStreamDedup d;std::uint64_t sequence=0;
    for(int i=0;i<40;++i)d.observe(i%2?repeatPicture():newPicture(),++sequence,0,i==0);
    // A new picture whose measurement missed the wait: kept without a number.
    veyra::pipeline::StreamChangeOutcome slow;slow.timedOut=true;slow.queued=true;
    CHECK(!d.observe(slow,++sequence,0,false).numbered);
    // Its repeat arrives with the late answer attached: the rhythm is intact and the repeat is found.
    auto next=repeatPicture();next.hasLate=true;next.late=newPicture().now;
    const auto r=d.observe(next,++sequence,0,false);
    CHECK(r.numbered);CHECK(r.repeat);CHECK(d.cadence().locked());CHECK(d.stats().late==1);
    // Without the late answer (the GPU was still busy: nothing queued) the rhythm starts again.
    veyra::pipeline::StreamChangeOutcome busy;busy.timedOut=true;
    CHECK(!d.observe(newPicture(),++sequence,0,false).repeat);
    CHECK(!d.observe(slow,++sequence,0,false).numbered);
    CHECK(!d.observe(busy,++sequence,0,false).numbered);
    CHECK(!d.observe(newPicture(),++sequence,0,false).numbered);CHECK(!d.cadence().locked());
    // A late answer that is itself not a measurement is not used.
    PushStreamDedup e;sequence=0;
    for(int i=0;i<40;++i)e.observe(i%2?repeatPicture():newPicture(),++sequence,0,i==0);
    CHECK(!e.observe(slow,++sequence,0,false).numbered);
    auto bad=repeatPicture();bad.hasLate=true;
    CHECK(!e.observe(bad,++sequence,0,false).numbered);CHECK(e.stats().late==0);
}
TEST(push_stream_dedup_low_confidence_gives_no_number){
    using namespace cadence_tests;using veyra::engine::PushStreamDedup;
    PushStreamDedup d;std::uint64_t sequence=0;
    for(int i=0;i<40;++i)d.observe(i%2?repeatPicture():newPicture(),++sequence,0,i==0);
    CHECK(d.observe(newPicture(),++sequence,0,false).numbered);
    // In the repeat's place, but it changed five times what repeats change: kept, no number.
    const auto doubtful=d.observe(measured(15.0f,0.9f,0.05f),++sequence,0,false);
    CHECK(doubtful.cadence.fallback);CHECK(!doubtful.numbered);CHECK(!doubtful.repeat);
    // With a confidence floor nothing reaches, no repeat is ever reported.
    veyra::remoteplay::StreamCadenceConfig never;never.confidenceFloor=1.5f;
    PushStreamDedup n(never);sequence=0;
    for(int i=0;i<200;++i)CHECK(!n.observe(i%2?repeatPicture():newPicture(),++sequence,0,i==0).repeat);
    CHECK(n.stats().repeats==0);
}
TEST(push_stream_dedup_measures_only_with_frame_generation_and_duplicate_removal){
    using veyra::engine::PushStreamDedup;
    // The measurement costs a wait on the engine thread. It is only worth it, and only allowed,
    // while frame generation runs AND duplicate removal is active, for a hardware picture that has
    // no number yet, with the experiment switched on.
    CHECK(PushStreamDedup::measures(true,true,true,true,false));
    CHECK(!PushStreamDedup::measures(false,true,true,true,false));   // switch off
    CHECK(!PushStreamDedup::measures(true,false,true,true,false));   // no frame generation
    CHECK(!PushStreamDedup::measures(true,true,false,true,false));   // content rate Transport: nothing is removed
    CHECK(!PushStreamDedup::measures(true,true,true,false,false));   // a CPU frame
    CHECK(!PushStreamDedup::measures(true,true,true,true,true));     // already has a number
    // A stretch without measurements (frame generation was off) is a gap in the picture sequence:
    // the rhythm starts again, and the first picture after it gets no number.
    using namespace cadence_tests;
    PushStreamDedup d;std::uint64_t sequence=0;
    for(int i=0;i<40;++i)d.observe(i%2?repeatPicture():newPicture(),++sequence,0,i==0);
    CHECK(d.cadence().locked());
    sequence+=120;
    const auto after=d.observe(repeatPicture(),++sequence,0,false);
    CHECK(!after.numbered);CHECK(!d.cadence().locked());
}
TEST(push_stream_dedup_switch_is_on_only_for_the_value_one){
    using veyra::engine::PushStreamDedup;
    // VEYRA_EXP_RP_STREAM_DEDUP=1 turns the experiment on. Anything else, "0" above all, leaves it off.
    CHECK(PushStreamDedup::switchedOn(L"1"));
    for(const wchar_t* off:{L"",L"0",L"00",L"01",L"10",L"11",L" 1",L"1 ",L"2",L"true",L"on",L"yes",L"-1"})CHECK(!PushStreamDedup::switchedOn(off));
}
TEST(stream_change_refuses_pictures_whose_sums_would_not_fit){
    using namespace veyra::pipeline;
    // The pass adds 8.8 fixed-point block differences into one 32-bit word. That is safe up to
    // 4096x2304; a larger picture is not measured (its frames are kept, as without the experiment).
    CHECK(streamChangeMeasurable(1280,720));CHECK(streamChangeMeasurable(1920,1080));CHECK(streamChangeMeasurable(3840,2160));CHECK(streamChangeMeasurable(4096,2304));
    CHECK(!streamChangeMeasurable(4097,2304));CHECK(!streamChangeMeasurable(4096,2305));CHECK(!streamChangeMeasurable(7680,4320));
    CHECK(!streamChangeMeasurable(15,400));CHECK(!streamChangeMeasurable(400,15));CHECK(!streamChangeMeasurable(0,0));CHECK(streamChangeMeasurable(16,16));
    // The worst case at the limit: every block moves by the full 255 steps, every sample differs.
    const auto grid=streamChangeBlocks(kStreamChangeMaxWidth,kStreamChangeMaxHeight);
    const std::uint64_t worst=std::uint64_t(grid.count())*255u*kStreamChangeFixed;
    CHECK(worst<=std::numeric_limits<std::uint32_t>::max());
    CHECK(std::uint64_t(kStreamChangeMaxWidth)*kStreamChangeMaxHeight<=std::numeric_limits<std::uint32_t>::max());
    std::vector<float> black(grid.count(),0.0f),white(grid.count(),255.0f);
    auto words=streamChangeWords(black,white);
    CHECK(std::uint64_t(words.sumFixed)==worst);
    words.pixels=kStreamChangeMaxWidth*kStreamChangeMaxHeight;words.pixelsOver=words.pixels;words.pixelMaxFixed=255u*kStreamChangeFixed;
    const auto m=decodeStreamChange(words,kStreamChangeMaxWidth,kStreamChangeMaxHeight,true);
    CHECK(m.valid);CHECK(std::abs(m.meanBlock-255.0f)<1e-3f);CHECK(m.maxBlock==255.0f);CHECK(m.changedShare==1.0f);CHECK(m.pixelMax==255.0f);
    CHECK(m.pixelsOver==float(kStreamChangeMaxWidth*kStreamChangeMaxHeight));
}
TEST(push_stream_dedup_gives_a_number_only_inside_a_trusted_rhythm){
    using namespace cadence_tests;using veyra::engine::PushStreamDedup;using veyra::engine::ContentDedup;
    // What the reviewer's probes looked like to the first version: real 60 fps content whose blocks
    // hardly move (a small object over a still picture, a slow fade). 376, 535 and 599 of 600
    // pictures were called repeats and 226 to 301 dropped. Without a trusted rhythm no picture
    // gets a number at all, so ContentDedup keeps every one.
    for(const float level:{0.02f,0.3f,0.55f}){
        PushStreamDedup d;ContentDedup content;unsigned dropped=0;
        for(int i=0;i<600;++i){
            const std::int64_t pts=1000000+166833ll*i;
            const auto r=d.observe(measured(level+0.01f*float(i%4),level*0.5f,0.0f),std::uint64_t(i+1),pts,i==0);
            CHECK(!r.numbered);CHECK(!r.repeat);CHECK(!r.cadence.repeat);
            if(!content.observe(r.numbered?&r.picture:nullptr,pts,166833,i==0).keep)++dropped;
        }
        CHECK(dropped==0);CHECK(d.stats().numbered==0);CHECK(d.stats().repeats==0);
    }
    // Inside a trusted rhythm the number also needs the classifier's confidence: with a floor the
    // rhythm's trust has not reached yet, new pictures get no number either.
    veyra::remoteplay::StreamCadenceConfig careful;careful.confidenceFloor=0.7f;
    PushStreamDedup e(careful);std::uint64_t sequence=0;bool sawLockedUnnumbered=false,sawNumbered=false;
    for(int i=0;i<60;++i){
        const auto r=e.observe(i%2?repeatPicture():newPicture(),++sequence,0,i==0);
        if(r.numbered){CHECK(r.cadence.locked);CHECK(r.cadence.confidence>=0.7f);CHECK(!r.cadence.fallback);sawNumbered=true;}
        else if(r.cadence.locked)sawLockedUnnumbered=true;
    }
    CHECK(sawLockedUnnumbered);CHECK(sawNumbered);
}
TEST(stream_cadence_a_visible_local_change_vetoes_a_repeat){
    using namespace cadence_tests;
    // With the per-sample evidence: new pictures move 60000 samples by up to 140 steps, repeats 40 samples by up to 5.
    const auto fresh2=[]{Input v=in(60.0f,3.0f);v.pixelMax=140.0f;v.pixelsOver=60000.0f;return v;};
    const auto copy2=[]{Input v=in(3.0f,0.2f);v.pixelMax=5.0f;v.pixelsOver=40.0f;return v;};
    StreamCadence c;
    for(int i=0;i<40;++i){Input v=i%2?copy2():fresh2();v.boundary=i==0;const auto r=c.observe(v);if(i>=10)CHECK(r.repeat==(i%2==1));}
    CHECK(c.locked());CHECK(c.stats().pixelVetoes==0);
    // In a repeat's place, with a repeat's block numbers, but twelve samples moved by 100 steps: a
    // small object moved inside a block. Not a repeat.
    c.observe(fresh2());
    Input object=copy2();object.pixelMax=100.0f;object.pixelsOver=52.0f;
    const auto vetoed=c.observe(object);CHECK(!vetoed.repeat);CHECK(vetoed.fallback);CHECK(c.stats().pixelVetoes==1);CHECK(c.locked());
    // Just above the ceiling is vetoed, at the ceiling is not.
    c.observe(fresh2());Input above=copy2();above.pixelMax=c.config().pixelCeiling+0.5f;CHECK(!c.observe(above).repeat);
    c.observe(fresh2());Input at=copy2();at.pixelMax=c.config().pixelCeiling;CHECK(c.observe(at).repeat);
    // No sample moved far, but as many samples moved as in the picture before: the whole picture
    // is still changing (a fade over a moving scene). Not a repeat.
    c.observe(fresh2());Input busy=copy2();busy.pixelsOver=60000.0f*c.config().pixelRatio+1.0f;
    CHECK(!c.observe(busy).repeat);CHECK(c.stats().pixelVetoes==3);
    // The rhythm itself was never in doubt: the next ordinary repeat is one.
    c.observe(fresh2());CHECK(c.observe(copy2()).repeat);
    // A picture measured without the samples, followed by one with them: nothing to compare its count with. Kept.
    StreamCadence d;
    for(int i=0;i<40;++i){Input v=i%2?copy2():fresh2();v.boundary=i==0;d.observe(v);}
    d.observe(fresh(60.0f));CHECK(!d.observe(copy2()).repeat);
    // Garbage in the sample numbers counts as not measured, never as "nothing moved".
    StreamCadence e;
    for(int i=0;i<40;++i){Input v=i%2?copy2():fresh2();v.boundary=i==0;e.observe(v);}
    e.observe(fresh2());Input unusable=copy2();unusable.pixelMax=std::numeric_limits<float>::quiet_NaN();
    CHECK(!e.observe(unusable).repeat);
    e.observe(fresh2());Input oneMissing=copy2();oneMissing.pixelsOver=-1.0f;CHECK(!e.observe(oneMissing).repeat);   // one of the two numbers missing
    Input bad=fresh2();bad.pixelsOver=std::numeric_limits<float>::infinity();e.observe(bad);
    CHECK(!e.observe(copy2()).repeat);                  // the picture before it had no usable count to fall from
    e.observe(fresh2());CHECK(e.observe(copy2()).repeat);
}
TEST(stream_cadence_noise_is_not_a_rhythm){
    using namespace cadence_tests;
    // A still picture: the largest block moves by a tenth of a step or half a step, by chance in
    // turn. With no floor that alternation was learned as a doubled stream.
    StreamCadence c;
    for(int i=0;i<400;++i){const auto r=c.observe(i%2?in(0.05f,0.01f):in(0.5f,0.05f));CHECK(!r.repeat);CHECK(!r.locked);}
    veyra::remoteplay::StreamCadenceConfig noFloor;noFloor.rhythmFloor=0.0f;StreamCadence d(noFloor);
    for(int i=0;i<400;++i)d.observe(i%2?in(0.05f,0.01f):in(0.5f,0.05f));
    CHECK(d.stats().locks>=1);                          // what the floor is for
    // A real doubled stream of a slow scene is still found: 2.0 / 0.2.
    StreamCadence e;unsigned found=0;
    for(int i=0;i<100;++i){const auto r=e.observe(i%2?in(0.2f,0.02f):in(2.0f,0.4f));if(i%2==0)CHECK(!r.repeat);else if(r.repeat)++found;}
    CHECK(found>=40);
}
// ---- The scenes of the independent review, as pictures: what a block mean cannot see.
// A still picture in which one small thing happens, measured exactly as the GPU pass measures it
// (pipeline::streamChangeReference), numbered by PushStreamDedup and given to the engine's own
// ContentDedup. The bar: not one new picture is dropped.
namespace cadence_scenes {
using veyra::engine::ContentDedup;using veyra::engine::PushStreamDedup;
enum class Kind{Square,Particle,Fade};
enum class Rate{Sixty,Thirty,ThirtyThenSixty,ThirtyEarly,HalfWorld};
struct Scene{
    unsigned width=208,height=120;   // 13 x 8 blocks, the bottom row of blocks 8 high
    Kind kind=Kind::Square;Rate rate=Rate::Sixty;
    unsigned frames=600,lead=0;      // lead: game pictures during which the camera pans (the rhythm locks there)
    bool noise=false;                // a model of the encoder: an error on some samples of a new picture, halved by its repeat
    int contrast=120;                // how far the object is from the picture under it
};
struct Outcome{unsigned newPictures=0,repeats=0,droppedNew=0,droppedRepeats=0,falseRepeats=0,numbered=0;bool everLocked=false;};
inline std::uint32_t hash(std::uint32_t x,std::uint32_t y,std::uint32_t z){std::uint32_t k=x*374761393u+y*668265263u+z*2246822519u;k=(k^(k>>13))*1274126177u;return k^(k>>16);}
// The game picture number `picture` of the scene (what the console would show), without the encoder.
inline void render(const Scene& s,unsigned picture,std::vector<std::uint8_t>& out){
    out.resize(std::size_t(s.width)*s.height);
    unsigned pan=6u*(picture<s.lead?picture:s.lead);
    if(s.rate==Rate::HalfWorld)pan=6u*(picture/2u*2u);                       // the world moves on every second picture only
    // A fade of a quarter step per picture, up and down within 24 steps; in quarter steps here.
    const unsigned phase=picture%192u,fadeQuarters=s.kind==Kind::Fade?(phase<96u?phase:192u-phase):0u;
    for(unsigned y=0;y<s.height;++y)for(unsigned x=0;x<s.width;++x){
        const std::uint32_t h=hash((x+pan)/3u,y/3u,7u);                          // a texture of 3x3 patches
        const unsigned base=60u+(h>>25);                                         // 60..187
        // Each sample has its own fraction, so a fade moves different samples on different pictures.
        const unsigned quarter=s.kind==Kind::Fade?(hash(x+pan,y,11u)>>30):0u;
        out[std::size_t(y)*s.width+x]=std::uint8_t(base+(fadeQuarters+quarter)/4u);
    }
    if(s.kind==Kind::Fade)return;
    const unsigned size=s.kind==Kind::Square?6u:2u;
    const unsigned ox=(37u+picture)%(s.width-size),oy=(s.height/3u+5u+(s.kind==Kind::Particle?picture/3u:0u))%(s.height-size);
    for(unsigned y=oy;y<oy+size;++y)for(unsigned x=ox;x<ox+size;++x){
        auto& v=out[std::size_t(y)*s.width+x];v=std::uint8_t(std::min(255,int(v)+s.contrast));
    }
}
// What the decoder shows for stream frame `frame`: the game picture plus what the encoder left of
// its error. A new picture is off by up to 3 steps on one sample in eight; its repeat halves that.
inline void decode(const Scene& s,const std::vector<std::uint8_t>& source,unsigned picture,bool repeat,std::vector<std::uint8_t>& out){
    out=source;
    if(!s.noise)return;
    for(unsigned y=0;y<s.height;++y)for(unsigned x=0;x<s.width;++x){
        const std::uint32_t h=hash(x,y,1000u+picture);
        if(h&7u)continue;
        int error=int((h>>8)%7u)-3;if(repeat)error/=2;
        auto& v=out[std::size_t(y)*s.width+x];v=std::uint8_t(std::clamp(int(v)+error,0,255));
    }
}
inline Outcome run(const Scene& s,veyra::remoteplay::StreamCadenceConfig config={}){
    PushStreamDedup dedup(config);ContentDedup content;Outcome o;
    std::vector<std::uint8_t> source,now,before;
    unsigned picture=0;
    for(unsigned frame=0;frame<s.frames;++frame){
        // Which stream frames show a new game picture.
        bool isNew=true;
        if(s.rate==Rate::Thirty)isNew=frame%2==0;
        else if(s.rate==Rate::ThirtyThenSixty)isNew=frame>=s.frames/2||frame%2==0;
        else if(s.rate==Rate::ThirtyEarly){static constexpr unsigned early[]={150,151,233,234,377};   // now and then a picture is shown once only
            unsigned shift=0;for(const unsigned e:early)if(frame>e)++shift;isNew=(frame+shift)%2==0;for(const unsigned e:early)if(frame==e)isNew=true;}
        if(frame==0)isNew=true;
        if(isNew){if(frame)++picture;render(s,picture,source);}
        decode(s,source,picture,!isNew,now);
        veyra::pipeline::StreamChangeOutcome measured;
        if(frame==0){measured.now.valid=true;}
        else measured.now=veyra::pipeline::decodeStreamChange(veyra::pipeline::streamChangeReference(before.data(),now.data(),s.width,s.width,s.height,1),s.width,s.height,true);
        const std::int64_t pts=1000000+166833ll*std::int64_t(frame);
        const auto d=dedup.observe(measured,frame+1,pts,frame==0);
        const bool keep=content.observe(d.numbered?&d.picture:nullptr,pts,166833,frame==0).keep;
        if(d.cadence.locked)o.everLocked=true;
        if(d.numbered)++o.numbered;
        if(isNew){++o.newPictures;if(!keep)++o.droppedNew;if(d.cadence.repeat)++o.falseRepeats;}
        else{++o.repeats;if(!keep)++o.droppedRepeats;}
        before.swap(now);
    }
    return o;
}
}
TEST(stream_dedup_scenes_real_sixty_fps_over_a_still_picture_drops_nothing){
    using namespace cadence_scenes;
    // The reviewer's three probes, 600 pictures each: 226, 294 and 301 real frames were dropped.
    for(const Kind kind:{Kind::Square,Kind::Particle,Kind::Fade})for(const bool noise:{false,true}){
        Scene s;s.kind=kind;s.rate=Rate::Sixty;s.noise=noise;
        const auto o=run(s);
        CHECK(o.newPictures==600);CHECK(o.droppedNew==0);CHECK(o.falseRepeats==0);CHECK(o.numbered==0);   // never a rhythm: never a number
    }
    // A faint object (24 steps over the picture under it), with the encoder's noise.
    for(const Kind kind:{Kind::Square,Kind::Particle}){
        Scene s;s.kind=kind;s.rate=Rate::Sixty;s.noise=true;s.contrast=24;
        const auto o=run(s);CHECK(o.droppedNew==0);CHECK(o.numbered==0);
    }
}
TEST(stream_dedup_scenes_inside_a_locked_thirty_in_sixty_stream_drop_no_new_picture){
    using namespace cadence_scenes;
    // The camera pans for 60 game pictures (the doubled rhythm is found and repeats are removed),
    // then stops: from there only the small thing happens. As a 30 fps game throughout, across a
    // switch to 60 fps, and with pictures that come one refresh early.
    for(const Kind kind:{Kind::Square,Kind::Particle,Kind::Fade})for(const Rate rate:{Rate::Thirty,Rate::ThirtyThenSixty,Rate::ThirtyEarly})
        for(const bool noise:{true,false}){
            if(!noise&&rate!=Rate::Thirty)continue;
            Scene s;s.kind=kind;s.rate=rate;s.noise=noise;s.lead=60;s.frames=360;
            const auto o=run(s);
            CHECK(o.droppedNew==0);CHECK(o.falseRepeats==0);
            CHECK(o.everLocked);CHECK(o.droppedRepeats>=40);       // not vacuous: the rhythm was trusted and repeats were removed during the pan
        }
    for(const Kind kind:{Kind::Square,Kind::Particle}){
        Scene s;s.kind=kind;s.rate=Rate::ThirtyThenSixty;s.noise=true;s.contrast=24;s.lead=60;s.frames=360;
        const auto o=run(s);CHECK(o.droppedNew==0);CHECK(o.everLocked);
    }
}
TEST(stream_dedup_scenes_a_small_thing_at_sixty_over_a_world_at_thirty_is_kept){
    using namespace cadence_scenes;
    // Every picture is new, and every second one differs from the one before it by the object
    // alone: the blocks show a doubled stream, and without the samples every second picture was
    // dropped. The samples show the object.
    for(const Kind kind:{Kind::Square,Kind::Particle})for(const int contrast:{120,24}){
        Scene s;s.kind=kind;s.rate=Rate::HalfWorld;s.noise=true;s.contrast=contrast;s.frames=300;
        const auto o=run(s);
        CHECK(o.newPictures==300);CHECK(o.droppedNew==0);CHECK(o.falseRepeats==0);CHECK(o.everLocked);
        // The limit, stated: the same with the sample evidence switched off by a ceiling nothing
        // reaches and a ratio nothing exceeds loses every second picture.
        veyra::remoteplay::StreamCadenceConfig blind;blind.pixelCeiling=1.0e9f;blind.pixelRatio=1.0e9f;
        CHECK(run(s,blind).droppedNew>=100);
    }
}
TEST(stream_dedup_scenes_at_1080p){
    using namespace cadence_scenes;
    // The same at 1920x1080 (68 rows of blocks, the last 8 samples high), fewer pictures: a pan of
    // 10 game pictures at 30 fps, then the still picture with the small thing, then 60 fps.
    for(const Kind kind:{Kind::Square,Kind::Particle,Kind::Fade}){
        Scene s;s.width=1920;s.height=1080;s.kind=kind;s.rate=Rate::ThirtyThenSixty;s.noise=true;s.lead=10;s.frames=48;
        const auto o=run(s);
        CHECK(o.droppedNew==0);CHECK(o.falseRepeats==0);CHECK(o.everLocked);CHECK(o.droppedRepeats>=4);
    }
}
