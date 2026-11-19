#pragma once
#include <cstdint>
namespace veyra::remoteplay {
// Local monotonic milliseconds. Three retries per outage; only sustained
// decoded progress replenishes them. Lifetime attempts remain monotonic so
// source epochs cannot collide when the retry allowance is renewed.
class StreamRecovery {
public:
    enum class Action {Wait,Keyframe,Reconnect,Fail};
    // Experiment VEYRA_EXP_RP_IDR_RETRY_MS: how long without a decoded frame
    // before the first keyframe request of an outage. 1000 is the original and
    // the default; 250 is the floor. The second request stays 2000 ms after the
    // first. The console may throttle a client that asks often: unverified
    // until a real PS5 session has run with a lower value.
    void setKeyframeRetryMs(int ms){keyframeRetryMs_=ms<250?250:ms>1000?1000:ms;}
    int keyframeRetryMs()const{return keyframeRetryMs_;}
    // Experiment VEYRA_EXP_RP_RECONNECT_PATIENT: reconnection attempts per
    // outage, 3 (the original and the default) up to kPatientRetries. Only
    // sustained decoded progress (30 s) renews them, as before.
    static constexpr unsigned kRetries=3,kPatientRetries=8;
    void setMaxEpisodeRetries(unsigned n){maxRetries_=n<kRetries?kRetries:n>kPatientRetries?kPatientRetries:n;}
    unsigned maxEpisodeRetries()const{return maxRetries_;}
    // Seconds to wait before reconnection attempt `retry` (1-based, as
    // episodeRetries() reads after poll() said Reconnect). The original
    // schedule, moved here unchanged from RemotePlaySessionSource.cpp: 1/2/4 s,
    // and 10/20/40 s when the console still held the previous session
    // (RP_IN_USE). Attempts past the third (patient experiment only) wait at
    // most 8 s, or 40 s for RP_IN_USE, so a cable plugged back in is noticed
    // within 8 s plus one connect.
    static unsigned backoffSeconds(unsigned retry,bool rpInUse){
        const unsigned shift=retry<1?0u:retry>4?3u:retry-1u;
        const unsigned value=rpInUse?(10u<<shift):(1u<<shift);
        return rpInUse?(value>40u?40u:value):value;
    }
    void beginAttempt(int64_t now){lastProgress_=now;healthySince_=-1;keyframes_=0;currentVideo_=false;}
    bool frame(int64_t now){
        if(!currentVideo_||healthySince_<0||now<lastProgress_||now-lastProgress_>=1000)healthySince_=now;
        lastProgress_=now;keyframes_=0;everVideo_=currentVideo_=true;
        if(episodeRetries_&&now-healthySince_>=30000){episodeRetries_=0;return true;}
        return false;
    }
    Action poll(int64_t now,bool loginPin,bool failed,bool retryAllowed=true,bool startupRetryAllowed=false){
        const auto silent=now-lastProgress_;
        if(loginPin)return silent>=120000?Action::Fail:Action::Wait;
        if(failed&&!retryAllowed)return Action::Fail;
        // Each rebuilt session needs its own handshake/first-frame allowance.
        // A previous session having video must not shorten a new handshake.
        if(failed||silent>=(currentVideo_?6000:30000)){
            if((!everVideo_&&!(failed&&startupRetryAllowed))||episodeRetries_>=maxRetries_)return Action::Fail;
            ++episodeRetries_;++reconnects_;return Action::Reconnect;
        }
        if(currentVideo_&&keyframes_<2&&silent>=int64_t(keyframeRetryMs_)+int64_t(keyframes_)*2000){++keyframes_;return Action::Keyframe;}
        return Action::Wait;
    }
    unsigned reconnects()const{return reconnects_;}
    unsigned episodeRetries()const{return episodeRetries_;}
private:
    int64_t lastProgress_=0,healthySince_=-1;unsigned keyframes_=0,reconnects_=0,episodeRetries_=0;
    bool everVideo_=false,currentVideo_=false;
    int keyframeRetryMs_=1000;
    unsigned maxRetries_=kRetries;
};
}
