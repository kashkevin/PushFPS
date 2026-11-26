# Stream s12-pace: pacing measure and 3840 -> 2560 downscale (draft, status of the six items)

Branch `work/s12-pace` (pushed to `ck/s12-pace`). The batch behind items 2-5 was written by an interrupted team and committed as found
(`ca1e1f96`, never compiled); it was reviewed and its first compile errors fixed afterwards. This file replaces the draft that was lost with
the earlier private branch: it does not recreate any lost numbers.

Evidence classes used below:

- **simulation**: pure C++ or Python models run in CI (`remoteplay-checks`, g++ and MSVC) or by the script in `tools/pace_downscale`.
- **WARP**: the D3D12 software renderer in the `gpu-quick` lane (`tests/integration/PushGpuTests.cpp`). Checks that shader and CPU reference agree; says nothing about speed or about a real GPU.
- **real GPU**: only the owner's logs. None are quoted here.
- **display measurement**: none exist (no photodiode or high-speed camera data).

This draft prints no measured table. The kernel table is produced by `ONSCREEN_SIM_PRINT=1 core_tests` (simulation) or
`python -B tools/pace_downscale/downscale_ref.py` (simulation); paste its output here when someone has run it. No claim about real hardware is made.

## 1. The two QML failures (`tst_push_quick_setup.qml`)

`test_steady_detail_is_off_by_default_and_a_preference` and `test_unfocused_notice_switch_is_a_preference` read `undefined`. Cause by reading:
the mock `veyra` in `tests/qml/QuickSmokeTests.cpp` copies the real validator's rule that any `push*` key not in its list is refused
(`setPreference` returns false and stores nothing). `pushSteadyDetail` and `pushUnfocusedNotice` are accepted by the real
`Preferences.cpp` (lines ~301-303) but were missing from the mock's `pushKeys`. The batch added both. Status: **done by construction;
unverified on Windows** (QML tests only run in the full Windows build, which was not run for this head).

## 2. Unbiased on-screen pacing measure, 2X against 3X

`PushOnScreenCadence.h` gains `showJitterMs`, `holdSdMs`, `judderMs` (milliseconds, not relative to the frame length) and writes them into the
`[on-screen-cadence]` line. The old `judderPct` is relative to the frame length, so one refresh of error is 50 % at 3X and 33 % at 2X
for identical display jitter (a factor 1.5 by construction); the new figures do not have that bias. Tests
`onscreen_unbiased_scores_*` in `tests/remoteplay/OnScreenCadenceTests.inc`: one shared jitter sequence applied to a 2X and a 3X timeline
must give equal ms scores, `judderPct` must differ by 1.5, window length must not matter, zero jitter must read zero. **Simulation only.**
Fixes made in review: a printf format string with a real newline (did not compile), and the spread estimators now use exact integer sums and a
shifted line fit so a perfect schedule reads exactly 0 (the zero-jitter check failed in CI on `ca1e1f96`+fix with rounding noise
as the likely cause; see the commit history for the CI result). Status: **done in simulation; no display measurement**.

## 3. The accepted 2X duplicate fix kept under test

The tests around `PushAdmissionAnchor.h` (section "2X + duplicate removal defect" in `OnScreenCadenceTests.inc`) pre-date the batch and were
not changed by it. Status: **kept (simulation tests green in `remoteplay-checks`)**; nothing new was added for it.

## 4. Linear-light downscale 3840 -> 2560, sharper variants

`PushDownscale.h` (CPU reference), `shaders/PresentBlit.hlsl` (`ShrinkSample`), `VideoPresenter.cpp` (kernel choice, default
`AreaSharpClamp`, A/B through `VEYRA_EXP_RP_SHRINK_KERNEL` = area | sharp | clamp | catmull | catmull-clamp | lanczos | lanczos-clamp).
Every texel under the footprint is decoded to linear light (PQ, sRGB or none for scRGB), weighted, normalised, re-encoded. New
variants: `*Clamp` (result clamped per channel to the range of the texels under the pixel's own box, so no overshoot past a 1000-nit
highlight and no negative light), Catmull-Rom, Lanczos-2. Tests: `downscale_kernel_table` and
`downscale_hdr_accuracy_and_no_overshoot_beyond_the_source_peak` (simulation, f = 1.5 and 2.63, PQ and sRGB), `present-shrink` in
`PushGpuTests.cpp` compares the shader with the reference for every selectable kernel (WARP). Review: HLSL and C++ weights, loop
bounds (`floor(c - r)` .. `ceil(c + r) - 1`), the own-box test (1e-3) and flag bits (8192 / 16384 / 32768) agree. The shader caps a footprint
at 64 texels per axis (about 21 source pixels per screen pixel) which the C++ reference does not: irrelevant at 1.5 and 2.63.
Not decided: which variant is "sharpest enough"; the table is the input for that choice, and the choice stays off by default
(`pushSteadyDetail`). GPU cost on real hardware (`gpu_blit` in `[fg-health]`) is **not measured**. Status: **partial, simulation + WARP only**.

## 5. `[frame-probe]` without false alarms in slow camera movement

Groups whose real-pair difference is below `kJudgeMotion` (0.02) are counted as `slowNotJudged` and not scored as duplicates, blends or
uneven steps; the log line carries `slowNotJudged=`. Test `frame_probe_slow_pan_is_not_judged_as_duplicates` (added in review; the batch
had the guard without a test) shows the same slow-pan group reading as near-duplicates with the guard off. The threshold 0.02 is a
reasoned value (the 16 point taps per cell move in lumps below it), not tuned on real captures. Status: **done in simulation; the
threshold is unvalidated on real game footage**.

## 6. Window-occlusion checks off the presentation thread

**Open.** The batch did not touch this (no change in `VideoPresenter.cpp` beyond the shrink kernel and the frame-probe switch).

## What is not verified

- Everything in the presenter and shader on a real GPU, and every statement about what a viewer sees.
- The QML tests (need the full Windows build).
