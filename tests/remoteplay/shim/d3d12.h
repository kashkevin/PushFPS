// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Linux stand-in for the three names veyra/pipeline/FramePacket.h takes from <d3d12.h>, so the portable core tests can
// include veyra/engine/LivePresentationTiming.h (the live pacing decisions the scheduling simulator runs,
// FgScheduleSimTests.inc). Only on the include path of the g++ builds (-Itests/remoteplay/shim); MSVC uses the
// Windows SDK's own header. Nothing here is ever called.
#ifdef _WIN32
#error "tests/remoteplay/shim is for non-Windows builds only: use the Windows SDK's d3d12.h"
#endif
struct ID3D12Resource;
struct ID3D12Fence;
enum D3D12_RESOURCE_STATES { D3D12_RESOURCE_STATE_COMMON = 0 };
