# tools/pushwarp_eval: PushWarp offline (CPU only, no graphics card)

Everything here runs PushWarp's CPU reference (`include/veyra/pipeline/PushWarp.h`, `pushWarpReference`), the same rules
the GPU passes are checked against on WARP in CI. Nothing here measures speed on a graphics card. Results are SIMULATED.

## Build (g++ alone)

```
g++ -O2 -std=c++20 -I include -I tools/pushwarp_eval tools/pushwarp_eval/pushwarp_cli.cpp -o pushwarp_cli
```

## The quality bench's "own generator" stage: `pushwarp_cli frames`

Two real frames in, the generated frames between them out, with the rules of the GPU path.

```
pushwarp_cli frames --a A.ppm --b B.ppm --multiplier 3 --out gen.ppm [--hdr10] [--max N] [--overlay on|off] [--write-field field.bin]
```

- **Inputs** `--a` (the earlier real frame) and `--b` (the later one): the same size, width and height multiples of 4.
  - PPM `P6` / PGM `P5`, 8-bit or 16-bit (big-endian, as the format says), maxval from the file.
  - `.npy`, C order, `uint8` or `uint16`, shape `(H, W, 3)` or `(H, W)`; maxval 255 or 65535.
  - `--max N` overrides the maxval (for example `--max 1023` for 16-bit files that hold 10-bit codes 0..1023).
- **SDR** (default): samples become 8-bit codes, `round(v * 255 / maxval)` (the GPU's R8G8B8A8 path).
- **`--hdr10`**: samples are HDR10 PQ code values, `round(v * 1023 / maxval)` (the GPU's R10G10B10A2 path; BT.2020 PQ
  codes are used as they are, as the GPU does).
- **`--multiplier 2|3`**: the frames at t = 1/2, or t = 1/3 and 2/3.
- **`--overlay on|off`**: the static-overlay protection (crosshair, HUD text, minimap ring; PushWarp.h "Static overlays").
  On by default, as on the GPU whenever PushWarp generates; `off` is the comparison (`VEYRA_TEST_PUSHWARP_OVERLAY=0` in the
  app).
- **Outputs**: `gen_1of3.ppm`, `gen_2of3.ppm` (or `gen_1of2.ppm`) beside the `--out` stem; the extension picks the format:
  - `.ppm`: SDR maxval 255; HDR10 maxval 1023 (two bytes per sample, big-endian, the PQ codes);
  - `.npy`: `uint8` (SDR) or `uint16` PQ codes (HDR10), shape `(H, W, 3)`.
  One JSON line per generated frame on stdout: `t`, the pixel counters (`still`, `blend`, `conflict`, `hole`),
  `wholeFrameNearest` (the whole frame fell back to the nearer real frame), `overlayPx`.
- **`--write-field FILE`**: the field used (u32 width, height; then float32 planes x, y, trust; B -> A in flow pixels).

**The motion field is a MODEL, not AMD's code.** In the app PushWarp's only motion source is AMD FidelityFX optical flow
at a quarter of the work size (960x540 for 3840x2160). Here `FlowModel.h` stands in for it: the same block search as read
from FidelityFX SDK 1.1.4's source (8x8 blocks, +-8 per level, 7 pyramid levels, each level's vectors doubled into the
next, ties to the centre, zero fallback at the finest level, a 3x3 vector median), and PushFPS's densify (nearest block,
photometric trust). It is the C++ port of `ffx_check_sim.py` `flow()` and `hud_eval.py` `field_of()` (checked: the same
field to float rounding, the same frames bit for bit, 26 Nov 2026). AMD's real vectors differ; what the bench reports
from this stage is "PushWarp's rules on a FidelityFX-like field". For HDR10 the field is computed from the PQ codes
reduced to 8 bits (the app feeds FidelityFX an 8-bit picture).

The parameters are the GPU path's (`EnhanceGraphOwnFrame.cpp` `recordPushWarpGenerated`): the defaults of
`PushWarpParams`, `t`, and the overlay rules. One field per pair, as in the app. A 3840x2160 pair at 3X took about 6 s on
one CPU core here (the reference is single-threaded; this is not a speed of anything in the app).

## Other tools

- `pushwarp_cli job.bin out.rgba`: the older job-file interface used by the Python scripts (header in the source).
- `hud_eval.py --cli BIN`: crosshair / HUD ghosting before and after the overlay rules.
- `hud_eval.py --cli BIN --damage`: the damage cases (the rules must not make a picture worse; OWN_FG_DESIGN 9.11).
- `fast_pan.py`, `hdr_eval.py`, `measure.py`, `ffx_check_sim.py`: fast turns, HDR10 blending, the study's harness, the
  FidelityFX check picture.
