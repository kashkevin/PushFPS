// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Linux stand-in for <dxgiformat.h> (see d3d12.h in this folder). Nothing here is ever called.
#ifdef _WIN32
#error "tests/remoteplay/shim is for non-Windows builds only: use the Windows SDK's dxgiformat.h"
#endif
enum DXGI_FORMAT { DXGI_FORMAT_UNKNOWN = 0 };
