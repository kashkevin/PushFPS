// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Stream "s10-stall": the Windows half of the capture-stall witness (portable part: veyra/engine/PushStallWitness.h).
// Runs only while a capture card source runs (acquire / release from CaptureCardSource start / close): a sampler thread
// at the lowest priority reads system counters once a second into a ring (with VEYRA_EXP_RP_STALL_WITNESS_DEEP=1 also a
// normal-priority probe thread that measures its own wake delay every 10 ms), and a stall reported by the capture callback makes the sampler write [stall-witness] (private,
// with image names) and [capture-stall-cause] (numbers only). The capture callback's calls are lock-free and allocation-
// free. VEYRA_TEST_RP_LEGACY_STALL_WITNESS=1 (or PACKAGE15=1) turns all of it off. Windows sources only.
#include <cstdint>

namespace veyra::source::stall_witness {
// False when the legacy switch restores Package 15 (read once).
bool enabled();
// Start / stop the sampler (reference counted; call from the capture source's start / close, never from a callback).
void acquire();
void release();
// The calling thread's cycle count (QueryThreadCycleTime), 0 when the witness is off. Capture callback thread.
std::uint64_t threadCycles();
// One capture callback: arrival step, timestamp step, entry-to-copied time and its thread cycles, the nominal interval, the
// source's callback counter (1 = it (re)started: the copy baseline is forgotten) and the sample's size in bytes (a new size
// forgets it too). Lock-free; capture callback thread only.
void onCallback(double arrivalDeltaMs, double ptsDeltaMs, double copyMs, std::uint64_t copyCycles, double intervalMs, std::uint64_t received, std::uint64_t bytes);
// The engine thread picked a frame up this long after the callback received it. Lock-free.
void onEnginePickup(double ms);
}  // namespace veyra::source::stall_witness
