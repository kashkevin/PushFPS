// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Experiment VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1 (off unless set to exactly 1): after the GPU device
// of a PS5 session is lost (driver reset, device removed), the engine starts the same session again by
// itself. Portable policy only; the Windows glue is in EngineController::run() (teardown) and
// RemotePlaySessionSource (keepForResume / takeResume). Unverified on Windows and on a PS5.
//
// Rules (docs/PS5_REMOTE_PLAY_EXPERIMENTS.md, "Resume after a GPU device loss"):
// - only after a device loss of a PS5 session whose session ended because the engine closed it;
// - never when the user pressed Stop, opened something else, or the app is closing; never when the
//   session ended on its own (StreamRecovery gave up, the console refused) or the engine threw;
// - never while the session's owner thread (and with it StreamRecovery) still runs;
// - at most kMaxResumes in any kWindowMs (sliding), each after kBackoffMs[n] and once a new device can
//   be created;
// - the connect description (pairing secrets) stays in memory, moves exactly once, is never copied or
//   logged, and is wiped (PairingCredentials' destructor) on every path that does not resume.
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace veyra::engine {

// "1" enables; anything else (unset, 0, on, true) leaves the original behaviour.
inline bool pushDeviceLossResumeSwitchedOn(std::wstring_view value) noexcept { return value==L"1"; }

// What run()'s teardown knows when the session has been closed.
struct PushDeviceLossFacts {
    bool remoteSession=false;     // this run was a PS5 Remote Play session
    bool deviceLost=false;        // the run's D3D12 device reported removal at teardown
    bool userStopped=false;       // stop() was called (transport Stopping)
    bool otherSource=false;       // another open was queued, or PS5 controller-only was requested
    bool appExit=false;           // the engine is shutting down
    bool engineError=false;       // run() caught an exception
    bool sessionFailed=false;     // the session ended on its own (recovery gave up, refused, threw)
    bool sessionOwnerAlive=false; // the session's owner thread (StreamRecovery) still runs
    bool haveDescription=false;   // takeResume() handed over the connect description
};

enum class PushResumeReason {
    Resume, NotRemote, NoDeviceLoss, UserStop, OtherSource, AppExit, EngineError, SessionEnded,
    RecoveryRunning, NoDescription, Budget, DeviceUnavailable
};

// The word in "[remoteplay-recovery] device-loss not resumed reason=<word>".
constexpr const char* pushResumeReasonWord(PushResumeReason r) noexcept {
    switch(r){
    case PushResumeReason::Resume: return "resume";
    case PushResumeReason::NotRemote: return "not-remote";
    case PushResumeReason::NoDeviceLoss: return "no-device-loss";
    case PushResumeReason::UserStop: return "user-stop";
    case PushResumeReason::OtherSource: return "other-source";
    case PushResumeReason::AppExit: return "app-exit";
    case PushResumeReason::EngineError: return "engine-error";
    case PushResumeReason::SessionEnded: return "session-ended";
    case PushResumeReason::RecoveryRunning: return "recovery-running";
    case PushResumeReason::NoDescription: return "no-description";
    case PushResumeReason::Budget: return "budget";
    case PushResumeReason::DeviceUnavailable: return "device-unavailable";
    }
    return "unknown";
}

// The conditions, user actions first. The attempt budget is checked afterwards (PushDeviceLossResumeBudget).
constexpr PushResumeReason decidePushDeviceLossResume(const PushDeviceLossFacts& f) noexcept {
    if(f.appExit) return PushResumeReason::AppExit;
    if(f.otherSource) return PushResumeReason::OtherSource;
    if(f.userStopped) return PushResumeReason::UserStop;
    if(!f.remoteSession) return PushResumeReason::NotRemote;
    if(!f.deviceLost) return PushResumeReason::NoDeviceLoss;
    if(f.engineError) return PushResumeReason::EngineError;
    if(f.sessionFailed) return PushResumeReason::SessionEnded;
    if(f.sessionOwnerAlive) return PushResumeReason::RecoveryRunning;
    if(!f.haveDescription) return PushResumeReason::NoDescription;
    return PushResumeReason::Resume;
}

// The session keeps its description only when the engine asked for it (after a device loss) and
// the owner loop ended because close() requested the stop, not because the session failed.
constexpr bool pushKeepForResume(bool armed, bool stopRequested, bool failed) noexcept {
    return armed&&stopRequested&&!failed;
}

// The interface's view (PlayerSnapshot::deviceLossResumePending). Set at the device loss, before the run
// publishes its failure, when the switch is on, the run is a PS5 session and the budget has room (plan(),
// nothing spent); cleared by the resume's snapshot reset when it is posted, and at teardown on every path
// that does not resume (another condition, the budget, no usable device, stop, a new open, app exit).
constexpr bool pushResumePendingAtLoss(bool switchOn, bool remoteSession, bool budgetRoom) noexcept {
    return switchOn&&remoteSession&&budgetRoom;
}
// Whether the bridge shows a live source's failure (notice, back to the start page) now: on the edge to
// failed while no resume is pending, or when a pending resume is abandoned with the run still failed.
// With the switch off `pending` is never true and this is the original `!wasFailed && failed`.
constexpr bool pushLiveFailureShownNow(bool wasFailed, bool wasPending, bool failed, bool pending) noexcept {
    return failed&&!pending&&(!wasFailed||wasPending);
}

// At most kMaxResumes in any kWindowMs; the n-th resume in the window waits kBackoffMs[n-1].
// Its own mutex: safe from any thread, though only the engine's owner thread uses it.
class PushDeviceLossResumeBudget {
public:
    static constexpr unsigned kMaxResumes=3;
    static constexpr int64_t kWindowMs=10*60*1000;
    static constexpr std::array<int64_t,kMaxResumes> kBackoffMs{1000,3000,10000};
    static constexpr int64_t kDeviceWaitMs=10000; // after the backoff, how long a new device may take
    static constexpr int64_t kDeviceProbeMs=250;
    struct Plan { unsigned attempt=0; int64_t delayMs=0; };
    // The next resume if the budget allows one; nothing is spent until commit().
    std::optional<Plan> plan(int64_t nowMs) {
        std::lock_guard lock(mutex_);prune(nowMs);
        if(times_.size()>=kMaxResumes) return std::nullopt;
        return Plan{unsigned(times_.size())+1u,kBackoffMs[times_.size()]};
    }
    // The resume was posted.
    void commit(int64_t nowMs) {
        std::lock_guard lock(mutex_);prune(nowMs);
        if(times_.size()<kMaxResumes) times_.push_back(nowMs);
    }
    unsigned used(int64_t nowMs) { std::lock_guard lock(mutex_);prune(nowMs);return unsigned(times_.size()); }
private:
    void prune(int64_t nowMs) { while(!times_.empty()&&nowMs-times_.front()>=kWindowMs) times_.pop_front(); }
    std::mutex mutex_;
    std::deque<int64_t> times_;
};

// Waits up to `delay` on `cv` (the caller holds `lock`; it is released while waiting) unless
// cancelled() becomes true. The wake sources are the notifies of whoever changes what cancelled()
// reads (stop, a new open, shutdown); the timeout bounds the wait. True when the whole delay passed.
template<class Pred>
bool pushResumeBackoff(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                       std::chrono::milliseconds delay, Pred cancelled) {
    return !cv.wait_for(lock,delay,cancelled);
}

// Holds the connect description between the end of the session and the engine's resume. Move-only:
// put in once, taken out once; whatever is still held is destroyed with the slot (T's destructor
// wipes the secrets). The owner of the slot provides the lock.
template<class T>
class PushResumeSlot {
public:
    PushResumeSlot()=default;
    PushResumeSlot(const PushResumeSlot&)=delete;
    PushResumeSlot& operator=(const PushResumeSlot&)=delete;
    void arm() noexcept { armed_=true; }
    bool armed() const noexcept { return armed_; }
    bool holding() const noexcept { return value_.has_value(); }
    void keep(T&& value) { value_.emplace(std::move(value)); }
    // Exactly once: the second call returns nothing.
    std::optional<T> take() {
        std::optional<T> out;
        if(value_){ out.emplace(std::move(*value_)); value_.reset(); }
        armed_=false;
        return out;
    }
private:
    bool armed_=false;
    std::optional<T> value_;
};
static_assert(!std::is_copy_constructible_v<PushResumeSlot<int>>);

}
