// SPDX-License-Identifier: GPL-3.0-only
// include/veyra/remoteplay/NetDiagnostics.h ([slipstream-net]) and StreamBottleneck.h (the dominant cause).
// Every counter value below is made up to exercise a rule; none is a PS5 measurement.
#include "veyra/remoteplay/NetDiagnostics.h"
#include "veyra/remoteplay/StreamBottleneck.h"
#include "veyra/remoteplay/ReorderStudy.h"
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace netdiag_test {
using namespace veyra::remoteplay;
constexpr HostTime kNetSecond=10000000;
// Cumulative counters of a clean 60 fps stream after `seconds`.
inline HealthCounters clean(double seconds){
    HealthCounters c;
    const auto frames=static_cast<std::uint64_t>(seconds*60.0);
    c.packetsReceived=frames*24;c.framesReceived=frames;c.bytesReceived=frames*30000;
    c.decodeP95Ms=1.2;c.frameJitterMs=0.3;c.frameJitterPeakMs=1.0;
    return c;
}
// Feeds 5 s of a 60 fps stream: one decoded frame every 16.7 ms, 2 ms network-to-decode.
inline void feedFrames(NetDiagnostics& d,HostTime from,std::uint32_t width=1920,std::uint32_t height=1080){
    for(int i=0;i<300;++i){
        const HostTime arrival=from+static_cast<HostTime>(i)*166667;
        d.frameDecoded(arrival,arrival+20000,width,height);
        d.observeBytes(arrival,static_cast<std::uint64_t>(i)*30000u);
        d.observeWaitingIdr(arrival,false);
    }
}
inline NetworkEvidence cleanEvidence(){
    NetworkEvidence n;n.valid=true;n.packets=7200;n.frames=300;n.fps=60;n.decodeP95Ms=1.2;n.jitterMs=0.3;n.jitterPeakMs=1.0;
    n.kbps=12000;n.requestedKbps=15000;n.udpInErrors=0;n.nicDiscards=0;
    return n;
}
}

TEST(net_clean_window_names_nothing_and_marks_unread_counters_unavailable){
    using namespace netdiag_test;
    NetDiagnostics d;
    d.begin(kNetSecond,RouteAssessment{},RouteHop::OnLink,15000,"hevc");
    feedFrames(d,kNetSecond);
    CHECK(!d.sample(kNetSecond+kNetSecond,clean(1),{}));            // not due yet
    const auto s=d.sample(6*kNetSecond,clean(5),{});
    CHECK(s&&s->valid&&s->sequence==1);
    CHECK(s->missing==0&&s->late==0&&s->missingNotLate==0&&s->framesLost==0);
    CHECK(s->frames==300&&s->width==1920&&s->height==1080&&s->resolutionChanges==0);
    CHECK(s->netToDecodeP50Ms&&std::abs(*s->netToDecodeP50Ms-2.0)<1e-9&&std::abs(*s->netToDecodeP99Ms-2.0)<1e-9);
    CHECK(s->burstMax==1&&std::abs(s->gapMaxMs-16.6667)<0.01);
    CHECK(!s->udpInErrors&&!s->nicDiscards);                    // never read: unavailable, not 0
    CHECK(s->verdict.cause==StreamCause::None);
    const auto line=formatNetLine(*s);
    CHECK(line.find("udpInErrors=-1 ")!=std::string::npos&&line.find("nicDiscards=-1 ")!=std::string::npos);
    CHECK(line.find("route=on-link routeClass=unknown family=unknown bottleneck=none signs=none counterWrap=0 (software")!=std::string::npos); // not truncated; a route without an IPv4 literal has no known family
    CHECK(line.find("codec=hevc resolution=1920x1080")!=std::string::npos);
}

TEST(net_late_packets_are_not_counted_as_network_loss){
    using namespace netdiag_test;
    NetDiagnostics d;
    d.begin(0,RouteAssessment{},RouteHop::Unknown,15000,"hevc");
    auto c=clean(5);c.packetsLost=10;c.reorderLateArrivals=4;c.reorderDropped=7;c.fecRecovered=3;
    feedFrames(d,0);
    const auto s=d.sample(5*kNetSecond,c,{});
    CHECK(s&&s->missing==10&&s->late==4&&s->missingNotLate==6&&s->dropOther==3);
    CHECK(s->fecRecoveryPct&&std::abs(*s->fecRecoveryPct-100.0)<1e-9);
    // Every missing packet was repaired by FEC: a sign of loss, not a bottleneck.
    CHECK(s->verdict.cause==StreamCause::None);
}

TEST(net_system_counters_give_deltas_and_refuse_a_counter_that_went_back){
    using namespace netdiag_test;
    NetDiagnostics d;
    d.begin(0,RouteAssessment{},RouteHop::OnLink,15000,"hevc");
    SystemNetCounters a;a.udpKnown=true;a.udpInDatagrams=1000;a.udpInErrors=5;a.nicKnown=true;a.nicInPackets=2000;a.nicInDiscards=1;a.nicInErrors=0;
    d.primeSystem(a);
    feedFrames(d,0);
    SystemNetCounters b=a;b.udpInDatagrams=8200;b.udpInErrors=5;b.nicInPackets=9300;b.nicInDiscards=1;
    const auto s=d.sample(5*kNetSecond,clean(5),b);
    CHECK(s&&s->udpInErrors&&*s->udpInErrors==0&&*s->udpInDatagrams==7200&&*s->nicDiscards==0&&*s->nicInPackets==7300);
    feedFrames(d,5*kNetSecond);
    SystemNetCounters back=b;back.udpInErrors=2; // reset or wrap: unknown for this window, never a negative or huge delta
    const auto t=d.sample(10*kNetSecond,clean(10),back);
    CHECK(t&&!t->udpInErrors&&t->nicDiscards&&*t->nicDiscards==0);
}

TEST(net_bursts_resolution_changes_one_second_rates_and_keyframe_waits){
    using namespace netdiag_test;
    NetDiagnostics d;
    d.begin(0,RouteAssessment{},RouteHop::OnLink,15000,"hevc");
    // 5 frames arrive together after an 80 ms stall, then one 720p frame (a console profile switch).
    HostTime t=kNetSecond;
    d.frameDecoded(t,t+10000,1920,1080);
    t+=800000;
    for(int i=0;i<5;++i)d.frameDecoded(t+i*5000,t+i*5000+30000,1920,1080);
    d.frameDecoded(t+200000,t+220000,1280,720);
    // Bytes: 2 Mbit in the first second, 6 Mbit in the second.
    d.observeBytes(0,0);d.observeBytes(kNetSecond,250000);d.observeBytes(2*kNetSecond,1000000);
    d.observeWaitingIdr(3*kNetSecond,true);d.observeWaitingIdr(3*kNetSecond+3000000,false); // 300 ms frozen
    const auto s=d.sample(5*kNetSecond,clean(5),{});
    CHECK(s&&s->burstMax==5&&std::abs(s->gapMaxMs-80.0)<1e-9);
    CHECK(s->resolutionChanges==1&&s->width==1280&&s->height==720);
    CHECK(s->kbpsMin1s&&std::abs(*s->kbpsMin1s-2000.0)<1e-6&&std::abs(*s->kbpsMax1s-6000.0)<1e-6);
    CHECK(std::abs(s->waitingIdrMs-300.0)<1e-9);
    // A wait still open at the window's end is counted up to it and carried on.
    d.observeWaitingIdr(9*kNetSecond,true);
    const auto u=d.sample(10*kNetSecond,clean(10),{});
    CHECK(u&&std::abs(u->waitingIdrMs-1000.0)<1e-9&&u->resolutionChanges==0&&u->width==1280);
}

TEST(net_sample_buffer_is_bounded){
    using namespace netdiag_test;
    NetDiagnostics d;
    d.begin(0,RouteAssessment{},RouteHop::Unknown,15000,"hevc");
    for(std::size_t i=0;i<NetDiagnostics::kMaxSamples*3;++i){const HostTime a=1+static_cast<HostTime>(i)*1000;d.frameDecoded(a,a+10000,1920,1080);}
    const auto s=d.sample(5*kNetSecond,clean(5),{});
    CHECK(s&&s->frames==NetDiagnostics::kMaxSamples*3&&s->netToDecodeP95Ms&&std::abs(*s->netToDecodeP95Ms-1.0)<1e-9);
}

TEST(stream_bottleneck_network_loss_versus_packet_recovery_versus_local_drops){
    using namespace netdiag_test;
    auto n=cleanEvidence();n.fecFailed=2;n.framesLost=2;n.missingNotLate=40;
    CHECK(classifyStream(n).cause==StreamCause::NetworkLoss);
    n.lateArrivals=6; // the packets came, the wait had given them up
    CHECK(classifyStream(n).cause==StreamCause::PacketRecovery);
    n.lateArrivals=0;n.udpInErrors=12; // this PC dropped datagrams in the same window
    const auto v=classifyStream(n);
    CHECK(v.cause==StreamCause::SocketOrAdapter&&(v.signs&streamCauseBit(StreamCause::NetworkLoss))!=0);
    n.udpInErrors.reset();n.nicDiscards=3; // 3 local discards against 40 packets that never came: a sign, not the cause
    const auto few=classifyStream(n);
    CHECK(few.cause==StreamCause::NetworkLoss&&(few.signs&streamCauseBit(StreamCause::SocketOrAdapter))!=0);
    n.nicDiscards=5; // of the order of the missing packets (a tenth or more): this PC dropped them
    CHECK(classifyStream(n).cause==StreamCause::SocketOrAdapter);
    n.udpInErrors=0;n.nicDiscards.reset();
    CHECK(classifyStream(n).cause==StreamCause::NetworkLoss); // unread adapter: no local claim
    // UDP errors elsewhere on the PC while nothing of ours is missing: not our problem.
    auto clean=cleanEvidence();clean.udpInErrors=50;
    CHECK(classifyStream(clean).cause==StreamCause::None);
}

TEST(stream_bottleneck_app_drops_only_when_the_library_lost_nothing){
    using namespace netdiag_test;
    auto n=cleanEvidence();n.ingressDropped=4;
    CHECK(classifyStream(n).cause==StreamCause::AppDrops);
    n.framesLost=1;n.missingNotLate=10; // frames dropped while waiting for a keyframe after a real loss
    CHECK(classifyStream(n).cause==StreamCause::NetworkLoss);
}

TEST(stream_bottleneck_never_blames_gpu_decoder_or_encoder_while_frames_are_lost){
    using namespace netdiag_test;
    auto n=cleanEvidence();n.fecFailed=1;n.framesLost=1;n.missingNotLate=30;n.mailboxOverwrites=50;n.decodeP95Ms=20.0;n.kbps=3000;
    EngineEvidence e;e.valid=true;e.gpuUtilizationPct=99;e.enhancementActive=true;
    const auto v=classifyStream(n,e);
    CHECK(v.cause==StreamCause::NetworkLoss);
    CHECK((v.signs&streamCauseBit(StreamCause::Encoder))==0); // a starved encoder is not claimed while packets are missing
    CHECK((v.signs&streamCauseBit(StreamCause::Enhancement))!=0&&(v.signs&streamCauseBit(StreamCause::Decoder))!=0);
}

TEST(stream_bottleneck_gpu_side_causes_from_engine_evidence){
    using namespace netdiag_test;
    auto n=cleanEvidence();n.mailboxOverwrites=12; // 4 % of decoded frames never taken by the engine
    CHECK(classifyStream(n).cause==StreamCause::GpuProcessing);
    EngineEvidence e;e.valid=true;e.fgActive=true;e.fgBudgetLimited=true;
    CHECK(classifyStream(n,e).cause==StreamCause::FrameGeneration);
    e.fgBudgetLimited=false;e.gpuUtilizationPct=97;e.enhancementActive=true;
    CHECK(classifyStream(n,e).cause==StreamCause::Enhancement);
    // Every frame taken, but fewer real frames presented than decoded: presentation.
    auto m=cleanEvidence();EngineEvidence p;p.valid=true;p.gpuUtilizationPct=60;p.realPresentFps=50;p.decodedFps=59.9;
    CHECK(classifyStream(m,p).cause==StreamCause::Presentation);
    p.realPresentFps=59.5;
    CHECK(classifyStream(m,p).cause==StreamCause::None);
    n.mailboxOverwrites=5; // under 2 %
    CHECK(classifyStream(n).cause==StreamCause::None);
}

TEST(stream_bottleneck_encoder_and_decoder_and_jitter){
    using namespace netdiag_test;
    auto n=cleanEvidence();n.kbps=5000; // a third of the request, network clean: the console sent few bits
    CHECK(classifyStream(n).cause==StreamCause::Encoder);
    n.requestedKbps=0; // request unknown: no encoder claim
    CHECK(classifyStream(n).cause==StreamCause::None);
    auto d=cleanEvidence();d.decodeP95Ms=14.0; // 0.84 of a 60 fps frame
    CHECK(classifyStream(d).cause==StreamCause::Decoder);
    auto j=cleanEvidence();j.jitterPeakMs=25;j.kbps=5000;
    const auto v=classifyStream(j);
    CHECK(v.cause==StreamCause::Jitter&&(v.signs&streamCauseBit(StreamCause::Encoder))==0);
    CHECK(formatStreamCauseSigns(v.signs)=="jitter");
    CHECK(v.evidence.find("jitterMs=0.30")!=std::string::npos);
    NetworkEvidence none;CHECK(classifyStream(none).cause==StreamCause::Unknown&&classifyStream(none).evidence=="no-window");
}

TEST(stream_bottleneck_a_late_receive_thread_comes_before_the_losses_it_causes){
    using namespace netdiag_test;
    auto n=cleanEvidence();n.reorderTimeouts=12;n.reorderOvershootMs=3.5;n.fecFailed=2;n.framesLost=2;n.lateArrivals=5;
    const auto v=classifyStream(n);
    CHECK(v.cause==StreamCause::ReceiveThread&&(v.signs&streamCauseBit(StreamCause::PacketRecovery))!=0);
    n.reorderOvershootMs=1.0; // the library's own 1 ms rounding: normal
    CHECK(classifyStream(n).cause==StreamCause::PacketRecovery);
    n.reorderOvershootMs=3.5;n.reorderTimeouts=3; // too few waits to say
    CHECK(classifyStream(n).cause==StreamCause::PacketRecovery);
}

TEST(stream_bottleneck_a_reorder_wait_that_holds_frames_for_lost_packets_is_packet_recovery){
    using namespace netdiag_test;
    // Frames came unevenly, waits ran out, nothing came late, FEC completed every frame: the wait held them.
    auto n=cleanEvidence();n.jitterPeakMs=15;n.reorderTimeouts=8;n.missingNotLate=8;
    const auto v=classifyStream(n);
    CHECK(v.cause==StreamCause::PacketRecovery&&(v.signs&streamCauseBit(StreamCause::Jitter))!=0);
    n.reorderTimeouts=0; // uneven frames with no wait involved: the path itself
    CHECK(classifyStream(n).cause==StreamCause::Jitter);
    n.reorderTimeouts=8;n.lateArrivals=3; // packets did come late: reordering on the path, not a wait for nothing
    CHECK(classifyStream(n).cause==StreamCause::Jitter);
}

// The impairment study (tools/remoteplay_latency_bench/impairment_study.cpp) in short: a Linux model, one
// seed, the owner's cases that decide something. Rules compared on the same made-up arrivals.
namespace netdiag_test {
inline const veyra::remoteplay::study::Condition& impairment(const char* name){
    static const auto all=veyra::remoteplay::study::impairmentConditions();
    for(const auto& c:all)if(std::string(c.name)==name)return c;
    throw std::runtime_error(std::string("no condition ")+name);
}
struct StudyRun {std::uint64_t lost=0;std::vector<std::int64_t> added;std::uint64_t stutters=0;std::map<std::string,unsigned> verdicts;};
inline StudyRun studyRun(const char* name,std::uint32_t waitUs){
    using namespace veyra::remoteplay::study;
    const auto& c=impairment(name);
    const auto trace=makeTrace(c,4242,6000);
    veyra::remoteplay::study::Policy p;p.startUs=waitUs;
    const auto r=runPolicy(c,trace,p);
    StudyRun out;std::int64_t last=-1;
    for(const auto& f:r.frames){
        if(f.handUs<0){++out.lost;continue;}
        out.added.push_back(f.handUs-f.possibleUs);
        if(last>=0&&f.handUs-last>2*kFrameUs)++out.stutters;
        last=f.handUs;
    }
    std::sort(out.added.begin(),out.added.end());
    for(const auto& h:r.intervals)++out.verdicts[veyra::remoteplay::streamCauseName(veyra::remoteplay::classifyStream(studyEvidence(h)).cause)];
    return out;
}
inline std::int64_t p99(const std::vector<std::int64_t>& v){return v.empty()?0:v[(v.size()*99+99)/100-1];}
}

TEST(impairment_model_clean_link_and_duplicates_lose_nothing_and_name_nothing){
    using namespace netdiag_test;
    for(const char* name:{"no_loss","duplication"}){
        const auto r=studyRun(name,16000);
        CHECK(r.lost==0&&r.stutters==0&&p99(r.added)==0);
        CHECK(r.verdicts.size()==1&&r.verdicts.count("none")==1);
    }
}

TEST(impairment_model_independent_loss_is_repaired_by_fec_and_the_long_wait_only_adds_delay){
    using namespace netdiag_test;
    const auto longWait=studyRun("loss_0p5",16000),shortWait=studyRun("loss_0p5",2000);
    CHECK(longWait.lost==shortWait.lost);                 // the wait saves no frame from a packet that never comes
    CHECK(longWait.lost<=5);                              // FEC repaired almost every frame (6000 frames, 0.5 % for 60 s)
    CHECK(p99(longWait.added)>=10000&&p99(shortWait.added)<=4000); // the 16 ms wait holds the frames behind each loss
    CHECK(longWait.stutters>shortWait.stutters);
    CHECK(longWait.verdicts.count("packet-recovery")==1&&shortWait.verdicts.count("packet-recovery")==0);
}

TEST(impairment_model_bursts_longer_than_fec_cost_the_same_frames_whatever_the_wait){
    using namespace netdiag_test;
    const auto longWait=studyRun("short_bursts",16000),shortWait=studyRun("short_bursts",2000);
    CHECK(longWait.lost>0&&longWait.lost==shortWait.lost);
    CHECK(shortWait.verdicts.count("network-loss")==1);
}

TEST(impairment_model_a_loaded_pc_is_named_as_such_not_as_the_network){
    using namespace netdiag_test;
    const auto r=studyRun("loaded_pc_proxy",2000);
    CHECK(r.lost==0&&r.verdicts.count("receive-thread")==1&&r.verdicts.count("network-loss")==0);
}

// Review of 21 Nov (/home/claude/build/review-network/adv/cls.cpp, wrap.c), kept as tests.
TEST(review_a_window_with_no_packet_and_no_frame_is_unknown_never_none){
    using namespace netdiag_test;
    NetworkEvidence n;n.valid=true;n.packets=0;n.frames=0;n.fps=0;n.kbps=0;n.requestedKbps=15000;
    CHECK(classifyStream(n).cause==StreamCause::Unknown);
    n.framesLost=4; // an outage that cost frames: still nothing to judge in the window itself
    CHECK(classifyStream(n).cause==StreamCause::Unknown);
    CHECK(std::string(streamCauseName(StreamCause::Unknown))=="unknown");
    NetDiagnostics d;d.begin(1,RouteAssessment{},RouteHop::OnLink,15000,"hevc");
    const auto s=d.sample(1+NetDiagnostics::kInterval100ns,HealthCounters{},{});
    CHECK(s&&s->valid&&s->verdict.cause==StreamCause::Unknown);
    CHECK(formatNetLine(*s).find("bottleneck=unknown ")!=std::string::npos);
}

TEST(review_one_udp_error_elsewhere_on_the_pc_beside_fec_healed_loss_is_not_the_cause){
    using namespace netdiag_test;
    NetworkEvidence m;m.valid=true;m.packets=8000;m.missingNotLate=3;m.udpInErrors=1;m.nicDiscards=0;m.frames=300;m.fps=60;m.kbps=14000;m.requestedKbps=15000;
    const auto v=classifyStream(m);
    CHECK(v.cause==StreamCause::None&&(v.signs&streamCauseBit(StreamCause::SocketOrAdapter))!=0);
    // Lost frames, 200 packets that never came, 2 local errors: the network, with the local errors as a sign.
    auto l=m;l.fecFailed=5;l.framesLost=5;l.missingNotLate=200;l.udpInErrors=2;
    const auto w=classifyStream(l);
    CHECK(w.cause==StreamCause::NetworkLoss&&(w.signs&streamCauseBit(StreamCause::SocketOrAdapter))!=0);
}

TEST(review_a_wrapped_loss_counter_is_unavailable_not_loss){
    using namespace netdiag_test;
    // chiaki packetstats.c: seq_max - seq_min of two uint16 promoted to int, stored in uint64: at the wrap
    // (65500 -> 20) the "lost" count becomes about 2^64 (wrap.c of the review).
    const std::uint16_t seqMin=65500,seqMax=20;
    const std::uint64_t diff=static_cast<std::uint64_t>(static_cast<std::int64_t>(seqMax)-static_cast<std::int64_t>(seqMin));
    CHECK(diff>(std::uint64_t{1}<<63));
    NetDiagnostics d;d.begin(0,RouteAssessment{},RouteHop::OnLink,15000,"hevc");
    feedFrames(d,0);
    auto c=clean(5);c.packetsLost=diff-56;
    const auto s=d.sample(5*kNetSecond,c,{});
    CHECK(s&&s->counterWrap&&s->health.counterWrap&&s->health.lossPercent<0&&s->missing==0&&s->missingNotLate==0);
    const auto line=formatNetLine(*s);
    CHECK(line.find(" missing=-1 ")!=std::string::npos&&line.find(" missingNotLate=-1 ")!=std::string::npos&&line.find(" lossPct=-1.000 ")!=std::string::npos);
    CHECK(line.find("counterWrap=1")!=std::string::npos&&s->verdict.cause==StreamCause::None);
    CHECK(formatHealthLine(s->health).find("lossPercent=-1.000")!=std::string::npos&&formatHealthLine(s->health).find("counterWrap=1")!=std::string::npos);
    // The next window is measured from the poisoned value: an ordinary window again.
    feedFrames(d,5*kNetSecond);
    auto next=clean(10);next.packetsLost=c.packetsLost+6;
    const auto t=d.sample(10*kNetSecond,next,{});
    CHECK(t&&!t->counterWrap&&t->missing==6);
    // A classifier handed a count above every packet of the window claims nothing from it.
    NetworkEvidence w;w.valid=true;w.packets=8000;w.missingNotLate=18446744073709486080ull;w.udpInErrors=0;w.frames=300;w.fps=60;w.kbps=14000;w.requestedKbps=15000;
    CHECK(classifyStream(w).cause==StreamCause::None&&classifyStream(w).signs==0);
    w.fecFailed=2;w.framesLost=2; // frames were lost: the network is named, but no local drop is claimed
    CHECK(classifyStream(w).cause==StreamCause::NetworkLoss);
}

// Re-review (/home/claude/build/review-network/adv/wrap2.cpp): the wrap is decided from the counter, not from loss
// against reception.
TEST(review_heavy_real_loss_stays_loss_and_a_wrap_after_a_long_history_is_caught){
    using namespace veyra::remoteplay;
    {   // (a) 3000 received, 4000 missing in 5 s: real heavy loss, not a wrap
        ConnectionHealth h;h.begin(1,RouteAssessment{});HealthCounters c{};c.packetsReceived=3000;c.packetsLost=4000;
        const auto s=h.sample(1+ConnectionHealth::kInterval100ns,c);
        CHECK(s&&!s->counterWrap&&s->packetsLost==4000&&std::abs(s->lossPercent-400.0/7.0)<1e-9&&s->intervalGrade==HealthGrade::Unstable);
    }
    {   // (b) after 70000 cumulative losses the counter wraps and goes DOWN by about 65500
        ConnectionHealth h;h.begin(1,RouteAssessment{});HealthCounters c{};c.packetsReceived=1000000;c.packetsLost=70000;
        CHECK(h.sample(1+ConnectionHealth::kInterval100ns,c));
        c.packetsReceived+=8000;c.packetsLost=70000-65500+2;
        const auto s=h.sample(1+2*ConnectionHealth::kInterval100ns,c);
        CHECK(s&&s->counterWrap&&s->packetsLost==0&&s->lossPercent<0);
        c.packetsReceived+=8000;c.packetsLost+=3; // the next interval: measured from the wrapped value
        const auto t=h.sample(1+3*ConnectionHealth::kInterval100ns,c);
        CHECK(t&&!t->counterWrap&&t->packetsLost==3);
    }
}
