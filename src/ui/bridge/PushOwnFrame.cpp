// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// QML bridge, stream "ownfg": the Lower delay row's frame source (pref pushLowDelaySource) and the experimental causal
// schedule (pref pushLowDelayCausal). Both reach the engine as relaxed stores read once a pair. With VFG, PushWarp exists
// in the graph only once the user chose one of them (review 1, SHOULD 9: nothing of it runs or allocates for the owner's
// default "NVIDIA"); the first such choice in a run commits EnhancementSettings::lowDelayOwnWarp, which rebuilds the graph
// while the row's pass exists (the picture restarts briefly and the row says "checking" while PushWarp's start-up
// self-check runs). It stays true for the rest of the run, so choosing NVIDIA again rebuilds nothing (PushWarp is then
// simply not used). Unverified on Windows.
#include "veyra/ui/QmlPlayerBridge.h"
#include "../QmlPlayerBridge_p.h"
#include "veyra/ui/PushOwnFrameBridge.h"
#include "veyra/Log.h"

#include <format>

namespace veyra::ui {

void QmlPlayerBridge::applyPushOwnWarp() {
    const QString source = impl_->prefs.value(QStringLiteral("pushLowDelaySource")).toString();
    const bool causal = impl_->prefBool("pushLowDelayCausal", false);
    pushOwnFrameApplySource(source);
    pushOwnFrameApplyCausal(causal);
    if (!pushOwnFrameWarpChosen(source, causal)) return;
    auto s = settings();
    if (s.lowDelayOwnWarp) return;   // already in the graph's settings for this run
    s.lowDelayOwnWarp = true;
    veyra::log::info("push-warp", std::format("the user chose {}: PushWarp is created with the next graph{}", causal ? "the causal schedule" : "the PushFPS frame source",
                                              s.lowDelayOwnWarpShapesGraph() ? " (rebuilt now)" : " (the row's pass is not in this graph: nothing rebuilt)"));
    if (!impl_->commit(std::move(s))) {
        veyra::log::warn("push-warp", "the engine did not accept the PushWarp setting; NVIDIA's frame stays in use");
        emit notice(tr("引擎未接受该设置，仍使用 NVIDIA 的画面"), true);
        return;
    }
    emit settingsChanged();
}

}  // namespace veyra::ui
