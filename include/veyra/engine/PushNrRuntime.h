// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Slipstream: which runtime the experimental neural layer ("AI picture enhancement", NVIDIA's DLSSNR model applied
// to a decoded video stream) may use, why none can run, and whether a file offered for installation is an
// NVIDIA-signed original. Pure: no Windows, no engine types. The Windows facts (WinVerifyTrust, the certificate's
// subject fields, the version resource, SHA-256) are gathered in src/ui/bridge/PushNeural.cpp and judged here.
// Decisions use capability answers only (the GPU supports the runtime yes/no, the driver is new enough yes/no/unknown),
// never a card model or device id. Tests: tests/remoteplay/NrRuntimeTests.inc.
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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
// The version resource must name the file or NVIDIA's DLSS product, from NVIDIA.
constexpr bool pushNrNamesDlssnr(const PushNrFileFacts& f) {
    const bool named = pushNrEqualNoCase(f.originalFilename, "nvngx_dlssnr.dll") || pushNrEqualNoCase(f.internalName, "nvngx_dlssnr.dll") ||
                       pushNrEqualNoCase(f.internalName, "nvngx_dlssnr") ||
                       pushNrContainsNoCase(f.productName, "DLSS") || pushNrContainsNoCase(f.fileDescription, "DLSS");
    return named && pushNrContainsNoCase(f.companyName, "NVIDIA");
}
inline PushNrVerdict pushNrJudgeFile(const PushNrFileFacts& f) {
    PushNrVerdict v;
    const auto refuse = [&](PushNrRefusal r) { v.accepted = false; v.refusal = r; v.tested = false; return v; };
    if (!f.exists) return refuse(PushNrRefusal::Missing);
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
    if (!pushNrNamesDlssnr(f)) return refuse(PushNrRefusal::WrongFile);
    if (f.bytes < kPushNrMinBytes || f.bytes > kPushNrMaxBytes) return refuse(PushNrRefusal::SizeOutOfRange);
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

// --- VEYRA_TEST_RP_PIPELINE (contract item 1): key=value;key=value --------------------------------------------------
// Test-only, read from the environment by src/ui/bridge/PushNeural.cpp. The pure part is here: which keys exist, which
// values each takes, the order they are applied in (profile first, the others as written) and the one log line. The
// bridge applies each key through the setters the interface uses and names a key it cannot apply.
struct PushPipeItem {
    std::string key, value;
    std::string refusal;      // empty = accepted so far; else unknown-key, bad-value, duplicate, or the bridge's reason
    bool applied = false;
};
inline constexpr std::size_t kPushPipeMaxBytes = 512, kPushPipeMaxItems = 16;
constexpr bool pushPipeOneOf(std::string_view value, std::initializer_list<std::string_view> set) {
    for (const auto candidate : set)
        if (value == candidate) return true;
    return false;
}
// The number in [lowest, highest], or -1 (digits only, at most four).
constexpr int pushPipeNumber(std::string_view value, int lowest, int highest) {
    if (value.empty() || value.size() > 4) return -1;
    int number = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') return -1;
        number = number * 10 + (c - '0');
    }
    return number >= lowest && number <= highest ? number : -1;
}
// srModel: the letters and ids of the app's DLSS SR model choice (dlssSrModelChoices: 0 K, 1 M, 2 L); -1 otherwise.
constexpr int pushPipeSrModel(std::string_view value) {
    if (value == "k" || value == "0") return 0;
    if (value == "m" || value == "1") return 1;
    if (value == "l" || value == "2") return 2;
    return -1;
}
constexpr bool pushPipeKnownKey(std::string_view key) {
    return pushPipeOneOf(key, {"sr", "srModel", "srTarget", "clean", "nr", "nrRuntime", "nrSize", "nrLook", "fg", "fgMultiplier", "hdr", "profile"});
}
inline bool pushPipeValueOk(std::string_view key, std::string_view value) {
    if (key == "sr") return pushPipeOneOf(value, {"off", "dlss", "vsr-stream", "nis"});
    if (key == "srModel") return pushPipeSrModel(value) >= 0;
    if (key == "srTarget") return pushPipeOneOf(value, {"qhd", "4k"});
    if (key == "clean") return pushPipeNumber(value, 0, 100) >= 0;
    if (key == "nr" || key == "hdr") return pushPipeOneOf(value, {"0", "1"});
    if (key == "nrRuntime") return pushPipeOneOf(value, {"2", "3", "4"});
    if (key == "nrSize") return pushPipeOneOf(value, {"480", "720", "900", "1080", "1440", "native"});
    if (key == "nrLook") return pushNrLook(value) != nullptr;
    if (key == "fg") return pushPipeOneOf(value, {"off", "vfg", "dlss", "pushflow", "fsr"});
    if (key == "fgMultiplier") return pushPipeNumber(value, 2, 8) >= 0;
    if (key == "profile") return pushPipeOneOf(value, {"original", "clean-1440", "enhanced-1440", "max-fidelity", "ultra-low-latency"});
    return false;
}
// Every item of the text, the profile first. Nothing unknown is kept as written: a key that is not one of the
// contract's is logged as "invalid" when it has other characters than letters, digits and '-'.
inline std::vector<PushPipeItem> pushPipeParse(std::string_view text) {
    std::vector<PushPipeItem> items;
    if (text.size() > kPushPipeMaxBytes) { items.push_back({"plan", "", "too-long", false}); return items; }
    const auto trim = [](std::string_view part) {
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.remove_prefix(1);
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.remove_suffix(1);
        return part;
    };
    const auto plainWord = [](std::string_view word) {
        if (word.empty() || word.size() > 24) return false;
        for (const char c : word)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
        return true;
    };
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find(';', begin);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view part = trim(text.substr(begin, end - begin));
        begin = end + 1;
        if (part.empty()) continue;
        if (items.size() >= kPushPipeMaxItems) { items.push_back({"more", "", "too-many", false}); break; }
        const std::size_t equals = part.find('=');
        const std::string_view key = trim(part.substr(0, equals));
        PushPipeItem item;
        item.key = plainWord(key) ? std::string(key) : std::string("invalid");
        if (!pushPipeKnownKey(key)) { item.refusal = "unknown-key"; items.push_back(std::move(item)); continue; }
        std::string value(equals == std::string_view::npos ? std::string_view{} : trim(part.substr(equals + 1)));
        for (char& c : value) c = pushNrLower(c);
        if (!pushPipeValueOk(key, value)) { item.refusal = key == "srModel" ? "unknown-model" : "bad-value"; items.push_back(std::move(item)); continue; }
        item.value = std::move(value);
        for (const auto& earlier : items)
            if (earlier.key == item.key && earlier.refusal.empty()) item.refusal = "duplicate";
        items.push_back(std::move(item));
    }
    // The profile first; the other keys keep their order and override it.
    for (std::size_t i = 1; i < items.size(); ++i)
        if (items[i].key == "profile" && items[i].refusal.empty()) {
            PushPipeItem profile = std::move(items[i]);
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
            items.insert(items.begin(), std::move(profile));
            break;
        }
    return items;
}
// [slipstream-pipeline] applied=<n> refused=<n> detail=<key:ok|key:refused:reason,...>
inline std::string pushPipeLine(const std::vector<PushPipeItem>& items) {
    std::size_t applied = 0, refused = 0;
    std::string detail;
    for (const auto& item : items) {
        if (!detail.empty()) detail += ',';
        detail += item.key;
        if (item.applied && item.refusal.empty()) { ++applied; detail += ":ok"; }
        else { ++refused; detail += ":refused:" + (item.refusal.empty() ? std::string("not-applied") : item.refusal); }
    }
    return "applied=" + std::to_string(applied) + " refused=" + std::to_string(refused) + " detail=" + (detail.empty() ? std::string("none") : detail);
}

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
