#pragma once
#include "RemotePlaySource.h"
#include "veyra/remoteplay/ControllerQueue.h"
#include "veyra/remoteplay/WakeSignal.h"
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
    sink::CaptureAudioState audioState() const { return audio_.snapshot(); }
    void setAudioGain(float value) { audio_.setGain(value); }
    void setAudioSync(unsigned mode, int offset) { audio_.setSync(mode, offset); }
    void videoPresented(double pts, int64_t host) { audio_.videoPresented(pts, host); }
    void videoReset(bool resetAudio=true) { audio_.videoReset(resetAudio); }
    // Auto-reset Win32 event (a HANDLE), set when a decoded frame is published:
    // the engine loop waits on it instead of polling read() in timer slices,
    // as it already does for a capture card. Null when the event could not be
    // created or VEYRA_TEST_RP_LEGACY_TIMING / _ENGINE_WAKE asks for polling.
    void* frameEvent() const noexcept { return engineWake_ ? frameEvent_ : nullptr; }
private:
    friend struct RemotePlaySessionSourceTestAccess;
    void publishDecoded(const AVFrame*,pipeline::FramePacket,const SourceInfo&);
    void run(std::stop_token, RemotePlayConnectDesc);
    remoteplay::ControllerState takeControllerLocked(remoteplay::HostTime now);
    struct Frame { std::shared_ptr<AVFrame> frame; pipeline::FramePacket packet; SourceInfo info; };
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::jthread owner_;
    std::atomic<bool> alive_{false};
    bool initialized_=false, started_=false, failed_=false;
    std::optional<Frame> latest_;
    std::shared_ptr<AVFrame> view_;
    SourceInfo info_, publishedInfo_;
    remoteplay::SessionInbox::Snapshot snapshot_;
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
};
}
