// SPDX-License-Identifier: GPL-3.0-only
// Included by CoreTests.cpp: motion samples survive the coalescing of analog-only changes.
namespace controller_queue_fix_tests {
ControllerState withMotion(std::uint32_t buttons,std::uint64_t firstUs,int count,std::int16_t stick=0){
    ControllerState s;s.inputActive=true;s.buttons=buttons;s.leftX=stick;
    for(int i=0;i<count;++i){s.motionSamples[std::size_t(i)].timestampUs=firstUs+std::uint64_t(i);s.motionSamples[std::size_t(i)].gyro[0]=float(firstUs+std::uint64_t(i));}
    s.motionSampleCount=std::uint8_t(count);return s;
}
}
TEST(controller_queue_coalescing_keeps_motion_samples_in_order){
    using namespace controller_queue_fix_tests;
    ControllerQueue q;
    CHECK(q.push(1,withMotion(0,100,3,1)));
    CHECK(q.push(2,withMotion(0,200,2,2)));   // motion-only change: replaces, keeps the older samples
    CHECK(q.push(3,withMotion(0,300,4,3)));
    CHECK(q.depth()==1);
    const auto s=q.take(3);
    CHECK(s.leftX==3);CHECK(s.motionSampleCount==9);
    const std::uint64_t expected[]={100,101,102,200,201,300,301,302,303};
    for(std::size_t i=0;i<9;++i){CHECK(s.motionSamples[i].timestampUs==expected[i]);CHECK(s.motionSamples[i].gyro[0]==float(expected[i]));}
    // Taken samples are not sent again: the next poll's samples stand alone.
    CHECK(q.push(4,withMotion(0,400,1,3)));
    const auto next=q.take(4);CHECK(next.motionSampleCount==1&&next.motionSamples[0].timestampUs==400);
}
TEST(controller_queue_coalescing_caps_motion_samples_keeping_the_newest){
    using namespace controller_queue_fix_tests;
    ControllerQueue q;
    CHECK(q.push(1,withMotion(0,100,10)));
    CHECK(q.push(2,withMotion(0,200,10)));
    const auto s=q.take(2);
    CHECK(s.motionSampleCount==16);
    // 20 samples, cap 16: the four oldest are dropped, order kept.
    for(std::size_t i=0;i<6;++i)CHECK(s.motionSamples[i].timestampUs==104+i);
    for(std::size_t i=0;i<10;++i)CHECK(s.motionSamples[6+i].timestampUs==200+i);
    // A full newer poll alone keeps exactly its own 16.
    CHECK(q.push(3,withMotion(0,300,5)));CHECK(q.push(4,withMotion(0,500,16)));
    const auto full=q.take(4);CHECK(full.motionSampleCount==16);
    for(std::size_t i=0;i<16;++i)CHECK(full.motionSamples[i].timestampUs==500+i);
}
TEST(controller_queue_motion_merge_never_coalesces_button_edges){
    using namespace controller_queue_fix_tests;
    ControllerQueue q;
    CHECK(q.push(1,withMotion(ControllerState::Cross,100,2)));
    CHECK(q.push(2,withMotion(0,200,2)));            // release: an edge, its own entry
    CHECK(q.push(3,withMotion(ControllerState::Cross,300,2)));
    CHECK(q.push(4,withMotion(ControllerState::Cross,400,2,7))); // analog change: merges into the last press
    CHECK(q.depth()==3);
    auto a=q.take(4);CHECK(a.buttons==ControllerState::Cross&&a.motionSampleCount==2&&a.motionSamples[0].timestampUs==100);
    auto b=q.take(4);CHECK(b.buttons==0&&b.motionSampleCount==2&&b.motionSamples[0].timestampUs==200);
    auto c=q.take(4);CHECK(c.buttons==ControllerState::Cross&&c.leftX==7&&c.motionSampleCount==4);
    CHECK(c.motionSamples[0].timestampUs==300&&c.motionSamples[3].timestampUs==401);
}
