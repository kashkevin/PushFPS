// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// LINUX MODEL for SmartLink: a deterministic, seeded network + receive
// pipeline in virtual time, for tools/smartlink_sim and the unit tests only;
// the application never uses it. Portable and header-only.
//
// What it models:
//   - a 60 fps stream (ReorderStudy.h's stream: 24 source + 3 FEC packets per
//     frame, sent over 2 ms, frame interval 16.683 ms), through a FIFO path whose
//     delay can wander (round-trip variation), stall (Wi-Fi droughts: everything
//     sent during the stall arrives at its end, order kept), pass a bottleneck
//     (queue growth then drain, drop-tail), lose packets (random, Gilbert-Elliott
//     bursts, outages) and displace some packets (reordering);
//   - the library's video reorder queue, exactly as ReorderQueueModel.h restates
//     it, with the wait each policy chooses;
//   - frame completion (24 distinct packets of 27), hand-over time, given-up
//     frames, keyframe requests (one per 500 ms at most, as PushFPS spaces them);
//   - an audio playout reserve (10 ms frames on the same path; underrun when a
//     frame is not there when due; >= 30 ms starvation re-anchors; the
//     "today" rule lets excess delay decay at 5 ms/s, the fast rule cuts it at
//     once; growing the reserve stretches at <= 5 ms/s), written from
//     docs/AUDIO.md findings 2, 4 and 6. AudioPathModel.h is the complete audio
//     model, but it has no input for an external reserve target and this
//     stream may not change it, so SmartLink's audio decision is judged in this
//     simpler model;
//   - the console's reports: round trip once a second (2 x the path's one-way
//     delay plus a base), its bitrate target (lowered after a reaction time when
//     the link's capacity drops).
// What it does NOT model: the decoder and the presentation path, the console's
// encoder, Wi-Fi itself (only its effect on arrival times), keyframe sizes,
// anything measured on a PS5. Every number it produces is a model result.
#include "ReorderQueueModel.h"
#include "ReorderStudy.h"
#include "ReorderWindow.h"
#include "SmartLinkController.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
namespace veyra::remoteplay::smartlink::sim {
namespace st=veyra::remoteplay::study;
inline constexpr std::int64_t kFrameUs=st::kFrameUs;            // 16683
inline constexpr unsigned kSource=st::kSourceUnits;               // 24
inline constexpr std::int64_t kWindowUs=200000;                   // one Sample
inline constexpr double kRequestedKbps=15000;                     // BENCH1: 15 Mbit/s requested
inline constexpr double kAudioFloorMs=20;                         // docs/AUDIO.md finding 6

struct Outage {double atS=0,lenS=0;};
struct Scenario {
    const char* name="";
    const char* description="";
    Route route=Route::Wired;
    double seconds=240,fromS=30,untilS=120; // the disturbance window
    unsigned fec=3;
    double rttBaseMs=3;                     // console round trip on an idle path
    // Round-trip variation: the one-way path delay wanders (sine of this period, plus steps).
    double wanderMs=0,wanderPeriodS=20,stepEveryS=0,stepMaxMs=0;
    // Wi-Fi droughts: random (exponential gaps, mean), or uniform gaps in [min,max] s, or periodic.
    double stallMeanGapS=0,stallGapMinS=0,stallGapMaxS=0,stallMinMs=0,stallMaxMs=0;
    double periodS=0,periodJitterS=0;
    // Out-of-order packets.
    double reorderShare=0,reorderMinMs=0,reorderMaxMs=0;
    // Loss.
    double randomLoss=0,geEnter=0,geBurst=0;
    // Bottleneck: capacity as a share of the stream's packet rate, buffer, active `congestLenS` every `congestEveryS`.
    double capacityShare=0,bufferMs=0,congestEveryS=0,congestLenS=0;
    double consoleReactS=-1;                // >= 0: the console lowers its target to 75 % after this long under the bottleneck
    std::array<Outage,4> outages{};
    double signalStart=-1,signalLow=-1;     // Wi-Fi signal %: start, and the value at the end of the window (linear)
    double baseJitterUs=50;                 // exponential per-packet queueing noise (ReorderStudy: 50 us)
};

// The scenarios of the directive (section 8), plus wired references.
inline std::vector<Scenario> scenarios(){
    std::vector<Scenario> v;Scenario x;
    x={};x.name="wired_clean";x.description="wired, no impairment";v.push_back(x);
    x={};x.name="wired_rare_loss";x.description="wired, 0.02 % of packets lost at random for 90 s";x.randomLoss=0.0002;v.push_back(x);
    x={};x.name="rtt_variation";x.route=Route::Wifi;x.description="90 s: one-way delay wanders 0-48 ms (round trip ~3-100 ms), period 20 s, plus a 0-20 ms step every ~5 s";
        x.wanderMs=48;x.wanderPeriodS=20;x.stepEveryS=5;x.stepMaxMs=20;x.signalStart=75;x.signalLow=70;v.push_back(x);
    x={};x.name="wifi_random_spikes";x.route=Route::Wifi;x.description="90 s: droughts of 20-80 ms at random, mean gap 5 s; 0.3 % of packets 1-6 ms late";
        x.stallMeanGapS=5;x.stallMinMs=20;x.stallMaxMs=80;x.reorderShare=0.003;x.reorderMinMs=1;x.reorderMaxMs=6;x.signalStart=70;x.signalLow=65;v.push_back(x);
    x={};x.name="wifi_periodic_spikes";x.route=Route::Wifi;x.description="90 s: droughts of 30-70 ms every 10 s (+-0.3 s), as background scans do";
        x.periodS=10;x.periodJitterS=0.3;x.stallMinMs=30;x.stallMaxMs=70;x.signalStart=70;x.signalLow=70;v.push_back(x);
    x={};x.name="governor_case";x.route=Route::Wifi;x.description="90 s: stalls of 20-80 ms every 2-8 s (uniform), mean jitter under 3 ms (the governor review's case)";
        x.stallGapMinS=2;x.stallGapMaxS=8;x.stallMinMs=20;x.stallMaxMs=80;x.signalStart=70;x.signalLow=70;v.push_back(x);
    x={};x.name="burst_loss";x.route=Route::Wifi;x.description="90 s: Gilbert-Elliott loss bursts, mean 5 packets, one every ~2.5 s; 3 FEC packets per frame";
        x.geEnter=1.0/4000;x.geBurst=5;x.signalStart=60;x.signalLow=60;v.push_back(x);
    x={};x.name="reordering";x.route=Route::Wifi;x.description="90 s: 5 % of packets 1-12 ms late";
        x.reorderShare=0.05;x.reorderMinMs=1;x.reorderMaxMs=12;x.signalStart=70;x.signalLow=70;v.push_back(x);
    x={};x.name="congestion";x.route=Route::Wifi;x.description="90 s: every 20 s a 3 s bottleneck at 95 % of the stream's rate, 100 ms buffer (queue grows, then drains)";
        x.capacityShare=0.95;x.bufferMs=100;x.congestEveryS=20;x.congestLenS=3;x.signalStart=70;x.signalLow=70;v.push_back(x);
    x={};x.name="bandwidth_drop";x.route=Route::Wifi;x.description="90 s: capacity 85 % of the stream (30 ms buffer); the console lowers its target to 75 % after 2 s";
        x.capacityShare=0.85;x.bufferMs=30;x.congestEveryS=90;x.congestLenS=90;x.consoleReactS=2;x.signalStart=70;x.signalLow=70;v.push_back(x);
    x={};x.name="interruptions";x.route=Route::Wifi;x.description="outages of 0.2, 1 and 5 s at 40, 70 and 100 s";
        x.outages={Outage{10,0.2},Outage{40,1},Outage{70,5},Outage{}};x.signalStart=70;x.signalLow=70;v.push_back(x);
    x={};x.name="weak_signal";x.route=Route::Wifi;x.description="90 s: signal falls 70 -> 25 %; droughts of 20-60 ms (mean gap 3 s) and 0.5 % random loss";
        x.stallMeanGapS=3;x.stallMinMs=20;x.stallMaxMs=60;x.randomLoss=0.005;x.signalStart=70;x.signalLow=25;v.push_back(x);
    // Added after the independent review (its variant where SmartLink did worst).
    x={};x.name="reordering_loss";x.route=Route::Wifi;x.description="90 s: 2 % of packets 1-12 ms late and 0.2 % lost at random";
        x.reorderShare=0.02;x.reorderMinMs=1;x.reorderMaxMs=12;x.randomLoss=0.002;x.signalStart=70;x.signalLow=70;v.push_back(x);
    return v;
}

enum class PolicyKind : std::uint8_t {Original,Rc4Default,Rc4Switches,SmartLink,SmartLinkLan,SmartLinkLearned,EarlyLoss,EarlyLoss16};
struct PolicySpec {const char* name;PolicyKind kind;const char* description;};
inline constexpr std::array<PolicySpec,8> kPolicies{{
    {"original_pushfps",PolicyKind::Original,"fixed 16 ms reorder wait (library default, TransportTuning::kLibraryReorderUs); audio 'today' rule, 20 ms reserve"},
    {"slipstream_rc4",PolicyKind::Rc4Default,"exp/rc4 with every switch unset: the same 16 ms wait and audio rule (nothing differs on this path in the model)"},
    {"slipstream_rc4_switches",PolicyKind::Rc4Switches,"VEYRA_EXP_RP_LAN_PROFILE=1 (2 ms) + VEYRA_EXP_RP_REORDER_ADAPTIVE=1 (ReorderWindow.h, with the model's per-frame evidence) + VEYRA_EXP_RP_AUDIO_FAST_RECOVER=1"},
    {"smartlink_apply",PolicyKind::SmartLink,"SmartLink apply on top of slipstream_rc4 (configured wait 16 ms), audio fast rule with SmartLink's target"},
    {"smartlink_apply_lan",PolicyKind::SmartLinkLan,"SmartLink apply with the LAN profile (configured wait 2 ms), audio fast rule with SmartLink's target"},
    {"smartlink_apply_learned",PolicyKind::SmartLinkLearned,"as smartlink_apply_lan, stall risk from the learned comparison model"},
    {"early_loss_lan",PolicyKind::EarlyLoss,"LAN profile 2 ms + 'early loss': a frame missing more units than its FEC units is given up at once while no out-of-order packet was seen for 300 s (Moonlight's speculative RFI idea, research doc C)"},
    {"early_loss_16",PolicyKind::EarlyLoss16,"the library's 16 ms wait + the same 'early loss' rule"},
}};

struct Metrics {
    std::uint64_t frames=0,delivered=0,givenUp=0,linkLost=0,recoveryRequests=0,stutters=0,settingChanges=0;
    double minutes=0;
    std::vector<float> added;            // per delivered frame: hand-over minus "complete at the socket", ms
    double addedP50=0,addedP95=0,addedP99=0;
    std::uint64_t audioGaps=0;double audioMeanMs=0,audioP95Ms=0;
    std::array<double,5> secondsAtWait{}; // 2, 4, 8, 16 ms, other
    double giveUpP95Ms=0;                 // time from a frame's last packet due to the moment it was given up
    std::vector<float> giveUp;
    // Return to "leave" after the disturbance (SmartLink policies): seconds after untilS until every output is leave (-1 never).
    double leaveAfterS=-1;
    double maxChangesPerMinute=0;
};
// Per window, for the predictor study (labels need the future, so they are filled after the run).
struct WindowRecord {
    Sample sample;Features features{};double riskStat=0,riskLearned=0;double excess=0;bool label=false;
};
struct RunOptions {
    bool recordWindows=false;
    std::uint64_t seed=1;
    // SmartLink's decisions reach the library and the sink this late (the library
    // applies a new reorder wait at its next look, every 250 ms; the session thread
    // hands the decision over first). Applied at the first look or window boundary
    // at or after decision + lag: 250 ms = between one and two 200 ms windows.
    // 0 = at once (the model before the review, which hid blocker 1).
    std::int64_t applyLagUs=kDefaultApplyLagUs;
    static constexpr std::int64_t kDefaultApplyLagUs=250000;
};
struct RunOutput {Metrics metrics;std::vector<WindowRecord> windows;std::vector<Decision> decisions;};

namespace detail {
inline double percentile(std::vector<float>& v,double p){
    if(v.empty())return 0;
    const auto k=static_cast<std::size_t>(std::min<double>(static_cast<double>(v.size()-1),std::floor(p*static_cast<double>(v.size()))));
    std::nth_element(v.begin(),v.begin()+static_cast<std::ptrdiff_t>(k),v.end());
    return static_cast<double>(v[k]);
}
}

// The trace of one scenario: video packet arrivals, link losses and the path
// delay per frame (for audio).
struct Path {
    st::Trace trace;
    std::vector<float> frameDelayMs;  // one-way extra delay of each frame's first packet; < 0: lost
    std::vector<std::uint8_t> bottleneck; // per frame: sent while the bottleneck was active
};
inline Path makePath(const Scenario& c,std::uint64_t seed){
    st::Rng rng(seed*7919+13);
    Path p;
    const unsigned units=kSource+c.fec;
    const auto frames=static_cast<unsigned>(c.seconds*1e6/static_cast<double>(kFrameUs));
    p.trace.frames=frames;p.trace.arrivals.reserve(static_cast<std::size_t>(frames)*units);
    p.frameDelayMs.assign(frames,0);p.bottleneck.assign(frames,0);
    const std::int64_t from=st::kStartUs+static_cast<std::int64_t>(c.fromS*1e6),until=st::kStartUs+static_cast<std::int64_t>(c.untilS*1e6);
    // Stall schedule.
    std::vector<std::pair<std::int64_t,std::int64_t>> stalls; // [start, end)
    if(c.stallMeanGapS>0||c.stallGapMaxS>0||c.periodS>0){
        double t=c.fromS;
        if(c.periodS>0)t+=rng.between(0,c.periodS);
        while(true){
            if(c.periodS<=0)t+=c.stallMeanGapS>0?rng.exponential(c.stallMeanGapS):rng.between(c.stallGapMinS,c.stallGapMaxS);
            if(t>=c.untilS)break;
            const double at=c.periodS>0?t+rng.between(-c.periodJitterS,c.periodJitterS):t;
            const auto start=st::kStartUs+static_cast<std::int64_t>(at*1e6);
            stalls.push_back({start,start+static_cast<std::int64_t>(rng.between(c.stallMinMs,c.stallMaxMs)*1000)});
            if(c.periodS>0)t+=c.periodS;
        }
    }
    std::size_t nextStall=0;
    double step=0;std::int64_t nextStep=from;
    std::int64_t lastFifo=0;bool bad=false;
    const double streamPps=static_cast<double>(units)*1e6/static_cast<double>(kFrameUs);
    for(unsigned f=0;f<frames;++f){
        const std::int64_t frameSent=st::frameSentUs(f);
        const bool on=frameSent>=from&&frameSent<until;
        // Bottleneck active?
        bool congest=false;
        if(on&&c.capacityShare>0&&c.congestEveryS>0){
            const double into=static_cast<double>(frameSent-from)/1e6;
            congest=std::fmod(into,c.congestEveryS)<c.congestLenS;
            // The console reacts: after consoleReactS under the bottleneck it lowers its rate, which removes the bottleneck.
            if(congest&&c.consoleReactS>=0&&std::fmod(into,c.congestEveryS)>=c.consoleReactS)congest=false;
        }
        p.bottleneck[f]=static_cast<std::uint8_t>(congest?1:(on&&c.consoleReactS>=0&&c.capacityShare>0)?2:0);
        if(on&&c.stepEveryS>0&&frameSent>=nextStep){step=rng.between(0,c.stepMaxMs);nextStep=frameSent+static_cast<std::int64_t>(rng.exponential(c.stepEveryS)*1e6);}
        if(!on)step=0;
        const double wander=on&&c.wanderMs>0?c.wanderMs*0.5*(1-std::cos(2*3.141592653589793*static_cast<double>(frameSent-from)/1e6/c.wanderPeriodS)):0.0;
        const auto extraUs=static_cast<std::int64_t>((wander+step)*1000);
        p.frameDelayMs[f]=-1;
        for(unsigned u=0;u<units;++u){
            const std::uint32_t packet=f*units+u;
            const std::int64_t sent=frameSent+static_cast<std::int64_t>(u)*st::kSendSpreadUs/static_cast<std::int64_t>(units);
            bool outage=false;
            for(const auto& o:c.outages)if(o.lenS>0&&sent>=st::kStartUs+static_cast<std::int64_t>((c.fromS+o.atS)*1e6)&&sent<st::kStartUs+static_cast<std::int64_t>((c.fromS+o.atS+o.lenS)*1e6))outage=true;
            if(outage){p.trace.lostSentUs.push_back(sent);continue;}
            if(on&&c.geEnter>0){
                if(bad){if(rng.chance(1.0/(std::max)(1.0,c.geBurst)))bad=false;}
                else if(rng.chance(c.geEnter))bad=true;
                if(bad){p.trace.lostSentUs.push_back(sent);continue;}
            }
            if(on&&rng.chance(c.randomLoss)){p.trace.lostSentUs.push_back(sent);continue;}
            const double jitter=(std::min)(rng.exponential(c.baseJitterUs),1000.0);
            const std::int64_t ready=sent+st::kBaseDelayUs+static_cast<std::int64_t>(jitter)+extraUs;
            const std::int64_t service=congest?static_cast<std::int64_t>(1e6/(c.capacityShare*streamPps)):10;
            std::int64_t at=(std::max)(lastFifo+service,ready);
            while(nextStall<stalls.size()&&stalls[nextStall].second<=sent)++nextStall;
            if(nextStall<stalls.size()&&sent>=stalls[nextStall].first)at=(std::max)(at,stalls[nextStall].second+service);
            if(congest&&c.bufferMs>0&&at-ready>static_cast<std::int64_t>(c.bufferMs*1000)){p.trace.lostSentUs.push_back(sent);continue;}
            lastFifo=at;
            if(u==0)p.frameDelayMs[f]=static_cast<float>(static_cast<double>(at-sent-st::kBaseDelayUs)/1000.0);
            if(on&&rng.chance(c.reorderShare))at+=static_cast<std::int64_t>(rng.between(c.reorderMinMs,c.reorderMaxMs)*1000);
            p.trace.arrivals.push_back({at,packet});
        }
    }
    std::stable_sort(p.trace.arrivals.begin(),p.trace.arrivals.end(),[](const st::Arrival& a,const st::Arrival& b){return a.atUs<b.atUs;});
    std::sort(p.trace.lostSentUs.begin(),p.trace.lostSentUs.end());
    return p;
}

// Audio reserve model (see the file header). One 10 ms frame at a time.
// Delays are beyond the path's base delay. The reserve is measured from the
// lower envelope of the recent arrivals (2 s, as the session's ingress
// mapping in docs/AUDIO.md), so a slow change of the path delay moves the
// playout with it; only what arrives later than the reserve allows is late.
class AudioModel {
public:
    AudioModel(bool fast,double floorMs)noexcept:fast_(fast),floor_(floorMs),d_(floorMs),target_(floorMs){}
    void setTarget(double ms)noexcept{target_=(std::max)(floor_,ms);}
    // extraMs: when this frame arrives, beyond its ideal arrival (lost frames:
    // when the next received one arrives, as Chiaki conceals and delivers them then).
    // A frame that arrives (concealed) more than kIngressMs behind its time is
    // discarded (AudioIngress keeps the newest 100 ms): the playout starves
    // (one gap) and restarts with a fresh reserve at the next frame.
    static constexpr double kIngressMs=100;
    void dropped()noexcept{if(!inGap_){++gaps_;inGap_=true;++underrunsWindow_;}starving_=true;}
    void frame(double extraMs)noexcept{
        constexpr double kStepMs=10,kSlewMsPerS=5;
        if(starving_){starving_=false;d_=extraMs+target_;}
        // 2 s lower envelope (200 frames), monotone queue.
        ++index_;
        while(count_>0&&env_[(head_+count_-1)%kEnv].value>=extraMs)--count_;
        env_[(head_+count_)%kEnv]={index_,extraMs};++count_;
        while(env_[head_].index+kEnv<=index_){head_=(head_+1)%kEnv;--count_;}
        const double envelope=env_[head_].value;
        const double want=envelope+target_;
        const double slew=kSlewMsPerS*kStepMs/1000.0;
        if(d_>want){
            if(fast_)d_=want;
            else if(d_-want>60){d_=want;if(!inGap_)++gaps_;inGap_=true;} // hard reset above 60 ms (finding 3)
            else d_=(std::max)(want,d_-slew);
        }else if(d_<want)d_=(std::min)(want,d_+slew);
        // Lateness below kAbsorbMs is absorbed by the endpoint's own buffered
        // period (shared-mode WASAPI periods are 10 ms; 2 ms is conservative):
        // playback slides later without an audible gap.
        constexpr double kAbsorbMs=2;
        if(extraMs>d_&&extraMs<=d_+kAbsorbMs){d_=extraMs;inGap_=false;}
        else if(extraMs>d_){
            const double gap=extraMs-d_;
            if(!inGap_){++gaps_;inGap_=true;}
            d_=gap>=30?extraMs+target_:extraMs; // starvation re-anchors with a fresh reserve (finding 4)
            ++underrunsWindow_;
        }else inGap_=false;
        sum_+=d_-envelope;++count2_;
        if(count2_%4==0)hist_.push_back(static_cast<float>(d_-envelope));
    }
    double reserveMs()const noexcept{return count_?d_-env_[head_].value:d_;}
    std::uint64_t gaps()const noexcept{return gaps_;}
    std::int64_t takeUnderruns()noexcept{const auto u=underrunsWindow_;underrunsWindow_=0;return u;}
    double mean()const noexcept{return count2_?sum_/static_cast<double>(count2_):0;}
    std::vector<float>& samples()noexcept{return hist_;}
private:
    static constexpr std::size_t kEnv=200;
    struct Entry {std::uint64_t index=0;double value=0;};
    bool fast_;double floor_,d_,target_,sum_=0;
    std::array<Entry,kEnv> env_{};std::size_t head_=0,count_=0;std::uint64_t index_=0,count2_=0;
    std::uint64_t gaps_=0;std::int64_t underrunsWindow_=0;bool inGap_=false,starving_=false;
    std::vector<float> hist_;
};

// Runs one policy over one path.
inline RunOutput run(const Scenario& c,const Path& path,PolicyKind policy,const RunOptions& options){
    RunOutput out;Metrics& m=out.metrics;
    const unsigned units=kSource+c.fec,frames=path.trace.frames;
    const bool lan=policy==PolicyKind::Rc4Switches||policy==PolicyKind::SmartLinkLan||policy==PolicyKind::SmartLinkLearned||policy==PolicyKind::EarlyLoss;
    const bool smart=policy==PolicyKind::SmartLink||policy==PolicyKind::SmartLinkLan||policy==PolicyKind::SmartLinkLearned;
    const std::uint32_t startUs=lan?2000u:16000u;
    ReorderQueueModel queue(startUs);
    ReorderWindowController adaptive(startUs);
    Config cfg;cfg.configuredReorderMs=startUs/1000.0;cfg.audioFloorMs=kAudioFloorMs;
    cfg.model=policy==PolicyKind::SmartLinkLearned?ModelKind::Learned:ModelKind::Statistics;
    Engine engine(cfg);
    Predictor learnedShadow(ModelKind::Learned); // only for the predictor study (recordWindows)
    AudioModel audio(policy==PolicyKind::Rc4Switches||smart,kAudioFloorMs);
    std::uint32_t waitUs=startUs;

    // Per frame.
    std::vector<std::int64_t> possible(frames,-1),hand(frames,-1);
    std::vector<std::uint8_t> have(frames,0),srcHave(frames,0),flags(frames,0);
    std::vector<float> rescue(frames,0);
    {
        std::vector<std::uint8_t> seen(static_cast<std::size_t>(frames)*units,0);
        for(const auto& a:path.trace.arrivals){
            if(seen[a.packet])continue;
            seen[a.packet]=1;
            const unsigned f=a.packet/units;
            if(++have[f]==kSource)possible[f]=a.atUs;
        }
        std::fill(have.begin(),have.end(),0);
    }
    std::vector<std::int64_t> skippedAt(static_cast<std::size_t>(frames)*units,-1);
    std::vector<std::uint8_t> arrived(static_cast<std::size_t>(frames)*units,0);
    std::vector<ReorderQueueModel::Released> rel;
    std::int64_t current=-1;unsigned got=0;bool handed=false;
    std::uint32_t lastPacket=0;bool anyReleased=false;std::uint16_t lastSeq=0;
    std::int64_t lastRequest=-1000000000,lastHandUs=-1;
    // Window accumulators.
    std::int64_t wReceived=0,wHanded=0,wFec=0,wFailed=0;double wGapMax=0,wPktGapSum=0,wPktGapMax=0,wLateSum=0;std::int64_t wPktGaps=0,lastArrival=-1;
    double wRescue=0;
    std::uint64_t failedTotal=0,lateFailedTotal=0;
    std::int64_t lastOutOfOrder=-1000000000000;
    std::uint32_t highestSeen=0;bool anySeen=false;
    ReorderQueueModel::Counters before{};
    std::size_t lostIndex=0;
    const auto flag=[&](std::int64_t f,std::uint8_t bit,std::int64_t at){
        if(f<0||f>=static_cast<std::int64_t>(frames))return;
        auto& v=flags[static_cast<std::size_t>(f)];
        if(v&bit)return;
        v=static_cast<std::uint8_t>(v|bit);
        if(v==3u&&possible[static_cast<std::size_t>(f)]>=0){
            ++lateFailedTotal;
            wRescue=(std::max)(wRescue,static_cast<double>(rescue[static_cast<std::size_t>(f)]));
            (void)at;
        }
    };
    const auto giveUp=[&](std::int64_t f,std::int64_t at){
        ++failedTotal;++wFailed;flag(f,1u,at);
        const double due=static_cast<double>(st::frameSentUs(f)+st::kSendSpreadUs+st::kBaseDelayUs);
        m.giveUp.push_back(static_cast<float>((static_cast<double>(at)-due)/1000.0));
        if(at-lastRequest>=st::kRequestSpacingUs){++m.recoveryRequests;lastRequest=at;}
    };
    const auto release=[&](const ReorderQueueModel::Released& x){
        const std::uint32_t packet=anyReleased?lastPacket+static_cast<std::uint16_t>(x.seq-lastSeq):static_cast<std::uint32_t>(x.seq);
        // Packets between the last released one and this one were given up now.
        if(anyReleased)for(std::uint32_t q=lastPacket+1;q<packet&&q<frames*units;++q)if(!arrived[q])skippedAt[q]=x.atUs;
        anyReleased=true;lastPacket=packet;lastSeq=x.seq;
        const std::int64_t f=packet/units;
        if(f>=static_cast<std::int64_t>(frames)||f<current)return;
        if(f>current){
            if(current>=0&&!handed)giveUp(current,x.atUs);
            for(std::int64_t g=current+1;g<f;++g)if(g>=0)giveUp(g,x.atUs); // frames skipped whole
            current=f;got=0;handed=false;
        }
        if(packet%units<kSource)++srcHave[static_cast<std::size_t>(f)];
        if(++got>=kSource&&!handed){
            handed=true;hand[static_cast<std::size_t>(f)]=x.atUs;++wHanded;
            if(srcHave[static_cast<std::size_t>(f)]<kSource)++wFec;
            if(lastHandUs>=0){
                const double gap=static_cast<double>(x.atUs-lastHandUs)/1000.0;
                wGapMax=(std::max)(wGapMax,gap);
                if(gap>1.5*static_cast<double>(kFrameUs)/1000.0)++m.stutters;
            }
            lastHandUs=x.atUs;
            wLateSum+=static_cast<double>(x.atUs-st::frameSentUs(f))/1000.0;
            m.added.push_back(static_cast<float>(static_cast<double>(x.atUs-possible[static_cast<std::size_t>(f)])/1000.0));
        }
    };
    const auto drain=[&]{for(const auto& x:rel)release(x);rel.clear();};
    // Audio frames are produced at 10 ms steps up to `until`.
    std::int64_t audioNext=st::kStartUs;
    const auto audioUntil=[&](std::int64_t until){
        while(audioNext+20000<=until){ // a frame is judged once its arrival could be known
            const auto f=static_cast<std::size_t>((audioNext-st::kStartUs)/kFrameUs);
            double extra=0;
            if(f<frames){
                std::size_t j=f;
                while(j<frames&&path.frameDelayMs[j]<0)++j; // lost: arrives (concealed) with the next received frame
                extra=j<frames?static_cast<double>(path.frameDelayMs[j])+static_cast<double>(j-f)*static_cast<double>(kFrameUs)/1000.0:0.0;
            }
            if(extra>AudioModel::kIngressMs)audio.dropped();else audio.frame(extra);
            audioNext+=10000;
        }
    };
    double rttReported=c.rttBaseMs;std::int64_t nextRtt=st::kStartUs;
    std::int64_t nextWindow=st::kStartUs+kWindowUs,nextLook=st::kStartUs+st::kLookUs,now=0;
    std::uint32_t lastDecisionWait=0,lastDecisionAudio=0;
    std::array<std::int64_t,64> changeAt{};std::size_t changeCount=0;
    double leaveSince=-1;
    const std::int64_t untilUs=st::kStartUs+static_cast<std::int64_t>(c.untilS*1e6);
    const auto waitIndex=[](std::uint32_t us){return us==2000?0u:us==4000?1u:us==8000?2u:us==16000?3u:4u;};
    std::int64_t lastWaitChange=st::kStartUs;
    const auto setWait=[&](std::uint32_t us,std::int64_t at){
        if(us==waitUs)return;
        m.secondsAtWait[waitIndex(waitUs)]+=static_cast<double>(at-lastWaitChange)/1e6;lastWaitChange=at;
        waitUs=us;queue.setTimeoutUs(us);++m.settingChanges;
        changeAt[changeCount%changeAt.size()]=at;++changeCount;
    };
    struct Pending {std::int64_t at;std::uint32_t waitUs;double audioMs;};
    std::array<Pending,16> pending{};std::size_t pendingHead=0,pendingCount=0;
    const auto applyDue=[&](std::int64_t at){
        while(pendingCount>0&&pending[pendingHead].at<=at){
            const Pending& q=pending[pendingHead];
            setWait(q.waitUs,at);audio.setTarget(q.audioMs);
            pendingHead=(pendingHead+1)%pending.size();--pendingCount;
        }
    };
    const auto closeWindow=[&](std::int64_t at){
        applyDue(at);
        audioUntil(at);
        Sample s;s.endMs=at/1000;s.windowMs=static_cast<double>(kWindowUs)/1000.0;s.route=c.route;
        std::int64_t lostNow=0;
        while(lostIndex<path.trace.lostSentUs.size()&&path.trace.lostSentUs[lostIndex]<at){++lostIndex;++m.linkLost;++lostNow;}
        const auto& k=queue.counters();
        s.packetsReceived=wReceived;
        s.packetsLost=lostNow;
        s.packetsReordered=static_cast<std::int64_t>((k.gaps-before.gaps)-(k.timeouts-before.timeouts));
        s.packetsLate=static_cast<std::int64_t>(k.lateArrivals-before.lateArrivals);
        s.packetGapMeanMs=wPktGaps?wPktGapSum/static_cast<double>(wPktGaps):-1;s.packetGapMaxMs=wPktGaps?wPktGapMax:-1;
        s.framesArrived=wHanded;s.framesIncomplete=wFailed;s.framesFecRecovered=wFec;
        double gap=wGapMax;
        if(lastHandUs>=0)gap=(std::max)(gap,static_cast<double>(at-lastHandUs)/1000.0); // an ongoing gap counts too
        s.frameGapMaxMs=gap;s.frameGapExcessMs=(std::max)(0.0,gap-static_cast<double>(kFrameUs)/1000.0);
        s.lateRescueMs=wRescue;s.reorderWaitMs=waitUs/1000.0;
        if(at>=nextRtt){
            const auto f=static_cast<std::size_t>((at-st::kStartUs)/kFrameUs);
            const double d=f<frames&&path.frameDelayMs[f]>=0?static_cast<double>(path.frameDelayMs[f]):0.0;
            rttReported=c.rttBaseMs+2*d;nextRtt=at+1000000;
        }
        s.rttMs=rttReported;
        const auto f=static_cast<std::size_t>((at-st::kStartUs)/kFrameUs);
        const bool lowered=f<frames&&path.bottleneck[f]==2;
        s.requestedKbps=kRequestedKbps;s.consoleTargetKbps=lowered?kRequestedKbps*0.75:kRequestedKbps;
        s.measuredKbps=static_cast<double>(wReceived)*kRequestedKbps/(static_cast<double>(units)*0.2*1e6/static_cast<double>(kFrameUs))*(lowered?0.75:1.0);
        s.videoQueueDepth=0;s.audioBufferedMs=audio.reserveMs();s.audioUnderruns=audio.takeUnderruns();
        if(c.route==Route::Wifi&&c.signalStart>=0){
            const double t=static_cast<double>(at-st::kStartUs)/1e6;
            const double u=std::clamp((t-c.fromS)/(c.untilS-c.fromS),0.0,1.0);
            s.wifiSignalPercent=std::round(c.signalStart+(c.signalLow-c.signalStart)*u);
            s.wifiRssiDbm=s.wifiSignalPercent/2-100;s.wifiLinkMbps=286;s.wifiBandGhz=5;
        }
        s.frameLateMeanMs=wHanded?wLateSum/static_cast<double>(wHanded):-1;
        const Decision d=engine.update(s);
        if(options.recordWindows){
            WindowRecord w;w.sample=s;w.features=engine.prediction().features;w.riskStat=engine.prediction().stallRisk;
            w.riskLearned=learnedShadow.update(s).stallRisk;w.excess=s.frameGapExcessMs;out.windows.push_back(w);
            out.decisions.push_back(d);
        }
        if(smart){
            const std::uint32_t want=d.reorderWindowMs?d.reorderWindowMs*1000u:startUs;
            const double audioWant=d.audioTargetMs?d.audioTargetMs:kAudioFloorMs;
            if(options.applyLagUs<=0){setWait(want,at);audio.setTarget(audioWant);}
            else{
                if(pendingCount==pending.size()){pendingHead=(pendingHead+1)%pending.size();--pendingCount;} // cannot happen at 250 ms lag
                pending[(pendingHead+pendingCount)%pending.size()]={at+options.applyLagUs,want,audioWant};++pendingCount;
            }
            if(d.audioTargetMs!=lastDecisionAudio)++m.settingChanges;
            const bool leave=d.reorderWindowMs==0&&d.audioTargetMs==0&&!d.networkLimited;
            if(at>=untilUs){if(leave){if(leaveSince<0)leaveSince=static_cast<double>(at-untilUs)/1e6;}else leaveSince=-1;}
            lastDecisionWait=d.reorderWindowMs;lastDecisionAudio=d.audioTargetMs;
        }
        before=k;wReceived=wHanded=wFec=wFailed=0;wGapMax=0;wPktGapSum=0;wPktGapMax=0;wPktGaps=0;wRescue=0;wLateSum=0;
        std::size_t inMinute=0;for(std::size_t i=0;i<(std::min)(changeCount,changeAt.size());++i)if(at-changeAt[i]<60000000)++inMinute;
        m.maxChangesPerMinute=(std::max)(m.maxChangesPerMinute,static_cast<double>(inMinute));
    };
    const auto look=[&](std::int64_t at){
        applyDue(at);
        if(policy!=PolicyKind::Rc4Switches)return;
        ReorderEvidence e=queue.counters();e.fecFailed=failedTotal;e.lateFramesFailed=lateFailedTotal;
        const auto d=adaptive.update(at*10,e);
        if(d.changed)setWait(d.windowUs,at);
    };
    const auto advance=[&](std::int64_t until){
        for(;;){
            const auto wake=queue.nextWakeUs(now);
            const std::int64_t next=wake?*wake:(std::numeric_limits<std::int64_t>::max)();
            const std::int64_t boundary=(std::min)(nextLook,nextWindow);
            if(boundary<next&&boundary<=until){
                if(nextLook<=nextWindow){look(nextLook);nextLook+=st::kLookUs;}
                else{closeWindow(nextWindow);nextWindow+=kWindowUs;}
                continue;
            }
            if(!wake||next>until)break;
            queue.timer(next,rel);now=queue.nowUs();drain();
        }
    };
    for(const auto& a:path.trace.arrivals){
        advance(a.atUs);
        now=a.atUs;++wReceived;
        if(lastArrival>=0){const double g=static_cast<double>(a.atUs-lastArrival)/1000.0;wPktGapSum+=g;wPktGapMax=(std::max)(wPktGapMax,g);++wPktGaps;}
        lastArrival=a.atUs;
        arrived[a.packet]=1;
        if(anySeen&&a.packet<highestSeen)lastOutOfOrder=a.atUs;
        if(!anySeen||a.packet>highestSeen)highestSeen=a.packet;
        anySeen=true;
        if(skippedAt[a.packet]>=0){
            const auto f=static_cast<std::size_t>(a.packet/units);
            rescue[f]=(std::max)(rescue[f],static_cast<float>(static_cast<double>(a.atUs-skippedAt[a.packet])/1000.0+0.001));
        }
        const auto lateBefore=queue.counters().lateArrivals;
        queue.packet(now,static_cast<std::uint16_t>(a.packet),rel);
        if(queue.counters().lateArrivals!=lateBefore)flag(static_cast<std::int64_t>(a.packet/units),2u,now);
        drain();
        // Early loss: the head frame already misses more units than its FEC can rebuild, and the link has
        // shown no out-of-order packet for 300 s: give the gap up now instead of waiting.
        if((policy==PolicyKind::EarlyLoss||policy==PolicyKind::EarlyLoss16)&&queue.waiting()&&a.atUs-lastOutOfOrder>=300000000){
            const std::uint32_t head=anyReleased?lastPacket+1:0;
            const std::uint32_t f=head/units;
            if(f<frames&&c.fec<units){
                unsigned missing=0;
                const std::uint32_t end=(std::min)((f+1)*units,highestSeen+1);
                for(std::uint32_t q=f*units;q<end;++q)if(!arrived[q])++missing;
                if(missing>c.fec){
                    queue.setTimeoutUs(1);queue.timer(now+1,rel);now=queue.nowUs();drain();queue.setTimeoutUs(waitUs);queue.timer(now,rel);drain();
                }
            }
        }
    }
    advance(st::frameSentUs(frames)+200000);
    m.secondsAtWait[waitIndex(waitUs)]+=static_cast<double>(now-lastWaitChange)/1e6;
    m.frames=frames;
    for(unsigned f=0;f<frames;++f)if(hand[f]>=0)++m.delivered;
    m.givenUp=failedTotal;
    m.minutes=c.seconds/60.0;
    m.addedP50=detail::percentile(m.added,0.50);m.addedP95=detail::percentile(m.added,0.95);m.addedP99=detail::percentile(m.added,0.99);
    m.giveUpP95Ms=detail::percentile(m.giveUp,0.95);
    m.audioGaps=audio.gaps();m.audioMeanMs=audio.mean();m.audioP95Ms=detail::percentile(audio.samples(),0.95);
    m.leaveAfterS=smart?leaveSince:-1;
    (void)lastDecisionWait;
    // Labels for the predictor study: a stall (excess above T) in a window within the next H.
    if(options.recordWindows){
        const auto horizon=static_cast<std::size_t>(Predictor::kHorizonMs/200.0);
        auto& w=out.windows;
        for(std::size_t i=0;i<w.size();++i){
            bool any=false;
            for(std::size_t j=i+1;j<w.size()&&j<=i+horizon;++j)if(w[j].excess>Predictor::kStallExcessMs){any=true;break;}
            w[i].label=any;
        }
    }
    return out;
}
} // namespace veyra::remoteplay::smartlink::sim
