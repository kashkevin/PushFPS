// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace veyra::engine {
// What this PC can do, as capabilities, and the safe defaults and fallbacks that follow from them.
//
// Portable and header-only: no Windows, no Qt, no engine types (EnhancementSettings.h pulls in headers
// that are not clean under the strict test flags). The enums below carry the same numbers as the engine's
// FrameGenerationBackend and OpticalFlowBackend; src/ui/bridge/Hardware.cpp static_asserts that, and that
// pushFgMultiplierSelectable() agrees with displayFgMultiplierSelectable() for every method and multiplier.
// Tests: tests/remoteplay/GpuBudgetTests.inc. Unverified on Windows.
//
// Rules (docs/HARDWARE_COMPATIBILITY.md):
//   - Everything is a capability the PC reported, never a model name or a per-GPU constant. A vendor-specific
//     method is chosen only when its own availability bit says it exists.
//   - Unknown is never treated as present. Unknown input gives the most conservative choice that still lets
//     Remote Play run: H.264, SDR, frame generation off.
//   - The result is a recommendation with machine-readable reasons. Nothing here changes a setting: the
//     caller decides whether to apply it (the engine reads none of it yet).

enum class PushGpuVendor : std::uint8_t { Unknown = 0, Nvidia = 1, Amd = 2, Intel = 3, Other = 4 };
constexpr PushGpuVendor pushGpuVendorFromPciId(std::uint32_t vendorId) {
    switch (vendorId) {
    case 0: return PushGpuVendor::Unknown;
    case 0x10DE: return PushGpuVendor::Nvidia;
    case 0x1002: return PushGpuVendor::Amd;
    case 0x8086: return PushGpuVendor::Intel;
    default: return PushGpuVendor::Other;
    }
}
constexpr std::string_view pushGpuVendorName(PushGpuVendor vendor) {
    switch (vendor) {
    case PushGpuVendor::Nvidia: return "nvidia";
    case PushGpuVendor::Amd: return "amd";
    case PushGpuVendor::Intel: return "intel";
    case PushGpuVendor::Other: return "other";
    case PushGpuVendor::Unknown: break;
    }
    return "unknown";
}

// A capability that was asked for and answered, answered no, or could not be asked.
enum class PushCapability : std::uint8_t { Unknown = 0, No = 1, Yes = 2 };
constexpr PushCapability pushCapability(bool known, bool value) {
    return !known ? PushCapability::Unknown : value ? PushCapability::Yes : PushCapability::No;
}
enum class PushPowerSource : std::uint8_t { Unknown = 0, Mains = 1, Battery = 2 };
// Same numbers as the bridge's vrrState (PushModes.js): 0 unknown, 1 the screen cannot, 2 it can but not now,
// 3 possible, 4 active.
enum class PushVrrState : std::uint8_t { Unknown = 0, Unsupported = 1, NotNow = 2, Possible = 3, Active = 4 };
constexpr PushVrrState pushVrrState(int bridgeValue) {
    return bridgeValue >= 0 && bridgeValue <= 4 ? static_cast<PushVrrState>(bridgeValue) : PushVrrState::Unknown;
}
// VEYRA_EXP_VRR_DXGI_FALLBACK=1 (gfx::queryVrr, NvidiaVrr.h): when NVAPI cannot be asked at all (no NVIDIA driver, or
// its initialisation failed), what DXGI says about tearing support (DXGI_FEATURE_PRESENT_ALLOW_TEARING). That is an
// OS / driver capability, true on almost every PC whatever the screen, so it is recorded as a diagnostic only and
// never changes the VRR level (a fixed-refresh screen would otherwise look variable-refresh capable). Unknown
// whenever the switch is off or NVAPI answers.
constexpr PushCapability pushDxgiTearingReport(bool switchOn, bool nvapiReady, PushCapability tearing) {
    return switchOn && !nvapiReady ? tearing : PushCapability::Unknown;
}

// Same numbers as engine::FrameGenerationBackend.
enum class PushFgMethod : std::uint8_t { Dlss = 0, XeSS = 1, Fsr = 2, Fsr4 = 3, Vfg = 4, Anr = 5, AmdFrc = 6, PushFlow = 7 };
inline constexpr unsigned kPushFgMethodCount = 8;
// Same numbers as engine::OpticalFlowBackend (used by the vendor generators, not by PushFlow).
enum class PushOpticalFlow : std::uint8_t { Nvidia = 0, AmdFidelityFx = 1, GpuDis = 2 };
inline constexpr unsigned kPushOpticalFlowCount = 3;
enum class PushStreamCodec : std::uint8_t { H264 = 0, Hevc = 1 };

constexpr std::uint32_t pushFgBit(PushFgMethod method) { return 1u << static_cast<unsigned>(method); }
constexpr std::uint32_t pushFlowBit(PushOpticalFlow flow) { return 1u << static_cast<unsigned>(flow); }

// Multipliers each method offers (a copy of displayFgMultiplierSelectable, held to it by a static_assert in
// Hardware.cpp). 1 = generation off is not a multiplier here.
constexpr bool pushFgMultiplierSelectable(PushFgMethod method, unsigned multiplier) {
    switch (method) {
    case PushFgMethod::Dlss: return multiplier == 2 || multiplier == 3 || multiplier == 4 || multiplier == 6;
    case PushFgMethod::XeSS: return multiplier >= 2 && multiplier <= 4;
    case PushFgMethod::Anr: return multiplier >= 2 && multiplier <= 6;
    case PushFgMethod::Vfg: return multiplier >= 2 && multiplier <= 8;
    case PushFgMethod::PushFlow: return multiplier >= 2 && multiplier <= 4;
    case PushFgMethod::Fsr:
    case PushFgMethod::Fsr4:
    case PushFgMethod::AmdFrc: return multiplier == 2;
    }
    return false;
}
inline constexpr unsigned kPushFgLargestMultiplier = 8;
// Quality tiers a method has: PushFlow Fast / Balanced / Quality (vfgQuality 0..2). The others are treated as
// one tier here: their quality settings do not form a cost ladder this model can rely on.
constexpr unsigned pushFgTierCount(PushFgMethod method) { return method == PushFgMethod::PushFlow ? 3u : 1u; }

// D3D_FEATURE_LEVEL values, so the bridge can pass what D3D12 reports unchanged.
inline constexpr std::uint32_t kPushFeatureLevel12_0 = 0xc000;
inline constexpr std::uint32_t kPushFeatureLevel12_1 = 0xc100;
inline constexpr std::uint32_t kPushFeatureLevel12_2 = 0xc200;

struct PushHardwareCapabilities {
    PushGpuVendor vendor = PushGpuVendor::Unknown;
    std::uint64_t dedicatedVramBytes = 0;     // DXGI DedicatedVideoMemory of the engine's adapter; 0 = unknown
    std::uint32_t featureLevel = 0;           // highest D3D12 feature level of that adapter; 0 = unknown
    PushCapability uma = PushCapability::Unknown;   // the adapter shares system memory (integrated GPU)
    PushCapability decodeH264 = PushCapability::Unknown;    // as the D3D video device lists its decoder profiles
    PushCapability decodeHevc = PushCapability::Unknown;
    PushCapability decodeHevc10 = PushCapability::Unknown;
    std::uint32_t fgMethods = 0;              // pushFgBit() per frame-generation method whose hardware AND files are there
    std::uint32_t opticalFlows = 0;           // pushFlowBit() per optical-flow method available
    double refreshHz = 0;                     // current mode of the display the picture is on; 0 = unknown
    PushVrrState vrr = PushVrrState::Unknown;
    PushCapability hdrDisplay = PushCapability::Unknown;    // a display output is in HDR (PQ / BT.2020) mode
    PushCapability hybridGpu = PushCapability::Unknown;     // another hardware adapter is an integrated one (laptop-style)
    PushPowerSource power = PushPowerSource::Unknown;

    constexpr bool hasFg(PushFgMethod method) const { return (fgMethods & pushFgBit(method)) != 0; }
    constexpr bool hasFlow(PushOpticalFlow flow) const { return (opticalFlows & pushFlowBit(flow)) != 0; }
    constexpr bool vramKnown() const { return dedicatedVramBytes > 0; }
};

// --- output targets for a refresh rate -----------------------------------------------------------------------
//
// Whole multipliers only (60 -> 90 would be 1.5X and does not exist). A target fits the display when
// source x M <= 1.02 x refresh (the same tolerance as DisplayFgBudget.h). evenWithoutVrr: the refresh rate is a
// whole number of refreshes per output frame (90 and 180 on 180 Hz, not 120). A 59.94 fps stream still slips
// one refresh every few seconds at a fixed rate, so VRR is wanted either way; this flag says which targets
// judder visibly without it.
inline constexpr double kPushHardwareDisplayFit = 1.02;
inline constexpr double kPushHardwareDisplayFill = 0.97;
struct PushOutputTarget {
    unsigned multiplier = 0;
    double outputFps = 0;
    bool evenWithoutVrr = false;
};
struct PushOutputTargets {
    std::array<PushOutputTarget, kPushFgLargestMultiplier> items{};
    std::size_t count = 0;
    bool sourceFillsDisplay = false;   // the source alone reaches the refresh: no generated frame can be shown
    constexpr const PushOutputTarget* begin() const { return items.data(); }
    constexpr const PushOutputTarget* end() const { return items.data() + count; }
    constexpr unsigned largest() const { return count ? items[count - 1].multiplier : 0u; }
};
inline bool pushOutputEvenWithoutVrr(double outputFps, double refreshHz) {
    if (!std::isfinite(outputFps) || !std::isfinite(refreshHz) || outputFps <= 0 || refreshHz <= 0) return false;
    const double ratio = refreshHz / outputFps;
    if (ratio < 1.0 / kPushHardwareDisplayFit) return false;
    const double whole = std::round(ratio);
    return whole >= 1 && std::abs(ratio / whole - 1.0) <= kPushHardwareDisplayFit - 1.0;
}
// Every selectable multiplier of the method whose output fits the refresh rate, smallest first. Unknown rates
// give no targets: nothing is offered on input that cannot be checked.
inline PushOutputTargets pushValidOutputTargets(PushFgMethod method, double sourceFps, double refreshHz) {
    PushOutputTargets out;
    if (!std::isfinite(sourceFps) || !std::isfinite(refreshHz) || sourceFps <= 0 || refreshHz <= 0) return out;
    if (sourceFps >= refreshHz * kPushHardwareDisplayFill) { out.sourceFillsDisplay = true; return out; }
    for (unsigned m = 2; m <= kPushFgLargestMultiplier; ++m) {
        if (!pushFgMultiplierSelectable(method, m)) continue;
        const double fps = sourceFps * static_cast<double>(m);
        if (fps > refreshHz * kPushHardwareDisplayFit) break;
        out.items[out.count++] = PushOutputTarget{m, fps, pushOutputEvenWithoutVrr(fps, refreshHz)};
    }
    return out;
}

// --- safe defaults ------------------------------------------------------------------------------------------
//
// VRAM thresholds for the PushFlow tier. They are starting points (estimates), not measurements: the RIFE
// network at 1080p is small next to these, but the same memory holds the decoder's surfaces, the enhancement
// chain's textures and the swap chain, and a desktop with other programs open. Below kPushVramFastOnly only
// Fast; below kPushVramBalanced at most Balanced. Unknown VRAM: Fast.
inline constexpr std::uint64_t kPushVramFastOnly = std::uint64_t(6) << 30;
inline constexpr std::uint64_t kPushVramBalanced = std::uint64_t(8) << 30;
// Below this frame generation is not recommended at all (the method may still be chosen by hand).
inline constexpr std::uint64_t kPushVramMinimumForFg = std::uint64_t(4) << 30;

struct PushHardwareRequest {
    PushStreamCodec codec = PushStreamCodec::Hevc;
    bool hdr = false;
    bool frameGeneration = false;
    PushFgMethod fgMethod = PushFgMethod::PushFlow;
    unsigned multiplier = 2;
    unsigned tier = 1;                 // PushFlow tier (vfgQuality); ignored for one-tier methods
    PushOpticalFlow opticalFlow = PushOpticalFlow::Nvidia;
    double sourceFps = 0;              // 0 = not known yet: no display limit is applied to the multiplier
};

class PushReasons {
public:
    constexpr bool add(std::string_view reason) {
        for (std::size_t i = 0; i < count_; ++i)
            if (items_[i] == reason) return true;
        if (count_ == items_.size()) return false;
        items_[count_++] = reason;
        return true;
    }
    constexpr bool has(std::string_view reason) const {
        for (std::size_t i = 0; i < count_; ++i)
            if (items_[i] == reason) return true;
        return false;
    }
    constexpr std::size_t size() const { return count_; }
    constexpr const std::string_view* begin() const { return items_.data(); }
    constexpr const std::string_view* end() const { return items_.data() + count_; }

private:
    std::array<std::string_view, 16> items_{};
    std::size_t count_ = 0;
};

// Reasons (machine-readable, stable; also the log vocabulary):
//   hevc-decode-missing, hevc-decode-unknown   -> H.264 requested of the console
//   hdr-not-requested is not a reason; hdr-needs-hevc10-decode, hdr-needs-hevc, hdr-display-off, hdr-display-unknown -> SDR
//   fg-gpu-unknown, fg-feature-level, fg-feature-level-unknown, fg-vram-low -> frame generation off
//   fg-method-unavailable -> the next compatible method; fg-no-method -> frame generation off
//   fg-multiplier-unsupported -> the method's largest multiplier at or below the request
//   fg-display-limit -> lowered so the output fits the refresh; fg-source-fills-display -> off
//   flow-nvidia-unavailable -> AMD FidelityFX optical flow; flow-unavailable -> GPU DIS
//   tier-vram-low, tier-vram-unknown, tier-battery -> lower PushFlow tier
//   hybrid-gpu -> informational: the display may be driven by the other adapter (a cross-adapter copy per frame)
struct PushHardwareDefaults {
    PushStreamCodec codec = PushStreamCodec::H264;
    bool hdr = false;
    bool frameGeneration = false;
    PushFgMethod fgMethod = PushFgMethod::PushFlow;
    unsigned multiplier = 1;
    unsigned tier = 0;
    PushOpticalFlow opticalFlow = PushOpticalFlow::AmdFidelityFx;
    PushReasons reasons;
};

// The next compatible method when the requested one is not available. Same order and the same rule as the
// engine's restoredFrameGenerationBackend (FrameGenerationSelection.h): FSR 4 ML where offered, then FSR 3.1,
// then XeSS, then DLSS; never an experimental provider (ANR, VFG, AMD FRC) and never PushFlow, which the
// sanitizer does not pick by itself either. Availability bits come from the PC; nothing here looks at the vendor.
constexpr bool pushNextFgMethod(const PushHardwareCapabilities& caps, PushFgMethod requested, PushFgMethod& chosen) {
    if (caps.hasFg(requested)) { chosen = requested; return true; }
    for (const PushFgMethod method : {PushFgMethod::Fsr4, PushFgMethod::Fsr, PushFgMethod::XeSS, PushFgMethod::Dlss})
        if (caps.hasFg(method)) { chosen = method; return true; }
    return false;
}

inline PushHardwareDefaults pushHardwareDefaults(const PushHardwareCapabilities& caps, const PushHardwareRequest& request) {
    PushHardwareDefaults out;

    // Stream codec: HEVC only when the adapter lists a hardware HEVC decoder. Software HEVC at 4K60 costs a CPU
    // more than any setting saves; H.264 is decoded everywhere.
    out.codec = request.codec;
    if (request.codec == PushStreamCodec::Hevc && caps.decodeHevc != PushCapability::Yes) {
        out.codec = PushStreamCodec::H264;
        out.reasons.add(caps.decodeHevc == PushCapability::No ? "hevc-decode-missing" : "hevc-decode-unknown");
    }
    // HDR from the PS5 is 10-bit HEVC: it needs that decoder, HEVC kept, and a display in HDR mode.
    if (request.hdr) {
        if (out.codec != PushStreamCodec::Hevc) out.reasons.add("hdr-needs-hevc");
        else if (caps.decodeHevc10 != PushCapability::Yes) out.reasons.add("hdr-needs-hevc10-decode");
        else if (caps.hdrDisplay == PushCapability::No) out.reasons.add("hdr-display-off");
        else if (caps.hdrDisplay == PushCapability::Unknown) out.reasons.add("hdr-display-unknown");
        else out.hdr = true;
    }

    // Optical flow (vendor generators only): NVIDIA's only when reported, as the settings sanitizer does.
    out.opticalFlow = request.opticalFlow;
    if (!caps.hasFlow(out.opticalFlow)) {
        if (caps.hasFlow(PushOpticalFlow::AmdFidelityFx)) {
            out.opticalFlow = PushOpticalFlow::AmdFidelityFx;
            out.reasons.add(request.opticalFlow == PushOpticalFlow::Nvidia ? "flow-nvidia-unavailable" : "flow-unavailable");
        } else {
            out.opticalFlow = PushOpticalFlow::GpuDis;
            out.reasons.add("flow-unavailable");
        }
    }

    if (caps.hybridGpu == PushCapability::Yes) out.reasons.add("hybrid-gpu");

    // Frame generation.
    out.fgMethod = request.fgMethod;
    if (!request.frameGeneration) return out;
    if (caps.vendor == PushGpuVendor::Unknown) { out.reasons.add("fg-gpu-unknown"); return out; }
    // The engine opens only an adapter that creates a device at 12_0 (engineAdapterDesc), so a known vendor with an
    // unknown level is a contradictory report: off, like every other unknown.
    if (caps.featureLevel == 0) { out.reasons.add("fg-feature-level-unknown"); return out; }
    if (caps.featureLevel < kPushFeatureLevel12_0) { out.reasons.add("fg-feature-level"); return out; }
    if (caps.vramKnown() && caps.dedicatedVramBytes < kPushVramMinimumForFg && caps.uma != PushCapability::Yes) {
        out.reasons.add("fg-vram-low");
        return out;
    }
    PushFgMethod method = request.fgMethod;
    if (!pushNextFgMethod(caps, request.fgMethod, method)) { out.reasons.add("fg-no-method"); return out; }
    if (method != request.fgMethod) out.reasons.add("fg-method-unavailable");
    out.fgMethod = method;

    // Multiplier: the method's largest selectable value at or below the request (2X at least).
    const unsigned wanted = request.multiplier < 2 ? 2u : request.multiplier > kPushFgLargestMultiplier ? kPushFgLargestMultiplier : request.multiplier;
    unsigned multiplier = 0;
    for (unsigned m = wanted; m >= 2; --m)
        if (pushFgMultiplierSelectable(method, m)) { multiplier = m; break; }
    if (multiplier == 0) { out.reasons.add("fg-no-method"); return out; }
    if (multiplier != request.multiplier) out.reasons.add("fg-multiplier-unsupported");
    // Display limit, only when both rates are known.
    if (std::isfinite(request.sourceFps) && request.sourceFps > 0 && std::isfinite(caps.refreshHz) && caps.refreshHz > 0) {
        const PushOutputTargets targets = pushValidOutputTargets(method, request.sourceFps, caps.refreshHz);
        if (targets.sourceFillsDisplay || targets.count == 0) { out.reasons.add("fg-source-fills-display"); return out; }
        if (multiplier > targets.largest()) {
            for (unsigned m = multiplier; m >= 2; --m) {
                bool fits = false;
                for (const PushOutputTarget& target : targets) fits = fits || target.multiplier == m;
                if (fits) { multiplier = m; break; }
            }
            out.reasons.add("fg-display-limit");
        }
    }
    out.frameGeneration = true;
    out.multiplier = multiplier;

    // Tier (PushFlow): capped by VRAM and by battery power. An integrated GPU's "dedicated" memory says little, so
    // UMA adapters are capped like unknown VRAM.
    const unsigned tiers = pushFgTierCount(method);
    unsigned tier = request.tier < tiers ? request.tier : tiers - 1;
    if (tiers > 1) {
        unsigned cap = tiers - 1;
        if (!caps.vramKnown() || caps.uma == PushCapability::Yes) { cap = 0; if (tier > cap) out.reasons.add("tier-vram-unknown"); }
        else if (caps.dedicatedVramBytes < kPushVramFastOnly) { cap = 0; if (tier > cap) out.reasons.add("tier-vram-low"); }
        else if (caps.dedicatedVramBytes < kPushVramBalanced) { cap = 1; if (tier > cap) out.reasons.add("tier-vram-low"); }
        if (caps.power == PushPowerSource::Battery && cap > 0) { cap = 0; if (tier > cap) out.reasons.add("tier-battery"); }
        if (tier > cap) tier = cap;
    }
    out.tier = tier;
    return out;
}
} // namespace veyra::engine
