#pragma once
#include "RemotePlaySource.h"
#include "veyra/remoteplay/ConnectionHealth.h"
#include "veyra/remoteplay/ControllerQueue.h"
#include "veyra/remoteplay/FrameDelivery.h"
#include "veyra/remoteplay/WakeSignal.h"
#include "veyra/engine/PushDeviceLossResume.h"
#include "veyra/sink/CaptureAudioSession.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace veyra::source {
// Network/decoder owner keeps consuming reference pictures while the GPU is
// busy. Only already-decoded presentation opportunities use the latest slot.
class RemotePlaySessionSource final : public IFrameSource {
public:
    RemotePlaySessionSource();
    ~RemotePlaySessionSource() override;
    bool connect(RemotePlayConnectDesc desc);
    // True while the owner loop, including its reconnect retries, still runs.
    bool alive() const { return alive_.load(); }
    bool failed() const { std::lock_guard lock(mutex_); return failed_; }
    bool open(const SourceOpenDesc&) override { return false; }
    const SourceInfo& info() const override { return info_; }
    SourceReadStatus read(pipeline::FramePacket&, const AVFrame**) override;
    bool seek(const pipeline::Rational&) override { return false; }
    void close() noexcept override;
    void controller(remoteplay::ControllerState state);
    void loginPin(std::string pin);
    remoteplay::ControllerFeedback takeFeedback();
    remoteplay::SessionInbox::Snapshot sessionSnapshot() const;
    uint64_t skipped() const;
    struct Rates {uint64_t received=0,decoded=0,ingressDropped=0;double receivedFps=0,decodedFps=0;bool ready=false;};
    Rates rates() const;
    struct RecoveryStatus {bool active=false;unsigned attempts=0;std::wstring message;};
    RecoveryStatus recoveryStatus()const {std::lock_guard lock(mutex_);return recovery_;}
    // Experiment VEYRA_EXP_RP_HEALTH: the newest five-second connection health
    // snapshot (the "[remoteplay-health]" log line as values). valid is false
    // while the switch is off or before the first interval has passed.
    remoteplay::HealthSnapshot connectionHealth()const {std::lock_guard lock(mutex_);return health_;}
    sink::CaptureAudioState audioState() const { return audio_.snapshot(); }
    void setAudioGain(float value) { audio_.setGain(value); }
    void setAudioSync(unsigned mode, int offset) { audio_.setSync(mode, offset); }
    // realArrival: host time the shown REAL frame's data arrived (the engine's own record for that frame);
    // 0 for a generated or re-read frame. Only experiment VEYRA_EXP_RP_AUDIO_OWN_CLOCK passes it on;
    // without the switch the call is the one made before.
    void videoPresented(double pts, int64_t host, int64_t realArrival = 0) {
        if(realArrival>0&&audioOwnClock_.load(std::memory_order_relaxed))audio_.videoPresented(pts, host, realArrival);
        else audio_.videoPresented(pts, host);
    }
    void videoReset(bool resetAudio=true) { audio_.videoReset(resetAudio); }
    // Auto-reset Win32 event (a HANDLE), set when a decoded frame is published:
    // the engine loop waits on it instead of polling read() in timer slices,
    // as it already does for a capture card. Null when the event could not be
    // created or VEYRA_TEST_RP_LEGACY_TIMING / _ENGINE_WAKE asks for polling.
    void* frameEvent() const noexcept { return engineWake_ ? frameEvent_ : nullptr; }
    // Experiments VEYRA_EXP_RP_STATS / VEYRA_EXP_RP_DELIVERY (FrameDelivery.h):
    // what the mailbox cannot know, from the engine thread (display refresh,
    // frame generation on). Two relaxed atomic stores; read at pickup.
    void deliveryContext(double refreshHz,bool frameGeneration) noexcept {
        deliveryRefreshHz_.store(refreshHz,std::memory_order_relaxed);deliveryFrameGeneration_.store(frameGeneration,std::memory_order_relaxed);
    }
    // Experiment VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS (engine/PushDeviceLossResume.h): called by the
    // engine before close() when the switch is on. If the owner loop then ends because close() stopped
    // it (not a failure), it keeps its connect description (without the lost decode device) instead of
    // destroying it; takeResume() moves it out exactly once. Never called without the switch.
    void keepForResume();
    std::optional<RemotePlayConnectDesc> takeResume();
private:
    friend struct RemotePlaySessionSourceTestAccess;
    void publishDecoded(const AVFrame*,pipeline::FramePacket,const SourceInfo&);
    void run(std::stop_token, RemotePlayConnectDesc);
    remoteplay::ControllerState takeControllerLocked(remoteplay::HostTime now);
    struct Frame { std::shared_ptr<AVFrame> frame; pipeline::FramePacket packet; SourceInfo info; };
    // Two-slot path, only with VEYRA_EXP_RP_STATS or VEYRA_EXP_RP_DELIVERY set.
    void publishDelivery(Frame item);
    SourceReadStatus readDelivery(pipeline::FramePacket&,const AVFrame**);
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::jthread owner_;
    std::atomic<bool> alive_{false};
    bool initialized_=false, started_=false, failed_=false;
    std::optional<Frame> latest_;
    std::shared_ptr<AVFrame> view_;
    SourceInfo info_, publishedInfo_;
    remoteplay::SessionInbox::Snapshot snapshot_;
    remoteplay::HealthSnapshot health_;
    std::shared_ptr<const remoteplay::SessionInbox> telemetryInbox_;
    // Pad states from the UI poll to the owner; see ControllerQueue for the
    // two policies. The owner waits on wake_ for a video sample, a new pad
    // state, a login PIN or stop, instead of sleeping in 1 ms slices.
    remoteplay::ControllerQueue controllers_;
    std::shared_ptr<remoteplay::WakeSignal> wake_;
    void* frameEvent_=nullptr;
    bool ownerWake_=true, engineWake_=true;
    std::string pin_;
    uint64_t skipped_=0;
    Rates rates_;
    RecoveryStatus recovery_;
    uint64_t publishedSequence_=0;
    remoteplay::ControllerFeedback feedback_;
    sink::CaptureAudioSession audio_;
    // Frame delivery experiments, read once at construction. deliveryPath_ is
    // false with both switches unset: latest_ and the original code are used.
    bool deliveryStats_=false,deliveryPath_=false;
    remoteplay::DeliveryPolicy deliveryPolicy_=remoteplay::DeliveryPolicy::Newest;
    remoteplay::DeliverySlots<Frame> slots_;            // mutex_
    remoteplay::CatchUpGuard catchUpGuard_;             // mutex_ (engine thread in read)
    // Producer half: owner thread (publishDelivery, reconnect); consumer half:
    // engine thread (readDelivery). Lock-free between the two (SPSC ring).
    remoteplay::DeliveryStats deliveryStatistics_;
    std::atomic<double> deliveryRefreshHz_{0};
    std::atomic<bool> audioOwnClock_{false};
    std::atomic<bool> deliveryFrameGeneration_{true};
    // Under mutex_. Destroyed with the session if never taken: ~PairingCredentials wipes the secrets.
    engine::PushResumeSlot<RemotePlayConnectDesc> resume_;
};
}
