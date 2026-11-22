// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
namespace veyra::remoteplay {
// Latched wake-up for the Remote Play owner thread. The owner used to sleep in
// 1 ms slices between polls of the video inbox and send the controller on a
// 4 ms tick; a frame or a button press then waited for the rest of the slice
// (longer when the system timer runs at its 15.6 ms default). notify() from a
// producer (Chiaki video callback, UI controller poll, login PIN, stop) ends
// the wait at once. A notify that arrives while the owner is working is kept:
// the next wait returns immediately, so no event is lost between a poll that
// found nothing and the wait that follows it.
// Producers never block on the consumer: notify() holds the mutex only to set
// the flag. One consumer; any number of producers.
class WakeSignal {
public:
    void notify() noexcept {
        {std::lock_guard lock(mutex_);pending_=true;++notified_;}
        cv_.notify_one();
    }
    // True when a notify ended the wait (or was already pending), false on timeout.
    template<class Rep,class Period> bool waitFor(std::chrono::duration<Rep,Period> timeout) {
        std::unique_lock lock(mutex_);
        const bool woke=cv_.wait_for(lock,timeout,[&]{return pending_;});
        pending_=false;
        return woke;
    }
    // Drops a pending notify without waiting (start of a new session attempt).
    void clear() noexcept {std::lock_guard lock(mutex_);pending_=false;}
    std::uint64_t notified() const noexcept {std::lock_guard lock(mutex_);return notified_;}
private:
    mutable std::mutex mutex_;std::condition_variable cv_;bool pending_=false;std::uint64_t notified_=0;
};
} // namespace veyra::remoteplay
