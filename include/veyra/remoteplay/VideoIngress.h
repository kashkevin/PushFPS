// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Types.h"
#include "Timeline.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
namespace veyra::remoteplay {
struct QueueLimits {
    std::size_t maxFrames=4, maxBytes=8u*1024u*1024u, maxConfigBytes=64u*1024u;
    HostTime maxAge100ns=1000000; // 100 ms development safety ceiling, not target latency.
    // Experiment VEYRA_EXP_RP_BACKLOG. 0 (default): a frame arriving while
    // maxFrames wait drops the queue and asks for a keyframe, as always.
    // N > maxFrames: a burst is kept up to N frames and decoded through. Two
    // things keep that from ever becoming standing delay: no frame may wait
    // longer than maxAge100ns (unchanged), and the queue must be back below
    // maxFrames within maxAge100ns of first exceeding it (below, not at: a
    // queue that only returns to maxFrames is one frame from the next
    // overflow and would be allowed to stand). Either breach takes the
    // original path: drop everything, ask for a keyframe.
    // With N set, both age checks also ignore a `now` older than the time it
    // is compared with (the consumer reads its clock before taking the lock);
    // with 0 the original comparison, which calls that expired, is kept.
    std::size_t backlogFrames=0;
    // Minimum time between two keyframe requests: the original 500 ms, fixed.
    static constexpr HostTime kIdrSpacing100ns=5000000;
    // Experiment VEYRA_EXP_RP_IDR_RETRY_MS below 500. 0 (default): nothing but
    // the spacing above. N (250 ms up to, not including, the spacing): the
    // first repeat request of an outage may follow the request that opened it
    // after N. Only that one: every later request keeps the 500 ms spacing,
    // and a request opens an outage only when none was sent in the second
    // before it, so no stretch of time holds more than one request above
    // what the spacing alone allows.
    HostTime idrFirstRetry100ns=0;
    // Experiment VEYRA_EXP_RP_EXPIRY_FIX. false (default): the original age
    // rule, which calls a head frame that arrived after the consumer read its
    // clock expired (a recovery and a keyframe request for a frame that had
    // just arrived). true: such a frame is never expired; its age is judged at
    // the next look. Nothing else changes (no backlog).
    bool expiryFix=false;
    void validate() const;
};
enum class PushStatus {Accepted,ConfigStored,WaitingForIdr,Malformed,WrongGeneration,Closed};
// Stream "sync" (Package 9: two decoder resets at 30 Mbit/s whose cause the logs could only narrow down): what
// opened each recovery episode (a dropped queue and a wait for a keyframe). Counted once per episode, by the
// rule that opened it; the [remoteplay-ingress] causes line reports them. Diagnostics only.
enum class RecoveryCause : std::uint8_t {
    Malformed,      // a sample the ingress refused (size, padding, resolution, NAL, kind)
    Config,         // codec configuration changed, or a picture without a matching one
    Gap,            // a wire frame index skipped
    Lost,           // the library reported lost frames or a changed reference (framesLost / referenceRecovered)
    TooOld,         // a sample older than maxAge100ns when it was pushed
    TooLarge,       // a sample that cannot fit maxBytes
    Overflow,       // maxFrames (or the backlog limit) already waiting, or the byte budget full
    Expired,        // the head frame really older than maxAge100ns (the consumer did not take it in time)
    StaleClock,     // legacy rule only: the consumer's clock read was older than the head frame's arrival
    BacklogOverdue, // backlog experiment: the queue did not come back below maxFrames in time
    Decoder,        // the decoder owner asked for a keyframe (decode failure, wire index refused)
    Count
};
inline constexpr const char* recoveryCauseName(RecoveryCause c) noexcept {
    switch(c){
    case RecoveryCause::Malformed:return "malformed";case RecoveryCause::Config:return "config";
    case RecoveryCause::Gap:return "gap";case RecoveryCause::Lost:return "lost";case RecoveryCause::TooOld:return "tooOld";
    case RecoveryCause::TooLarge:return "tooLarge";case RecoveryCause::Overflow:return "overflow";
    case RecoveryCause::Expired:return "expired";case RecoveryCause::StaleClock:return "staleClock";
    case RecoveryCause::BacklogOverdue:return "backlogOverdue";case RecoveryCause::Decoder:return "decoder";
    case RecoveryCause::Count:break;
    }
    return "unknown";
}
struct VideoQueueStats {
    std::uint64_t accessUnits=0,configMessages=0,accepted=0,consumed=0,dropped=0,
        rejected=0,recoveries=0,idrRequests=0;
    // Of idrRequests, those let through sooner than 500 ms after the one
    // before (idrFirstRetry100ns); always 0 without that experiment.
    std::uint64_t idrEarlyRequests=0;
    // Backlog experiment only: bursts that went past maxFrames, and the frames
    // accepted in them that the original rule would have turned into a recovery.
    std::uint64_t backlogEpisodes=0,backlogFrames=0;
    std::size_t depth=0,bytes=0,highWaterDepth=0,highWaterBytes=0;
    bool waitingForIdr=true;std::uint64_t epoch=1;
    // Stream "sync": episodes by the cause that opened them (index = RecoveryCause), frames waiting when the newest
    // episode opened, samples refused because a keyframe was awaited, and consumer clock reads older than the head
    // frame that the current rule did NOT call expired (each would have been a StaleClock episode before).
    std::array<std::uint64_t,static_cast<std::size_t>(RecoveryCause::Count)> recoveryCauses{};
    std::size_t lastRecoveryDepth=0;
    std::uint64_t refusedWaiting=0,staleClockTolerated=0;
};
// One "[remoteplay-ingress] causes" line whenever a count changed (stream "sync"); the decoder owner calls
// update() with each second's snapshot. Never more than one line a second, and none while nothing happens.
class RecoveryCauseLog {
public:
    // The text after the component name, or empty when nothing changed since the previous call. callbackRejected:
    // the video callback's refusals (ChiakiBackend), of which refusedWaiting are the ingress's own.
    std::string update(const VideoQueueStats&,std::uint64_t callbackRejected);
private:
    std::array<std::uint64_t,static_cast<std::size_t>(RecoveryCause::Count)> causes_{};
    std::uint64_t refused_=0,tolerated_=0,callback_=0;
};
struct QueuedVideo {
    VideoSample sample;
    // Full configuration snapshot must be fed before this AU when non-null.
    // The caller drains EAGAIN then retries; do not drop it as "not a picture".
    std::shared_ptr<const VideoSample> configBefore;
    std::uint64_t epoch=1;
    bool resetDecoder=false;
};
class VideoIngress {
public:
    explicit VideoIngress(QueueLimits limits={});
    void begin(Generation);
    PushStatus push(VideoSample,HostTime now);
    std::optional<QueuedVideo> tryPop(HostTime now);
    bool waitForData(std::chrono::milliseconds timeout);
    // Poll from the protocol OWNER, never request an IDR while holding a C callback's lock.
    bool takeIdrRequest(HostTime now);
    void requireKeyframe(); // Decoder reports fatal error / lost references.
    void close();
    VideoQueueStats stats() const;
private:
    void recoverLocked(RecoveryCause);
    void updateStatsLocked();
    // Why the head frame is expired at `now`, if it is; a stale clock read that the current rule tolerates is
    // counted in staleClockTolerated (hence not const).
    std::optional<RecoveryCause> expiredLocked(HostTime now);
    bool backlogOverdueLocked(HostTime now) const;
    QueueLimits limits_;mutable std::mutex mutex_;std::condition_variable cv_;
    std::deque<QueuedVideo> queue_;std::shared_ptr<const VideoSample> config_;
    Generation generation_=0;std::size_t payloadBytes_=0;bool closed_=true,waiting_=true,pendingIdr_=false;
    HostTime lastIdr_=-1,backlogSince_=-1;Sequence16Extender sequence_;VideoQueueStats stats_{};
    // The newest request opened an outage and its early repeat is still unused.
    // Written by every granted request and read only after one (lastIdr_ >= 0).
    bool idrEarlyRetry_=false;
};
struct AudioQueueStats {std::uint64_t accepted=0,consumed=0,dropped=0,rejected=0;double bufferedMs=0;};
class AudioIngress {
public:
    explicit AudioIngress(std::uint32_t maxBufferedMs=100);
    void begin(Generation);
    bool push(PcmBlock);
    std::optional<PcmBlock> tryPop();
    void close();
    AudioQueueStats stats() const;
private:
    mutable std::mutex mutex_;std::deque<PcmBlock> queue_;
    Generation generation_=0;std::uint32_t rate_=0,channels_=0,maxMs_;std::size_t frames_=0;
    std::uint64_t nextSample_=0;bool closed_=true,gap_=false;AudioQueueStats stats_{};
};
} // namespace veyra::remoteplay
