# HUD Guard: keeping a still crosshair / HUD out of frame generation (stream s12-hud, 25 Nov 2026)

Status: implemented on branch `exp/s12-hud` for the graph's frame generation (NVIDIA VFG with pre-clean; PushFlow,
AMF, DLSS-G/FSR, "Lower delay" with compose only). TESTED: unit (CPU reference, synthetic scenes) and WARP (GPU passes
against the reference). Not tested on any graphics card; whether the copies are gone on the owner's screen is known only
when he looks. Research and measurements: `HUD_GHOSTING.md` (research folder).

## What the user sees and why

An interpolator moves each pixel with the motion of its block. A 1-3 px crosshair is far smaller than any block, so a
generated frame at time t draws it where the background carries it (c − t·d from A, c + (1 − t)·d from B, d = the
background's shift per real frame) and not at its place: at 3X one sharp and four faded copies per real-frame period.
Live HUD Lock alone puts the opaque core back at c, but the copies lie on moving pixels and stay.

## How it works (pipeline/PushHudGuard.h = the rules and CPU reference; pipeline/PushHudGuardPass.h = GPU)

Revised after review S12_HUD_REVIEW1 (25 Nov 2026). Per real frame B (against A, the previous real frame; nothing waits
for a later frame):
1. change = |luma(B) − luma(A)| and contrast = largest |luma(B) − luma(4-neighbour)|, R8G8_UNORM (BT.709 weights for
   SDR, BT.2020 for HDR10); moving pixels per 16x16 tile.
2. state (R32_UINT): `run` (still frames in a row, change ≤ 0.015), `nearby` (frames of the run with a moving pixel,
   change > 0.03, within 8 px on one of 4 axes WHILE ≥ 30 % of the 80x80 px around moved: a camera turn, not one object
   passing a still scene), `thin` (moving on both sides of one axis), `edge` (up with contrast ≥ 0.05, down without),
   `held` (a protected pixel may change by ≤ 0.04 for at most 2 frames in a row), `recent` (2 s after being protected:
   re-arms after 2 still frames, counts kept). HDR10: every threshold × 0.6.
3. mask (R8_UINT): protected = run ≥ 6 (2 when recent) ∧ nearby ≥ 3 ∧ a still structure (run ≥ its arm frames,
   edge ≥ 3) within 1 px; cleaned = protected ∧ thin ≥ 1.
4. pre-clean: the generator's input copy gets, at cleaned pixels grown by 1 px, the 1/distance-weighted mean of the
   nearest pixel outside along 8 directions (≤ 12 px).
5. compose: each generated frame at t: protected pixels, and pixels within 1 px of one that did not move themselves,
   = A + t·(B − A) of the real frames.

What the revision fixed (OFFLINE, CPU, PERFECT generator = the truth, so every wrong pixel is the guard's own): a thin bar
crossing a still picture at 12-20 px per frame lost 478-616 px per generated frame before, 0 now; sky with thin trunks in
a pan: 6070-7080 wrong px before, 0 now; a crosshair that changes when aiming was exact in 26 % of its pixels' generated
frames, 75 % now (re-arm after 2 frames).

Restoring switches (`include/veyra/engine/PushLegacy.h`, slipstream.ini): `VEYRA_TEST_RP_LEGACY_HUD_GUARD = 1` (Live
HUD Lock as in Package 16), `VEYRA_TEST_RP_LEGACY_HUD_GUARD_CLEAN = 1` (compose only: a diagnostic, not a mode to offer:
compose alone leaves every copy, 160 → 160 ghost px at 3X in the circle pan), `VEYRA_TEST_RP_LEGACY_PACKAGE16 = 1` (both).
Evidence: `[hud-guard] state=executing ... preClean=on protectPx= cleanPx= centreProtectPx= centreCleanPx= aimProtectPx=
aimCleanPx= aimRunMax= gpuHudGuardMs=` once a second (aim = 128x128 px at the exact centre), `gpu_hudGuard*` in
`[fg-health]`. Cost: update + pre-clean run in the real frame's base list, which every frame of the pair waits for:
every frame shown is later by that GPU time (estimated 0.25-0.6 ms at 4K from bytes moved; his log will show it).

Known limits: translucent overlays below about 0.9 opacity are not protected (they change with the background); a flat
still background touching the overlay is protected too, so a fast thin object crossing exactly there between two real
frames is missing in that 1-3 px band (about 45-60 px per generated frame in the sky-and-trunks scene with a crosshair,
none farther away); elements thicker than 3 px are protected only where they have contrast or within 1 px of it;
protection starts 6 real frames (200 ms) after an overlay appears and only while the picture around it moves (a camera
turn): over large flat areas (sky) little moves, so little is protected there; hit markers shorter than 200 ms are never
protected.

## Aim anchor (26 Nov 2026)

Why: over a FLAT picture (sky, wall, fog) nothing around the crosshair moves above the thresholds, so the `nearby` rule
never armed and the crosshair kept its copies. The anchor protects a still structure in the 128 x 128 aim window at the
exact centre after `anchorFrames` = 30 still frames.

Scope (second revision, after the first CI run showed the first version protected nearly a whole textured still picture,
`remoteplay-checks` #449, and drove four tests red):
- Flat surround only: at most `anchorFlatMax` = 144 of the 17 x 17 px around the pixel are a still structure (run and edge
  counts as for the normal rule). A crosshair's own arms are about 120; texture, and the 2-px strokes of text, are far more.
  So text over flat sky is NOT anchored (SIMULATED: 836 px protected at frame 39 in the 160 x 112 text-over-sky scene,
  informational: that is the normal rule's neighbours, not a statement that the text is covered).
- Never while anything near the aim window changes: state bit 28 (`iso`) is now set for aim-window pixels when any 16 x 16
  tile overlapping the aim window grown by `anchorMargin` = 64 px has a pixel with change > `still`. This replaces the
  per-pixel "moving pixel within `reach`" test, which a small object jumping 40 px between real frames escaped. A lone
  mover that jumps further than the margin in one frame (200+ px at 4K, outside the 256 x 256 area both before and after)
  is still not seen: a limit.
- CPU reference (`PushHudGuardReference::update`), `shaders/PushHudGuardUpdate.hlsl` (busy test) and
  `shaders/PushHudGuardMask.hlsl` (flat surround) implement the same rule; `testHudGuard(..., anchorCase)` in
  `tests/integration/PushGpuTests.cpp` compares state words, masks and counters at every pixel for a flat-sky scene with a
  square jumping in at frame 34 (WARP, SDR and HDR10; see below for what was run).

Numbers (all SIMULATED: CPU reference, synthetic 160 x 112 pictures, g++ on Linux CI; the simulated generator moves the
crosshair with the true camera motion, a real generator over a textureless picture may leave it in place, so the benefit
may be smaller or absent on real hardware; nothing here was seen on a graphics card):
- flat sky + crosshair, pan 20 px/frame: crosshair exact in 144/1200 (2X) and 312/2400 (3X) pixel-frames raw, 1200/1200 and
  2400/2400 with the guard; SDR and HDR10 alike. Wrong pixels 230 -> 112 (2X), 250 -> 133 (3X); the rest are ghosts of the
  sky's own gradient, not of the crosshair.
- PERFECT generator, still flat sky, crosshair anchored, an object enters at frame 40 (8x8 at 40 px/frame, 24x2 at 60,
  6x60 at 20; 2X and 3X): 0 wrong pixels and the crosshair exact in every case (protected pixel-frames 1408-2364).
- Textured still picture with a crossing object (the original five cases): 0 wrong pixels, as before the anchor.
- Crosshair gone at frame 45 over still flat sky: protected 352 px at frame 44, 0 at 45 (the picture around changed), then
  252 px at 46-56 and 0 from 58. KNOWN LIMIT: the still sky beside the old crosshair keeps its `edge` count (falls by 1 a
  frame from at most 15) and is anchored again once nothing changes, for at most 12 frames. They are unchanged pixels
  (A + t(B - A) is the truth for them) and anything that changes drops the anchor, so harmless; pinned (gone by frame 58).
- Dark pictures: nothing moves above the thresholds and the picture's own changes exceed `still` in the aim area, so there
  is no protection (expected, same as raw); what is pinned is "not worse" (see the test comments for the bound).

Findings of the same CI run for scenes that do not involve the anchor (SIMULATED):
- HDR10 highlights (gain 1.6), circle pan 24, frames 16-23: the whole crosshair is protected (100 px) from frame 6, exact in
  699/800 (2X) and 1398/1600 (3X) pixel-frames, raw 5/800 and 2/1600, wrong pixels 6422 -> 6328 and 5730 -> 5646. About one
  crosshair pixel in eight is not exact: where the bright picture's PQ value is within `backgroundMatch` of the crosshair's
  the compose keeps the generator's output. (The first run, frames 8-13, gave 510/600 at 2X: the same cause, since protection was
  already on at frame 6.) Pinned at >= 80 % exact.
- Soft-edged crosshair (anti-aliased 1-px fringe), circle pan 24, frames 8-13, fringe mean error in 8-bit codes, raw ->
  guard: fringe 0.5: 41.0 -> 44.3 (2X), 41.8 -> 44.2 (3X); fringe 0.3: 27.4 -> 31.1 (2X), 27.4 -> 31.0 (3X). So the guard is
  slightly WORSE than the raw generator on the fringe pixels (about +3 codes) while the core goes from 13 to 605 exact
  pixel-frames and wrong pixels fall (3184 -> 2766). Cause: the pre-clean takes the fringe out of the generator's input and
  the compose does not touch pixels that change every frame. NOT FIXED: a fix (blend the generator's pixel with the overlay at
  the pixel's estimated coverage) needs a shader change that could not be run here. Pinned at 1.5 x raw + 5 codes.
- Fast camera 60 px/frame and the dark-picture scenes: numbers not yet read (the run stopped earlier in the same test); the
  assertions there are loose (25 % slack) until they are.

- Review, 26 Nov (a reader who did not write the rule): the busy test switches the anchor off for a pair when ANY pixel
  in the aim window + 64 px changed by more than `still`. Synthetic sky has no grain; a real picture with film grain,
  dithering or sensor-like noise may trip it on every frame, in which case the anchor never arms and the guard behaves as
  in Package 16 (safe, but no benefit). Whether it arms on real footage is UNKNOWN until an owner's log shows
  `anchor` protection counts.
- Review, 26 Nov: the mask shader's 17 x 17 flat-surround scan and the update shader's busy scan are behind explicit
  `[branch]`es, so they run only for anchored aim-window pixels (at most 128 x 128), not for every pixel of the picture.
  Their GPU cost on a real card is unmeasured.

Verification status: unit tests on g++ and MSVC (`remoteplay-checks`) read per run; the WARP anchor comparison is added in
`PushGpuTests.cpp` and its result is reported separately. Unverified on any graphics card.

## Interface for the own frame generator (branch exp/s11-ownfg, PushWarp)

PushWarp's compose already keeps pixels with A == B (`stillDelta`) as A, which restores the crosshair's core at c (like
compose-only), but its splat and its two bilinear taps still fetch the crosshair from A at p + t·g and from B at
p − (1 − t)·g wherever the background's vector points at it: the copies. To adopt the guard:

1. **Inputs.** Call `PushHudGuardPass::update(list, tracker, parity, reset)` once per real frame before the flow and
   the warp are recorded (the base list), as EnhanceGraph does for VFG. Read the state texture
   `PushHudGuardPass::state()` (R32_UINT, see pushHudGuardPackCell) with the predicates of `PushHudGuard.h`
   (`pushHudGuardUnpackCell`, `pushHudGuardArmed`, `pushHudGuardStructure`), or better the mask
   `PushHudGuardPass::mask()` (R8_UINT: bit 0 protected, bit 1 cleaned; grow by `params().grow`), or the cleaned picture
   `PushHudGuardPass::cleaned()` (after `clean(list, tracker, parity)`; it is a full copy of B at the working size and
   format, only the cleaned pixels differ).
2. **Exclude from the flow.** Feed the flow (FidelityFX / densify) the cleaned A and B (the cleaned B of the previous
   real frame is A's: keep it, or clean A again from the current state; cleaning is idempotent on unchanged
   neighbours). A thin overlay then cannot pull a block's vector to zero or create a false match.
3. **Exclude from the samples.** In R3 compose, a tap whose source pixel is in the clean mask must not be used:
   sample the cleaned pictures instead of A and B (simplest: bind `cleaned()` copies in place of A and B for the two
   taps), and in R1 splat skip cells whose 4x4 footprint is mostly masked (their vector belongs to the overlay).
   This is the "only use pixels from the other direction" rule of TV MEMC logo handling (US 8,446,524).
4. **Static override.** After R3, for protected pixels write A + t·(B − A) of the *real* (uncleaned) pictures: call
   `PushHudGuardPass::compose(list, tracker, slot, parity, t)` on the generated texture (it writes only those pixels
   with a typed UAV store; the slot's texture must be among those given to `createViews`), or apply the same rule
   inside R3 (cheaper: one state read per pixel). PushWarp's own `stillDelta` rule stays (it is a subset for opaque
   overlays: A == B).
5. **Zero-motion candidate (optional, inside R3).** For a pixel not protected, compare the warped result with A(x)
   and B(x): if A(x) and B(x) agree within `stillDelta`... that is already PushWarp's still rule; extending it to
   "A(x) ≈ B(x) within `hold` and the pixel is protected" is exactly step 4.
6. **Tests.** The synthetic scenes in `tests/remoteplay/HudGuardScene.h` (crosshair 1-3 px, translucent, text,
   minimap ring with moving content; straight, diagonal and circular pans) and `hud_guard_scene::measure` (ghost pixels
   outside the overlay against the generator's own overlay-free output, overlay error, background error around it)
   can run PushWarp's CPU reference (`pushWarpReference`) in place of the SIMULATED interpolator: add a `Mode` that
   calls it with the scene's known field. Expected from the simulation: compose alone leaves every copy; pre-clean +
   compose removes 78-95 % of the ghost pixels of opaque 1-3 px elements.

Do not adopt the guard's thresholds blindly for PushWarp's 960x540 flow: the mask is per pixel at the working size;
the flow sees it only through the cleaned pictures.
