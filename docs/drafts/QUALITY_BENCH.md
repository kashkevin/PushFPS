# Quality bench: objective comparisons of 30 / 60 / 90 FPS, the downscale and the generators (stream s12-bench)

Status: Phase 1 (offline bench) BUILT and RAN on Linux (CPU). Phase 2 ("On the PC") is a DESIGN only, nothing implemented.
Results: `out/research/OBJECTIVE_COMPARISON.md`. Labels: SIMULATED (this bench), TESTED (pytest here), DESIGN (not built).

**What the offline bench is.** A simulation of the whole chain as the eye receives it, on synthetic 4K pictures with exactly
known motion. It shows mechanisms and their sizes relative to each other. It is NOT a measurement of the owner's screen,
capture card, graphics card or NVIDIA's generator: no number from it may be presented as measured.

## 1. Running it

```
python3 -B tools/quality_bench/run_all.py --quick            # about 4 minutes here (2 CPU cores)
python3 -B tools/quality_bench/run_all.py                    # full (about 9 minutes): more speeds, seeds, phases, HUD cases
python3 -B tools/quality_bench/run_all.py --only Q3 --out /tmp/q3.md
python3 -B -m pytest -q tools/quality_bench/tests            # unit tests of the metric functions and the downscale port
```

Needs Python 3 with numpy and scipy, and g++ (C++20) for the vendored references. Built tools and job files go to
`$QB_BUILD` or `/tmp/quality_bench_build` (a few MB). 4K frames are regions of interest (crops) held in memory only.
Everything is deterministic (fixed seeds). The vendored references (`tools/quality_bench/vendor/`, source commits in
`SOURCES.txt`) are read-only copies; `vendor/retake.sh` copies them again from their branches.

## 2. The chain (every stage swappable)

| Stage | Choices | Where |
|---|---|---|
| Source | synthetic 4K crops, 30 FPS, periodic worlds (exact camera shifts); pans (1.5-900 4K px a frame), circular camera move, still camera with a thin fast object, an object over a counter-pan; textured / low-contrast / periodic (fence, gratings) backgrounds; HUD: crosshair (1/2/3 px, opaque or translucent), 4-px dot, small text (5x7 font, 2-px strokes), minimap ring; SDR (gamma 2.2, 8-bit) or HDR10 (PQ, 10-bit, highlights to 1000 nits); console motion blur baked into each real frame (shutter 0 / 50 / 100 %) | `qb/scenes.py` |
| Frame generation | none (native 30), ideal (true in-between frame: upper bound), block-motion MODEL (16x16 blocks with oracle vectors, bilinear taps, blend in codes), weak (block mixed with a cross-fade, beta 0.3 / 0.15: a KNOB for "a lower-quality model"), mixed (3X: weak at 1/3, block at 2/3: his VFG level 5's pattern), PushWarp (vendored `pushwarp_cli frames`: the GPU path's rules on the FidelityFX-like flow MODEL); HUD protection: HUD Guard reference (block family) or PushWarp's overlay rules; 2X and 3X | `qb/generators.py`, `qb/chain.py`, `qb/native.py` |
| Downscale 3840 -> 2560 | `p16` (one bilinear tap on PQ codes), `area` (s12-pace's committed filter: 3x3 bilinear taps on codes, decoded per tap), `exact` (every texel decoded, exact box weights, linear light: the target recipe of S12_PACE_REVIEW2 MUST 2) | `qb/downscale.py` |
| Display | 179.952 Hz OLED sample-and-hold, no strobing; holds `ideal` (6/3/2 refreshes) or `composed` (s12-pace's model of the DWM path fitted to his phase bins: cut-off 0.3-3.0 ms before each vblank, 12 % late passes, 29.97 Hz source; `composed29.6` with session A's source) | `qb/display.py` |
| Observer | tracking eye (smooth pursuit at the true speed; retinal image = light integrated along the trajectory, 16 time samples per refresh, 8 space samples per output pixel, panel pixels as boxes) or fixating eye (HUD) | `qb/observer.py` |

## 3. Metric definitions (units a person can picture)

1. **Unique displayed frames per second**: distinct pictures actually shown in one second. A frame with hold 0 is never shown;
   a generated frame is a near-duplicate when its mean absolute difference to the previous or next shown frame is below 10 %
   of d(A, B) of its pair (PushFrameProbe.h `kDuplicateShare`; here on the output picture, not the 128x72 thumbnail).
   Ideal: 30 / 60 / 90.
2. **Frame spacing**: on-screen hold histogram (refreshes per frame, k = 6 / 3 / 2 ideal; even = exactly k) and **judder**:
   for the tracking eye, each displayed frame's position error = the time-weighted mean over its hold of (picture
   position - eye position), in output pixels; judder RMS = time-weighted standard deviation over frames, worst = largest
   deviation. 0 = every frame sits at the same place relative to the eye (only blur remains). Also as % of the frame's
   step (speed / FPS), because a 2-px error is a larger share of a 10.7-px step at 90 than of a 16-px step at 60.
3. **Motion clarity**: the tracked retinal image of a sharp edge. *Blur width* = sqrt(12) x the standard deviation of
   the edge's line-spread function (a box blur of w pixels reads exactly w; physics: speed x hold, Blur Busters' law).
   *Rise width* = 10-90 % rise / 0.8 (also w for a box; insensitive to faint far copies). *Tracked MTF* at 0.1 / 0.2 /
   0.3 cycles per output pixel = retinal contrast / source contrast of a moving sine grating (linear light).
   Separated by switching one stage at a time: hold (30/60/90), baked source blur, generator, downscale.
4. **Fine-detail preservation** (output, nits): 1 / 2 / 3-px 4K lines at all quarter-pixel phases: peak above background
   (mean and worst phase), energy error vs the exact area result (light lost), *energy pulse* and *peak pulse* =
   (max - min) / mean across phases (what a slowly moving thin line does over time: the "every third column" flicker),
   FWHM; small text stroke contrast; moire RMS (a grating beyond the output Nyquist, % of its amplitude; ideal 0) and
   shimmer (temporal RMS while it drifts); contrast kept at periods of 10 / 5 / 4 / 3 / 2.5 output px (the price).
5. **HUD stability** (fixating eye, output resolution): crosshair brightness = light added by the overlay inside its own
   footprint / the same for the true picture through the same filter (1 = right; 0 = the crosshair is missing); its
   30 Hz flicker = amplitude of the 30 Hz component of that brightness sampled every refresh, % of its mean; position
   error = distance (px) between the centroid of the added light around the footprint and the true one; width SD = the
   arm's FWHM standard deviation across frames. "Crosshair light vs exact filter" = the static crosshair's light through
   this filter / through the exact filter (what the downscale alone does to it).
6. **Ghosting** (output resolution, codes): ghost pixels = pixels outside the overlay's footprint (grown 2 px), within 40 px,
   where the frame differs from the SAME generator run on the overlay-free scene by more than 8/255 of the code range;
   copies = connected groups of >= 2 such pixels; per second = per displayed frame x frames per second; energy = sum of
   the excess. Doubled edges / thin objects (Q4): error vs the ideal frame (mean, P99, share > 32 PQ codes) and the share
   of a thin object's pixels within 32 codes.
7. **HDR accuracy**: nits and PQ codes of a large 1000-nit patch and of thin 1000-nit lines (peak, energy error);
   delta E ITP (BT.2124; about 1 = just noticeable) and hue-angle shift of a saturated BT.2020 red stroke on green;
   light of thin highlights in generated frames (blend in PQ codes) and its 30 Hz flicker; the nits of a cross-faded
   crosshair copy blended in codes vs in linear light.

Validation built in (`run_all.py` section V; unit tests `tests/`): a static picture gives zero blur and judder; ideal holds
give blur = speed / FPS (32.0 / 16.0 / 10.75 px at 960 px/s); the crosshair gives 3 images at 2X and 5 at 3X; Package 16's
and s12-pace's downscale reproduce the reviewer's numbers (1-px stroke 44 / 1 nits; moving line 1.06 / 1.33 / 114 vs
1.10 / 1.35 / 112, area 0.42 / 0.74 / 98 vs 0.42 / 0.75 / 99); the port of PushDownscale.h equals the header (g++).

## 4. On the PC (Phase 2: DESIGN ONLY, not implemented)

### 4.1 What one NORMAL play session can already answer (evidence built by the other streams)

| Offline metric | On-PC counterpart today | Answers | Cannot answer |
|---|---|---|---|
| 1 unique frames/s | `[frame-probe]` (s12-pace: 128x72 thumbnail of every presented frame) `nearDuplicatePct`, byPosition `dupPct` | are generated frames distinct, per position (1 = Low model at his level 5) | pictures after the downscale (the probe reads the 4K graph output) |
| 2 frame spacing | `[cadence]` (s12-pace) composed path: hold histogram, `evenPctMin..Max`, judderPct, phase bins; direct path: Present spacing only | on-screen holds on the composed path (narrow range since review 2 MUST 1: one reading per refresh) | **true on-screen holds on the direct path (independent flip + VRR)**: not measurable in-process (DXGI refresh counter stands still; ETW needs rights) |
| 2 evenness in motion | `[frame-probe]` `evennessP50`, `stepShareP50` (ideal 0.333 x 3 / 0.5 x 2), `blendLikeP50` | thirds or not; cross-fade-like frames | small objects (thumbnails) |
| 3 motion clarity (perceived blur) | none; derivable: hold (from `[cadence]`) x speed | the hold part only, by arithmetic | **perceived blur: not measurable in-process** (needs the eye; the bench gives the model) |
| 3 baked source blur | none | - | needs a still-vs-moving sharpness probe on real frames (proposal 4.3 item 4) |
| 4 fine detail / shimmer | `gpu_blit` time in `[fg-health]` (cost of the filter only) | the filter's GPU cost | the picture after the downscale (no probe reads it) |
| 5 HUD stability | `[hud-guard]` (s12-hud): `state`, `aimProtectPx`, `aimCleanPx`, `aimRunMax`, `gpuHudGuardMs` | whether the crosshair is protected / cleaned while he plays | the crosshair's brightness and copies on the picture (proposal 4.3 item 3) |
| 6 ghosting | frame quality test (PushQualityProbe: withholds every 2nd real frame, compares the generated frame with it: PSNR, HUD-box PSNR, bad-pixel share) | generator error vs the truth on HIS game, with the HUD boxes | runs only on demand; at 2X its "withheld" truth is a real frame 1/30 s later (it measures a t = 1/2 frame); not per multiplier |
| 7 HDR accuracy | `[vfg] encoded format`, `[present] output=` | which path (PQ codes into VFG, HDR10 out) | nits on screen |
| settings actually running | `[settings-effect]` (s12-settings: requested / applied / executing, why) | that a 2X / 3X / quality level really ran | - |

### 4.2 The one-click "Compare 30 / 60 / 90" (extend the EXISTING frame quality test; no new setting)

Goal: he presses the existing "Frame quality test" (dialog `qualitytest`, `PushQualityTestDialog.qml`) or "Optimise for my
PC"'s `quality` step and gets one table, without repeating manual tests. The run switches the multiplier itself:

1. Pre-checks (refuse with a plain reason): a live capture source, frame generation available, he is not recording or
   streaming. Show: "The picture freezes for 1-3 s at each switch (3 switches + the return)". His log shows 2.6-3.6 s
   `[engine-stall]` per VFG re-creation with a new multiplier (P16_REAL_LOG_ANALYSIS 0.2).
2. Order 3X -> 2X -> 1X -> back to his settings (start at his current multiplier when it is 3X, so the first segment needs no
   switch). Each segment: switch, wait until `[settings-effect]` reports the multiplier executing and 2 s after the last
   `[engine-stall]`, then measure for a fixed 20 s (`--seconds`, 10-60). He keeps playing normally; ask him to turn the
   camera now and then (the probes only judge pairs with motion).
3. Per segment, collect from the existing windows (no new GPU work except item 4.3-3 if built): `[cadence]` (path,
   evenPctMin..Max, hold histogram, judderPct), `[frame-probe]` (nearDuplicatePct, evenness, stepShares, blendLike, by
   position), `[hud-guard]` aim counters, `[fg-health]` gpu_blit / generation times, `[settings-effect]` rows; and the
   withheld-frame comparison of PushQualityProbe for 2X and 3X (it already withholds every second real frame; for 3X it
   must compare the generated frames at 1/3 and 2/3 of the A..C span with the withheld B at 1/2: NOT the same time; so
   report its 3X PSNR as "t = 1/3 and 2/3 against the frame at 1/2" or compare only the 2X segment: a limit to state, not
   to hide).
4. Restore his settings exactly (the optimiser's `pushOptimizeRestore` mechanism, already crash-safe) and show the table.

Proposed log line, one per segment, then a summary (fields absent = not measured, never 0):

Format only: every value below is a PLACEHOLDER (<...>), not a result.

```
[compare-fps] segment=<1..3> multiplier=<1|2|3> seconds=<s> path=<composed|direct|unknown> refreshHz=<hz> presented=<n> generated=<n>
  unique=<n>/s nearDupPct=<p> byPos=1:<p>,2:<p> evenness=<0..1> stepShares=<a>,<b>,<c> blendLike=<x>
  holdEvenPct=<min>..<max> holdHist=1:<p>%,2:<p>%,3:<p>%,4+:<p>% judderPct=<p> spacingEvenPct=<p> direct=<0|1>
  hudAimProtect=<0|1> gpuBlitP95Ms=<ms> genP50Ms=<ms>/<ms> settingsExecuting=<rows> stalls=<n>
[compare-fps] summary 30:<fields>; 60:<fields>; 90:<fields>
  verdict="90 showed <a> new pictures per second, 60 showed <b>; <x> % / <y> % of them stayed on screen for an even time"
```

Table shown to him (plain words; every number would come from his PC and says how it was obtained; placeholders here):

| | 30 FPS | 60 FPS | 90 FPS |
|---|---|---|---|
| New pictures per second (measured) | <n> | <n> | <n> |
| Pictures shown for an even time (measured, window path only) | <min-max %> | <min-max %> | <min-max %> |
| Pictures evenly spaced in motion (measured on small thumbnails) | - | <0..1> | <0..1> |
| Generated pictures that looked like a fade, not motion (measured) | - | <%> | <%> (per position) |
| Smear while following motion at 960 px/s (CALCULATED from the holds, not measured) | <px> | <px> | <px> |

### 4.3 Still missing for that table (in order of value)

1. **Direct-path holds**: not measurable in-process (state it; offer PresentMon as an optional manual step only if he asks).
2. **The picture after the downscale**: extend the frame probe's thumbnail pass to also read the presented back buffer
   (after PresentBlit) every Nth frame, so shimmer / thin-line flicker (metric 4) and copies at output size can be judged.
3. **Crosshair probe**: a 64x64 output-pixel crop at the picture centre from the same readback, every presented frame
   for 2 s per segment: brightness per frame, 30 Hz flicker %, a copy count (the bench's metric 5/6 on real pictures).
   Cost estimate: one 16 KB copy per frame on the presentation queue after Present (as the probe), not measured.
4. **Baked-blur estimate**: edge sharpness of real frames in motion vs still (from the existing thumbnails is too coarse;
   needs a 1/4-size luma readback of real frames every 0.5 s): tells how much of the 60 -> 90 gain the game's own blur
   leaves (the bench's largest factor).
5. The quality-test's time mismatch at 3X (4.2 item 3).
Nothing here is implemented; each needs the owner's OK and the normal review before code.
