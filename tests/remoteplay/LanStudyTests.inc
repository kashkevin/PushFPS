// SPDX-License-Identifier: GPL-3.0-only
// Sprint 3 (network): the reorder-wait study conditions (ReorderStudy.h,
// Linux model) against the health classification and the adaptive rule, and
// long randomized impairment runs through the reorder wait, the ingress
// queue and the reconnect rule together: no added delay may stay behind
// after an impairment, and nothing may ratchet upward from one to the next.
// Everything here is a model; nothing ran on a PS5, a network or Windows.
#include "veyra/remoteplay/ReorderStudy.h"
#include <map>
#include <set>
#include <string>
namespace lan_study {
using namespace veyra::remoteplay::study;
// Declarations, not only the directive: the test file has its own Rng.
using veyra::remoteplay::study::Rng;using veyra::remoteplay::study::Arrival;using veyra::remoteplay::study::Condition;
using veyra::remoteplay::study::Trace;using veyra::remoteplay::study::Policy;using veyra::remoteplay::study::RunResult;
inline constexpr unsigned kStudyFrames=7192; // 120 s
inline const Condition& condition(const char* name){
    static const auto all=studyConditions();
    for(const auto& c:all)if(std::string(c.name)==name)return c;
    throw std::runtime_error(std::string("no condition ")+name);
}
inline std::uint64_t lost(const RunResult& r){
    std::uint64_t n=0;for(const auto& f:r.frames)n+=f.handUs<0?1u:0u;return n;
}
inline double p99Added(const RunResult& r){
    std::vector<std::int64_t> v;
    for(const auto& f:r.frames)if(f.handUs>=0)v.push_back(f.handUs-f.possibleUs);
    if(v.empty())return 0;
    std::sort(v.begin(),v.end());
    return static_cast<double>(v[(v.size()*99)/100])/1000.0;
}
} // namespace lan_study

// --- Item 4: does each condition get the right class? ---
TEST(study_bottleneck_classes_match_each_condition){
    using namespace lan_study;
    const std::map<std::string,std::set<std::string>> allowed{
        {"light_reordering",{"reordering"}},{"heavy_reordering",{"reordering"}},
        {"burst_loss_ge2_fec",{"loss","none"}},{"burst_loss_ge5_fec",{"loss","none"}},{"burst_loss_ge15_fec",{"loss","none"}},
        {"burst_loss_ge2_nofec",{"loss","none"}},{"burst_loss_ge5_nofec",{"loss","none"}},
        {"congestion_queue",{"jitter"}},{"congestion_with_reordering",{"loss","reordering","jitter"}},
        {"jitter_spikes",{"jitter"}},{"interruption_1s",{"loss"}},{"interruption_5s",{"loss"}},
        {"bandwidth_step",{"loss","jitter"}},{"late_receive_thread",{"receive-scheduling"}},{"slow_decoder",{"decoder-backlog"}}};
    const std::map<std::string,std::string> mostly{
        {"light_reordering","reordering"},{"heavy_reordering","reordering"},{"burst_loss_ge15_fec","loss"},{"burst_loss_ge2_nofec","loss"},
        {"jitter_spikes","jitter"},{"late_receive_thread","receive-scheduling"},{"slow_decoder","decoder-backlog"}};
    const auto all=studyConditions();
    for(std::size_t i=0;i<all.size();++i){
        const auto& c=all[i];
        const auto trace=makeTrace(c,1000u*static_cast<unsigned>(i+1)+1u,kStudyFrames);
        for(const bool adaptive:{false,true}){
            const auto r=runPolicy(c,trace,{2000,adaptive});
            std::map<std::string,unsigned> named;bool anyLoss=false;
            for(const auto& h:r.intervals){
                const std::int64_t end=kStartUs+static_cast<std::int64_t>(h.sequence)*kIntervalUs,begin=end-kIntervalUs;
                const std::string name=bottleneckName(h.bottleneck);
                if(c.seconds>0&&end>c.windowStartUs()&&begin<c.windowEndUs()){
                    ++named[name];anyLoss=anyLoss||name=="loss";
                    if(!allowed.at(c.name).count(name))throw std::runtime_error(std::string(c.name)+": impaired interval named "+name);
                }else if(begin>=(c.seconds>0?c.windowEndUs():kStartUs)+10*1000000){
                    // Ten seconds after the impairment the link is clean again: nothing named, graded stable.
                    if(name!="none"||h.intervalGrade!=HealthGrade::Stable)throw std::runtime_error(std::string(c.name)+": clean interval named "+name);
                }
            }
            const auto want=mostly.find(c.name);
            if(want!=mostly.end()){
                unsigned top=0;std::string topName;
                for(const auto& [name,count]:named)if(count>top){top=count;topName=name;}
                if(topName!=want->second)throw std::runtime_error(std::string(c.name)+": mostly "+topName);
            }
            const std::string n=c.name;
            if(n=="burst_loss_ge15_fec"||n.find("nofec")!=std::string::npos||n.rfind("interruption",0)==0)CHECK(anyLoss);
        }
    }
}
TEST(study_late_given_up_packets_are_reordering_not_loss){
    // A frame FEC could not complete while packets arrived after being given
    // up: the wait was too short for the link, nothing was lost on the wire.
    HealthSnapshot s;s.valid=true;s.seconds=5;s.packetsReceived=8000;s.receivedFps=60;s.decodeP95Ms=3.0;s.ingressPeak=1;
    s.fecFailed=3;s.lateArrivals=5;
    CHECK(classifyBottleneck(s)==Bottleneck::Reordering);
    CHECK((bottleneckSigns(s)&bottleneckBit(Bottleneck::Loss))==0);
    s.lateArrivals=0;CHECK(classifyBottleneck(s)==Bottleneck::Loss);             // nothing came late: lost
    s.lateArrivals=5;s.lossPercent=0.5;s.packetsLost=40;CHECK(classifyBottleneck(s)==Bottleneck::Loss); // measured loss too
}
TEST(study_a_single_long_stall_is_jitter_even_after_the_average_forgot_it){
    HealthSnapshot s;s.valid=true;s.seconds=5;s.packetsReceived=8000;s.receivedFps=60;s.decodeP95Ms=3.0;s.ingressPeak=1;
    s.frameJitterMs=0.4;s.frameJitterPeakMs=25.0;
    CHECK(classifyBottleneck(s)==Bottleneck::Jitter);
    s.frameJitterPeakMs=7.9;CHECK(classifyBottleneck(s)==Bottleneck::None); // below the provisional 8 ms
}
TEST(study_health_line_names_late_arrivals_at_its_end){
    HealthSnapshot s;s.valid=true;s.lateArrivals=7;
    const auto line=formatHealthLine(s);
    CHECK(line.find(" lateArrivals=7 routeClass=unknown")!=std::string::npos);
    HealthCounters c;c.packetsReceived=100;c.reorderLateArrivals=7;
    ConnectionHealth h;h.begin(0);
    CHECK(h.sample(ConnectionHealth::kInterval100ns,c)->lateArrivals==7);
}

// --- Item 1: the adaptive rule against the fixed waits (Linux model) ---
TEST(study_adaptive_never_loses_more_than_the_fixed_floor_or_waits_longer_than_the_ceiling){
    using namespace lan_study;
    const auto all=studyConditions();
    for(std::size_t i=0;i<all.size();++i){
        const auto& c=all[i];
        const auto trace=makeTrace(c,1000u*static_cast<unsigned>(i+1)+2u,kStudyFrames);
        const auto two=runPolicy(c,trace,{2000,false}),sixteen=runPolicy(c,trace,{16000,false}),adaptive=runPolicy(c,trace,{2000,true});
        if(lost(adaptive)>lost(two))throw std::runtime_error(std::string(c.name)+": adaptive lost more than fixed 2 ms");
        CHECK(p99Added(adaptive)<=p99Added(sixteen)+0.5);
        CHECK(adaptive.faults==0);
        for(const auto& w:adaptive.windows)CHECK(w.windowUs>=2000&&w.windowUs<=16000);
    }
    // Where frames are lost to a wait that is too short, it widens and loses
    // far fewer than the fixed 2 ms (study: 20 against 156 over five seeds).
    const auto& heavy=condition("heavy_reordering");
    const auto trace=makeTrace(heavy,77,kStudyFrames);
    const auto two=runPolicy(heavy,trace,{2000,false}),adaptive=runPolicy(heavy,trace,{2000,true});
    CHECK(lost(adaptive)*3<=lost(two));
}

// --- Item 2: no accumulating latency (Linux model, long randomized runs) ---
// One model of the whole receive path in virtual time: the study's packet
// traces (60 s segments, each with one random impairment from 10 s on, then
// clean) -> the library's reorder queue (ReorderQueueModel) under a fixed
// 2 ms wait or the adaptive controller -> frames handed over when 24 of their
// packets are out -> VideoIngress (the real class; gaps, keyframe requests,
// optional backlog) -> one decoder (3 ms a frame, 18 ms in a slow-decoder
// window) -> StreamRecovery polled every 50 ms (keyframe requests, a
// reconnect after 6 s of silence with its backoff; a connect fails after 2 s
// while the link is down, takes 0.5 s while it is up). The console answers a
// keyframe request with the first frame it sends 30 ms later, and starts every
// connection with one. "Added" for a decoded frame = decode start minus the
// moment 24 of its packets had reached the socket: the reorder hold plus the
// ingress wait, the two delays the receiver itself adds.
namespace lan_study {
struct LongRunOptions {
    std::uint64_t seed=1;unsigned segments=30;bool adaptive=true;std::size_t backlogFrames=0;bool patient=false;
    std::int64_t heldSessionUs=0; // after an outage the console still holds the old session this long (RP_IN_USE)
    unsigned cleanTail=5;         // the last segments are clean: the adaptive wait must come back within them
};
struct SegmentResult {
    std::string condition;std::int64_t windowEndUs=0;
    std::int64_t settleUs=0;        // from the impairment's end until no decoded frame was added more than 1 ms
    std::int64_t resumeUs=0;        // from the impairment's end until the first decoded frame
    std::int64_t tailWorstUs=0;     // worst added delay in the last 10 s of the segment
    std::size_t tailFrames=0,tailExpected=0,endDepth=0;
    bool refusedInUse=false;        // a reconnect in this segment was refused with RP_IN_USE
};
struct LongRunResult {
    std::vector<SegmentResult> segments;
    unsigned reconnects=0,inUseRefusals=0,widerAfterReconnect=0;std::uint64_t decoded=0,keyframeRequests=0;
    std::uint32_t windowAfterRunUs=0,windowAfterCleanHourUs=0;
};
inline LongRunResult longRun(const LongRunOptions& o){
    using A=StreamRecovery::Action;
    constexpr unsigned kSegmentFrames=3596; // 60 s
    constexpr std::int64_t kSegmentUs=static_cast<std::int64_t>(kSegmentFrames)*kFrameUs;
    const auto all=studyConditions();
    Rng rng(o.seed*7919u+13u);
    // The whole run's arrivals: segments one after the other, merged in time.
    struct Segment {std::uint32_t firstPacket;std::int64_t firstFrame;unsigned units;Condition c;std::int64_t windowStart,windowEnd;};
    std::vector<Segment> segs;std::vector<Arrival> arrivals;
    std::uint32_t nextPacket=0;
    for(unsigned k=0;k<o.segments;++k){
        Condition c=all[static_cast<std::size_t>(rng.next()%all.size())];
        if(k+o.cleanTail>=o.segments)c=all[0]; // clean_wired_lan
        c.fromSeconds=10;
        if(c.outage){const double lengths[]={1,5,8};c.seconds=lengths[rng.next()%3];}
        else c.seconds=c.seconds>0?2.0+18.0*rng.unit():0;
        const std::int64_t firstFrame=static_cast<std::int64_t>(k)*kSegmentFrames;
        auto t=makeTrace(c,o.seed*100003u+k,kSegmentFrames,firstFrame,nextPacket);
        const std::int64_t shift=firstFrame*kFrameUs;
        segs.push_back({nextPacket,firstFrame,c.units(),c,c.windowStartUs()+shift,c.seconds>0?c.windowEndUs()+shift:c.windowStartUs()+shift});
        nextPacket+=kSegmentFrames*c.units();
        arrivals.insert(arrivals.end(),t.arrivals.begin(),t.arrivals.end());
    }
    std::stable_sort(arrivals.begin(),arrivals.end(),[](const Arrival& a,const Arrival& b){return a.atUs<b.atUs;});
    const std::int64_t totalFrames=static_cast<std::int64_t>(o.segments)*kSegmentFrames;
    const auto segmentOf=[&](std::uint32_t packet)->const Segment&{
        std::size_t lo=0,hi=segs.size();
        while(hi-lo>1){const std::size_t mid=(lo+hi)/2;if(segs[mid].firstPacket<=packet)lo=mid;else hi=mid;}
        return segs[lo];
    };
    const auto segmentAt=[&](std::int64_t us)->const Segment&{
        const std::int64_t k=std::clamp<std::int64_t>((us-kStartUs)/kSegmentUs,0,static_cast<std::int64_t>(segs.size())-1);
        return segs[static_cast<std::size_t>(k)];
    };
    const auto linkUp=[&](std::int64_t us){const auto& s=segmentAt(us);return !(s.c.outage&&us>=s.windowStart&&us<s.windowEnd);};
    std::vector<std::int64_t> refusals;
    std::vector<std::int64_t> possible(static_cast<std::size_t>(totalFrames),-1);
    {std::vector<std::uint8_t> have(static_cast<std::size_t>(totalFrames),0);
     for(const auto& a:arrivals){const auto& s=segmentOf(a.packet);const auto f=static_cast<std::size_t>(s.firstFrame+(a.packet-s.firstPacket)/s.units);
         if(++have[f]==kSourceUnits)possible[f]=a.atUs;}}
    LongRunResult out;
    QueueLimits limits;limits.backlogFrames=o.backlogFrames;
    VideoIngress q(limits);Generation gen=1;q.begin(gen);(void)q.push(config(gen,0),0);
    ReorderWindowController controller(2000);
    auto queue=std::make_unique<ReorderQueueModel>(2000u);
    StreamRecovery recovery;if(o.patient)recovery.setMaxEpisodeRetries(StreamRecovery::kPatientRetries);
    recovery.beginAttempt(0);
    std::vector<ReorderQueueModel::Released> released;
    bool connected=true,anyReleased=true;std::uint32_t lastPacket=0;std::uint16_t lastSeq=0;
    std::int64_t current=-1;unsigned got=0;bool handed=false;std::uint64_t fecFailed=0,failedBase=0;
    std::int64_t idrFromSent=0,retryAt=-1,connectAt=-1,refusedAt=-1,now=0,decoderFree=0,lastPush=0,nextTick=kStartUs,nextLook=kStartUs;
    std::uint32_t firstFedPacket=0;bool needBase=false;std::int64_t lastHanded=0;
    struct Decoded {std::int64_t atUs,added;std::int64_t frame;};
    std::vector<Decoded> decoded;decoded.reserve(static_cast<std::size_t>(totalFrames));
    const auto ms=[](std::int64_t us){return us/1000;};
    const auto consume=[&](std::int64_t until){
        for(;;){
            const std::int64_t at=(std::max)(decoderFree,lastPush);
            if(at>until)break;
            auto v=q.tryPop(at*10);
            if(!v)break;
            // The full frame index from the 16-bit wire index: never more than
            // a few frames behind the newest one handed over.
            const std::int64_t f=lastHanded-static_cast<std::uint16_t>(static_cast<std::uint16_t>(lastHanded)-*v->sample.wireFrameIndex);
            const auto& s=segmentAt(at);
            const std::int64_t cost=s.c.slowDecodeUs>0&&at>=s.windowStart&&at<s.windowEnd?s.c.slowDecodeUs:s.c.decodeUs;
            decoderFree=at+cost;
            decoded.push_back({at,at-possible[static_cast<std::size_t>(f)],f});++out.decoded;
            recovery.frame(ms(at));
        }
    };
    const auto idrCheck=[&](std::int64_t at){if(q.takeIdrRequest(at*10)){++out.keyframeRequests;if(idrFromSent<0)idrFromSent=at+30000;}};
    const auto hand=[&](std::int64_t f,std::int64_t at){
        bool idr=false;
        if(idrFromSent>=0&&frameSentUs(f)>=idrFromSent){idr=true;idrFromSent=-1;}
        lastHanded=f;
        (void)q.push(frame(static_cast<std::uint16_t>(f),idr,at*10,gen),at*10);lastPush=at;
        idrCheck(at);consume(at);
    };
    const auto release=[&](const ReorderQueueModel::Released& x){
        if(needBase){needBase=false;lastPacket=firstFedPacket;lastSeq=static_cast<std::uint16_t>(firstFedPacket);}
        const std::uint32_t packet=anyReleased?lastPacket+static_cast<std::uint16_t>(x.seq-lastSeq):x.seq;
        anyReleased=true;lastPacket=packet;lastSeq=x.seq;
        const auto& s=segmentOf(packet);
        const std::int64_t f=s.firstFrame+(packet-s.firstPacket)/s.units;
        if(f<current)return;
        if(f>current){if(current>=0&&!handed)++fecFailed;current=f;got=0;handed=false;}
        if(++got>=kSourceUnits&&!handed){handed=true;hand(f,x.atUs);}
    };
    const auto drain=[&]{for(const auto& x:released)release(x);released.clear();};
    const auto connect=[&](std::int64_t at){
        connected=true;connectAt=-1;++gen;q.begin(gen);(void)q.push(config(gen,at*10),at*10);
        const auto windowBefore=controller.windowUs();
        controller.newConnection();
        // A reconnect is no evidence: the first decision of the new connection keeps the wait.
        ReorderEvidence fresh;fresh.reorderTimeoutUs=controller.windowUs();
        if(controller.update(at*10,fresh).windowUs>windowBefore||controller.windowUs()!=windowBefore)++out.widerAfterReconnect;
        queue=std::make_unique<ReorderQueueModel>(controller.windowUs());
        failedBase=fecFailed;current=-1;handed=false;got=0;needBase=true;anyReleased=true;firstFedPacket=0;
        idrFromSent=at;lastPush=at;decoderFree=at;recovery.beginAttempt(ms(at));
    };
    const auto tick=[&](std::int64_t at){
        if(o.adaptive&&connected&&at>=nextLook){
            nextLook=at+kLookUs;
            ReorderEvidence e=queue->counters();e.fecFailed=fecFailed-failedBase;
            const auto d=controller.update(at*10,e);
            if(d.changed)queue->setTimeoutUs(d.windowUs);
        }
        if(retryAt>=0&&at>=retryAt){
            retryAt=-1;
            if(linkUp(at)){
                const auto& s=segmentAt(at);
                const bool held=o.heldSessionUs>0&&s.c.outage&&at<s.windowEnd+o.heldSessionUs;
                if(held)refusedAt=at+300000; // connect() returned; the RP_IN_USE quit event follows
                else connectAt=at+500000;
            }
            else{
                const auto a=recovery.poll(ms(at+2000000),false,true); // the connect fails 2 s later
                if(a==A::Fail)throw std::runtime_error("recovery gave up");
                retryAt=at+2000000+static_cast<std::int64_t>(StreamRecovery::backoffSeconds(recovery.episodeRetries(),false))*1000000;
            }
        }
        if(connectAt>=0&&at>=connectAt)connect(at);
        if(refusedAt>=0&&at>=refusedAt){
            refusedAt=-1;++out.inUseRefusals;refusals.push_back(at);
            // The streaming loop's poll with Failed + startupRetryAllowed (RemotePlaySessionSource.cpp).
            const auto a=o.patient?recovery.pollRefusedInUse(ms(at),false,true,true,true):recovery.poll(ms(at),false,true,true,true);
            if(a!=A::Reconnect)throw std::runtime_error("recovery gave up on a held session");
            retryAt=at+static_cast<std::int64_t>(StreamRecovery::backoffSeconds(recovery.episodeRetries(),true))*1000000;
        }
        if(!connected)return;
        idrCheck(at);consume(at);
        const auto a=recovery.poll(ms(at),false,false);
        if(a==A::Keyframe)q.requireKeyframe();
        else if(a==A::Reconnect){
            ++out.reconnects;connected=false;q.close();
            retryAt=at+static_cast<std::int64_t>(StreamRecovery::backoffSeconds(recovery.episodeRetries(),false))*1000000;
        }else if(a==A::Fail)throw std::runtime_error("recovery failed");
    };
    const auto advance=[&](std::int64_t until){
        for(;;){
            std::optional<std::int64_t> wake=connected?queue->nextWakeUs(now):std::nullopt;
            const std::int64_t timerAt=wake?*wake:(std::numeric_limits<std::int64_t>::max)();
            if(nextTick<=timerAt&&nextTick<=until){tick(nextTick);nextTick+=50000;continue;}
            if(!wake||timerAt>until)break;
            queue->timer(timerAt,released);now=queue->nowUs();drain();
        }
    };
    for(const auto& a:arrivals){
        advance(a.atUs);
        if(!connected)continue;
        if(needBase&&firstFedPacket==0)firstFedPacket=a.packet;
        now=a.atUs;queue->packet(now,static_cast<std::uint16_t>(a.packet),released);drain();
    }
    advance(kStartUs+static_cast<std::int64_t>(o.segments)*kSegmentUs);
    // Per segment.
    std::size_t d=0;
    for(std::size_t k=0;k<segs.size();++k){
        const auto& s=segs[k];SegmentResult r;r.condition=s.c.name;r.windowEndUs=s.windowEnd;
        const std::int64_t segEnd=kStartUs+static_cast<std::int64_t>(k+1)*kSegmentUs;
        for(const auto t:refusals)r.refusedInUse=r.refusedInUse||(t>=segEnd-kSegmentUs&&t<segEnd);
        while(d<decoded.size()&&decoded[d].atUs<s.windowEnd)++d;
        r.resumeUs=-1;
        for(std::size_t i=d;i<decoded.size()&&decoded[i].atUs<segEnd;++i){
            if(r.resumeUs<0)r.resumeUs=decoded[i].atUs-s.windowEnd;
            if(decoded[i].added>1000)r.settleUs=decoded[i].atUs-s.windowEnd;
            if(decoded[i].atUs>=segEnd-10000000){++r.tailFrames;r.tailWorstUs=(std::max)(r.tailWorstUs,decoded[i].added);}
        }
        r.tailExpected=static_cast<std::size_t>(10000000/kFrameUs);
        out.segments.push_back(r);
    }
    out.windowAfterRunUs=controller.windowUs();
    // An hour of clean link afterwards, as the library would count it: video
    // flowing, nothing out of order. The wait must come back to the floor.
    ReorderEvidence e=queue->counters();e.fecFailed=fecFailed-failedBase;
    std::int64_t at=kStartUs+static_cast<std::int64_t>(o.segments)*kSegmentUs;
    for(int i=0;i<4*3600;++i){at+=kLookUs;e.videoPackets+=400;e.reorderTimeoutUs=controller.windowUs();(void)controller.update(at*10,e);}
    out.windowAfterCleanHourUs=controller.windowUs();
    return out;
}
inline void checkLongRun(const LongRunResult& r){
    CHECK(!r.segments.empty());
    std::int64_t firstTail=-1;
    for(const auto& s:r.segments){
        const bool outage=s.condition.rfind("interruption",0)==0;
        // A console that still holds the old session: one RP_IN_USE backoff (at most 40 s) more.
        const std::int64_t extra=s.refusedInUse?40500000:0;
        // The stream is back within 6 s of the link (reconnect after 6 s of
        // silence, backoff and connect included) and nothing the receiver adds
        // stays above 1 ms for more than 2 s after it is clean (5 s after an outage).
        if(s.resumeUs<0||s.resumeUs>6000000+extra)throw std::runtime_error(s.condition+": no frame decoded within 6 s of the impairment's end");
        if(s.settleUs>(outage?5000000+extra:2000000))throw std::runtime_error(s.condition+": added delay still above 1 ms "+std::to_string(s.settleUs/1000)+" ms after the impairment");
        // The last 10 s of every segment: every frame decoded, none added more than 1 ms.
        if(s.tailFrames+2<s.tailExpected)throw std::runtime_error(s.condition+": frames missing in the clean tail");
        if(s.tailWorstUs>1000)throw std::runtime_error(s.condition+": "+std::to_string(s.tailWorstUs)+" us added in the clean tail");
        if(firstTail<0)firstTail=s.tailWorstUs;
    }
    // No ratchet: the last clean tail adds no more than the first one (+0.1 ms).
    CHECK(r.segments.back().tailWorstUs<=firstTail+100);
    // And the reorder wait does not stay wide: back at 2 ms within the clean
    // last segments of the run (5 minutes), and after a clean hour.
    CHECK(r.windowAfterRunUs==2000);
    CHECK(r.windowAfterCleanHourUs==2000);
    CHECK(r.widerAfterReconnect==0);
}
} // namespace lan_study
// Four fixed seeds of 30 one-minute segments: two simulated hours in all.
TEST(no_accumulating_latency_adaptive_wait_original_queue){
    const auto r=lan_study::longRun({1,30,true,0,false});lan_study::checkLongRun(r);CHECK(r.decoded>90000);
}
TEST(no_accumulating_latency_adaptive_wait_backlog_queue){
    const auto r=lan_study::longRun({2,30,true,16,false});lan_study::checkLongRun(r);CHECK(r.decoded>90000);
}
TEST(no_accumulating_latency_fixed_2ms_backlog_queue){
    const auto r=lan_study::longRun({3,30,false,16,false});lan_study::checkLongRun(r);CHECK(r.windowAfterRunUs==2000);
}
TEST(no_accumulating_latency_adaptive_wait_patient_reconnect){
    const auto r=lan_study::longRun({4,30,true,0,true,4000000});lan_study::checkLongRun(r);CHECK(r.decoded>90000);
    CHECK(r.inUseRefusals>=1); // the RP_IN_USE path ran
}
// SessionInbox (real clock, so short): bursts released together drain to an
// empty queue every time and never leave a recovery or a deeper queue behind.
TEST(no_accumulating_latency_session_inbox_bursts_drain_every_time){
    SessionInbox inbox;auto token=inbox.begin(monotonic100ns());token.connected();
    const auto g=token.generation();
    CHECK(token.video(config(g,monotonic100ns())));
    std::uint16_t index=0;
    CHECK(token.video(frame(index++,true,monotonic100ns(),g)));
    while(inbox.tryVideo(monotonic100ns())){}
    for(int episode=0;episode<300;++episode){
        const int burst=1+episode%4; // up to four at once: the original queue's limit
        for(int i=0;i<burst;++i)CHECK(token.video(frame(index++,false,monotonic100ns(),g)));
        int popped=0;while(inbox.tryVideo(monotonic100ns()))++popped;
        CHECK(popped==burst);
        const auto s=inbox.snapshot();
        CHECK(s.video.depth==0);CHECK(s.video.recoveries==0);CHECK(s.video.highWaterDepth<=4);
    }
}

// --- Item 3c: the 60 s outage with VEYRA_EXP_RP_RECONNECT_PATIENT ---
// The owner loop's rule in virtual time, as the app sees it: while the link
// is down connect() fails (after 2 s) and RemotePlaySessionSource.cpp polls
// poll(now,false,true) (the backend's snapshot is reset then, so
// startupRetryAllowed is never set there). While the console still holds the
// old session, connect() SUCCEEDS (chiaki_session_start only starts the
// session thread) and the refusal arrives 300 ms later as the session failing
// with RP_IN_USE: automaticRetryAllowed and startupRetryAllowed set, polled in
// the streaming loop (pollRefusedInUse with the switch, poll without it).
namespace lan_study {
struct PatientOutcome {bool recovered=false,failed=false;unsigned attempts=0,inUseRefusals=0;std::int64_t resumedAt=-1;std::vector<unsigned> backoffs;};
inline PatientOutcome outageWithHeldSession(bool patient,std::int64_t downMs,std::int64_t heldMs){
    using A=StreamRecovery::Action;
    StreamRecovery r;if(patient)r.setMaxEpisodeRetries(StreamRecovery::kPatientRetries);
    constexpr std::int64_t down=10000;const std::int64_t up=down+downMs;
    const auto pollLoop=[&](std::int64_t t,bool failed,bool inUse){
        return patient?r.pollRefusedInUse(t,false,failed,true,inUse):r.poll(t,false,failed,true,inUse);
    };
    PatientOutcome o;std::int64_t t=0;r.beginAttempt(0);
    while(t<400000){
        if(t<down){r.frame(t);t+=16;continue;}
        const auto a=pollLoop(t,false,false);
        if(a==A::Fail){o.failed=true;return o;}
        if(a!=A::Reconnect){t+=10;continue;}
        bool inUse=false;
        for(;;){
            ++o.attempts;
            const unsigned wait=StreamRecovery::backoffSeconds(r.episodeRetries(),inUse);
            o.backoffs.push_back(wait);t+=static_cast<std::int64_t>(wait)*1000;r.beginAttempt(t);
            if(t<up){ // connect() fails: the connect-failed path
                t+=2000;inUse=false;
                if(r.poll(t,false,true)!=A::Reconnect){o.failed=true;return o;}
                continue;
            }
            if(t>=up+heldMs){t+=500;r.frame(t);o.recovered=true;o.resumedAt=t;return o;}
            // connect() returned true; the RP_IN_USE quit event follows.
            t+=300;inUse=true;++o.inUseRefusals;
            if(pollLoop(t,true,true)!=A::Reconnect){o.failed=true;return o;}
        }
    }
    return o;
}
} // namespace lan_study
TEST(recovery_patient_sixty_second_outage_with_the_console_holding_the_session){
    using namespace lan_study;
    const auto original=outageWithHeldSession(false,60000,20000);
    CHECK(original.failed&&!original.recovered&&original.attempts==3); // Start must be pressed again
    const auto patient=outageWithHeldSession(true,60000,20000);
    CHECK(patient.recovered&&!patient.failed&&patient.inUseRefusals>=1);
    CHECK(patient.attempts<=StreamRecovery::kPatientRetries+StreamRecovery::kPatientInUseRetries);
    // Back within one RP_IN_USE backoff (at most 40 s) and a connect of the console letting go.
    CHECK(patient.resumedAt-(10000+60000+20000)<=40000+500);
    for(const auto b:patient.backoffs)CHECK(b<=40u);
    // A console that lets go at once: back within one plain backoff (8 s) of the link.
    const auto quick=outageWithHeldSession(true,60000,0);
    CHECK(quick.recovered&&quick.inUseRefusals==0&&quick.resumedAt-70000<=8000+500+2000);
    // A console that never lets go: the patient rule still stops, in bounded time.
    const auto never=outageWithHeldSession(true,60000,100000000);
    CHECK(never.failed&&never.attempts==StreamRecovery::kPatientRetries+StreamRecovery::kPatientInUseRetries);
    // A short outage with a held session: the original rule's budget is enough.
    const auto shortOne=outageWithHeldSession(false,2000,15000);
    CHECK(shortOne.recovered);
}
TEST(recovery_refused_in_use_is_the_original_rule_without_the_patient_switch){
    using A=StreamRecovery::Action;
    // Not patient: pollRefusedInUse decides exactly as poll with the same arguments.
    for(const bool video:{false,true})for(const bool failed:{false,true})for(const bool inUse:{false,true})for(const bool allowed:{false,true}){
        StreamRecovery a,b;a.beginAttempt(0);b.beginAttempt(0);
        if(video){a.frame(10);b.frame(10);}
        for(int i=0;i<6;++i){
            const auto x=a.pollRefusedInUse(7000+i,false,failed,allowed,inUse),y=b.poll(7000+i,false,failed,allowed,inUse);
            CHECK(x==y);if(x==A::Fail)break;
        }
        CHECK(a.inUseRetries()==0&&a.reconnects()==b.reconnects());
    }
    // Patient, but never any video: no allowance; the startup rule decides as before.
    StreamRecovery p,q;p.setMaxEpisodeRetries(StreamRecovery::kPatientRetries);q.setMaxEpisodeRetries(StreamRecovery::kPatientRetries);
    p.beginAttempt(0);q.beginAttempt(0);
    CHECK(p.pollRefusedInUse(10,false,true,true,true)==q.poll(10,false,true,true,true));CHECK(p.inUseRetries()==0);
    // Patient with video: the refusal is counted apart, the attempts stay.
    p.frame(20);const auto before=p.episodeRetries();
    CHECK(p.pollRefusedInUse(30,false,true,true,true)==A::Reconnect);CHECK(p.inUseRetries()==1&&p.episodeRetries()==before);
}

// Review of sprint 3: one frame lost to the link every 8, 10 or 12 s (bursts
// of 5 or 30 packets) beside 0.1 % of packets 3-5 ms late that cost nothing.
// Earlier rules widened there and stayed at 6 ms (11 of 60 runs in the
// review). Now widening needs a lost frame whose own given-up packet arrived
// later, and a period without such losses narrows. 60 runs of 300 s, seeds
// apart from every other test: the wait ends at 2 ms, is wider for at most
// 5 % of the time, and never loses more frames than the fixed 2 ms.
TEST(reorder_window_unrelated_losses_beside_late_packets_do_not_hold_it_wide){
    using namespace lan_study;
    constexpr unsigned kFrames=17982; // 300 s
    for(const double spacing:{8.0,10.0,12.0})for(const double burst:{5.0,30.0})for(std::uint64_t seed=0;seed<10;++seed){
        Condition c;c.name="sparse_loss_with_late_packets";c.fromSeconds=0;c.seconds=300;
        c.geEnter=1.0/(spacing*1620.0);c.geMeanBurst=burst;c.reorderShare=0.001;c.reorderMinUs=3000;c.reorderMaxUs=5000;
        const auto trace=makeTrace(c,424242u+static_cast<std::uint64_t>(spacing)*100u+static_cast<std::uint64_t>(burst)*1000u+seed,kFrames);
        const auto fixed=runPolicy(c,trace,{2000,false}),adaptive=runPolicy(c,trace,{2000,true});
        CHECK(adaptive.windows.back().windowUs==2000);
        std::int64_t wide=0;
        for(std::size_t i=0;i+1<adaptive.windows.size();++i)if(adaptive.windows[i].windowUs>2000)wide+=adaptive.windows[i+1].atUs-adaptive.windows[i].atUs;
        CHECK(wide*20<=static_cast<std::int64_t>(kFrames)*kFrameUs);
        CHECK(lost(adaptive)<=lost(fixed));
    }
}

// Final review of sprint 3: 12 s between lost frames, bursts of 5, 1800 s,
// 20 seeds. Two genuine late-packet losses can widen the wait and a pay check
// can pass by chance on rare losses; the paid step must still give way after
// three quiet periods. Every run ends at 2 ms and is wide at most 5 % of the time.
TEST(reorder_window_a_paid_widening_expires_when_no_longer_needed){
    using namespace lan_study;
    constexpr unsigned kFrames=107893; // 1800 s
    for(std::uint64_t seed=0;seed<20;++seed){
        Condition c;c.name="sparse_loss_with_late_packets_long";c.fromSeconds=0;c.seconds=1800;
        c.geEnter=1.0/(12.0*1620.0);c.geMeanBurst=5;c.reorderShare=0.001;c.reorderMinUs=3000;c.reorderMaxUs=5000;
        const auto trace=makeTrace(c,777000u+seed,kFrames);
        const auto adaptive=runPolicy(c,trace,{2000,true});
        CHECK(adaptive.windows.back().windowUs==2000);
        std::int64_t wide=0;
        for(std::size_t i=0;i+1<adaptive.windows.size();++i)if(adaptive.windows[i].windowUs>2000)wide+=adaptive.windows[i+1].atUs-adaptive.windows[i].atUs;
        CHECK(wide*20<=static_cast<std::int64_t>(kFrames)*kFrameUs);
    }
}

// --- Sprint 4: Wi-Fi-like conditions (Linux model) ---
TEST(study_wifi_conditions_bottleneck_classes){
    // A stall followed by a clump is network jitter, never a decoder backlog;
    // with the route on Wi-Fi it is named wifi-route with jitter among the signs.
    using namespace lan_study;
    const std::map<std::string,std::set<std::string>> allowed{
        {"wifi_stall_clumps",{"jitter","none"}},{"wifi_retransmission_reorder",{"reordering","loss"}},
        {"wifi_rate_dips",{"loss","jitter","none"}},{"wifi_mixed",{"loss","reordering","jitter"}},
        {"wifi_retx_short",{"reordering","loss","none"}},{"wifi_retx_in_order",{"jitter","loss","none"}},{"wifi_retx_rare",{"reordering","loss","none"}}};
    const std::map<std::string,std::string> mostly{{"wifi_stall_clumps","jitter"},{"wifi_retransmission_reorder","reordering"},{"wifi_rate_dips","loss"}};
    const LocalInterface wifi[]={{*parseIpv4("192.168.1.10"),24,LinkKind::WiFi,286000000u}};
    const auto route=assessRoute("192.168.1.20",wifi);
    const auto all=wifiConditions();
    CHECK(all.size()==7);
    for(std::size_t i=0;i<all.size();++i){
        const auto& c=all[i];
        const auto trace=makeTrace(c,77000u+static_cast<unsigned>(i),kStudyFrames);
        for(const bool adaptive:{false,true}){
            const auto r=runPolicy(c,trace,{2000,adaptive});
            std::map<std::string,unsigned> named;
            for(auto h:r.intervals){
                const std::int64_t end=kStartUs+static_cast<std::int64_t>(h.sequence)*kIntervalUs,begin=end-kIntervalUs;
                const std::string name=bottleneckName(h.bottleneck);
                if(end>c.windowStartUs()&&begin<c.windowEndUs()){
                    ++named[name];
                    if(!allowed.at(c.name).count(name))throw std::runtime_error(std::string(c.name)+": impaired interval named "+name);
                    CHECK((h.bottleneckSigns&bottleneckBit(Bottleneck::DecoderBacklog))==0);
                    // The same interval seen from a PC on Wi-Fi.
                    h.route=route;h.bottleneckSigns=bottleneckSigns(h);h.bottleneck=classifyBottleneck(h.bottleneckSigns);
                    CHECK(h.bottleneck==(name=="none"?Bottleneck::None:Bottleneck::WifiRoute));
                    if(name!="none")CHECK((h.bottleneckSigns&bottleneckBit(Bottleneck::WifiRoute))!=0);
                    if(std::string(c.name)=="wifi_stall_clumps"&&name!="none")CHECK((h.bottleneckSigns&bottleneckBit(Bottleneck::Jitter))!=0);
                }else if(begin>=c.windowEndUs()+10*1000000){
                    if(name!="none"||h.intervalGrade!=HealthGrade::Stable)throw std::runtime_error(std::string(c.name)+": clean interval named "+name);
                }
            }
            const auto want=mostly.find(c.name);
            if(want!=mostly.end()){
                unsigned top=0;std::string topName;
                for(const auto& [name,count]:named)if(count>top){top=count;topName=name;}
                if(topName!=want->second)throw std::runtime_error(std::string(c.name)+": mostly "+topName);
            }
        }
    }
}
TEST(study_wifi_retries_widen_the_adaptive_wait_from_library_evidence){
    // The evidence the app now gets (LateFrameEvidenceModel, as patch 0004
    // counts it) is enough for the rule to widen where link-layer retries
    // arrive after their successors, to lose fewer frames than the fixed 2 ms,
    // and to come back to 2 ms once the link is clean (within the 270 s after
    // the impairment; a paid widening is kept for 3 quiet periods first).
    using namespace lan_study;
    const auto all=wifiConditions();
    const auto& c=all[1];CHECK(std::string(c.name)=="wifi_retransmission_reorder");
    for(unsigned seed=1;seed<=3;++seed){
        const auto trace=makeTrace(c,78000u+seed,3*kStudyFrames);
        const auto two=runPolicy(c,trace,{2000,false}),adaptive=runPolicy(c,trace,{2000,true});
        CHECK(adaptive.lateFramesFailed>0&&adaptive.lateFramesFailed<=adaptive.fecFailed);
        std::uint32_t widest=0;for(const auto& w:adaptive.windows)widest=(std::max)(widest,w.windowUs);
        CHECK(widest>2000);
        CHECK(lost(adaptive)<lost(two));
        CHECK(adaptive.windows.back().windowUs==2000);
        if(seed==1)std::cout<<"[wifi-retries] adaptive wait back at 2 ms "<<(adaptive.windows.back().atUs-c.windowEndUs())/1000000<<" s after the impairment\n";
    }
    // A stall followed by a clump puts nothing out of order: the wait never moves.
    const auto& stall=all[0];
    const auto r=runPolicy(stall,makeTrace(stall,78100u,kStudyFrames),{2000,true});
    CHECK(r.windows.size()==1&&r.lateFramesFailed==0);
}
