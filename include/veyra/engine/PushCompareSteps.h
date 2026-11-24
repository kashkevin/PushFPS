// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// The autorun hook steps `sr`, `fg` and `quality` (src/ui/bridge/PushCompare.cpp, docs/SUPER_RESOLUTION.md).
// registerPushCompareAutorunSteps registers them with engine::autorun::registerStep (PushAutorunPlan.h) and, with
// VEYRA_EXP_RP_STATS=1, starts the 5 s [slipstream-fg] line. Call once on the interface thread after the bridge
// exists. Unverified on Windows.
namespace veyra::ui {
class QmlPlayerBridge;
void registerPushCompareAutorunSteps(QmlPlayerBridge& bridge);
}
