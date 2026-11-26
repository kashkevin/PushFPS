// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
// Stream "s10-display": state shared by every quick-setup panel of this process (one library instance per process).
// repairedThisRun: the G-SYNC profile repair ran in this process, so the driver applies it only after a restart;
// reportedThisRun: the driver's answer after that restart has been logged once.
.pragma library
var repairedThisRun = false
var reportedThisRun = false
