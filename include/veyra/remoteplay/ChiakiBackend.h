// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "SessionInbox.h"
#include "TransportTuning.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>

namespace veyra::remoteplay {
// No serialization/ostream support: never put registration/session keys into
// command lines, logs, fixtures or source control. DPAPI storage is NOT in patch 01.
struct PairingCredentials {
    PairingCredentials() = default;
    PairingCredentials(const PairingCredentials&) = delete;
    PairingCredentials& operator=(const PairingCredentials&) = delete;
    PairingCredentials(PairingCredentials&&) noexcept;
    PairingCredentials& operator=(PairingCredentials&&) noexcept;
    AccountId accountId{};
    std::array<char,16> registrationKey{};
    std::array<std::uint8_t,16> sessionKey{};
    ~PairingCredentials();
};
// Inline (portable) so the Linux tests run the real wiping code (tests/remoteplay/DeviceLossResumeTests.inc).
namespace credentials_detail {
inline void wipe(void* ptr,std::size_t n) noexcept {
    auto* p=static_cast<volatile unsigned char*>(ptr);
    while(n--) *p++=0;
}
}
inline PairingCredentials::PairingCredentials(PairingCredentials&& other) noexcept { *this=std::move(other); }
inline PairingCredentials& PairingCredentials::operator=(PairingCredentials&& other) noexcept {
    if(this==&other) return *this;
    registrationKey=other.registrationKey;sessionKey=other.sessionKey;accountId=other.accountId;
    credentials_detail::wipe(other.registrationKey.data(),other.registrationKey.size());
    credentials_detail::wipe(other.sessionKey.data(),other.sessionKey.size());credentials_detail::wipe(other.accountId.data(),other.accountId.size());
    return *this;
}
inline PairingCredentials::~PairingCredentials(){credentials_detail::wipe(registrationKey.data(),registrationKey.size());credentials_detail::wipe(sessionKey.data(),sessionKey.size());credentials_detail::wipe(accountId.data(),accountId.size());}
struct BackendResult {
    bool ok=false;
    int code=0;
    // One of our fixed operation names, never an unfiltered server string.
    std::string operation;
};
struct NativeConnectRequest {
    std::string host;
    std::string consoleId; // Stable discovery host_id / registration MAC, normalized hex.
    VideoProfile video;
    bool viewOnly=false;
    PairingCredentials credentials;
};
struct NativeSnapshot {
    bool started=false,connected=false;
    std::uint64_t warnings=0,errors=0,videoCallbacks=0,audioCallbacks=0;
    int lastQuitReason=0,lastApiError=0;
    uint64_t callbackRejected=0,packetReceived=0,packetLost=0;
    uint64_t transportErrors=0,assemblyErrors=0;
    bool automaticRetryAllowed=true;
    bool startupRetryAllowed=false; // Only transient RP_IN_USE, never an auth rejection.
    int64_t serverTargetBitrate=-1; // Raw upstream field; diagnostic opt-in only.
    uint64_t qualityReports=0;
    uint64_t audioConcealed=0; // Opus frames concealed by Chiaki's audio receiver (packet-loss concealment).
};
BackendResult initializeChiaki();
// Experimental transport profile (chiaki patch 0004; see TransportTuning.h).
// Process-wide in the library, read when the next session connects: call on
// the session owner before ChiakiBackend::start(). A default TransportTuning
// asks for the library's own values. Also restarts the library's counters, and
// with threadPriority raises the Takion and feedback sender threads of
// sessions started afterwards. Never called when no VEYRA_EXP_RP_* transport
// switch is set, so the library then runs exactly as unpatched.
void configureTransport(const TransportTuning&);
// Any thread; plain atomic reads, no lock.
TransportCounters readTransportCounters() noexcept;
// Experiment VEYRA_EXP_RP_REORDER_ADAPTIVE: the AV reorder wait of the running
// connection, taken by the Takion thread the next time it looks at its video
// queue (one atomic store here, no lock). 0 = what the connection started
// with. configureTransport() clears it. Never called without the switch.
void setLiveReorderTimeout(std::uint32_t microseconds) noexcept;
class ChiakiBackend {
public:
    ChiakiBackend();
    ~ChiakiBackend();
    ChiakiBackend(const ChiakiBackend&)=delete;
    ChiakiBackend& operator=(const ChiakiBackend&)=delete;
    // All methods, including snapshot(), must be called from one session-owner thread,
    // NEVER from a Chiaki callback. This object is not a renderer or a decoder.
    // discardMedia (PS5 手柄控制): video samples are counted and accepted without
    // a copy, audio goes to a no-op sink, so Chiaki keeps its reference-frame
    // bookkeeping and never reports a dropped sample to the console as a
    // corrupt frame. The DualSense channel is forced on; feedback is unchanged.
    BackendResult start(const NativeConnectRequest&,SessionInbox::Token,bool discardMedia=false);
    BackendResult stop(); // Does not call PS5 goto_bed; must precede inbox.finishStop().
    BackendResult requestIdr();
    BackendResult submitController(const ControllerState&);
    BackendResult submitLoginPin(std::string_view);
    NativeSnapshot snapshot() const;
    ControllerFeedback takeFeedback();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
struct PairResult {
    BackendResult result;
    PairingCredentials credentials;
    std::array<std::uint8_t,6> mac{};
    std::string nickname;
    bool canceled=false,timedOut=false;
};
// Synchronous OWNERSHIP operation; run on a dedicated session/setup worker, not HWND.
// Cancellation stops the upstream registration and joins before returning.
PairResult pairLocalPs5(std::string host,const AccountId&,std::string_view eightDigitPin,
    std::stop_token stop={},std::chrono::milliseconds timeout=std::chrono::seconds(30));
} // namespace veyra::remoteplay
