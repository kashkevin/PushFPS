// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
#include "veyra/engine/PushHardwareCapabilities.h"
#include "veyra/ui/EffectAvailability.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace veyra::engine {
// Simulated hardware profiles and a model of what the application does with them at start-up.
//
// A MODEL, not a measurement: every profile is a capability report as the bridge would build it for a
// representative PC of a target family (or an odd case), and pushSimulateStartup() copies the decisions the
// application makes today (settings defaults, sanitizer, engine recovery, decoder and encoder choice). Nothing
// here proves that a real PC behaves so; it proves that the decision code has a safe answer for every report.
// Portable and header-only. Tests: tests/remoteplay/HardwareProfileTests.inc. docs/HARDWARE_COMPATIBILITY.md.
//
// Copies of engine code (kept honest where the compiler can see both):
//   pushFirstLaunchFgMethod   = defaultFrameGenerationBackend (FrameGenerationSelection.h): static_assert in Hardware.cpp
//   pushNextFgMethod          = restoredFrameGenerationBackend: static_assert in Hardware.cpp
//   pushFgGate / pushFlowGate = the HardwareEffect each id gets in scanEffectCapabilities (Hardware.cpp), which
//                               reads its gates from these two functions
//   pushFgAfterInitFailure    = disableFailedBackend(FailedBackend::Fg) (BackendRecovery.h): a copy, not checked
//   pushRecordingEncoder      = LiveRecorder::capture's NVENC / Media Foundation choice: a copy, not checked
//   pushDecodePath            = RemotePlaySource::openDecoder / submit fallback: a copy, not checked

// --- availability gates (the hardware half of scanEffectCapabilities' add()) -------------------------------------
constexpr ui::HardwareEffect pushFgGate(PushFgMethod method) {
    switch (method) {
    case PushFgMethod::Dlss: return ui::HardwareEffect::Nvidia;
    case PushFgMethod::Fsr4: return ui::HardwareEffect::Fsr4;
    case PushFgMethod::Vfg: return ui::HardwareEffect::Vfg;
    case PushFgMethod::AmdFrc: return ui::HardwareEffect::AmdFrc;
    case PushFgMethod::XeSS:
    case PushFgMethod::Fsr:
    case PushFgMethod::Anr:
    case PushFgMethod::PushFlow: return ui::HardwareEffect::CrossVendor;
    }
    return ui::HardwareEffect::Nvidia;
}
constexpr ui::HardwareEffect pushFlowGate(PushOpticalFlow flow) {
    return flow == PushOpticalFlow::Nvidia ? ui::HardwareEffect::Nvidia : ui::HardwareEffect::CrossVendor;
}

// What a fresh profile starts on (frame generation itself is off: EnhancementSettings::multiplier = 1).
constexpr PushFgMethod pushFirstLaunchFgMethod(std::uint32_t vendorId, bool rx9000) {
    return vendorId == 0x1002 && rx9000 ? PushFgMethod::Fsr4
        : vendorId == 0x1002 || vendorId == 0x8086 ? PushFgMethod::Fsr : PushFgMethod::Dlss;
}
// After a frame-generation initialisation failure: FSR 4 ML moves to FSR 3.1, anything else turns generation off.
constexpr bool pushFgAfterInitFailure(PushFgMethod failed, PushFgMethod& next) {
    if (failed == PushFgMethod::Fsr4) { next = PushFgMethod::Fsr; return true; }
    return false;
}

// PushFlow crash guard (QmlPlayerBridge.cpp, VEYRA_EXP_PUSHFLOW_CRASH_ORDER=1): the method used at the next start
// after PushFPS stopped inside PushFlow. First the capability model's non-experimental order (pushNextFgMethod:
// FSR 4 ML, FSR 3.1, XeSS, DLSS), then the experimental providers in the guard's old relative order; never PushFlow.
// Without the switch the guard keeps its old order (VFG, DLSS, FSR 4, FSR 3.1, XeSS, ANR, AMD FRC).
inline constexpr std::array<PushFgMethod, 7> kPushCrashGuardOrder{
    PushFgMethod::Fsr4, PushFgMethod::Fsr, PushFgMethod::XeSS, PushFgMethod::Dlss, PushFgMethod::Vfg, PushFgMethod::Anr, PushFgMethod::AmdFrc};
inline constexpr std::size_t kPushCrashGuardNonExperimental = 4;
constexpr bool pushCrashGuardChoice(std::uint32_t available, PushFgMethod& chosen) {
    for (const PushFgMethod method : kPushCrashGuardOrder)
        if (available & pushFgBit(method)) { chosen = method; return true; }
    return false;
}

// --- a simulated PC --------------------------------------------------------------------------------------------
// Which runtime files are on disk (pushFgBit per method; flow0 needs a build with the NVOF headers).
struct PushInstalledRuntimes {
    std::uint32_t fgFiles = 0;
    bool nvofBuild = false;
};
// The installer's two packages (Install-PushFPS.ps1 picks NVIDIA when any adapter's name says NVIDIA). PushFlow
// ships in both; ANR never ships (the user installs it); AMD FRC is the AMD driver's own amfrt64.dll.
constexpr PushInstalledRuntimes pushPackageRuntimes(bool nvidiaPackage, bool amdDriver, bool nvofBuild) {
    PushInstalledRuntimes r;
    r.fgFiles = pushFgBit(PushFgMethod::Fsr) | pushFgBit(PushFgMethod::Fsr4) | pushFgBit(PushFgMethod::XeSS) | pushFgBit(PushFgMethod::PushFlow);
    if (nvidiaPackage) r.fgFiles |= pushFgBit(PushFgMethod::Dlss) | pushFgBit(PushFgMethod::Vfg);
    if (amdDriver) r.fgFiles |= pushFgBit(PushFgMethod::AmdFrc);
    r.nvofBuild = nvofBuild;
    return r;
}

enum class PushProfileFamily : std::uint8_t { Nvidia, Amd, Intel, Hybrid, Odd };

struct PushHardwareProfile {
    std::string_view id;
    PushProfileFamily family = PushProfileFamily::Odd;
    PushHardwareCapabilities caps;        // fgMethods / opticalFlows filled by pushProfileWithGates
    ui::EffectGpu gpu;                    // what effectGpu() derives (vendor, name flags)
    bool engineAdapter = true;            // a hardware adapter that creates a D3D12 device at 12_0 exists
    PushCapability mfHardwareEncoder = PushCapability::Unknown;   // a hardware H.264 MFT for the adapter
    PushInstalledRuntimes runtimes;
};

// Fills fgMethods and opticalFlows the way the bridge does: hardware gate AND files.
constexpr PushHardwareProfile pushProfileWithGates(PushHardwareProfile p) {
    p.caps.fgMethods = 0;
    for (unsigned m = 0; m < kPushFgMethodCount; ++m) {
        const auto method = static_cast<PushFgMethod>(m);
        if ((p.runtimes.fgFiles & pushFgBit(method)) && ui::hardwareSupports(pushFgGate(method), p.gpu)) p.caps.fgMethods |= pushFgBit(method);
    }
    p.caps.opticalFlows = pushFlowBit(PushOpticalFlow::GpuDis);
    if (p.runtimes.nvofBuild && ui::hardwareSupports(pushFlowGate(PushOpticalFlow::Nvidia), p.gpu)) p.caps.opticalFlows |= pushFlowBit(PushOpticalFlow::Nvidia);
    if (ui::hardwareSupports(pushFlowGate(PushOpticalFlow::AmdFidelityFx), p.gpu)) p.caps.opticalFlows |= pushFlowBit(PushOpticalFlow::AmdFidelityFx);
    return p;
}

// --- start-up model ------------------------------------------------------------------------------------------
enum class PushDecodePath : std::uint8_t { None = 0, Hardware = 1, SoftwareAfterHardwareFailed = 2, Software = 3 };
enum class PushDecodeMode : std::uint8_t { Automatic = 0, Software = 1, Hardware = 2 };   // RemotePlayConnectDesc::DecodeMode
enum class PushRecordingEncoder : std::uint8_t { None = 0, Nvenc = 1, MediaFoundation = 2 };

// What goes wrong at run time on the simulated PC (a backend whose initialisation fails, a decoder that refuses).
struct PushRuntimeFaults {
    std::uint32_t fgInitFails = 0;        // pushFgBit per method whose initialisation fails
    bool hardwareDecodeFails = false;     // the D3D12VA decoder refuses although the profile is listed
    bool nvencFails = false;
};

struct PushStartupOutcome {
    bool presents = false;                // the engine opens an adapter and shows the picture
    PushStreamCodec codec = PushStreamCodec::H264;
    bool hdr = false;
    PushDecodePath decode = PushDecodePath::None;
    bool frameGeneration = false;
    PushFgMethod fgMethod = PushFgMethod::PushFlow;
    unsigned multiplier = 1;
    unsigned tier = 0;
    unsigned fgAttempts = 0;              // graph initialisations used (the engine allows 6)
    PushRecordingEncoder recording = PushRecordingEncoder::None;
    bool recordingHdr = false;
    PushHardwareDefaults defaults;        // the capability model's answer (its reasons included)
};

// The decoder path RemotePlaySource takes for a codec: hardware when the engine has a device and the mode allows
// it; a refusal falls back to software in Automatic mode and fails the stream in Hardware mode.
constexpr PushDecodePath pushDecodePath(const PushHardwareProfile& p, PushStreamCodec codec, bool hdr, PushDecodeMode mode, bool decoderFails) {
    if (!p.engineAdapter) return PushDecodePath::None;
    if (mode == PushDecodeMode::Software) return PushDecodePath::Software;
    const PushCapability listed = hdr ? p.caps.decodeHevc10 : codec == PushStreamCodec::Hevc ? p.caps.decodeHevc : p.caps.decodeH264;
    const bool works = listed != PushCapability::No && !decoderFails;   // Unknown: FFmpeg tries and finds out
    if (works) return PushDecodePath::Hardware;
    return mode == PushDecodeMode::Automatic ? PushDecodePath::SoftwareAfterHardwareFailed : PushDecodePath::None;
}
// LiveRecorder: NVENC on NVIDIA (no Media Foundation fallback once chosen), the vendor's hardware MFT elsewhere;
// HDR only through NVENC; going live is SDR H.264 anyway.
constexpr PushRecordingEncoder pushRecordingEncoder(const PushHardwareProfile& p, bool hdr, bool nvencFails) {
    if (!p.engineAdapter) return PushRecordingEncoder::None;
    if (p.gpu.vendor == 0x10DE) return nvencFails ? PushRecordingEncoder::None : PushRecordingEncoder::Nvenc;
    if (hdr) return PushRecordingEncoder::None;
    return p.mfHardwareEncoder == PushCapability::No ? PushRecordingEncoder::None : PushRecordingEncoder::MediaFoundation;
}

// The whole start-up: the capability model's defaults for the request, then the engine's frame-generation
// initialisation with the faults injected, the decoder and the recorder.
inline PushStartupOutcome pushSimulateStartup(const PushHardwareProfile& p, const PushHardwareRequest& request,
                                              const PushRuntimeFaults& faults = {}, PushDecodeMode decodeMode = PushDecodeMode::Automatic) {
    PushStartupOutcome out;
    out.defaults = pushHardwareDefaults(p.caps, request);
    if (!p.engineAdapter) return out;
    out.presents = true;
    out.codec = out.defaults.codec;
    out.hdr = out.defaults.hdr;
    out.decode = pushDecodePath(p, out.codec, out.hdr, decodeMode, faults.hardwareDecodeFails);
    if (out.defaults.frameGeneration) {
        PushFgMethod method = out.defaults.fgMethod;
        unsigned multiplier = out.defaults.multiplier;
        bool on = true;
        while (on && out.fgAttempts < 6) {
            ++out.fgAttempts;
            if (!(faults.fgInitFails & pushFgBit(method))) break;
            PushFgMethod next = method;
            if (pushFgAfterInitFailure(method, next)) { method = next; multiplier = 2; }
            else on = false;
        }
        if (on && (faults.fgInitFails & pushFgBit(method))) on = false;   // six attempts spent
        if (on) {
            out.frameGeneration = true;
            out.fgMethod = method;
            out.multiplier = multiplier;
            out.tier = method == out.defaults.fgMethod ? out.defaults.tier : 0;
        } else {
            out.fgMethod = method;
        }
    }
    out.recording = pushRecordingEncoder(p, out.hdr, faults.nvencFails);
    out.recordingHdr = out.hdr && out.recording == PushRecordingEncoder::Nvenc;
    return out;
}

// Invariants every answer must keep (true = valid). Used by the tests for every profile x request.
inline bool pushDefaultsValid(const PushHardwareCapabilities& caps, const PushHardwareRequest& request, const PushHardwareDefaults& d) {
    if (d.reasons.size() >= 16) return false;                                   // a reason may have been lost
    if (d.codec == PushStreamCodec::Hevc && (request.codec != PushStreamCodec::Hevc || caps.decodeHevc != PushCapability::Yes)) return false;
    if (d.hdr && (!request.hdr || d.codec != PushStreamCodec::Hevc || caps.decodeHevc10 != PushCapability::Yes || caps.hdrDisplay != PushCapability::Yes)) return false;
    if (d.opticalFlow != PushOpticalFlow::GpuDis && !caps.hasFlow(d.opticalFlow)) return false;
    if (!d.frameGeneration) return d.multiplier == 1;
    if (!request.frameGeneration || caps.vendor == PushGpuVendor::Unknown || caps.featureLevel < kPushFeatureLevel12_0) return false;
    if (!caps.hasFg(d.fgMethod)) return false;                                  // proprietary methods only with their bit
    if (d.multiplier < 2 || !pushFgMultiplierSelectable(d.fgMethod, d.multiplier)) return false;
    if (d.tier >= pushFgTierCount(d.fgMethod)) return false;
    if (request.sourceFps > 0 && caps.refreshHz > 0 && request.sourceFps * d.multiplier > caps.refreshHz * kPushHardwareDisplayFit) return false;
    return true;
}

// --- the profiles -----------------------------------------------------------------------------------------------
namespace detail {
constexpr std::uint64_t kPushProfileGiB = std::uint64_t(1) << 30;
constexpr PushHardwareProfile pushDesktopProfile(std::string_view id, PushProfileFamily family, std::uint32_t vendorId, std::uint64_t vramGiB) {
    PushHardwareProfile p;
    p.id = id; p.family = family;
    p.caps.vendor = pushGpuVendorFromPciId(vendorId);
    p.caps.dedicatedVramBytes = vramGiB * kPushProfileGiB;
    p.caps.featureLevel = kPushFeatureLevel12_2;
    p.caps.uma = PushCapability::No;
    p.caps.decodeH264 = p.caps.decodeHevc = p.caps.decodeHevc10 = PushCapability::Yes;
    p.caps.refreshHz = 165;
    p.caps.vrr = PushVrrState::Possible;
    p.caps.hdrDisplay = PushCapability::No;
    p.caps.hybridGpu = PushCapability::No;
    p.caps.power = PushPowerSource::Mains;
    p.gpu.vendor = vendorId;
    p.mfHardwareEncoder = PushCapability::Yes;
    p.runtimes = pushPackageRuntimes(vendorId == 0x10DE, vendorId == 0x1002, true);
    return p;
}
constexpr PushHardwareProfile pushNvidia(std::string_view id, std::uint64_t vram, bool ampere, bool ada, bool blackwell) {
    auto p = pushDesktopProfile(id, PushProfileFamily::Nvidia, 0x10DE, vram);
    p.gpu.rtx = true; p.gpu.ampere = ampere; p.gpu.ada = ada; p.gpu.blackwell = blackwell;
    return p;
}
constexpr PushHardwareProfile pushAmd(std::string_view id, std::uint64_t vram, bool rx7000, bool rx9000) {
    auto p = pushDesktopProfile(id, PushProfileFamily::Amd, 0x1002, vram);
    p.gpu.rx7000 = rx7000; p.gpu.rx9000 = rx9000;
    return p;
}
} // namespace detail

inline constexpr std::size_t kPushHardwareProfileCount = 26;
inline std::array<PushHardwareProfile, kPushHardwareProfileCount> pushHardwareProfiles() {
    using namespace detail;
    std::array<PushHardwareProfile, kPushHardwareProfileCount> all{};
    std::size_t n = 0;
    const auto put = [&](PushHardwareProfile p) { all[n++] = pushProfileWithGates(p); };

    // Target families, desktop, everything reported.
    { auto p = pushNvidia("nvidia-rtx50-12gb-180hz-hdr", 12, false, false, true); p.caps.refreshHz = 180; p.caps.vrr = PushVrrState::Active; p.caps.hdrDisplay = PushCapability::Yes; put(p); }
    { auto p = pushNvidia("nvidia-rtx50-16gb-360hz", 16, false, false, true); p.caps.refreshHz = 360; put(p); }
    put(pushNvidia("nvidia-rtx40-8gb-144hz", 8, false, true, false));
    { auto p = pushNvidia("nvidia-rtx30-10gb-60hz-sdr", 10, true, false, false); p.caps.refreshHz = 60; p.caps.vrr = PushVrrState::Unsupported; put(p); }
    { auto p = pushNvidia("nvidia-rtx40-fork-build-no-nvof", 12, false, true, false); p.runtimes.nvofBuild = false; put(p); }
    { auto p = pushAmd("amd-rx9000-16gb-240hz", 16, false, true); p.caps.refreshHz = 240; put(p); }
    { auto p = pushAmd("amd-rx7000-20gb-144hz-hdr", 20, true, false); p.caps.refreshHz = 144; p.caps.hdrDisplay = PushCapability::Yes; put(p); }
    put(pushAmd("amd-rx6000-8gb-165hz", 8, false, false));
    { auto p = pushAmd("amd-rx6000-no-amf-runtime", 12, false, false); p.runtimes.fgFiles &= ~pushFgBit(PushFgMethod::AmdFrc); put(p); }
    { auto p = detail::pushDesktopProfile("intel-arc-b-12gb-180hz", PushProfileFamily::Intel, 0x8086, 12); p.caps.refreshHz = 180; put(p); }
    { auto p = detail::pushDesktopProfile("intel-arc-a-8gb-120hz", PushProfileFamily::Intel, 0x8086, 8); p.caps.refreshHz = 120; put(p); }

    // Laptops.
    { auto p = pushNvidia("hybrid-rtx40-laptop-battery", 8, false, true, false); p.family = PushProfileFamily::Hybrid; p.caps.hybridGpu = PushCapability::Yes;
      p.caps.power = PushPowerSource::Battery; p.caps.refreshHz = 240; put(p); }
    { auto p = pushAmd("hybrid-rx7000s-laptop-mains", 8, true, false); p.family = PushProfileFamily::Hybrid; p.caps.hybridGpu = PushCapability::Yes; put(p); }
    { auto p = detail::pushDesktopProfile("intel-integrated-uma", PushProfileFamily::Hybrid, 0x8086, 0); p.caps.dedicatedVramBytes = 128ull << 20;
      p.caps.uma = PushCapability::Yes; p.caps.featureLevel = kPushFeatureLevel12_1; p.caps.refreshHz = 60; p.caps.power = PushPowerSource::Battery; put(p); }

    // Odd reports.
    { auto p = pushNvidia("nvidia-4gb-vram", 4, true, false, false); p.caps.dedicatedVramBytes = 4 * kPushProfileGiB - (64ull << 20); put(p); }
    { auto p = pushAmd("no-hevc-decode", 8, false, false); p.caps.decodeHevc = p.caps.decodeHevc10 = PushCapability::No; put(p); }
    { auto p = detail::pushDesktopProfile("hevc-8-bit-only-hdr-display", PushProfileFamily::Odd, 0x8086, 8); p.caps.decodeHevc10 = PushCapability::No;
      p.caps.hdrDisplay = PushCapability::Yes; put(p); }
    { auto p = pushNvidia("decode-profiles-unknown", 12, false, true, false); p.caps.decodeH264 = p.caps.decodeHevc = p.caps.decodeHevc10 = PushCapability::Unknown; put(p); }
    { auto p = detail::pushDesktopProfile("software-adapter-only", PushProfileFamily::Odd, 0x1414, 0); p.engineAdapter = false; p.caps = PushHardwareCapabilities{};
      p.mfHardwareEncoder = PushCapability::No; p.runtimes = pushPackageRuntimes(false, false, false); put(p); }
    { auto p = detail::pushDesktopProfile("unknown-vendor-12_0", PushProfileFamily::Odd, 0x1ED5, 8); p.caps.featureLevel = kPushFeatureLevel12_0;
      p.mfHardwareEncoder = PushCapability::Unknown; put(p); }
    { PushHardwareProfile p; p.id = "driver-reports-nothing"; p.family = PushProfileFamily::Odd; p.engineAdapter = true; put(p); }
    { auto p = pushNvidia("contradictory-uma-16gb-fsr4-bit-on-nvidia", 16, false, true, false); p.caps.uma = PushCapability::Yes; p.caps.decodeHevc = PushCapability::No;
      p.caps.decodeHevc10 = PushCapability::Yes; p.caps.vrr = PushVrrState::Active; p.caps.refreshHz = 0; put(p);
      all[n - 1].caps.fgMethods |= pushFgBit(PushFgMethod::Fsr4); }   // a report the gates would never give
    { auto p = pushNvidia("contradictory-vendor-known-level-unknown", 12, false, false, true); p.caps.featureLevel = 0; put(p); }
    { auto p = pushNvidia("nvidia-no-runtime-files", 12, false, false, true); p.runtimes.fgFiles = 0; put(p); }
    { auto p = pushNvidia("non-rtx-nvidia-name", 6, false, false, false); p.gpu.rtx = false; p.caps.featureLevel = kPushFeatureLevel12_1; put(p); }
    { auto p = pushAmd("no-hardware-encoder", 8, false, false); p.mfHardwareEncoder = PushCapability::No; p.caps.refreshHz = 75; p.caps.vrr = PushVrrState::Unknown; put(p); }
    return all;
}
} // namespace veyra::engine
