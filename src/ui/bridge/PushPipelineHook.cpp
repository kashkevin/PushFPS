// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// QML bridge: the test hook VEYRA_TEST_RP_PIPELINE=<key=value;...> (test-only, environment only, like
// VEYRA_TEST_RP_AUTORUN; contract item 1). Applied once when the stream's first picture is shown, through the setters
// the interface uses; one line
//   [slipstream-pipeline] applied=<n> refused=<n> detail=<key:ok|key:refused:reason,...> streamHdr=0|1 videoHdr=0|1
// The image keys (sr, srModel, srTarget, clean, fg, fgMultiplier, hdr, profile, cap, fgTier, dedup, hudLock) are applied here. Another part of the
// app adds its own keys with registerPipelineHookExtension (the neural layer does, in its own file); without one,
// such a key is refused as `unknown-key`. Parser and line: engine/PushPipelineHook.h (pure, tested).
// With the variable unset nothing happens. Unverified on Windows: written without a Windows compiler.
#include "../QmlPlayerBridge_p.h"
#include "veyra/engine/PushPipelineHook.h"
#include "veyra/Log.h"

#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace veyra::ui {
namespace {

// The text as ASCII; nullopt when unset. Too long for the parser: kept too long, so that it is refused as too-long.
std::optional<std::string> pipelineHookText() {
    static const std::optional<std::string> text = []() -> std::optional<std::string> {
        wchar_t value[1024]{};
        const DWORD length = GetEnvironmentVariableW(L"VEYRA_TEST_RP_PIPELINE", value, 1024);
        if (length == 0) return std::nullopt;
        if (length >= 1024) return std::string(engine::kPipeHookMaxBytes + 1, '?');
        std::string out;
        for (DWORD i = 0; i < length; ++i) out.push_back(value[i] > 0 && value[i] < 128 ? static_cast<char>(value[i]) : '?');
        return out;
    }();
    return text;
}

struct PipelineHookState {
    bool parsed = false, profileApplied = false, done = false;
    std::vector<engine::PipeHookItem> items;
    std::chrono::steady_clock::time_point profileAt{};
};
PipelineHookState& pipelineHookState() { static PipelineHookState state; return state; }

struct PipelineHookExtensionSlot {
    const engine::PipeHookExtension* keys = nullptr;
    PipelineHookApply apply = nullptr;
};
PipelineHookExtensionSlot& pipelineHookExtension() { static PipelineHookExtensionSlot slot; return slot; }

// One image key through the bridge's own setters; the reason when it could not be applied, empty when it was.
// streamHdr: the stream's own picture is HDR (its codec, chosen before connecting: not something a key can change).
std::string applyImageKey(QmlPlayerBridge& b, const std::string& key, const std::string& value, bool streamHdr) {
    const auto available = [&](const QString& id) {
        return b.effectCapabilities().value(id).toMap().value(QStringLiteral("available")).toBool();
    };
    const QString word = QString::fromStdString(value);
    if (key == "profile") return b.setPreference(QStringLiteral("pushProfile"), word) ? std::string() : std::string("not-supported");
    if (key == "clean") return b.setPreference(QStringLiteral("pushClean"), engine::pipeHookNumber(value, 0, 100)) ? std::string() : std::string("not-supported");
    if (key == "sr") {
        if (value == "off") {
            if (b.srEnabled()) b.setSrEnabled(false);
            return b.srEnabled() ? "not-applied" : "";
        }
        const int quality = value == "nis" ? int(engine::kVideoSrNis)
                          : value == "vsr-stream" ? b.srQualityFor(QStringLiteral("rtx-st"), b.srTierOf(b.videoSrQuality())) : 0;
        if (quality < 0 || !available(QStringLiteral("sr") + QString::number(quality))) return "unavailable";
        if (b.dualSrEnabled()) b.setDualSrEnabled(false);
        if (b.videoSrQuality() != quality) b.setVideoSrQuality(quality);
        if (!b.srEnabled()) b.setSrEnabled(true);
        return b.srEnabled() && b.videoSrQuality() == quality ? "" : "not-applied";
    }
    if (key == "srModel") {
        const int model = engine::pipeHookSrModel(value);
        if (b.dlssSrModel() != model) b.setDlssSrModel(model);
        return b.dlssSrModel() == model ? "" : "not-applied";
    }
    if (key == "srTarget") {
        const int index = value == "qhd" ? 1 : 2;
        if (b.srTargetIndex() != index) b.setSrTargetIndex(index);
        return b.srTargetIndex() == index ? "" : "not-applied";
    }
    if (key == "fg") {
        if (value == "off") {
            if (b.fgEnabled()) b.setFgEnabled(false);
            return b.fgEnabled() ? "not-applied" : "";
        }
        const QString backend = value == "fsr" ? QStringLiteral("fsr3") : word;
        if (!available(backend)) return "unavailable";
        if (b.fgBackendName() != backend) b.setFgBackendName(backend);
        if (!b.fgEnabled()) b.setFgEnabled(true);
        return b.fgEnabled() && b.fgBackendName() == backend ? "" : "not-applied";
    }
    if (key == "fgMultiplier") {
        const int multiplier = engine::pipeHookNumber(value, 2, 8);
        if (b.fgMultiplier() != multiplier) b.setFgMultiplier(multiplier);
        return b.fgMultiplier() == multiplier ? "" : "not-applied";
    }
    if (key == "cap") {
        // The output cap (stream "fps": the kit's 90 FPS arm): Follow display (1), none (0) or a custom rate (2).
        if (value == "display" || value == "off") {
            const int mode = value == "display" ? 1 : 0;
            if (b.outputRateMode() != mode) b.setOutputRateMode(mode);
            return b.outputRateMode() == mode ? "" : "not-applied";
        }
        const int fps = engine::pipeHookNumber(value, 30, 360);
        b.setOutputCustomFps(double(fps));
        if (b.outputRateMode() != 2) b.setOutputRateMode(2);
        return b.outputRateMode() == 2 && std::abs(b.outputCustomFps() - double(fps)) < 0.5 ? "" : "not-applied";
    }
    if (key == "hdr") {
        // RTX Video HDR conversion of an SDR picture. A stream that is HDR already has nothing to convert: hdr=1 is
        // met as it is, and the line's streamHdr= says which kind of run this was.
        const bool on = value == "1";
        if (on && streamHdr) return "";
        if (on && !available(QStringLiteral("hdr"))) return "unavailable";
        if (b.videoHdr() != on) b.setVideoHdr(on);
        return b.videoHdr() == on ? "" : "not-applied";
    }
    // Stream "capture": the generation tier, duplicate removal and HUD Lock of the kit's capture arms.
    if (key == "fgTier") {
        // VFG 0-5 (Low, Medium, High, mixed 3-5) or PushFlow 0-2; written after fg, whose change may reset the tier.
        const int tier = engine::pipeHookNumber(value, 0, 5);
        if (b.vfgQuality() != tier) b.setVfgQuality(tier);
        return b.vfgQuality() == tier ? "" : "not-applied";
    }
    if (key == "dedup") {
        // Duplicate removal: Transport (0, every sample is content), Auto (1) or the fixed every-second-sample 60->30 (5).
        const int mode = value == "off" ? 0 : value == "auto" ? 1 : 5;
        if (b.contentRate() != mode) b.setContentRate(mode);
        return b.contentRate() == mode ? "" : "not-applied";
    }
    if (key == "hudLock") {
        const bool on = value == "1";
        if (b.hudLock() != on) b.setHudLock(on);
        return b.hudLock() == on ? "" : "not-applied";
    }
    const auto& extension = pipelineHookExtension();
    if (extension.apply) return extension.apply(b, key, value);
    return "unknown-key";
}

} // namespace

void registerPipelineHookExtension(const engine::PipeHookExtension* keys, PipelineHookApply apply) {
    auto& slot = pipelineHookExtension();
    slot.keys = keys;
    slot.apply = apply;
}

// From the bridge's poll timer: nothing until a stream's picture is being presented; then the profile (when the
// text has one), and the other keys in the order written. A profile is applied by the interface after its
// preference changed, so the other keys follow one second later and override it.
void QmlPlayerBridge::pushPipelineTestTick() {
    auto& hook = pipelineHookState();
    if (hook.done) return;
    const auto text = pipelineHookText();
    if (!text) { hook.done = true; return; }
    const auto& snapshot = impl_->snapshot;
    const auto& flow = snapshot.metrics.flow;
    if (!(snapshot.running && flow.rateWindowReady && flow.realPresentFps > 0)) return;
    const auto now = std::chrono::steady_clock::now();
    if (!hook.parsed) {
        hook.parsed = true;
        hook.items = engine::pipeHookParse(*text, pipelineHookExtension().keys);
    }
    std::size_t first = 0;
    if (!hook.items.empty() && hook.items.front().key == "profile" && hook.items.front().refusal.empty()) {
        first = 1;
        if (!hook.profileApplied) {
            hook.profileApplied = true;
            hook.profileAt = now;
            auto& profile = hook.items.front();
            profile.refusal = applyImageKey(*this, profile.key, profile.value, snapshot.sourceHdr);
            profile.applied = profile.refusal.empty();
            return;
        }
        if (now - hook.profileAt < std::chrono::seconds(1)) return;
    }
    for (std::size_t i = first; i < hook.items.size(); ++i) {
        auto& item = hook.items[i];
        if (!item.refusal.empty()) continue;
        item.refusal = applyImageKey(*this, item.key, item.value, snapshot.sourceHdr);
        item.applied = item.refusal.empty();
    }
    hook.done = true;
    // streamHdr / videoHdr: an HDR run can be told from an SDR one.
    veyra::log::info("slipstream-pipeline", engine::pipeHookLine(hook.items) +
        std::format(" streamHdr={} videoHdr={}", snapshot.sourceHdr ? 1 : 0, snapshot.videoHdrActive ? 1 : 0));
}

} // namespace veyra::ui
