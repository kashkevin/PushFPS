// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "ownfg", step 2: the Lower delay row's frame source (pipeline/PushOwnFrame.h) between the preference
// pushLowDelaySource ("auto" | "nvidia" | "pushfps", validated in Preferences.cpp) and the engine (one relaxed store; the
// graph reads it once a pair), and the row's live facts. With VFG the first choice of PushFPS (or of the causal schedule)
// in a run also rebuilds the graph once, so that PushWarp exists (bridge/PushOwnFrame.cpp).
#include "veyra/pipeline/PushCausal.h"
#include "veyra/pipeline/PushOwnFrame.h"

#include <QString>
#include <QVariantMap>
#include <cstdint>
#include <string_view>

namespace veyra::ui {

inline bool pushOwnFrameSourceValid(const QString& v) {
    return v == QLatin1String("auto") || v == QLatin1String("nvidia") || v == QLatin1String("pushfps");
}
inline void pushOwnFrameApplySource(const QString& v) {
    const std::u16string_view text(reinterpret_cast<const char16_t*>(v.utf16()), static_cast<std::size_t>(v.size()));
    pipeline::pushOwnFrameShared().source.store(static_cast<std::uint8_t>(pipeline::pushOwnFrameSourceFrom(text)), std::memory_order_relaxed);
}
// Review 1, SHOULD 9: whether the stored choices ask for PushWarp in a VFG graph (EnhancementSettings::lowDelayOwnWarp).
inline bool pushOwnFrameWarpChosen(const QString& source, bool causal) { return source == QLatin1String("pushfps") || causal; }
inline void pushOwnFrameApplyCausal(bool causal) { pipeline::pushCausalShared().chosen.store(causal, std::memory_order_relaxed); }
// Added to pushLowDelayState(): the source in use for the last predicted pair (none | chosen | not-vfg | vfg-refused),
// PushWarp's state (none | ready | check-failed) and the pairs it made I for in this run.
inline void pushOwnFrameAddState(QVariantMap& state) {
    const auto& shared = pipeline::pushOwnFrameShared();
    const auto warp = shared.warp.load(std::memory_order_relaxed);
    state.insert(QStringLiteral("ownInUse"), QString::fromLatin1(pipeline::pushOwnFrameUseName(static_cast<pipeline::PushOwnFrameUse>(shared.inUse.load(std::memory_order_relaxed)))));
    state.insert(QStringLiteral("ownWarp"), QString::fromLatin1(warp == 1 ? "ready" : warp == 2 ? "check-failed" : warp == 3 ? "too-slow" : "none"));
    state.insert(QStringLiteral("ownPairs"), static_cast<qulonglong>(shared.ownPairs.load(std::memory_order_relaxed)));
    // Step 4: the causal schedule's facts (PushCausalShared, written by the engine once a second and on a change).
    const auto& causal = pipeline::pushCausalShared();
    const auto exitName = [](std::uint8_t e) { return QString::fromLatin1(pipeline::pushCausalExitName(static_cast<pipeline::PushCausalExit>(e))); };
    state.insert(QStringLiteral("causalChosen"), causal.chosen.load(std::memory_order_relaxed));
    state.insert(QStringLiteral("causalLevel"), QString::fromLatin1(causal.level.load(std::memory_order_relaxed) == 0 ? "causal" : "onepredicted"));
    state.insert(QStringLiteral("causalLastExit"), exitName(causal.lastExit.load(std::memory_order_relaxed)));
    state.insert(QStringLiteral("causalStoppedFor"), exitName(causal.stoppedFor.load(std::memory_order_relaxed)));
    state.insert(QStringLiteral("causalReturnHave"), static_cast<uint>(causal.returnHave.load(std::memory_order_relaxed)));
    state.insert(QStringLiteral("causalReturnNeed"), static_cast<uint>(causal.returnNeed.load(std::memory_order_relaxed)));
    state.insert(QStringLiteral("causalPerSecond"), static_cast<uint>(causal.causalPerSecond.load(std::memory_order_relaxed)));
}

}  // namespace veyra::ui
