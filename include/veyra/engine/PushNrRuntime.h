// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Slipstream: which runtime the experimental neural layer ("AI picture enhancement", NVIDIA's DLSSNR model applied
// to a decoded video stream) may use, why none can run, and whether a file offered for installation is an
// NVIDIA-signed original. Pure: no Windows, no engine types. The Windows facts (WinVerifyTrust, the certificate's
// subject fields, the version resource, SHA-256) are gathered in src/ui/bridge/PushNeural.cpp and judged here.
// Decisions use capability answers only (the GPU supports the runtime yes/no, the driver is new enough yes/no/unknown),
// never a card model or device id. Tests: tests/remoteplay/NrRuntimeTests.inc.
#include "veyra/engine/PushPipelineHook.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace veyra::engine {

// Runtime slots under runtime\experimental (the ids of engine::NrRuntime: 0 and 1 are retired and mean 2).
enum class PushNrSlot : std::uint8_t { Sf = 2, Original = 3, Amd = 4 };
constexpr PushNrSlot pushNrSlotOf(int runtimeId) {
    return runtimeId == 3 ? PushNrSlot::Original : runtimeId == 4 ? PushNrSlot::Amd : PushNrSlot::Sf;
}
constexpr int pushNrRuntimeId(PushNrSlot slot) { return int(slot); }
constexpr std::string_view pushNrSlotName(PushNrSlot slot) {
    switch (slot) {
    case PushNrSlot::Sf: return "sf";
    case PushNrSlot::Original: return "original";
    case PushNrSlot::Amd: return "amd";
    }
    return "sf";
}

// What Windows said about a file's signature, reduced to what the decision needs.
enum class PushNrTrust : std::uint8_t {
    Unchecked,          // not verified (no file, or the check could not run)
    Valid,              // WinVerifyTrust: ERROR_SUCCESS
    NotSigned,          // TRUST_E_NOSIGNATURE and relatives
    HashMismatch,       // TRUST_E_BAD_DIGEST: signed, but the content was changed after signing
    Untrusted,          // CERT_E_UNTRUSTEDROOT, CERT_E_CHAINING, TRUST_E_EXPLICIT_DISTRUST, other chain errors
    Expired,            // CERT_E_EXPIRED (a certificate out of date without a valid timestamp)
    Revoked,            // CERT_E_REVOKED
    RevocationUnchecked,// CERT_E_REVOCATION_FAILURE / CRYPT_E_REVOCATION_OFFLINE when the check was required
    OtherError
};
constexpr std::string_view pushNrTrustName(PushNrTrust t) {
    switch (t) {
    case PushNrTrust::Unchecked: return "unchecked";
    case PushNrTrust::Valid: return "valid";
    case PushNrTrust::NotSigned: return "not-signed";
    case PushNrTrust::HashMismatch: return "hash-mismatch";
    case PushNrTrust::Untrusted: return "untrusted-chain";
    case PushNrTrust::Expired: return "expired";
    case PushNrTrust::Revoked: return "revoked";
    case PushNrTrust::RevocationUnchecked: return "revocation-unchecked";
    case PushNrTrust::OtherError: return "error";
    }
    return "error";
}

using PushNrVersion = std::array<std::uint16_t, 4>;
inline std::string pushNrVersionText(const PushNrVersion& v) {
    if (v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0) return "none";
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]) + "." + std::to_string(v[3]);
}

// Versions of nvngx_dlssnr.dll the project has records for: the file NVIDIA signed, byte for byte. A version that
// is not here can still be accepted when Windows says NVIDIA signed it; it is then "not tested with this app".
struct PushNrKnownBuild { PushNrVersion version; std::string_view sha256; std::uint64_t bytes; };
inline constexpr std::array<PushNrKnownBuild, 1> kPushNrKnownBuilds{{
    {{310, 8, 0, 0}, "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e", 165840496ull},
}};
constexpr char pushNrLower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
constexpr bool pushNrEqualNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (pushNrLower(a[i]) != pushNrLower(b[i])) return false;
    return true;
}
constexpr bool pushNrContainsNoCase(std::string_view text, std::string_view word) {
    if (word.empty() || word.size() > text.size()) return word.empty();
    for (std::size_t i = 0; i + word.size() <= text.size(); ++i)
        if (pushNrEqualNoCase(text.substr(i, word.size()), word)) return true;
    return false;
}
// The record for this version, or nullptr.
inline const PushNrKnownBuild* pushNrKnownBuild(const PushNrVersion& v) {
    for (const auto& b : kPushNrKnownBuilds)
        if (b.version == v) return &b;
    return nullptr;
}

// The record whose bytes these are (SHA-256 and size both equal), or nullptr. Such a file is identified by its
// bytes: the record was made from NVIDIA's own signed file, so its version strings are not needed to name it.
inline const PushNrKnownBuild* pushNrKnownBuildByBytes(std::string_view sha256, std::uint64_t bytes) {
    if (sha256.empty()) return nullptr;
    for (const auto& b : kPushNrKnownBuilds)
        if (b.bytes == bytes && pushNrEqualNoCase(sha256, b.sha256)) return &b;
    return nullptr;
}

// The signer: the leaf certificate's subject fields as Windows decodes them (CertGetNameStringW with
// CERT_NAME_ATTR_TYPE, szOID_COMMON_NAME and szOID_ORGANIZATION_NAME), compared whole, never as a substring
// of a longer string.
constexpr bool pushNrSignerIsNvidia(std::string_view commonName, std::string_view organization) {
    return commonName == "NVIDIA Corporation" && organization == "NVIDIA Corporation";
}

// Everything known about one file. Strings are UTF-8 as read from the version resource.
struct PushNrFileFacts {
    bool exists = false;
    std::uint64_t bytes = 0;
    PushNrTrust trust = PushNrTrust::Unchecked;
    bool signerNvidia = false;            // pushNrSignerIsNvidia on the leaf certificate
    std::string_view originalFilename, internalName, productName, fileDescription, companyName;
    PushNrVersion version{};
    std::string_view sha256;              // lower-case hex, empty when not computed
};

// Why a file is refused (or None when it is accepted).
enum class PushNrRefusal : std::uint8_t {
    None, Missing, NotSigned, HashMismatch, Untrusted, Expired, Revoked, RevocationUnchecked, OtherSigner,
    WrongFile, SizeOutOfRange, KnownVersionChanged, Unchecked, SignatureError
};
constexpr std::string_view pushNrRefusalName(PushNrRefusal r) {
    switch (r) {
    case PushNrRefusal::None: return "none";
    case PushNrRefusal::Missing: return "missing";
    case PushNrRefusal::NotSigned: return "not-signed";
    case PushNrRefusal::HashMismatch: return "hash-mismatch";
    case PushNrRefusal::Untrusted: return "untrusted-chain";
    case PushNrRefusal::Expired: return "expired";
    case PushNrRefusal::Revoked: return "revoked";
    case PushNrRefusal::RevocationUnchecked: return "revocation-unchecked";
    case PushNrRefusal::OtherSigner: return "other-signer";
    case PushNrRefusal::WrongFile: return "wrong-file";
    case PushNrRefusal::SizeOutOfRange: return "size-out-of-range";
    case PushNrRefusal::KnownVersionChanged: return "known-version-hash-differs";
    case PushNrRefusal::Unchecked: return "unchecked";
    case PushNrRefusal::SignatureError: return "signature-error";
    }
    return "signature-error";
}
inline constexpr std::uint64_t kPushNrMinBytes = 4ull << 20;   // the runtime carries its model: never a few KB
inline constexpr std::uint64_t kPushNrMaxBytes = 2ull << 30;

struct PushNrVerdict {
    bool accepted = false;
    PushNrRefusal refusal = PushNrRefusal::Unchecked;
    bool tested = false;                  // the version and its SHA-256 match a record above
};
// The version resource must name this very file (OriginalFilename or InternalName nvngx_dlssnr[.dll]), from
// NVIDIA. A product name alone is not enough: NVIDIA signs other DLSS modules too (nvngx_dlss.dll, the upscaler).
constexpr bool pushNrNamesDlssnr(const PushNrFileFacts& f) {
    const bool named = pushNrEqualNoCase(f.originalFilename, "nvngx_dlssnr.dll") || pushNrEqualNoCase(f.originalFilename, "nvngx_dlssnr") ||
                       pushNrEqualNoCase(f.internalName, "nvngx_dlssnr.dll") || pushNrEqualNoCase(f.internalName, "nvngx_dlssnr");
    return named && pushNrContainsNoCase(f.companyName, "NVIDIA");
}
// Checked first, before any signature check or hash: a file of the wrong size is not read at all.
constexpr bool pushNrSizeInRange(std::uint64_t bytes) { return bytes >= kPushNrMinBytes && bytes <= kPushNrMaxBytes; }
inline PushNrVerdict pushNrJudgeFile(const PushNrFileFacts& f) {
    PushNrVerdict v;
    const auto refuse = [&](PushNrRefusal r) { v.accepted = false; v.refusal = r; v.tested = false; return v; };
    if (!f.exists) return refuse(PushNrRefusal::Missing);
    if (!pushNrSizeInRange(f.bytes)) return refuse(PushNrRefusal::SizeOutOfRange);
    switch (f.trust) {
    case PushNrTrust::Valid: break;
    case PushNrTrust::Unchecked: return refuse(PushNrRefusal::Unchecked);
    case PushNrTrust::NotSigned: return refuse(PushNrRefusal::NotSigned);
    case PushNrTrust::HashMismatch: return refuse(PushNrRefusal::HashMismatch);
    case PushNrTrust::Untrusted: return refuse(PushNrRefusal::Untrusted);
    case PushNrTrust::Expired: return refuse(PushNrRefusal::Expired);
    case PushNrTrust::Revoked: return refuse(PushNrRefusal::Revoked);
    case PushNrTrust::RevocationUnchecked: return refuse(PushNrRefusal::RevocationUnchecked);
    case PushNrTrust::OtherError: return refuse(PushNrRefusal::SignatureError);
    }
    if (!f.signerNvidia) return refuse(PushNrRefusal::OtherSigner);
    // Still only after Windows verified the signature and the signer is NVIDIA: a recorded build's exact bytes
    // name the file even when its version strings say something else (the 8C field report, 21 November 2026).
    const PushNrKnownBuild* byBytes = pushNrKnownBuildByBytes(f.sha256, f.bytes);
    if (!byBytes && !pushNrNamesDlssnr(f)) return refuse(PushNrRefusal::WrongFile);
    if (byBytes) { v.accepted = true; v.refusal = PushNrRefusal::None; v.tested = true; return v; }
    if (const auto* known = pushNrKnownBuild(f.version)) {
        // A recorded version whose bytes differ is not that version, whatever its resource says.
        if (f.sha256.empty() || !pushNrEqualNoCase(f.sha256, known->sha256) || f.bytes != known->bytes)
            return refuse(PushNrRefusal::KnownVersionChanged);
        v.tested = true;
    }
    v.accepted = true;
    v.refusal = PushNrRefusal::None;
    return v;
}

// --- Which runtime the layer uses ---------------------------------------------------------------------------------
enum class PushNrTri : std::uint8_t { Unknown, No, Yes };
struct PushNrSlotFacts {
    bool present = false;
    bool signedOriginal = false;          // pushNrJudgeFile accepted it (only checked for the original slot)
    bool checked = false;                 // the check ran
    bool tested = false;                  // a recorded version
    PushNrVersion version{};
};
struct PushNrInstalled { PushNrSlotFacts sf, original, amd; };
// Capability answers about this PC; Hardware.cpp fills them from its existing gates. Never a card name.
struct PushNrGpu {
    bool supportsSf = false, supportsOriginal = false, supportsAmd = false;
    PushNrTri driverOk = PushNrTri::Unknown;    // Unknown is not a blocker: NGX reports an old driver itself at start
};
struct PushNrChoice {
    int requested = 2;                    // engine::NrRuntime id in force (0 and 1 mean 2)
    bool explicitChoice = false;          // the user picked it (pref pushNrRuntimeChosen); never changed then
};
enum class PushNrBlocker : std::uint8_t {
    None, RuntimeMissing, NotSignedOriginal, SignatureUnchecked, GpuNotSupported, DriverTooOld, RuntimeVersionUntested
};
constexpr std::string_view pushNrBlockerName(PushNrBlocker b) {
    switch (b) {
    case PushNrBlocker::None: return "none";
    case PushNrBlocker::RuntimeMissing: return "runtime-missing";
    case PushNrBlocker::NotSignedOriginal: return "not-signed-original";
    case PushNrBlocker::SignatureUnchecked: return "signature-unchecked";
    case PushNrBlocker::GpuNotSupported: return "gpu-not-supported";
    case PushNrBlocker::DriverTooOld: return "driver-too-old";
    case PushNrBlocker::RuntimeVersionUntested: return "runtime-version-untested";
    }
    return "none";
}
struct PushNrSlotStatus {
    bool usable = false;
    PushNrBlocker blocker = PushNrBlocker::RuntimeMissing;   // RuntimeVersionUntested comes with usable == true
};
constexpr const PushNrSlotFacts& pushNrFacts(const PushNrInstalled& in, PushNrSlot slot) {
    return slot == PushNrSlot::Original ? in.original : slot == PushNrSlot::Amd ? in.amd : in.sf;
}
// One slot on this PC. The original slot runs only an NVIDIA-signed original; the other slots keep the app's existing
// rules (their files are installed only by the user's own opt-in, -ExperimentalExtras Yes).
constexpr PushNrSlotStatus pushNrSlotStatus(const PushNrInstalled& in, const PushNrGpu& gpu, PushNrSlot slot) {
    const auto& f = pushNrFacts(in, slot);
    const bool supported = slot == PushNrSlot::Original ? gpu.supportsOriginal : slot == PushNrSlot::Amd ? gpu.supportsAmd : gpu.supportsSf;
    // The card first: where no runtime of this slot can run, a missing file is not the reason to name.
    if (!supported) return {false, PushNrBlocker::GpuNotSupported};
    if (!f.present) return {false, PushNrBlocker::RuntimeMissing};
    if (slot == PushNrSlot::Original) {
        if (!f.checked) return {false, PushNrBlocker::SignatureUnchecked};
        if (!f.signedOriginal) return {false, PushNrBlocker::NotSignedOriginal};
    }
    if (slot != PushNrSlot::Amd && gpu.driverOk == PushNrTri::No) return {false, PushNrBlocker::DriverTooOld};
    if (slot == PushNrSlot::Original && !f.tested) return {true, PushNrBlocker::RuntimeVersionUntested};
    return {true, PushNrBlocker::None};
}
struct PushNrDecision {
    PushNrSlot slot = PushNrSlot::Sf;     // what the layer uses
    bool usable = false;
    PushNrBlocker blocker = PushNrBlocker::RuntimeMissing;
    bool resolvedDefault = false;         // the untouched default moved to the signed original
};
// An explicit choice is never changed. The untouched default (slot 2) moves to the original slot only when its own
// file is absent and the original slot is usable (present, an NVIDIA-signed original, supported, driver not known to be
// too old). A file that is not a signed original is never chosen here.
constexpr PushNrDecision pushNrDecide(const PushNrInstalled& in, const PushNrGpu& gpu, const PushNrChoice& choice) {
    const PushNrSlot requested = pushNrSlotOf(choice.requested);
    const auto own = pushNrSlotStatus(in, gpu, requested);
    PushNrDecision d{requested, own.usable, own.blocker, false};
    if (choice.explicitChoice || requested != PushNrSlot::Sf || in.sf.present) return d;
    const auto original = pushNrSlotStatus(in, gpu, PushNrSlot::Original);
    if (original.usable) return {PushNrSlot::Original, true, original.blocker, true};
    // Nothing to move to: name what stands in the way of the runtime that could have been used (an installed
    // original that is refused; or, on a card that runs the original but not this slot, the missing original).
    if (in.original.present || (gpu.supportsOriginal && !own.usable && own.blocker == PushNrBlocker::GpuNotSupported)) d.blocker = original.blocker;
    return d;
}

// --- Stream "laghunt": the original-runtime layer at start-up --------------------------------------------------
// At start the app switches off a saved neural layer whose runtime cannot run (Hardware.cpp disableUnavailableEffects)
// for that session only and keeps it ON in the saved file (upstream's rule for every effect), so on the first start
// after nvngx_dlssnr.dll appeared (an installer "yes", a copy added in Settings) the neural layer ran again by itself:
// a heavy 4K stage nobody chose that day. The rule below never changes what the user saved (`enabled` stays as he
// saved it). For a layer on the NVIDIA original runtime (nr3) only, and only when the blocker needs a deliberate act to
// clear (the runtime file is missing, or it is not an NVIDIA-signed original), it sets a separate marker (preference
// pushNrHeldOff, PushFPS's own, keyed to the layer). While the marker is set the layer is kept off at start, also once
// the runtime is usable again (then logged once: "available again; switch it on to use it"); the user's own switch
// clears it. A blocker that can pass by itself (signature not checkable now, driver, GPU answer) sets no marker: the
// layer is off for that start only, as before. Other neural layers (nr2, nr4, upstream's) are never touched.
// VEYRA_TEST_NR_KEEP_SAVED_ON=1 restores the old behaviour (no marker read or written).
// This does NOT undo a setup where the saved session already has the layer on with a working runtime: that layer runs
// as before. It only prevents a future silent switch-on.
enum class PushNrHoldRule : std::uint8_t {
    NotOriginal,     // not an original-runtime layer: untouched
    KeepSavedOn,     // VEYRA_TEST_NR_KEEP_SAVED_ON=1: the old behaviour
    SavedOff,        // the user saved it off: nothing to hold (a stale marker is dropped)
    Runs,            // usable and not held: runs as saved
    Transient,       // cannot run now for a reason that can pass by itself: off for this start only, no marker
    MarkerSet,       // cannot run now and needs a deliberate act (file missing / not a signed original): marker set
    HeldWaiting,     // marker set earlier, still not usable: stays off
    AvailableAgain   // marker set earlier, usable now: stays off until the user switches it on (logged once)
};
constexpr std::string_view pushNrHoldRuleName(PushNrHoldRule r) {
    switch (r) {
    case PushNrHoldRule::NotOriginal: return "not-original";
    case PushNrHoldRule::KeepSavedOn: return "keep-saved-on";
    case PushNrHoldRule::SavedOff: return "saved-off";
    case PushNrHoldRule::Runs: return "runs";
    case PushNrHoldRule::Transient: return "transient";
    case PushNrHoldRule::MarkerSet: return "marker-set";
    case PushNrHoldRule::HeldWaiting: return "held-waiting";
    case PushNrHoldRule::AvailableAgain: return "available-again";
    }
    return "runs";
}
struct PushNrHoldInputs {
    bool originalLayer = false;   // the layer's saved runtime is the NVIDIA original (nr3)
    bool savedEnabled = false;    // as the user saved it
    bool usable = false;          // pushNrSlotStatus(..., Original).usable now
    PushNrBlocker blocker = PushNrBlocker::None;
    bool marked = false;          // the marker for this layer is set
    bool keepSavedOn = false;     // VEYRA_TEST_NR_KEEP_SAVED_ON=1
};
struct PushNrHoldDecision {
    PushNrHoldRule rule = PushNrHoldRule::NotOriginal;
    bool offThisStart = false;    // keep the layer off in the running chain (the saved file is not changed)
    bool marked = false;          // the marker afterwards
    bool announce = false;        // "available again; switch it on to use it" (once)
};
// Blockers that need a deliberate act (install / replace the file) to clear.
constexpr bool pushNrBlockerNeedsAct(PushNrBlocker b) {
    return b == PushNrBlocker::RuntimeMissing || b == PushNrBlocker::NotSignedOriginal;
}
constexpr PushNrHoldDecision pushNrStartupHold(const PushNrHoldInputs& in) {
    if (!in.originalLayer) return {PushNrHoldRule::NotOriginal, false, in.marked, false};
    if (in.keepSavedOn) return {PushNrHoldRule::KeepSavedOn, !in.usable && in.savedEnabled, in.marked, false};
    if (!in.savedEnabled) return {PushNrHoldRule::SavedOff, false, false, false};
    if (in.marked) return in.usable ? PushNrHoldDecision{PushNrHoldRule::AvailableAgain, true, true, true}
                                    : PushNrHoldDecision{PushNrHoldRule::HeldWaiting, true, true, false};
    if (in.usable) return {PushNrHoldRule::Runs, false, false, false};
    if (pushNrBlockerNeedsAct(in.blocker)) return {PushNrHoldRule::MarkerSet, true, true, false};
    return {PushNrHoldRule::Transient, true, false, false};
}
// After start-up: the user switched a held layer on (any of his own switches) -> its marker goes.
constexpr bool pushNrUserClears(bool marked, bool enabledNow) { return marked && enabledNow; }

// The marker (preference pushNrHeldOff): comma-separated keys "<configuration 0|1><c chain|e node editor><node index>-
// <layout print>", e.g. "0c1-0a1b2c3d"; at most kPushNrHoldMaxKeys keys; empty = no marker. Validated in Preferences.cpp
// with pushNrHoldValid. The node index alone would follow a POSITION: after he deletes, inserts or reorders nodes it could
// point at another layer and hold off a usable layer he chose. So each key carries the print of its node list's neural
// layout (pushNrLayoutPrint: the node count and the index and runtime of every neural node); a key whose list no longer
// has that print is dropped (logged in [nr-hold]), never applied to another node.
inline constexpr std::size_t kPushNrHoldMaxKeys = 8;
inline constexpr std::size_t kPushNrHoldMaxText = 128;
// FNV-1a over the list's node count and, per neural node, its index and runtime id. Chain: nodeCount, nodes[i];
// isNr(node) / runtimeOf(node) say what a node is (engine::EffectType::NrEnhance, engine::NrRuntime as an int).
template<class Chain, class IsNr, class RuntimeOf>
std::uint32_t pushNrLayoutPrint(const Chain& chain, IsNr isNr, RuntimeOf runtimeOf) {
    std::uint32_t h = 2166136261u;
    const auto mix = [&h](std::uint32_t v) {
        for (int b = 0; b < 4; ++b) { h ^= (v >> (8 * b)) & 0xFFu; h *= 16777619u; }
    };
    mix(static_cast<std::uint32_t>(chain.nodeCount));
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(chain.nodeCount); ++i)
        if (isNr(chain.nodes[i])) { mix(i); mix(static_cast<std::uint32_t>(runtimeOf(chain.nodes[i]))); }
    return h;
}
inline std::string pushNrHoldKey(unsigned configuration, bool editor, unsigned node, std::uint32_t print) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string k = std::to_string(configuration) + (editor ? "e" : "c") + std::to_string(node) + "-";
    for (int b = 7; b >= 0; --b) k += hex[(print >> (4 * b)) & 0xFu];
    return k;
}
struct PushNrHoldKeyParts { unsigned configuration = 0; bool editor = false; unsigned node = 0; std::uint32_t print = 0; };
inline std::optional<PushNrHoldKeyParts> pushNrHoldParse(std::string_view k) {
    const std::size_t dash = k.find('-');
    if (dash == std::string_view::npos || dash < 3 || dash > 4 || k.size() != dash + 9) return std::nullopt;
    if ((k[0] != '0' && k[0] != '1') || (k[1] != 'c' && k[1] != 'e')) return std::nullopt;
    PushNrHoldKeyParts p;
    p.configuration = unsigned(k[0] - '0');
    p.editor = k[1] == 'e';
    for (std::size_t i = 2; i < dash; ++i) {
        if (k[i] < '0' || k[i] > '9') return std::nullopt;
        p.node = p.node * 10u + unsigned(k[i] - '0');
    }
    for (std::size_t i = dash + 1; i < k.size(); ++i) {
        const char c = k[i];
        const unsigned v = c >= '0' && c <= '9' ? unsigned(c - '0') : c >= 'a' && c <= 'f' ? unsigned(c - 'a' + 10) : 16u;
        if (v > 15u) return std::nullopt;
        p.print = (p.print << 4) | v;
    }
    return p;
}
// Every key of a marker text, in order (empty when the text is not valid).
inline std::vector<std::string_view> pushNrHoldKeys(std::string_view text) {
    std::vector<std::string_view> keys;
    if (text.empty()) return keys;
    std::size_t pos = 0;
    while (true) {
        const std::size_t end = std::min(text.find(',', pos), text.size());
        keys.push_back(text.substr(pos, end - pos));
        if (end == text.size()) break;
        pos = end + 1;
    }
    return keys;
}
inline bool pushNrHoldValid(std::string_view text) {
    if (text.size() > kPushNrHoldMaxText) return false;
    const auto keys = pushNrHoldKeys(text);
    if (keys.size() > kPushNrHoldMaxKeys) return false;
    for (const auto k : keys)
        if (!pushNrHoldParse(k)) return false;
    return true;
}
// Whether the marker holds this node: a key for this configuration, list and index whose print is the list's print now.
// keyed: set when a key for this node exists at all (with any print), so a stale one can be reported as dropped.
inline bool pushNrHoldHas(std::string_view text, unsigned configuration, bool editor, unsigned node, std::uint32_t printNow,
                          bool* keyed = nullptr) {
    if (keyed) *keyed = false;
    for (const auto k : pushNrHoldKeys(text)) {
        const auto p = pushNrHoldParse(k);
        if (!p || p->configuration != configuration || p->editor != editor || p->node != node) continue;
        if (keyed) *keyed = true;
        if (p->print == printNow) return true;
    }
    return false;
}
inline std::string pushNrHoldJoin(const std::vector<std::string>& keys) {
    std::string out;
    for (std::size_t i = 0; i < keys.size() && i < kPushNrHoldMaxKeys; ++i) {
        if (i) out += ',';
        out += keys[i];
    }
    return out;
}
// After start-up, at each bridge tick while a marker exists: what one key says about the node list it belongs to now.
// Keep: still held. Dropped: that list's neural layout changed (nodes deleted, inserted, reordered, another runtime): the
// key no longer names the layer it was set for. ClearedByUser: its own node is on (his switch). The caller drops the
// whole marker on any ClearedByUser (the safe side: no layer stays held by a key that might be stale) and drops single
// keys that say Dropped.
enum class PushNrHoldCheck : std::uint8_t { Keep, Dropped, ClearedByUser };
template<class Chain, class IsNr, class RuntimeOf>
PushNrHoldCheck pushNrHoldCheckKey(const PushNrHoldKeyParts& key, const Chain* list, IsNr isNr, RuntimeOf runtimeOf) {
    if (!list || pushNrLayoutPrint(*list, isNr, runtimeOf) != key.print || key.node >= static_cast<unsigned>(list->nodeCount))
        return PushNrHoldCheck::Dropped;
    const auto& node = list->nodes[key.node];
    if (!isNr(node)) return PushNrHoldCheck::Dropped;
    return pushNrUserClears(true, node.enabled) ? PushNrHoldCheck::ClearedByUser : PushNrHoldCheck::Keep;
}

// The one [nr-hold] start-up line: the rule of every original-runtime layer looked at, the marker, and the keys dropped
// because their node list's layout changed.
inline std::string pushNrHoldLine(const std::vector<std::pair<std::string, PushNrHoldDecision>>& layers, std::string_view marker,
                                  PushNrBlocker blocker, bool keepSavedOn, std::string_view dropped = {}) {
    std::string s = "startup rule=held-off-until-switched-on";
    if (keepSavedOn) s = "startup rule=keep-saved-on (VEYRA_TEST_NR_KEEP_SAVED_ON=1, the old behaviour)";
    s += " originalReason=";
    s += pushNrBlockerName(blocker);
    s += " marker=";
    s += marker.empty() ? std::string("none") : std::string(marker);
    s += " layers=";
    if (layers.empty()) s += "none";
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (i) s += ',';
        s += layers[i].first + ":" + std::string(pushNrHoldRuleName(layers[i].second.rule));
    }
    if (!dropped.empty()) {
        s += " droppedLayoutChanged=";
        s += dropped;
    }
    return s;
}

// [nr-runtime-status] (contract item 3: its keys first, in its order; originalTested and resolvedDefault follow).
// Once per start, no paths, no names.
inline std::string pushNrStatusLine(const PushNrInstalled& in, const PushNrGpu& gpu, const PushNrDecision& d) {
    std::string s = "original=";
    s += in.original.present ? "present" : "absent";
    s += " originalVersion=" + (in.original.present ? pushNrVersionText(in.original.version) : std::string("none"));
    s += " originalSigned=";
    s += in.original.signedOriginal ? "1" : "0";
    s += " sf=";
    s += in.sf.present ? "present" : "absent";
    s += " selected=";
    s += pushNrSlotName(d.slot);
    s += " selectedAvailable=";
    s += d.usable ? "1" : "0";
    s += " gpuSupportsOriginal=";
    s += gpu.supportsOriginal ? "1" : "0";
    s += " reason=";
    s += pushNrBlockerName(d.blocker);
    s += " originalTested=";
    s += in.original.tested ? "1" : "0";
    s += " resolvedDefault=";
    s += d.resolvedDefault ? "1" : "0";
    return s;
}

// --- Strength of the neural layer -----------------------------------------------------------------------------------
// Every layer's tone / structure (Professional mode's Local tone / Local structure). The app passes both unchanged to
// the runtime's own tone-strength and structure-strength parameters (EnhanceGraph.cpp, the evaluate block), so the
// numbers are on the runtime's 0..1 scale. subtle, balanced and strong are the quick setup's looks
// (qml/Veyra/PushQuickSetup.qml `looks`); strong = 1 / 1 is what every layer starts with and stays so. conservative
// is half of NVIDIA's published RTX Remix defaults (RtxOptions.md rtx.dlssNeuralRendering: tone 0.3, structure 0.7),
// so clearly below both: the gentlest look, for the enhanced-1440 profile. Nothing here was judged on hardware.
inline constexpr float kPushNrRemixTone = 0.3f, kPushNrRemixStructure = 0.7f;
struct PushNrLook { std::string_view id; float tone, structure; };
inline constexpr std::array<PushNrLook, 4> kPushNrLooks{{
    {"conservative", 0.15f, 0.35f},
    {"subtle", 0.4f, 0.5f},
    {"balanced", 0.7f, 0.8f},
    {"strong", 1.0f, 1.0f},
}};
inline const PushNrLook* pushNrLook(std::string_view id) {
    for (const auto& l : kPushNrLooks)
        if (l.id == id) return &l;
    return nullptr;
}
// The look whose values these are (within 0.005, as the quick setup compares), or "" for custom values.
inline std::string_view pushNrLookOf(float tone, float structure) {
    for (const auto& l : kPushNrLooks) {
        const float dt = tone - l.tone, ds = structure - l.structure;
        if (dt < 0.005f && dt > -0.005f && ds < 0.005f && ds > -0.005f) return l.id;
    }
    return {};
}

// --- VEYRA_TEST_RP_PIPELINE (contract item 1): the neural layer's keys --------------------------------------------
// The hook itself (image keys, parser, log line) is engine/PushPipelineHook.h and src/ui/bridge/PushPipelineHook.cpp;
// the neural layer adds nr, nrRuntime, nrSize and nrLook as an extension (src/ui/bridge/PushNeural.cpp applies them).
// nrRuntime is applied right after the profile, so that "nr=1" written before "nrRuntime=3" is judged with the
// runtime the text asks for.
constexpr bool pushNrPipeKnown(std::string_view key) { return pipeHookOneOf(key, {"nr", "nrRuntime", "nrSize", "nrLook"}); }
inline bool pushNrPipeValueOk(std::string_view key, std::string_view value) {
    if (key == "nr") return pipeHookOneOf(value, {"0", "1"});
    if (key == "nrRuntime") return pipeHookOneOf(value, {"2", "3", "4"});
    if (key == "nrSize") return pipeHookOneOf(value, {"480", "720", "900", "1080", "1440", "native"});
    if (key == "nrLook") return pushNrLook(value) != nullptr;
    return false;
}
inline constexpr PipeHookExtension kPushNrPipeExtension{&pushNrPipeKnown, &pushNrPipeValueOk, "nrRuntime"};
// The whole hook with the neural keys (the names used before the split).
using PushPipeItem = PipeHookItem;
inline constexpr std::size_t kPushPipeMaxBytes = kPipeHookMaxBytes, kPushPipeMaxItems = kPipeHookMaxItems;
constexpr int pushPipeNumber(std::string_view value, int lowest, int highest) { return pipeHookNumber(value, lowest, highest); }
constexpr int pushPipeSrModel(std::string_view value) { return pipeHookSrModel(value); }
inline std::vector<PushPipeItem> pushPipeParse(std::string_view text) { return pipeHookParse(text, &kPushNrPipeExtension); }
inline std::string pushPipeLine(const std::vector<PushPipeItem>& items) { return pipeHookLine(items); }

// --- nrAuto by capability -------------------------------------------------------------------------------------------
// Automatic NR size needs a usable runtime. Upstream verified it on one configuration (verifiedHere); elsewhere it is
// offered only with VEYRA_EXP_NR_AUTO_CAPABILITY=1 and marked "not verified on this card". The graph's own measured
// headroom check (nr-auto-pool in EnhanceGraph.cpp) still decides the size at run time.
enum class PushNrAutoState : std::uint8_t { Available, AvailableUnverified, RuntimeUnusable, NotVerifiedHere };
constexpr PushNrAutoState pushNrAutoState(bool runtimeUsable, bool verifiedHere, bool capabilitySwitch) {
    if (!runtimeUsable) return PushNrAutoState::RuntimeUnusable;
    if (verifiedHere) return PushNrAutoState::Available;
    return capabilitySwitch ? PushNrAutoState::AvailableUnverified : PushNrAutoState::NotVerifiedHere;
}

}  // namespace veyra::engine
