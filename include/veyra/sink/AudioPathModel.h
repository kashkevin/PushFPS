// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// LINUX MODEL of the PS5 Remote Play audio path, end to end, in virtual time (1 ms steps). Used only by
// tests/remoteplay/AudioSoakTests.inc and tools/remoteplay_latency_bench/audio_sim.cpp; nothing in the app
// includes it. It drives the real decision code (AudioLatencyController.h, CaptureSyncTarget.h,
// ArrivalClockMapping.h, captureSafeCorrectionPpm) with a model of everything around it, written from a
// reading of the code (docs/AUDIO.md, "Stages"):
//   console clock (ppm offset + sinusoidal wander), 10 ms Opus frames, sound as loud/quiet per millisecond;
//   network (base delay, jitter, random loss, loss stalls concealed by Chiaki 3 frames later and delivered
//   in one burst, held stalls delivered at once, reconnects that restart the whole audio session);
//   AudioIngress (100 ms, drop oldest, discontinuity), feeder (2 ms poll, PTS anchored to the arrival of
//   the first block after a discontinuity or a sample gap), CaptureAudioSession (2 s overflow, reset,
//   prefill and expired-PCM discard, 30 ms starvation rule, 50 ms PTS-jump rule), libswresample
//   compensation (delta quantised to whole samples per 48000, ends after 48000 OUTPUT samples unless set
//   again), the WASAPI endpoint (capacity, event period, the device's own clock in ppm, latency the
//   device clock does not report, e.g. Bluetooth), device events (switch, unplug until replug, format
//   change, device clock stall, owner thread stalls), and the video delay to match (60 or 120
//   presentations/s, frame generation's extra delay switched mid-stream with a graph rebuild pause).
// It does NOT model: sample values (a cut's audibility is "loud or quiet" per ms; the splice itself is
// tested on waveforms in AudioSoakTests.inc), Windows engine scheduling beyond explicit stalls, swresample's
// filter delay (about 0.3 ms, constant), or anything a real PS5, network or headset does that is not
// listed above. Its numbers are a model of the code as read, never a measurement.
// Round 3 (docs/AUDIO.md): Wi-Fi-like arrival (bursts of 20-80 ms with nothing, then a clump), clicks (an
// underrun that starts right after a loud real millisecond: the endpoint stops on a non-zero sample; the
// resume is assumed faded in, as the renderer does after a detected gap), Config::smooth
// (VEYRA_EXP_RP_AUDIO_SMOOTH: a 4 ms synthetic tail when the endpoint holds less than one period after a
// pump) and Config::adaptive (VEYRA_EXP_RP_AUDIO_ADAPTIVE: AudioAdaptivePlayout drives the reserve and adds
// its extra to the schedule). Both default off: rows of earlier rounds are unchanged.
#include "veyra/sink/AudioLatencyController.h"
#include "veyra/sink/AudioOwnClock.h"
#include "veyra/sink/AudioPopGuard.h"
#include "veyra/sink/ArrivalClockMapping.h"
#include "veyra/sink/CaptureAudioDsp.h"
#include "veyra/sink/CaptureSyncTarget.h"
#include "veyra/remoteplay/AudioPictureTerm.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace veyra::sink::audio_model {

struct Rng {
    std::uint64_t s=0x9E3779B97F4A7C15ull;
    explicit Rng(std::uint64_t seed=1){s^=seed*0xD1B54A32D192ED03ull;if(!s)s=1;for(int i=0;i<4;++i)next();}
    double next(){s^=s<<13;s^=s>>7;s^=s<<17;return static_cast<double>(s>>11)*(1.0/9007199254740992.0);}
    double exp(double mean){return -mean*std::log(1.0-next());}
};

enum class Rule {Today,Fast,FastLowBuffer};
enum class Signal {Tone,Speech,Sparse,Music};

struct Device {
    double hiddenMs=0;          // latency after the device clock (Bluetooth link, engine): invisible to sync
    double clockPpm=0;          // device crystal against the host clock: + consumes faster
    bool lowLatencyCapable=true;// IAudioClient3 min period below default at the mix format (48 kHz float)
    double capacityMs=20,periodMs=10,lowCapacityMs=6,lowPeriodMs=3;
};

struct Event {
    enum Kind {LossStall,HeldStall,Reconnect,DeviceSwitch,Unplug,EndpointStall,ThreadStall,VideoDelay};
    Kind kind=LossStall;
    double atMs=0,lenMs=0;
    int device=0;               // DeviceSwitch/Unplug: index into Config::devices
    double value=0;             // VideoDelay: new extra delay (ms) after a 200 ms rebuild pause
};

struct Config {
    double seconds=60;std::uint64_t seed=1;Signal signal=Signal::Music;
    double sourcePpm=0,wanderPpm=0,wanderPeriodS=600;
    double netBaseMs=2,netJitterMs=1.5,randomLoss=0;
    double videoMs=35,videoJitterMs=1,videoHz=60,fgExtraMs=0;
    std::vector<Device> devices{Device{}};
    std::vector<Event> events;
    double warmupMs=3000,recoverTolMs=8;
    // Round 3. Wi-Fi-like arrival: windows with nothing delivered start on average every wifiEveryMs
    // (exponential), last wifiMinMs..wifiMaxMs (uniform), and release what they held in one clump.
    double wifiEveryMs=0,wifiMinMs=20,wifiMaxMs=80,wifiUntilMs=1e300;
    // Phase 2 (SmartLink). Repeated disturbances: bursts only in the first wifiOnMs of every wifiPeriodMs
    // (0 = always). smartlinkAskMs > 0: the network decision (apply mode, fresh) asks for that audio reserve
    // from smartlinkDetectMs after a disturbance window starts until smartlinkReleaseMs after it ends, else 0
    // (stale or "leave"). A model of the decision's timing, not of SmartLink itself.
    double wifiPeriodMs=0,wifiOnMs=0,smartlinkAskMs=0,smartlinkDetectMs=1000,smartlinkReleaseMs=3000;
    bool noRaiseAllowance=false; // true: the reset test before review fix B1 (a reserve change counts as error)
    bool smooth=false,adaptive=false;
    bool productDrain=false;    // review of round 3: drain input as the product does (see owner())
    // Sprint 5 (BENCH2). videoStampHz > 0: the video PTS advances by 1/videoStampHz per delivered frame while
    // videoHz frames are delivered per second (RemotePlayClock::video: index / nominal rate, corrected by at
    // most 0.5 % of a frame per frame, re-anchored after a 200 ms gap); 0 = a true PTS, as before.
    // ownClock: VEYRA_EXP_RP_AUDIO_OWN_CLOCK (AudioOwnClock.h): the target is the local picture delay.
    double videoStampHz=0;bool ownClock=false;
    // Stream "sync" (Package 9): the default rule's picture term through remoteplay::AudioPictureTerm (the later
    // of the PTS and the arrival), as RemotePlaySessionSource::videoPresented does by default; false = before.
    bool pictureFix=false;
};

struct Histogram {
    std::vector<std::uint64_t> bins=std::vector<std::uint64_t>(30000,0);std::uint64_t count=0;double max=0;
    void add(double v){v=std::max(0.0,v);max=std::max(max,v);++count;bins[static_cast<std::size_t>(std::min(29999.0,v*10))]++;}
    double pct(double p)const{
        if(!count)return 0;
        const auto want=static_cast<std::uint64_t>(std::ceil(p*static_cast<double>(count)));std::uint64_t seen=0;
        for(std::size_t i=0;i<bins.size();++i){seen+=bins[i];if(seen>=std::max<std::uint64_t>(want,1))return std::min(max,static_cast<double>(i)/10+0.1);}
        return max;
    }
};

struct Result {
    double avMed=0,avP95=0,avMax=0;     // |visible A/V offset| (play - (arrival + video delay)), ms
    double earMed=0;                    // the same with the device's hidden latency (what is heard)
    double latMed=0,latP95=0,latMax=0;  // play - lower-envelope arrival
    double silenceMs=0;                 // nothing playing while the stream and a device were up
    double cutMs=0,discardedMs=0;       // controller cuts; expired/overflow/ingress/clear discards
    double maxCutMs=0,maxLoudCutPerSecondMs=0;
    int resets=0,underruns=0,   // resets: restarts of audio that had played (the first start is not one)
       loudCuts=0,quietCuts=0,gapCuts=0,gaps=0;
    double maxPitchPpm=0,msAbove1000Ppm=0;
    double recoverMaxMs=0;int unrecovered=0;
    double firstQuarterMedSigned=0,lastQuarterMedSigned=0,lastHourP2P=0;
    bool futureAudio=false,nonFinite=false;
    double playedMs=0;
    int clicks=0,tails=0,wifiBursts=0;   // round 3 (measured after warm-up like the rest)
    double playoutMaxMs=0;               // largest adaptive reserve reached (ms)
    double targetMaxMs=0,stampLagMaxMs=0;// sprint 5: largest schedule target and video PTS lag after warm-up
    std::vector<double> perSecondSigned;   // mean signed visible offset per second (NaN: nothing played)
};

namespace detail {
struct Frame {double prod=0,arrive=0,deliver=-1;std::uint64_t sample=0;bool lost=false;std::uint16_t loud=0;int session=0;};
struct Unit {double pts=0,ideal=0,len=1,pushed=0,ppm=0;bool loud=false,synthetic=false;};
struct Chunk {double pts=0,pushed=0;std::array<double,10> ideal{};std::uint16_t loud=0;bool disc=false;};
inline bool bit(std::uint16_t m,int i){return ((m>>i)&1u)!=0;}
}

class Simulation {
public:
    Simulation(const Config& config,Rule rule):c_(config),rule_(rule),rng_(config.seed),srcRng_(config.seed+7),wifiRng_(config.seed+13){}

    Result run(){
        const double totalMs=c_.seconds*1000;
        double horizon=200;for(const auto& e:c_.events)if(e.kind==Event::LossStall||e.kind==Event::HeldStall)horizon=std::max(horizon,e.lenMs+200);
        device_=c_.devices.empty()?Device{}:c_.devices[0];
        openDevice(0);
        const auto ticks=static_cast<long long>(totalMs);
        std::vector<double> secondSigned;double secSum=0;int secN=0;
        for(long long tick=0;tick<ticks;++tick){
            const double t=static_cast<double>(tick);t_=t;
            startEvents(t);
            while(prodNext_<=t+horizon)produce();
            deliver(t);
            if(std::fmod(t,2)==0)feederPoll(t);
            presentVideo(t);
            if(t>=threadStallUntil_)owner(t);
            const double signedOffset=play(t);
            if(std::isfinite(signedOffset)){secSum+=signedOffset;++secN;}
            if(std::fmod(t+1,1000)==0){secondSigned.push_back(secN?secSum/secN:std::nan(""));secSum=0;secN=0;}
        }
        finish(secondSigned);
        return r_;
    }

private:
    // ---- source and network ------------------------------------------------------------------------
    double ppmAt(double t)const{return c_.sourcePpm+c_.wanderPpm*std::sin(2*3.141592653589793*t/(c_.wanderPeriodS*1000));}
    bool inWindow(Event::Kind k,double t)const{for(const auto& e:c_.events)if(e.kind==k&&t>=e.atMs&&t<e.atMs+e.lenMs)return true;return false;}
    void produce(){
        detail::Frame f;f.prod=prodNext_;f.sample=sample_;
        prodNext_+=10.0/(1.0+ppmAt(prodNext_)*1e-6);
        for(int m=0;m<10;++m){
            if(runLeft_<=0){loud_=!loud_;runLeft_=nextRun(loud_);}
            if(loud_){f.loud=static_cast<std::uint16_t>(f.loud|(1u<<m));}
            runLeft_-=1;
        }
        if(inWindow(Event::Reconnect,f.prod)){reconnectPending_=true;return;}  // nothing sent; the session restarts
        if(reconnectPending_){reconnectPending_=false;++session_;sample_=0;f.sample=0;sessionHasGood_=false;}
        f.session=session_;sample_+=480;
        f.arrive=f.prod+c_.netBaseMs+c_.netJitterMs*rng_.next();
        for(const auto& e:c_.events)if(e.kind==Event::HeldStall&&f.arrive>=e.atMs&&f.arrive<e.atMs+e.lenMs)f.arrive=e.atMs+e.lenMs+0.05*rng_.next();
        if(c_.wifiEveryMs>0){
            while(f.arrive>=wifiEnd_){wifiStart_=wifiEnd_+wifiRng_.exp(c_.wifiEveryMs);wifiEnd_=wifiStart_+c_.wifiMinMs+(c_.wifiMaxMs-c_.wifiMinMs)*wifiRng_.next();if(wifiStart_>=c_.warmupMs)++r_.wifiBursts;}
            if(f.arrive>=wifiStart_&&wifiStart_<c_.wifiUntilMs&&disturbed(wifiStart_))f.arrive=wifiEnd_+0.05*wifiRng_.next();
        }
        f.lost=inWindow(Event::LossStall,f.prod)||(c_.randomLoss>0&&rng_.next()<c_.randomLoss);
        // Chiaki conceals gaps between frames it has seen; frames lost before a new session's first frame
        // are never known to it (that frame simply starts the session).
        if(f.lost&&!sessionHasGood_)return;
        if(!f.lost)sessionHasGood_=true;
        if(f.lost)f.loud=0b11;   // Opus concealment: a short decaying tail, then quiet
        frames_.push_back(f);
    }
    double nextRun(bool loud){
        switch(c_.signal){
        case Signal::Tone:return loud?1e12:1;
        case Signal::Speech:return loud?srcRng_.exp(180)+20:srcRng_.exp(120)+10;
        case Signal::Sparse:return loud?300:srcRng_.exp(3000)+100;
        case Signal::Music:default:return loud?srcRng_.exp(400):srcRng_.exp(80);
        }
    }
    // Chiaki: in-order delivery; a lost run is concealed when the 3rd good frame after it arrives, and
    // that frame and the two before it go out in the same burst.
    void deliver(double t){
        while(!frames_.empty()){
            detail::Frame& f=frames_.front();
            if(f.deliver<0){
                if(!f.lost)f.deliver=std::max(f.arrive,lastDeliver_);
                else{
                    std::size_t good=0,j=0;double when=-1;
                    for(j=0;j<frames_.size();++j){if(frames_[j].session!=f.session){when=frames_[j>0?j-1:0].prod;break;}if(!frames_[j].lost&&++good==3){when=frames_[j].arrive;break;}}
                    if(when<0)return;   // not known yet
                    for(std::size_t k=0;k<=std::min(j,frames_.size()-1);++k){if(frames_[k].session!=f.session)break;frames_[k].deliver=std::max(when,lastDeliver_);}
                }
            }
            if(f.deliver>t+1)return;
            lastDeliver_=f.deliver;
            if(f.session!=feederSession_){feederSession_=f.session;ingress_.clear();resetApp(t);}  // a new connection
            ingress_.push_back(f);
            if(ingress_.size()>10){ingress_.pop_front();ingressGap_=true;r_.discardedMs+=10;}
            frames_.pop_front();
        }
    }
    void feederPoll(double t){
        while(!ingress_.empty()){
            const detail::Frame f=ingress_.front();ingress_.pop_front();
            const bool disc=ingressGap_||!anchored_||f.sample!=nextSample_;ingressGap_=false;
            if(disc){anchorPts_=f.deliver;anchorSample_=f.sample;anchored_=true;}
            nextSample_=f.sample+480;
            detail::Chunk ch;ch.pts=anchorPts_+static_cast<double>(f.sample-anchorSample_)/48.0;ch.pushed=t;ch.loud=f.loud;ch.disc=disc;
            for(int m=0;m<10;++m)ch.ideal[static_cast<std::size_t>(m)]=f.prod+c_.netBaseMs+m*(1.0/(1.0+ppmAt(f.prod)*1e-6));
            sessionPush(ch,t);
        }
    }
    // ---- CaptureAudioSession::push ----------------------------------------------------------------
    void sessionPush(const detail::Chunk& ch,double t){
        if(c_.adaptive&&lastArrival_>0&&!ch.disc)playout_.arrivalGap(t,t-lastArrival_);
        lastArrival_=t;
        if(!endpointReady_){r_.discardedMs+=10;return;}     // outputRecovering: dropped
        if(ch.disc){input_.clear();pendingReset_=true;pendingDisc_=true;haveVideo_=false;}
        if(!haveIngress_||ch.disc){ingressClock_.reset();haveIngress_=true;}
        ingress_ms_=ingressClock_.observe(t,ch.pts);
        if(c_.adaptive&&!ch.disc)playout_.arrivalDelay(t-ch.pts-ingress_ms_);   // CaptureAudioSession::push
        if(queuedMs()+10>kMaxQueuedMs){
            r_.discardedMs+=static_cast<double>(input_.size())*10;input_.clear();pendingReset_=true;
            if(queuedMs()+10>kMaxQueuedMs){r_.discardedMs+=10;return;}
        }
        input_.push_back(ch);
    }
    double pcmMs()const{double q=0;for(const auto& u:pcm_)q+=u.len;return q;}
    double queuedMs()const{return pcmMs()+static_cast<double>(input_.size())*10;}
    // ---- video ------------------------------------------------------------------------------------
    void presentVideo(double t){
        if(t<videoPausedUntil_||inWindow(Event::Reconnect,t-c_.netBaseMs)||inWindow(Event::HeldStall,t-videoMs_)||inWindow(Event::LossStall,t-videoMs_))return;
        const double hz=c_.videoHz*(fgExtra_>0?2:1);
        if(t<nextVideo_)return;
        nextVideo_=t+1000.0/hz;
        const double alt=fgExtra_>0?((++videoCount_)%2?1.0:-1.0):0.0;   // generated/real frames alternate
        lastVideoDelay_=videoMs_+fgExtra_+alt+c_.videoJitterMs*(2*rng_.next()-1);
        if(c_.videoStampHz>0){
            // One delivered frame per 1/videoHz, stamped 1/videoStampHz later than the previous one; the clock's
            // phase correction takes back at most 0.5 % of a stamp step per frame; a 200 ms gap re-anchors.
            const double real=fgExtra_>0?0.5:1.0,stamp=1000.0/c_.videoStampHz;
            if(lastVideoAt_>=0&&t-lastVideoAt_>200)stampLag_=0;
            else if(lastVideoAt_>=0)stampLag_=std::max(0.0,stampLag_+real*(1000.0/c_.videoHz-stamp)-real*std::min(stampLag_,stamp/200));
            lastVideoAt_=t;if(t>=c_.warmupMs)r_.stampLagMaxMs=std::max(r_.stampLagMaxMs,stampLag_);
        }
        vHost_=t;vPts_=t-lastVideoDelay_-stampLag_;vArrival_=t-lastVideoDelay_;haveVideo_=true;++videoObservation_;
        // Host axis = model time + 1000 (the term refuses an origin of 0, as for a session not yet connected).
        if(c_.pictureFix)vPts_=pictureTerm_.observe(vPts_,vHost_+1000.0,vArrival_+1000.0,1000.0);
    }
    double trueVideoDelay()const{return videoMs_+fgExtra_;}
    // ---- events -----------------------------------------------------------------------------------
    void startEvents(double t){
        for(const auto& e:c_.events){
            if(e.atMs!=t)continue;
            switch(e.kind){
            case Event::Reconnect:resetApp(t);endpointDown(t,e.lenMs);recoverStart_=std::max(recoverStart_,t+e.lenMs);noDeviceUntil_=t+e.lenMs;break;
            case Event::DeviceSwitch:endpointLost(t);nextDevice_=e.device;noDeviceUntil_=t;recoverStart_=std::max(recoverStart_,t);break;
            case Event::Unplug:endpointLost(t);nextDevice_=e.device;noDeviceUntil_=t+e.lenMs;recoverStart_=std::max(recoverStart_,t+e.lenMs);break;
            case Event::EndpointStall:deviceStallUntil_=t+e.lenMs;if(!endpoint_.empty())stalledPts_=endpoint_.front().pts;recoverStart_=std::max(recoverStart_,t+e.lenMs);break;
            case Event::ThreadStall:threadStallUntil_=t+e.lenMs;recoverStart_=std::max(recoverStart_,t+e.lenMs);break;
            case Event::VideoDelay:videoPausedUntil_=t+200;haveVideo_=false;fgExtra_=e.value;recoverStart_=std::max(recoverStart_,t+200);break;
            case Event::LossStall:case Event::HeldStall:recoverStart_=std::max(recoverStart_,t+e.lenMs);break;
            }
            noteEvent(t);
        }
    }
    // An event that follows within 20 s of the previous one ends that one's recovery window uncounted
    // (storms are judged after their last event); a window of 20 s or more without recovery counts.
    void noteEvent(double t){
        if(recoverFrom_>=0&&!recovered_&&t-recoverFrom_>=20000){++r_.unrecovered;r_.recoverMaxMs=std::max(r_.recoverMaxMs,t-recoverFrom_);}
        recoverFrom_=std::max(recoverStart_,t);recovered_=false;goodSince_=-1;
    }
    void endpointDown(double t,double minMs){endpointReady_=false;started_=false;endpoint_.clear();padding_=0;retryAt_=t+minMs;}
    // recoverEndpoint(): everything dropped, reopen after 500 ms (fast recovery: first one after 50 ms).
    void endpointLost(double t){
        endpointReady_=false;started_=false;endpoint_.clear();padding_=0;pcm_.clear();input_.clear();
        resetCompensation(t);
        retryAt_=t+(fast()&&t-lastRecovery_>5000?50:500);lastRecovery_=t;++deviceChanges_;
        haveVideo_=false;
    }
    void openDevice(int index){
        if(index>=0&&static_cast<std::size_t>(index)<c_.devices.size())device_=c_.devices[static_cast<std::size_t>(index)];
        lowActive_=rule_==Rule::FastLowBuffer&&!lowAbandoned_&&device_.lowLatencyCapable;
        capacity_=lowActive_?device_.lowCapacityMs:device_.capacityMs;period_=lowActive_?device_.lowPeriodMs:device_.periodMs;
        reserve_=lowActive_?std::clamp(capacity_,14.0,20.0):std::min(capacity_,20.0);baseReserve_=reserve_;
        if(playoutOn()){playout_.setBase(baseReserve_);reserve_=playout_.targetMs();}
    }
    bool fast()const{return rule_!=Rule::Today;}
    // A new connection: a new CaptureAudioSession with fresh state.
    void resetApp(double t){
        input_.clear();pcm_.clear();endpoint_.clear();padding_=0;started_=false;pendingReset_=false;haveIngress_=false;ingress_ms_=0;lastArrival_=0;pendingDisc_=false;havePcmTail_=false;
        anchored_=false;haveVideo_=false;own_.reset();syncTarget_.reset();videoFilter_.reset();drift_=AudioArrivalDrift{};controller_=AudioLatencyController{};
        arming_.reset();resetCompensation(t);lowUnderruns_=0;
        playout_.reset(t,baseReserve_);tailed_=false;haveReal_=false;
    }
    void resetCompensation(double t){compDelta_=0;compLeftMs_=0;filtered_=0;correctionPpm_=0;nextCorrection_=t+250;arming_.reset();controller_.reset(t);}
    void setCompensation(int delta){compDelta_=delta;compLeftMs_=delta?1000.0:0;}  // distance 48000 output samples
    // ---- the audio owner ------------------------------------------------------------------------------
    void owner(double t){
        if(!endpointReady_){
            if(t<retryAt_)return;
            if(t<noDeviceUntil_){retryAt_=t+500;lastRecovery_=t;return;}   // start fails: recoverEndpoint again
            openDevice(nextDevice_);endpointReady_=true;pendingReset_=true;nextPeriod_=t;
        }
        if(pendingReset_){
            pendingReset_=false;syncTarget_.reset();lastObservation_=0;
            if(fast()){videoFilter_.reset();controller_.reset(t);arming_.reset();drift_.rebase(t);}
            pendingDisc_=false;
            fadeAndReset();discard(pcm_);resetCompensation(t);if(everStarted_)++r_.resets;
        }
        // productDrain: as CaptureAudioSession.cpp, one 10 ms chunk per loop and only while the converted queue is
        // below the endpoint capacity + 10 ms once playing (the rest waits in input); default: all at once.
        while(!input_.empty()&&(!c_.productDrain||!started_||pcmMs()<capacity_+10)){   // convert at the resampler's current ratio
            const detail::Chunk ch=input_.front();input_.pop_front();
            if(havePcmTail_&&std::abs(ch.pts-pcmTailPts_)>50&&!pcm_.empty()){if(fast())drift_.rebase(t);fadeAndReset();discard(pcm_);if(everStarted_)++r_.resets;}
            for(int m=0;m<10;++m){
                const double ppm=compLeftMs_>0?compDelta_*1e6/48000.0:0;
                const double len=1+ppm*1e-6;compLeftMs_=std::max(0.0,compLeftMs_-len);
                pcm_.push_back({ch.pts+m,ch.ideal[static_cast<std::size_t>(m)],len,ch.pushed,ppm,detail::bit(ch.loud,m)});
            }
            pcmTailPts_=ch.pts+10;havePcmTail_=true;
        }
        const bool fresh=haveVideo_&&t>=vHost_&&t-vHost_<500;
        if(fresh&&videoObservation_!=lastObservation_){
            syncEstimate_=syncTarget_.observe(vHost_-vPts_-ingress_ms_,vHost_,std::nullopt);
            if(fast())videoFilter_.observe(vHost_,vHost_-vPts_);
            lastObservation_=videoObservation_;
        }
        if(!fresh)syncTarget_.invalidate();
        double requested=syncEstimate_.targetMs;
        if(fast()&&lastArrival_>0){
            const double d=drift_.observe(t,ingress_ms_);
            if(!std::isfinite(d))r_.nonFinite=true;
            if(const auto delay=videoFilter_.value(t))requested=*delay+d-ingress_ms_;
        }
        if(c_.ownClock){
            if(haveVideo_&&videoObservation_!=lastOwnObservation_){own_.observeVideo(vHost_,vArrival_);lastOwnObservation_=videoObservation_;}
            if(own_.known())requested=own_.update(t,started_);
        }
        const bool clockFresh=fresh||(c_.ownClock&&own_.known());
        if(t>=c_.warmupMs)r_.targetMaxMs=std::max(r_.targetMaxMs,std::clamp(requested,0.0,1500.0));
        // As CaptureAudioSession.cpp: the schedule takes the previous loop's extra, then the reserve updates.
        mappingExtra_=playoutOn()?playout_.extraMs():0.0;
        mapping_=ingress_ms_+std::clamp(requested,0.0,1500.0)+mappingExtra_;
        if(lastReset_!=allowanceAnchor_){allowance_.anchor(mappingExtra_);allowanceAnchor_=lastReset_;}
        if(playoutOn()){
            playout_.setBase(baseReserve_);if(c_.smartlinkAskMs>0)playout_.request(smartlinkAsk(t));
            reserve_=playout_.update(t);r_.playoutMaxMs=std::max(r_.playoutMaxMs,reserve_);
        }
        if(!started_){
            if(!pcm_.empty()&&pcmMs()>=reserve_&&clockFresh){
                const double expired=std::max(0.0,std::min(pcmMs()-reserve_,t-mapping_-pcm_.front().pts-5));
                double left=std::floor(expired);while(left>=1&&!pcm_.empty()){left-=pcm_.front().len;r_.discardedMs+=pcm_.front().len;pcm_.pop_front();}
                if(!pcm_.empty()&&t>=mapping_+pcm_.front().pts){
                    started_=true;everStarted_=true;lastReset_=t;nextCorrection_=t+250;nextPeriod_=t;pump();controller_.reset(t);arming_.reset();
                }
            }
            return;
        }
        if(t<nextPeriod_)return;
        nextPeriod_+=period_;if(nextPeriod_<=t)nextPeriod_=t+period_;
        pump();
        // Underrun bookkeeping as in CaptureAudioSession (30 ms starvation rule).
        const bool newUnderrun=underruns_>lastUnderruns_;
        if(c_.adaptive&&newUnderrun)playout_.underrun(t);
        if(newUnderrun||(starveSince_>=0&&padding_<=0)){
            if(starveSince_<0)starveSince_=t;
            if(t-starveSince_>=30&&t-lastArrival_>=30){fadeAndReset();discard(pcm_);resetCompensation(t);lastReset_=t;starveSince_=-1;++r_.resets;lastUnderruns_=underruns_;return;}
        }else starveSince_=-1;
        if(lowActive_&&newUnderrun&&t-lastArrival_<20&&++lowUnderruns_>=8){
            lowAbandoned_=true;lastUnderruns_=underruns_;endpointLost(t);retryAt_=t;--deviceChanges_;return;
        }
        lastUnderruns_=underruns_;
        std::optional<double> error;
        if(!endpoint_.empty()&&t<deviceStallUntil_)error=stalledPts_-(t-mapping_);
        else if(!endpoint_.empty()&&!endpoint_.front().synthetic)error=endpoint_.front().pts-(t-mapping_);   // mediaTimeMs: NaN in a tail
        const double queued=pcmMs()+padding_;
        if(!fast()){
            if(!clockFresh||!error||t<nextCorrection_)return;
            filtered_=0.75*filtered_+0.25**error;nextCorrection_=t+250;
            const double planned=c_.noRaiseAllowance?0.0:allowance_.update(mappingExtra_,*error);
            if(t-lastReset_>1000&&std::abs(*error-planned)>60){fadeAndReset();lastReset_=t;resetCompensation(t);++r_.resets;return;}
            const double target=std::clamp(filtered_*250,-5000.0,5000.0);
            correctionPpm_=captureSafeCorrectionPpm(target,correctionPpm_,queued,baseReserve_);
            setCompensation(static_cast<int>(std::llround(correctionPpm_*48000/1000000)));
            return;
        }
        AudioLatencyController::Input in;in.nowMs=t;in.queuedMs=queued;in.endpointMs=padding_;in.reserveMs=baseReserve_;
        if(clockFresh)in.errorMs=error;
        if(clockFresh&&error&&!c_.noRaiseAllowance)in.plannedMs=allowance_.update(mappingExtra_,*error);
        in.gap=underruns_>controllerUnderruns_;controllerUnderruns_=underruns_;
        const auto d=controller_.update(in);
        if(!std::isfinite(controller_.ratioPpm())||!std::isfinite(controller_.filteredErrorMs())||!std::isfinite(controller_.pendingMs())||!std::isfinite(d.dropMs))r_.nonFinite=true;
        if(d.reset){fadeAndReset();lastReset_=t;resetCompensation(t);controller_.reset(t);++r_.resets;return;}
        const int delta=static_cast<int>(std::llround(d.ratioPpm*48000/1000000));
        if(arming_.due(t,delta))setCompensation(delta);
        if(d.dropMs>0){
            const double fade=controller_.config().crossfadeMs;
            const bool gapCut=controller_.afterGap(t)&&d.loudDropMs>=d.dropMs;
            double removed=0;bool quiet=false;
            if(gapCut)removed=dropHead(d.dropMs,fade,false);
            else{removed=dropHead(d.dropMs,fade,true);quiet=removed>0;if(removed<=0&&d.loudDropMs>0)removed=dropHead(d.loudDropMs,fade,false);}
            if(removed>0){
                controller_.dropped(t,removed,quiet,gapCut);
                r_.cutMs+=removed;r_.maxCutMs=std::max(r_.maxCutMs,removed);
                if(gapCut)++r_.gapCuts;else if(quiet)++r_.quietCuts;
                else{++r_.loudCuts;loudCutLog_.push_back({t,removed});}
            }
        }
    }
    // dropHeadLocked(): whole milliseconds, clamped to what lies beyond the fade; quiet = no loud ms in
    // the cut or its fade.
    double dropHead(double ms,double fade,bool needQuiet){
        double available=pcmMs();if(available<=fade)return 0;
        double want=std::floor(std::min(ms,available-fade));if(want<1)return 0;
        if(needQuiet){double seen=0;for(const auto& u:pcm_){if(seen>=want+fade)break;if(u.loud)return 0;seen+=u.len;}}
        double removed=0;
        while(removed<want-1e-9&&!pcm_.empty()){
            detail::Unit& u=pcm_.front();const double take=std::min(u.len,want-removed);removed+=take;
            if(take<u.len){const double media=take/u.len;u.pts+=media;u.ideal+=media;u.len-=take;}else pcm_.pop_front();
        }
        return removed;
    }
    void discard(std::deque<detail::Unit>& q){for(const auto& u:q)r_.discardedMs+=u.len;q.clear();havePcmTail_=false;}
    void fadeAndReset(){for(const auto& u:endpoint_)if(!u.synthetic)r_.discardedMs+=u.len;endpoint_.clear();padding_=0;started_=false;}
    void pump(){
        const double before=padding_;
        double free=capacity_-padding_;
        while(free>1e-9&&!pcm_.empty()){
            detail::Unit& u=pcm_.front();const double take=std::min(free,u.len);
            detail::Unit part=u;part.len=take;endpoint_.push_back(part);padding_+=take;free-=take;
            if(take<u.len){const double media=take/u.len;u.pts+=media;u.ideal+=media;u.len-=take;}else pcm_.pop_front();
        }
        const double real=padding_-before;
        if(real>1e-9){haveReal_=true;tailed_=false;}
        // VEYRA_EXP_RP_AUDIO_SMOOTH: AudioEdgeSmoother::due in WasapiAudioSink.cpp pumpOnce (ms units here).
        if(c_.smooth&&started_&&haveReal_&&!tailed_&&(real>1e-9||before>1e-9)&&before+real<period_&&free>1e-9){   // dry already: no tail (review)
            detail::Unit tail;tail.len=std::min(free,4.0);tail.synthetic=true;tail.pts=endpoint_.empty()?0:endpoint_.back().pts;
            endpoint_.push_back(tail);padding_+=tail.len;tailed_=true;if(t_>=c_.warmupMs)++r_.tails;
        }
    }
    // ---- the device plays one host millisecond --------------------------------------------------------
    double play(double t){
        const bool measured=t>=c_.warmupMs;
        double signedOffset=std::nan("");
        if(!started_){
            if(measured&&endpointReady_&&t>=noDeviceUntil_&&!inWindow(Event::Reconnect,t))r_.silenceMs+=1;
            trackRecovery(t,std::nullopt);return signedOffset;
        }
        if(t<deviceStallUntil_){
            if(!endpoint_.empty())stalledPts_=endpoint_.front().pts;
            trackRecovery(t,std::nullopt);return signedOffset;
        }
        double left=1+device_.clockPpm*1e-6;bool played=false;
        while(left>1e-9&&!endpoint_.empty()){
            detail::Unit& u=endpoint_.front();const double take=std::min(left,u.len);
            lastSynthetic_=u.synthetic;lastLoud_=u.loud;
            if(u.synthetic){left-=take;padding_-=take;if(take<u.len)u.len-=take;else endpoint_.pop_front();continue;}
            if(!played){
                played=true;
                if(u.pushed>t)r_.futureAudio=true;
                const double visible=t-(u.ideal+trueVideoDelay());
                signedOffset=visible;
                if(measured){
                    r_.playedMs+=1;
                    av_.add(std::abs(visible));ear_.add(std::abs(visible+device_.hiddenMs));lat_.add(t-u.ideal);
                    r_.maxPitchPpm=std::max(r_.maxPitchPpm,std::abs(u.ppm));if(std::abs(u.ppm)>1000)r_.msAbove1000Ppm+=1;
                    signedAll_.push_back(static_cast<float>(visible));
                }
            }
            left-=take;padding_-=take;
            if(take<u.len){const double media=take/u.len;u.pts+=media;u.ideal+=media;u.len-=take;}else endpoint_.pop_front();
        }
        padding_=std::max(0.0,padding_);
        if(left>1e-9){
            if(!dry_){dry_=true;++underruns_;if(measured)++r_.gaps;if(measured&&!lastSynthetic_&&lastLoud_)++r_.clicks;}
            if(measured&&!played)r_.silenceMs+=1;
        }else dry_=false;
        trackRecovery(t,played?std::optional<double>(signedOffset):std::nullopt);
        return signedOffset;
    }
    void trackRecovery(double t,std::optional<double> offset){
        if(recoverFrom_<0||recovered_||t<recoverFrom_)return;
        if(offset&&std::abs(*offset)<=c_.recoverTolMs){if(goodSince_<0)goodSince_=t;if(t-goodSince_>=1000){recovered_=true;r_.recoverMaxMs=std::max(r_.recoverMaxMs,goodSince_-recoverFrom_);}}
        else goodSince_=-1;
    }
    void finish(const std::vector<double>& secondSigned){
        if(recoverFrom_>=0&&!recovered_&&c_.seconds*1000-recoverFrom_>=20000){++r_.unrecovered;r_.recoverMaxMs=std::max(r_.recoverMaxMs,c_.seconds*1000-recoverFrom_);}
        r_.avMed=av_.pct(.5);r_.avP95=av_.pct(.95);r_.avMax=av_.max;r_.earMed=ear_.pct(.5);
        r_.latMed=lat_.pct(.5);r_.latP95=lat_.pct(.95);r_.latMax=lat_.max;
        r_.underruns=static_cast<int>(underruns_);
        const std::size_t q=signedAll_.size()/4;
        auto median=[](std::vector<float> v){if(v.empty())return 0.0;std::nth_element(v.begin(),v.begin()+static_cast<std::ptrdiff_t>(v.size()/2),v.end());return static_cast<double>(v[v.size()/2]);};
        if(q){r_.firstQuarterMedSigned=median({signedAll_.begin(),signedAll_.begin()+static_cast<std::ptrdiff_t>(q)});r_.lastQuarterMedSigned=median({signedAll_.end()-static_cast<std::ptrdiff_t>(q),signedAll_.end()});}
        const std::size_t from=secondSigned.size()>3600?secondSigned.size()-3600:secondSigned.size()/2;
        double lo=1e300,hi=-1e300;for(std::size_t i=from;i<secondSigned.size();++i)if(std::isfinite(secondSigned[i])){lo=std::min(lo,secondSigned[i]);hi=std::max(hi,secondSigned[i]);}
        r_.lastHourP2P=hi>=lo?hi-lo:0;
        r_.perSecondSigned=secondSigned;
        std::size_t a=0;double sum=0;
        for(std::size_t b=0;b<loudCutLog_.size();++b){sum+=loudCutLog_[b].second;while(loudCutLog_[b].first-loudCutLog_[a].first>=1000){sum-=loudCutLog_[a].second;++a;}r_.maxLoudCutPerSecondMs=std::max(r_.maxLoudCutPerSecondMs,sum);}
    }

    static constexpr double kMaxQueuedMs=2000;
    Config c_;Rule rule_;Rng rng_,srcRng_,wifiRng_;Result r_;
    // round 3
    bool playoutOn()const{return c_.adaptive||c_.smartlinkAskMs>0;}
    bool disturbed(double t)const{return c_.wifiPeriodMs<=0||std::fmod(t,c_.wifiPeriodMs)<c_.wifiOnMs;}
    double smartlinkAsk(double t)const{
        if(c_.wifiEveryMs<=0||t>=c_.wifiUntilMs+c_.smartlinkReleaseMs)return 0;
        if(c_.wifiPeriodMs<=0)return t>=c_.smartlinkDetectMs?c_.smartlinkAskMs:0;
        const double in=std::fmod(t,c_.wifiPeriodMs);
        return in>=c_.smartlinkDetectMs&&in<c_.wifiOnMs+c_.smartlinkReleaseMs?c_.smartlinkAskMs:0;
    }
    AudioAdaptivePlayout playout_;double baseReserve_=20,wifiStart_=-1,wifiEnd_=-1,t_=0;bool tailed_=false,haveReal_=false,lastSynthetic_=false,lastLoud_=false;
    Histogram av_,ear_,lat_;std::vector<float> signedAll_;std::vector<std::pair<double,double>> loudCutLog_;
    // source
    double prodNext_=0,runLeft_=0,lastDeliver_=0;bool loud_=false,reconnectPending_=false,sessionHasGood_=false;std::uint64_t sample_=0;int session_=0,feederSession_=0;
    std::deque<detail::Frame> frames_,ingress_;bool ingressGap_=false;
    // feeder
    bool anchored_=false;double anchorPts_=0;std::uint64_t anchorSample_=0,nextSample_=0;
    // session
    std::deque<detail::Chunk> input_;std::deque<detail::Unit> pcm_,endpoint_;
    double ingress_ms_=0,lastArrival_=0,pcmTailPts_=0,mapping_=0;bool haveIngress_=false,pendingReset_=false,pendingDisc_=false,havePcmTail_=false;
    ArrivalClockMapping ingressClock_;CaptureSyncTarget syncTarget_;CaptureSyncTarget::Result syncEstimate_;
    AudioVideoDelayFilter videoFilter_;AudioArrivalDrift drift_;AudioLatencyController controller_;AudioRatioArming arming_;
    std::uint64_t videoObservation_=0,lastObservation_=0,videoCount_=0;
    double filtered_=0,correctionPpm_=0,nextCorrection_=0,lastReset_=0;
    double mappingExtra_=0,allowanceAnchor_=-1;ReserveChangeAllowance allowance_{}; // B1: a reserve change is not trouble
    int compDelta_=0;double compLeftMs_=0;
    // endpoint
    Device device_;bool endpointReady_=false,started_=false,everStarted_=false,dry_=false,lowActive_=false,lowAbandoned_=false;
    double capacity_=20,period_=10,reserve_=20,padding_=0,nextPeriod_=0,retryAt_=0,lastRecovery_=-1e9,noDeviceUntil_=0,starveSince_=-1,stalledPts_=0;
    int nextDevice_=0,deviceChanges_=0;std::uint64_t underruns_=0,lastUnderruns_=0,controllerUnderruns_=0,lowUnderruns_=0;
    double deviceStallUntil_=-1,threadStallUntil_=-1;
    // video
    bool haveVideo_=false;double vHost_=0,vPts_=0,nextVideo_=0,videoMs_=0,fgExtra_=0,videoPausedUntil_=-1,lastVideoDelay_=0;
    double vArrival_=0,stampLag_=0,lastVideoAt_=-1;AudioOwnClockTarget own_;std::uint64_t lastOwnObservation_=0;
    remoteplay::AudioPictureTerm pictureTerm_;
    // recovery
    double recoverStart_=0,recoverFrom_=-1,goodSince_=-1;bool recovered_=true;
public:
    void init(){videoMs_=c_.videoMs;fgExtra_=c_.fgExtraMs;}
};

inline Result simulate(const Config& config,Rule rule){Simulation s(config,rule);s.init();return s.run();}

// A random mix for property tests (AudioSoakTests.inc, audio_sim --fuzz): drift, wander, jitter, loss,
// three devices (some Bluetooth-like, some without a low-latency period) and every event kind.
inline Config randomMix(std::uint64_t seed,double seconds){
    Rng g(seed*977+3);Config c;c.seconds=seconds;c.seed=seed;
    c.signal=static_cast<Signal>(static_cast<int>(g.next()*4));
    c.sourcePpm=(g.next()*2-1)*500;c.wanderPpm=g.next()*50;c.wanderPeriodS=60+g.next()*600;
    c.netJitterMs=0.5+g.next()*3;c.randomLoss=g.next()<0.3?g.next()*0.01:0;
    c.videoMs=12+g.next()*40;c.fgExtraMs=g.next()<0.5?17:0;
    c.devices.clear();
    for(int i=0;i<3;++i){Device d;d.clockPpm=(g.next()*2-1)*200;d.hiddenMs=g.next()<0.3?150:g.next()*3;d.lowLatencyCapable=g.next()<0.6;c.devices.push_back(d);}
    const double lens[]={100,200,500,1000,2000,5000};
    for(double t=5000;t<seconds*1000-5000;t+=1000+g.next()*20000){
        Event e;e.atMs=std::floor(t);e.kind=static_cast<Event::Kind>(static_cast<int>(g.next()*8));
        e.lenMs=lens[static_cast<int>(g.next()*6)];
        if(e.kind==Event::ThreadStall)e.lenMs=5+std::floor(g.next()*40);
        if(e.kind==Event::EndpointStall)e.lenMs=10+std::floor(g.next()*300);
        if(e.kind==Event::DeviceSwitch||e.kind==Event::Unplug)e.device=static_cast<int>(g.next()*3);
        if(e.kind==Event::VideoDelay)e.value=g.next()<0.5?17:0;
        c.events.push_back(e);
    }
    return c;
}

} // namespace veyra::sink::audio_model
