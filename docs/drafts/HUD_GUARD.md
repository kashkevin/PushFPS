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
