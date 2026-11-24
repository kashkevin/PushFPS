// SPDX-License-Identifier: GPL-3.0-only
#include "veyra/remoteplay/VideoIngress.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace veyra::remoteplay {
void QueueLimits::validate() const {
    if(maxFrames<1||maxFrames>64||maxBytes<1024||maxBytes>64u*1024u*1024u ||
       maxConfigBytes<1||maxConfigBytes>maxBytes||maxConfigBytes>1024u*1024u ||
       maxAge100ns<10000||maxAge100ns>5000000 ||
       (backlogFrames!=0&&(backlogFrames<=maxFrames||backlogFrames>64)) ||
       (idrFirstRetry100ns!=0&&(idrFirstRetry100ns<2500000||idrFirstRetry100ns>=kIdrSpacing100ns)))
        throw std::invalid_argument("Unsafe remote-play queue limits");
}
VideoIngress::VideoIngress(QueueLimits limits):limits_(limits){limits_.validate();}
void VideoIngress::begin(Generation g) {
    if(!g)throw std::invalid_argument("Generation zero is invalid");
    std::lock_guard lock(mutex_);queue_.clear();config_.reset();generation_=g;payloadBytes_=0;
    closed_=false;waiting_=true;pendingIdr_=false;lastIdr_=-1;backlogSince_=-1;sequence_.reset();stats_={};cv_.notify_all();
}
void VideoIngress::recoverLocked(RecoveryCause cause) {
    // Only one epoch increment per recovery episode; repeated P frames do not reset forever.
    if(!waiting_){++stats_.epoch;++stats_.recoveries;++stats_.recoveryCauses[static_cast<std::size_t>(cause)];stats_.lastRecoveryDepth=queue_.size();}
    stats_.dropped+=queue_.size();queue_.clear();payloadBytes_=0;backlogSince_=-1;
    waiting_=true;pendingIdr_=true;updateStatsLocked();
}
void VideoIngress::updateStatsLocked() {
    stats_.depth=queue_.size();stats_.bytes=payloadBytes_+(config_?config_->payload.size():0);
    stats_.waitingForIdr=waiting_;
    stats_.highWaterDepth=std::max(stats_.highWaterDepth,stats_.depth);
    stats_.highWaterBytes=std::max(stats_.highWaterBytes,stats_.bytes);
}
// `now` is read by the caller before it takes the lock, so the consumer's value
// can be older than a frame the producer pushed in between. The original rule
// calls that head frame expired (a recovery and a keyframe request for a frame
// that just arrived); it stays exactly so unless the backlog experiment or
// VEYRA_EXP_RP_EXPIRY_FIX is on, which wait for a clock value that can say
// something about the frame's age.
std::optional<RecoveryCause> VideoIngress::expiredLocked(HostTime now) {
    if(queue_.empty())return {};
    const HostTime arrival=queue_.front().sample.arrival100ns;
    if(now<arrival){
        if(limits_.backlogFrames==0&&!limits_.expiryFix)return RecoveryCause::StaleClock;
        ++stats_.staleClockTolerated;return {};
    }
    if(now-arrival>limits_.maxAge100ns)return RecoveryCause::Expired;
    return {};
}
// Backlog experiment: the queue went past maxFrames and has not come back
// within the age ceiling. Never true with backlogFrames == 0. A clock value
// from before the episode started is not evidence that it is overdue.
bool VideoIngress::backlogOverdueLocked(HostTime now) const {
    return backlogSince_>=0 && now>backlogSince_ && now-backlogSince_>limits_.maxAge100ns;
}
PushStatus VideoIngress::push(VideoSample sample,HostTime now) {
    std::lock_guard lock(mutex_);
    if(closed_)return PushStatus::Closed;
    if(sample.generation!=generation_)return PushStatus::WrongGeneration;
    if(sample.payload.size()==0 || sample.arrival100ns<0 || now<sample.arrival100ns ||
       !sample.payload.paddingIsZero() || sample.framesLost<0 ||
       !((sample.width==1280&&sample.height==720)||(sample.width==1920&&sample.height==1080))){
        ++stats_.rejected;recoverLocked(RecoveryCause::Malformed);return PushStatus::Malformed;
    }
    const auto nal=inspectAnnexB(sample.payload.bytes(),sample.codec);
    if(!nal.valid){++stats_.rejected;recoverLocked(RecoveryCause::Malformed);return PushStatus::Malformed;}
    if(sample.kind==SampleKind::CodecConfig){
        if(!nal.hasConfig||nal.hasPicture||sample.wireFrameIndex||sample.payload.size()>limits_.maxConfigBytes){
            ++stats_.rejected;recoverLocked(RecoveryCause::Malformed);return PushStatus::Malformed;
        }
        ++stats_.configMessages;
        const bool same=config_&&config_->codec==sample.codec&&config_->width==sample.width&&config_->height==sample.height&&
            std::ranges::equal(config_->payload.bytes(),sample.payload.bytes());
        if(!same){recoverLocked(RecoveryCause::Config);config_=std::make_shared<const VideoSample>(std::move(sample));}
        updateStatsLocked();return PushStatus::ConfigStored;
    }
    if(sample.kind!=SampleKind::AccessUnit || !nal.hasPicture){++stats_.rejected;recoverLocked(RecoveryCause::Malformed);return PushStatus::Malformed;}
    ++stats_.accessUnits;
    if(sample.wireFrameIndex){
        const auto seq=sequence_.observe(*sample.wireFrameIndex);
        if(!seq.accepted){++stats_.rejected;return PushStatus::Malformed;}
        if(seq.gap)recoverLocked(RecoveryCause::Gap);
    }
    if(sample.framesLost>0 || sample.referenceRecovered)recoverLocked(RecoveryCause::Lost);
    if(now-sample.arrival100ns>limits_.maxAge100ns){++stats_.dropped;++stats_.refusedWaiting;recoverLocked(RecoveryCause::TooOld);return PushStatus::WaitingForIdr;}
    if(!config_ || sample.codec!=config_->codec || sample.width!=config_->width || sample.height!=config_->height){
        ++stats_.dropped;++stats_.refusedWaiting;recoverLocked(RecoveryCause::Config);return PushStatus::WaitingForIdr;
    }
    const auto headerBytes=config_->payload.size();
    if(sample.payload.size()>limits_.maxBytes-headerBytes){++stats_.dropped;++stats_.refusedWaiting;recoverLocked(RecoveryCause::TooLarge);return PushStatus::WaitingForIdr;}
    const std::size_t frameLimit=limits_.backlogFrames?limits_.backlogFrames:limits_.maxFrames;
    if(const auto expired=expiredLocked(now))recoverLocked(*expired);
    else if(backlogOverdueLocked(now))recoverLocked(RecoveryCause::BacklogOverdue);
    else if(queue_.size()>=frameLimit||payloadBytes_>limits_.maxBytes-headerBytes-sample.payload.size())recoverLocked(RecoveryCause::Overflow);
    if(waiting_&&!nal.idr){++stats_.dropped;++stats_.refusedWaiting;pendingIdr_=true;return PushStatus::WaitingForIdr;}
    const bool reset=waiting_;
    queue_.push_back(QueuedVideo{std::move(sample),reset?config_:nullptr,stats_.epoch,reset});
    payloadBytes_+=queue_.back().sample.payload.size();waiting_=false;
    if(reset)pendingIdr_=false;
    if(queue_.size()>limits_.maxFrames){
        // Only reachable with backlogFrames set: the original rule recovered above.
        ++stats_.backlogFrames;
        if(backlogSince_<0){backlogSince_=now;++stats_.backlogEpisodes;}
    }
    ++stats_.accepted;updateStatsLocked();cv_.notify_one();return PushStatus::Accepted;
}
std::optional<QueuedVideo> VideoIngress::tryPop(HostTime now) {
    std::lock_guard lock(mutex_);
    if(closed_)return {};
    if(const auto expired=expiredLocked(now)){recoverLocked(*expired);return {};}
    if(backlogOverdueLocked(now)){recoverLocked(RecoveryCause::BacklogOverdue);return {};}
    if(queue_.empty())return {};
    auto out=std::move(queue_.front());queue_.pop_front();payloadBytes_-=out.sample.payload.size();
    // Below the original limit, not at it: with a decoder exactly as fast as the
    // stream the depth goes maxFrames+1, maxFrames, maxFrames+1... and an episode
    // ended at maxFrames would start again, with a fresh allowance, on every push.
    if(queue_.size()<limits_.maxFrames)backlogSince_=-1;
    ++stats_.consumed;updateStatsLocked();return out;
}
bool VideoIngress::waitForData(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);cv_.wait_for(lock,timeout,[&]{return closed_||!queue_.empty();});return !closed_&&!queue_.empty();
}
bool VideoIngress::takeIdrRequest(HostTime now) {
    std::lock_guard lock(mutex_);
    if(closed_||!pendingIdr_||now<0)return false;
    bool opensOutage=true,early=false;
    if(lastIdr_>=0){
        if(now<lastIdr_)return false;
        const HostTime since=now-lastIdr_;
        if(since<QueueLimits::kIdrSpacing100ns){
            // The original rule refuses here. With idrFirstRetry100ns set, the one
            // repeat straight after a request that opened an outage may pass.
            if(!idrEarlyRetry_||since<limits_.idrFirstRetry100ns)return false;
            early=true;
        }
        opensOutage=since>=2*QueueLimits::kIdrSpacing100ns;
    }
    idrEarlyRetry_=opensOutage&&limits_.idrFirstRetry100ns!=0;
    if(early)++stats_.idrEarlyRequests;
    lastIdr_=now;pendingIdr_=false;++stats_.idrRequests;return true;
}
void VideoIngress::requireKeyframe(){std::lock_guard lock(mutex_);if(!closed_)recoverLocked(RecoveryCause::Decoder);}
void VideoIngress::close(){std::lock_guard lock(mutex_);closed_=true;queue_.clear();config_.reset();payloadBytes_=0;pendingIdr_=false;updateStatsLocked();cv_.notify_all();}
VideoQueueStats VideoIngress::stats()const{std::lock_guard lock(mutex_);return stats_;}

std::string RecoveryCauseLog::update(const VideoQueueStats& s,std::uint64_t callbackRejected){
    if(s.recoveryCauses==causes_&&s.refusedWaiting==refused_&&s.staleClockTolerated==tolerated_&&callbackRejected==callback_)return {};
    causes_=s.recoveryCauses;refused_=s.refusedWaiting;tolerated_=s.staleClockTolerated;callback_=callbackRejected;
    std::string line="causes episodes="+std::to_string(s.recoveries);
    for(std::size_t i=0;i<causes_.size();++i){line+=' ';line+=recoveryCauseName(static_cast<RecoveryCause>(i));line+='=';line+=std::to_string(causes_[i]);}
    line+=" depthAtLast="+std::to_string(s.lastRecoveryDepth)+" refusedWaiting="+std::to_string(refused_)+
        " staleClockTolerated="+std::to_string(tolerated_)+" callbackRejected="+std::to_string(callback_)+
        " (since the stream started: what opened each wait for a keyframe; refusedWaiting = frames refused while waiting)";
    return line;
}

AudioIngress::AudioIngress(std::uint32_t maxMs):maxMs_(maxMs){if(maxMs<10||maxMs>200)throw std::invalid_argument("Audio buffer must be 10..200 ms");}
void AudioIngress::begin(Generation g){
    if(!g)throw std::invalid_argument("Generation zero is invalid");
    std::lock_guard lock(mutex_);generation_=g;rate_=channels_=0;frames_=0;nextSample_=0;closed_=false;gap_=true;queue_.clear();stats_={};
}
bool AudioIngress::push(PcmBlock block){
    std::lock_guard lock(mutex_);
    if(closed_||block.generation!=generation_)return false;
    if(!block.valid()||block.arrival100ns<0||block.firstSample>std::numeric_limits<std::uint64_t>::max()-block.frames()){
        ++stats_.rejected;return false;
    }
    const auto budget=static_cast<std::size_t>(block.rate)*maxMs_/1000u;
    if(block.frames()>budget){++stats_.rejected;gap_=true;return false;}
    if(rate_!=block.rate||channels_!=block.channels){stats_.dropped+=queue_.size();queue_.clear();frames_=0;rate_=block.rate;channels_=block.channels;nextSample_=block.firstSample;gap_=true;}
    if(block.firstSample<nextSample_){++stats_.rejected;return false;}
    if(block.firstSample!=nextSample_)gap_=true;
    while(!queue_.empty()&&frames_+block.frames()>budget){frames_-=queue_.front().frames();queue_.pop_front();++stats_.dropped;gap_=true;}
    if(gap_&&!queue_.empty())queue_.front().discontinuity=true;
    block.discontinuity|=gap_;gap_=false;nextSample_=block.firstSample+block.frames();frames_+=block.frames();
    queue_.push_back(std::move(block));++stats_.accepted;stats_.bufferedMs=1000.0*static_cast<double>(frames_)/rate_;return true;
}
std::optional<PcmBlock> AudioIngress::tryPop(){
    std::lock_guard lock(mutex_);if(closed_||queue_.empty())return {};
    auto out=std::move(queue_.front());queue_.pop_front();frames_-=out.frames();++stats_.consumed;
    stats_.bufferedMs=rate_?1000.0*static_cast<double>(frames_)/rate_:0;return out;
}
void AudioIngress::close(){std::lock_guard lock(mutex_);closed_=true;queue_.clear();frames_=0;stats_.bufferedMs=0;}
AudioQueueStats AudioIngress::stats()const{std::lock_guard lock(mutex_);return stats_;}
} // namespace veyra::remoteplay
