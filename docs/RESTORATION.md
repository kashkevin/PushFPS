# Restoration: compression clean-up before the upscaler

Written 20 November 2026 (sprint 5, stream "restore", internal codename Slipstream). **Nothing in this document has
run on a graphics card, on the owner's PC or on a PS5 stream.** The maths was run on Linux as a CPU reference
(unit tests and the bench of section 8); the shader, the pass and the graph hooks are blind Windows code that had
only been compiled by GitHub's Windows runner when this was written (see section 10). The stage is **off by
default** (preference `pushClean` = 0): with it off the app behaves as Package 7.

## 1. What the stage does, and what it does not

One cheap, single-frame GPU stage at the stream's own size (1920x1080 for the PS5's 1080p stream). It runs on the
decoded picture **before** every upscaler, neural layer and frame generator, in plain HLSL (any DirectX 12 card:
NVIDIA, AMD, Intel), in SDR and in HDR, and adds **no frame of delay**: it reads one frame, has no history and no
motion input.

Per pixel, on the display-encoded signal (sRGB code values in SDR, BT.2020 PQ in HDR; both are close to
perceptually uniform, which is why thresholds "in 8-bit code values" mean about the same everywhere):

| part | what it does | switch (`pushCleanParts`) |
|---|---|---|
| protection | a pixel on a strong edge, on a thin line or text stroke (brighter or darker than both sides), or inside a texture (busy small differences) is **not written at all**: it keeps its own texel, bit for bit | always on; `pushCleanHud` = off loosens the thin-line test from 4 to 12 code values |
| deband | the mean of four samples at one random distance (up to 16 pixels) and angle replaces each colour component that is within a threshold of it; then a noise of one output code value hides what is left, fading to nothing at black | 1 |
| deblock | a weak step along a row or a column with flat pixels on both sides (a transform-block edge at ANY position: no 8-pixel grid is assumed) becomes a 4-pixel ramp | 2 |
| dering | beside a strong edge: the mean of the 5x5 neighbours whose luma is within a small distance of the pixel (mosquito noise) | 4 |
| grain | elsewhere: the same mean with a narrower distance (compression grain, smear boundaries) | 8 |

The result of every part is clamped to the range of the samples it read (no overshoot, no new extremes). Strength
(`pushClean`, 0..100) scales the thresholds; 0 dispatches nothing.

After the stage, the **final 8- or 10-bit write** of a frame it changed is rounded with an ordered dither that keeps
black exact (section 2): a debanded gradient would otherwise band again at that write.

What it does **not** do (and cannot):

- It cannot restore detail the encoder discarded. Smeared texture stays smeared, and a soft frame stays soft
  (making it look sharp is the upscaler's job, without halos); the grain part only evens out low-level noise and
  the hard edges of smears.
- It cannot tell compression grain from real fine texture of the same contrast in a single frame. Texture fainter
  than about 2 to 4 code values (by strength) is treated as grain and smoothed; in motion, where the encoder
  smears texture differently from frame to frame, the same surface can be smoothed in one frame and kept in the
  next, which may read as a faint shimmer. Only a look at a moving scene on the owner's screen can show whether
  that happens.
- It does not sharpen (so it adds no sharpening halo), upscale or change colours. Every value it writes stays
  inside the range of the samples it read, for every part alone and together, in SDR and PQ (tested:
  `clean_never_leaves_neighbourhood_range`), so it cannot overshoot. A halo inside that range was possible
  until 21 November: at strength 75 and above, the debanding's taps landing on a faint 1-pixel line (4.5 to 6
  code values) pulled about a hundred background pixels within 16 pixels by 2 to 3 code values (found by the
  independent review). The debanding now skips a pixel whose taps include anything farther in luma from it
  than the line threshold (a change to libplacebo's algorithm, marked in the code), and
  `clean_faint_line_leaves_no_halo` checks that no background pixel 2 or more pixels from such a line moves by
  more than the grain (half a code value).
- **It is single-frame only:** it reads one picture and keeps nothing from earlier frames, so it **cannot ghost
  or trail by construction**. The price is that it cannot use time either: no noise reduction across frames, and
  no way to tell grain from texture by how it moves.
- It does not find the HUD. HUD, text and fine lines are protected because they are edges, thin lines or texture
  by the tests above, not because they were recognised; a HUD box drawn in *HUD protection* still shows the
  console's own picture (the comparison reference is taken before the stage).
- Frames made by frame generation are written by their own backends and do not get the ordered dither.
- With HDR shown through an FP16 (scRGB) swap chain the final quantisation happens outside the graph; no dither is
  added there.

## 2. Sources: what is ported, what is our own

The fourth directive asks for proven implementations before inventions. libplacebo (LGPL-2.1-or-later,
https://code.videolan.org/videolan/libplacebo, commit `89b04663135b`; study file `S5_SRC_VIDEO.md`) is the source.

| part | source | ported or own |
|---|---|---|
| deband | `src/shaders/sampling.c` `pl_shader_deband`, defaults `PL_DEBAND_DEFAULTS` (`src/include/libplacebo/shaders/sampling.h`): 1 iteration, threshold 3.0/1000, radius 16, grain 4.0/1000 | **PORTED** (GLSL to C++ and HLSL). Strength 50 is exactly libplacebo's threshold; the threshold follows the square of the strength around it (25: a quarter, 100: four times); the radius is 16 at every strength; the grain is one code value of the output depth (8 bit: 3.92/1000, libplacebo 4.0; 10 bit: 0.98/1000, libplacebo's renderer gives 0.81 for a 1000 cd/m2 signal) |
| noise generator | `src/shaders.c` `sh_prng` (pcg3d) | **PORTED**, seeded with the frame number as libplacebo does (new noise every frame) |
| final-write dither | `src/shaders/dithering.c` `sh_dither`, method `PL_DITHER_ORDERED_FIXED` (16x16 ordered pattern from bit operations, black kept at zero, not temporal) | **PORTED** into `shaders/ScaleBlit.hlsl` (`OrderedDitherRound`), without the branch for 4 bits and fewer |
| stage order | `src/renderer.c`: deband on the encoded source planes first, scaling after, dither last | followed |
| protection, deblock, dering, grain | **libplacebo has none** (its anti-ringing is a clamp inside its scalers; "deblock" appears only in its film-grain synthesis) | **OWN logic, unproven.** The deblock follows the published shape of codec loop filters (flat sides, a small step, a short ramp), the dering / grain mean is a range-limited ("sigma") filter, the shape of AV1's CDEF constraint. Neither was copied from a project |

Differences from libplacebo's debanding, each for a stated reason:

- The tap offset comes from the generator's integer state through a 16-step quarter-circle table instead of
  `cos` / `sin`: the CPU reference and every GPU then read the same texels (the four taps are mirror images, so a
  quarter turn covers every direction). The distribution of distance is the same; the angle has 16 steps.
- The grain is added only to a pixel the debanding changed, and the result is clamped to the samples read:
  libplacebo adds grain to every pixel, which would alter text, HUD and flat colour that the stage promises not
  to touch.
- The noise can be frozen (`pushCleanParts` + 16) so that it is the same on every frame. The default follows
  libplacebo (new per frame). Risk of each: per-frame noise is a very faint shimmer of half a code value in
  debanded areas and differs between a frame and its repeat; a frozen pattern can show as a fixed "dirty
  window" when the picture pans. Neither was looked at on a screen.

rtx-upscale (no licence): nothing was copied or ported from it.

Attribution: `THIRD_PARTY_NOTICES.md` (last entry); the libplacebo notice stands above each ported part in
`include/veyra/engine/PushClean.h`, `shaders/PushClean.hlsl` and `shaders/ScaleBlit.hlsl`.

## 3. Why it sits before the upscaler

- Block edges, ringing and bands are defined on the encoder's pixel grid and in the encoder's code values. After a
  scaler they are spread over more pixels at fractional positions and mixed with real detail; thresholds "in code
  values over one pixel" no longer describe them (libplacebo debands the source planes before scaling for the same
  reason).
- An upscaler or a sharpening network treats a block edge as an edge and makes it crisper; a neural layer may
  build texture on mosquito noise. Removing them first gives every later stage a cleaner input once, instead of
  each later stage coping with them.
- It is the cheapest place: 2.07 Mpixel at 1080p against 3.69 Mpixel at 1440p or 8.29 Mpixel at 4K.

In the graph: `EnhanceGraph::runClean` (src/pipeline/EnhanceGraph.cpp) runs right after the input conversion
(YUV to the linear FP16 working picture, with the colour grade if one is set) and before anything reads that
picture. Two compute dispatches: encode the picture into the stage's one texture, then filter it back **in
place**, writing only the changed pixels. No copy is added; with the stage off neither the texture nor the pass
exists.

## 4. Why the clean 1440p path asks the upscaler for 2560x1440, not 3840x2160

The SR target defaults to 4K and nothing ties it to the window (docs/SUPER_RESOLUTION.md section 2). On a
2560x1440 screen a 4K target means: the upscaler, colour, HUD lock and frame generation all run on 8.29 Mpixel
instead of 3.69 Mpixel (2.25x), and the presenter then shrinks 4K to 1440p with one bilinear sample per output
pixel, which discards part of what the upscaler made and can alias thin lines. Asking for 2560x1440 (`srTarget=qhd`
in `VEYRA_TEST_RP_PIPELINE`, target "2K" in the SR editor) removes both the redundant 4K intermediate and the
shrink. This is reasoning from the code and pixel counts; **the owner's reference configuration (DLSS SR Model M
into 4K) is the comparison the hardware run has to make**, because a 4K result shrunk to 1440p can also look
cleaner (supersampling) than a direct 1440p result.

## 5. The candidate upscalers already in the app

| choice | where it runs | notes |
|---|---|---|
| DLSS Super Resolution (model M in the owner's reference) | NVIDIA RTX | temporal, reads optical-flow motion; the owner's own numbers call it much cheaper than one RTX Video SR configuration |
| RTX Video SR, "streaming" family (levels 21, 23) | NVIDIA RTX 30 and newer | spatial; made for compressed video, so it already contains artifact reduction: the clean-up stage in front of it may be redundant |
| NVIDIA Image Scaling | any DirectX 12 card | spatial with sharpening; the vendor-neutral path for AMD and Intel cards |
| AMD FSR upscaling | any DirectX 12 card | temporal; the other vendor-neutral path |

Nothing is verified yet. To prefer one, the owner's PC has to show, at 2560x1440 output from the same scenes:
the stage time and its P95 / P99 (`[slipstream-sr]`, `[slipstream-clean]`), the whole processing time and frame
rate with 1 % low, and the picture: the `[slipstream-quality]` numbers for raw and output and, above all, a look
at the comparison clips (numbers reward blur). In particular: whether the clean-up helps or is redundant in front
of each upscaler (arms B against C, and `v-clean-nis`), and whether DLSS's sharpening brings back what the
clean-up removed.

## 6. Controls

| preference | values | meaning |
|---|---|---|
| `pushClean` | 0..100, default 0 | strength; 0 = off: no pass, no texture, the picture bit-exact as before |
| `pushCleanHud` | bool, default on | stricter protection of thin lines and text strokes |
| `pushCleanParts` | sum of 1 deband, 2 deblock, 4 dering, 8 grain, 16 frozen noise; default 13 (all but the deblock: section 8) | which parts run; **1 is the part ported from libplacebo alone** |

`setPreference("pushClean", n)` applies at once. Going from 0 to a strength above 0 (or back) builds another
graph, because the pass exists only when it is on; any other change of the three values is read by the running
graph on its next frame (as NIS sharpness is). `EnhancementSettings::clean` / `EnhanceGraphDesc::clean` carry the
strength; `[slipstream-config]` and the `clean=` key of `VEYRA_TEST_RP_PIPELINE` belong to other streams and use
the same preference.

For the hardware A/B of the proven part against the whole stage: `pushCleanParts` = 1 (deband only), 13 (the
default), 15 (everything).

**Recommended setting when the stage is turned on: strength 50, parts 13** (section 8). The preference itself
stays 0 (off) until a hardware run has shown the effect on a screen.

## 7. Limits and risks

- Thresholds are in 8-bit code values. A stream with stronger artifacts than the model clips of section 8 (a
  very low bitrate, a dark scene with heavy quantisation) may need a higher strength; a very clean stream gains
  nothing.
- A texture whose detail is fainter than about 2 to 4 code values (depending on strength) looks like compression
  grain to the stage and is smoothed. The bench's "texture" columns show where that costs fidelity.
- The deblock can soften a real, faint, straight step between two flat areas (a soft shadow edge on a wall); it
  is limited to steps of 2 to 16 code values (by strength) and to a 4-pixel ramp.
- The stage's own parts (protection, deblock, dering, grain) are unproven outside this bench.
- HDR: the same thresholds are applied to PQ code values. Above reference white a PQ code value is a larger
  step in light than below; nothing was looked at on an HDR screen.
- The ordered dither at the final write is applied to the whole frame once the stage changed any pixel. After
  the FP16 round trip an exact code value sits up to about 0.03 code off, so with `floor(v * steps + bias)`
  about 1 % of the pixels the stage did NOT touch move by one code value, in a fixed 16x16 pattern (independent
  review S3; the earlier "1 pixel in 256" here was wrong). Fixed on 21 November: a value within 1/16 code of
  a code value is written as that code (`ScaleBlit.hlsl` `OrderedDitherRound` and
  `engine::pushCleanDitherRound`, a change to libplacebo's form; tested in
  `clean_final_write_dither_follows_libplacebo`); on the GPU only through `clean-dither` once a round runs it.
- The CPU reference does not guard against NaN: one NaN input spreads to the up to 9 output pixels whose window
  reads it (independent review S4). The graph's pictures come from the decoder's conversion and hold no NaN;
  the GPU shader behaves the same way. Not guarded.
- With no upscaler in the chain, the "base" side of the split comparison view is the source before the clean-up.
- Cost is unknown. By count: 37 texture reads per pixel plus the encode pass, at 1080p. Section 9 says what
  will measure it.

## 8. The bench (Linux, repeatable)

`tools/restore_bench/run_bench.sh` (g++, python3 with numpy and PIL, ffmpeg with libx264; x265 when present):
`gen_content.py` draws pristine 1920x1080 frames; ffmpeg encodes them like a low-latency stream (no B-frames,
zerolatency, about four frames of buffer, 60 fps, 8-bit 4:2:0) and decodes them; `restore_bench.cpp` runs the
CPU reference on the last frames of each stream and measures every setting against the pristine picture;
`summarize.py` writes the tables. "off" is the picture as decoded, which is Package 7's behaviour.

**Every number below: MODEL content drawn by a script (not game footage), a SOFTWARE encoder (not the PS5's
hardware encoder), the CPU reference (not the shader, not a GPU), 8-bit SDR only.** There is no VMAF in this
ffmpeg; PSNR and SSIM are computed by the bench itself on luma.

Streams of the run of 21 November 2026 (00:40 UTC; 30 frames each, the last 4 measured; average rates over
the 30 frames: x264 3.3 / 6.5 / 12.4 Mbit/s, x265 3.8 / 8.0 / 15.4 Mbit/s for the 4 / 8 / 15 ladder). This
ffmpeg has libx265 (the brief expected it not to); there is no VMAF. Reproduce with
`tools/restore_bench/run_bench.sh --out <dir>` (about 10 minutes on one core); every per-stream table is in
`<dir>/results.md`.

Model content, software encoder, CPU reference, 8-bit SDR. PSNR in dB of luma against the pristine
picture; every "change" is against "off" (the picture as decoded), so a loss is a minus sign.
Blockiness: 1 = no 8-pixel grid. Contours: band edges (a code step with flat runs on both sides) per
1000 pixels of the sky and dark regions after the final 8-bit write, plainly rounded / with the
ordered dither. HUD edge shift: mean distance in pixels between the HUD's strong edges and where the
pristine picture has them. HUD flicker: mean change of luma between two frames inside the HUD, which
does not move, in 8-bit code values.

### Setting `all-50` against off, per stream

| stream | PSNR off | change: all | gradient | dark | texture | HUD | lines | SSIM | blockiness off -> on | contours rounded off -> on | on, dithered | HUD edge shift off -> on | HUD flicker off -> on |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| x264-4M | 37.02 | +0.05 | +0.21 | +0.60 | +0.02 | +0.03 | +0.00 | +0.0013 | 1.340 -> 1.292 | 52.2 -> 10.7 | 5.6 | 0.142 -> 0.142 | 0.494 -> 0.565 |
| x264-8M | 41.99 | +0.10 | +0.53 | +1.20 | +0.04 | +0.03 | +0.00 | +0.0008 | 1.290 -> 1.260 | 64.0 -> 18.7 | 9.1 | 0.067 -> 0.067 | 0.744 -> 0.760 |
| x264-15M | 45.95 | +0.28 | +0.83 | +1.54 | +0.21 | +0.02 | +0.00 | +0.0011 | 1.269 -> 1.248 | 76.3 -> 25.3 | 10.7 | 0.038 -> 0.038 | 0.952 -> 0.895 |
| x265-4M | 37.79 | +0.08 | +0.27 | +0.96 | +0.04 | +0.04 | +0.00 | +0.0019 | 1.276 -> 1.273 | 37.7 -> 5.4 | 2.4 | 0.101 -> 0.101 | 0.782 -> 0.783 |
| x265-8M | 40.84 | +0.14 | +0.52 | +1.27 | +0.05 | +0.04 | +0.00 | +0.0019 | 1.253 -> 1.249 | 40.2 -> 6.7 | 2.8 | 0.052 -> 0.052 | 0.968 -> 0.927 |
| x265-15M | 44.16 | +0.23 | +1.12 | +1.45 | +0.06 | -0.16 | +0.00 | +0.0015 | 1.243 -> 1.240 | 47.3 -> 8.9 | 3.6 | 0.034 -> 0.033 | 1.047 -> 0.978 |

### Every setting: mean over the 6 streams, and the worst stream

| setting | strength | parts | PSNR all: mean (worst) | texture: mean (worst) | HUD: mean (worst) | dark: mean | contours rounded, mean change % | with dither % | blockiness, mean change | HUD flicker, mean change | pixels changed % |
|---|---|---|---|---|---|---|---|---|---|---|---|
| deband-25 | 25 | 1 | +0.00 (+0.00) | -0.00 (-0.00) | +0.00 (+0.00) | -0.00 | +0 | -3 | -0.000 | +0.000 | 1 |
| deband-50 | 50 | 1 | +0.00 (-0.00) | -0.02 (-0.04) | -0.00 (-0.01) | +0.65 | -76 | -84 | -0.018 | +0.060 | 40 |
| deband-75 | 75 | 1 | -0.04 (-0.11) | -0.11 (-0.20) | -0.02 (-0.08) | +1.01 | -87 | -91 | -0.030 | +0.110 | 53 |
| deband-100 | 100 | 1 | -0.36 (-0.78) | -0.57 (-1.07) | -0.21 (-0.47) | +0.87 | -88 | -92 | -0.044 | +0.233 | 61 |
| deblock-50 | 50 | 2 | +0.04 (+0.02) | +0.04 (+0.02) | -0.11 (-0.37) | +0.67 | -32 | -51 | -0.011 | -0.012 | 24 |
| dering-50 | 50 | 4 | +0.04 (+0.02) | +0.03 (+0.02) | +0.04 (+0.01) | +0.04 | -2 | -6 | +0.000 | -0.007 | 4 |
| grain-50 | 50 | 8 | +0.19 (+0.05) | +0.08 (+0.02) | +0.20 (+0.02) | +1.03 | -3 | -60 | -0.006 | -0.100 | 61 |
| all-25 | 25 | 15 | +0.16 (+0.05) | +0.08 (+0.03) | +0.16 (+0.02) | +0.88 | -40 | -65 | -0.010 | -0.046 | 62 |
| all-50 | 50 | 15 | +0.15 (+0.05) | +0.07 (+0.02) | -0.00 (-0.16) | +1.17 | -78 | -90 | -0.018 | -0.013 | 73 |
| all-75 | 75 | 15 | +0.00 (-0.09) | -0.11 (-0.18) | -0.16 (-0.48) | +1.22 | -87 | -94 | -0.029 | +0.052 | 76 |
| all-100 | 100 | 15 | -0.43 (-0.95) | -0.68 (-1.27) | -0.41 (-1.01) | +1.02 | -88 | -94 | -0.043 | +0.176 | 78 |
| all-50-static | 50 | 31 | +0.15 (+0.05) | +0.07 (+0.02) | -0.00 (-0.16) | +1.17 | -78 | -90 | -0.018 | -0.056 | 73 |
| no-deblock-25 | 25 | 13 | +0.17 (+0.05) | +0.07 (+0.02) | +0.19 (+0.02) | +0.98 | -24 | -63 | -0.009 | -0.065 | 63 |
| no-deblock-50 | 50 | 13 | +0.19 (+0.06) | +0.07 (+0.02) | +0.21 (+0.03) | +1.24 | -76 | -90 | -0.016 | -0.046 | 74 |
| no-deblock-75 | 75 | 13 | +0.05 (-0.01) | -0.11 (-0.19) | +0.10 (+0.02) | +1.24 | -87 | -94 | -0.027 | +0.031 | 76 |
| deband-grain-50 | 50 | 9 | +0.15 (+0.04) | +0.05 (+0.01) | +0.17 (+0.02) | +1.19 | -76 | -88 | -0.017 | -0.040 | 71 |

Contours of the pristine picture itself: 98.5 when only rounded to 8 bits (an ideal gradient is fully banded then), 18.4 with the ordered dither.

### What the numbers say, and the default chosen from them

- **The ported part (deband, 1) does what it is for and costs almost nothing at 50:** band edges after the final
  write fall by 76 % (84 % with the ordered dither), dark-region PSNR rises by 0.65 dB, the whole picture is
  unchanged (+0.00 dB) and texture loses 0.02 dB (worst 0.04). At 75 it starts to cost texture (-0.11, worst
  -0.20) for little more debanding; at 100 it plainly hurts (-0.36 dB whole picture, -1.07 texture on the worst
  stream). The threshold is libplacebo's at 50, and the data confirm it as the right point.
- **The deblock (2) is the one part that hurts what the stage promises to protect:** alone it costs the HUD /
  text regions 0.11 dB on average and 0.37 dB on the worst stream, for a blockiness change of -0.011 (the
  deband gives -0.018). It is therefore **off by default** (`pushCleanParts` = 13). The likely cause is that
  the flat-sides test also accepts the soft anti-aliased edge of small HUD glyphs; not investigated further.
- **The grain part (8) brings most of the PSNR** (+0.19 dB mean, best on the cleaner streams) and lowers the
  HUD's frame-to-frame flicker; it also changes about 60 % of the pixels by a fraction of a code value, which
  PSNR rewards and a viewer may or may not see as smoothing. The dering part (4) is small either way.
- **Chosen default when on: strength 50, parts 13**, justified by **fewer band edges** (-76 %, -90 % with the
  ordered dither) and blockiness (-0.016) at no loss of texture, HUD or lines, and NOT by PSNR. It errs on the
  side of detail: it is the highest strength at which no stream lost texture, HUD or line PSNR **on the float
  output** (75 already loses texture on every stream, worst -0.19 dB).
- **Read the PSNR columns with care: they are measured on the float output, before the final 8-bit write**
  (what an upscaler would receive). The independent review re-measured after the shipped ordered dither (what
  the screen shows with no upscaler): the gains nearly vanish (x264-8M whole picture +0.13 -> +0.05 dB, dark
  +1.24 -> -0.01 dB; the debanding alone at 50: -0.31 dB in the dark region), because a dither always costs
  PSNR while it removes contours. The bench does not yet print that "written" column; it should, beside the
  float one. The tables above were made before the halo guard of section 1 (it changes only pixels whose
  debanding taps reach a line).
- **Per-frame noise (default) against frozen (+16):** the same PSNR; the frozen noise lowers the measured HUD
  flicker a little more (-0.056 against -0.013 at all-50) because the debanding noise near the HUD no longer
  changes; on a pan it would stay in place on the screen. Kept per-frame as libplacebo does; the hardware look
  decides.
- **Where it hurts:** strengths above 50 on fine texture (foliage-like noise, fabric) on every stream, and the
  deblock near HUD text. Thin lines are never touched (+0.00 everywhere), and the HUD edge positions do not move
  (shift unchanged to 0.001 px).
- Limits of this bench: model pictures, 4 frames per stream, software encoders tuned like a low-latency stream
  but not the PS5's hardware encoder, SDR only, and PSNR / SSIM, which reward smoothing; the HUD flicker and
  contour counts are our own simple indices. None of it says how the stage looks on a screen.

## 9. What will show the effect on hardware

- Kit session `visual` (docs/HARDWARE_TEST_PROCEDURE.md): arms B `v-dlss` against C `v-clean-dlss`, E, and the
  vendor-neutral `v-clean-nis`.
- `[slipstream-clean] frames= strength= size= gpuMsP50= gpuMsP95= hudProtect= hdr= parts=` (only with
  `VEYRA_EXP_RP_STATS=1`, one line per 5 s): the stage's own GPU time from the graph's timestamps
  (`GpuStage::Clean`). The same time is part of the app's "processing time" readout.
- `[slipstream-quality]` (pipeline stream): `blockiness`, `banding`, `ringing`, `detail`, `hudDelta` for
  `stage=raw` (the console's picture, taken before the stage) and `stage=output`.
- Comparison clips (split or side by side): the console side is the raw picture, so the clip shows the stage's
  effect directly.

What the run must show before any profile turns the stage on by default: blockiness and banding of the output
below the raw picture's at the same `detail` and `hudDelta` within noise, a `gpuMsP95` that leaves the frame
budget of frame generation alone, and a human look at dark scenes, vegetation, faces, HUD text and a fast pan.

## 10. Verification status

- CPU reference and its tests (`tests/remoteplay/CleanTests.inc`): run on Linux (g++), and compiled and run by
  MSVC in the fast Windows workflow once merged.
- Shader, pass, graph hooks, preferences, final-write dither: **blind Windows code** that compiled under MSVC in
  the full Windows rounds of 20 November (build-windows #35 and #37). `tests/integration/PushGpuTests.cpp`
  (`clean`, `clean-hdr`, `clean-dither`) compares the shaders with the CPU reference on the software renderer
  (WARP) in CI. In both rounds the first dispatch that ran the filter did **not finish within 60 s on WARP**
  (the test then released its buffers while the GPU still used them, which the debug layer reported as
  exception 0x87D). The cause is not known: the shader's loops are now all unrolled, the test names the part it
  runs and the time it took, and a hung submission ends the test run with a plain message instead of a crash.
  Until a round shows `clean` passing, the stage must be treated as possibly hanging a GPU; it is off by
  default. No test covers the stage inside the running graph.
- AMD and Intel cards: reasoned only (plain compute shaders, FP16 textures, no vendor feature); nothing was run
  on them.
- HDR (PQ) on the CPU reference: highlights above reference white and a flat bright window are left bit-exact
  (`clean_hdr_highlights_preserved`); a saturated textured area and saturated edges are not written at all,
  and in a smooth saturated area each channel's mean moves by less than a tenth of a 10-bit code value and stays
  inside the area's range (`clean_hdr_colour_kept_outside_smooth_regions`). Out-of-gamut scRGB values of a pixel
  the stage changes are clipped to BT.2020 by the PQ encode (`HdrEncodePq`); untouched pixels keep them.
- Not verified anywhere: the look on a screen, the cost on a GPU, a real PS5 stream, HDR on an HDR screen. No
  "better" claim stands before the owner's A/B on his hardware.
