# DRAFT: Package 8 sections for docs/SLIPSTREAM_STATUS.md

> Draft for the orchestrator, who merges it into `docs/SLIPSTREAM_STATUS.md` and then deletes this file.
> Written from the team branches **as committed at 01:25 UTC on 21 November** (`exp/s5-*`) and from the
> teams' notes (`S5_INPUT_API.md`, `S5_PIPELINE_API.md`, `S5_PRESENT_API.md`). Teams still commit until
> 02:45 UTC: re-check every name below against the final merge.
> `TODO(orchestrator)`: replace "as committed at 01:25 UTC" with the merge commit id.

## Package 8 (21 November 2026): where each part stands

**How to read this.** Two kinds of statement are kept apart on purpose.

- **EXPECTED (reasoned, not measured)**: what we think will happen, from reading the code. It can be wrong.
- **MEASURED**: numbers from your own PC only. Nothing from Package 8 has been measured yet. The only
  measurements that exist are yours: the kit runs of 19 November (`BENCH1`, Wi-Fi) and 20 November
  (`BENCH2`, Wi-Fi), and the reference settings you reported (DLSS SR Model M to 4K, NVIDIA VFG 2x, H.265
  HDR stream: 120 FPS submitted, 93.2 FPS 1 % low, 61 % GPU, 9.3 ms processing time, 23 ms application
  latency, read by you during play).

**Everything new is OFF until you choose it**, with the exceptions listed under "On by default: fixes,
not features" below. Nothing here has run on a graphics card, a PS5 or Windows itself, apart from the
automatic builds and tests on GitHub's Windows machines named in "Builds".

| | Package 8 |
|---|---|
| Download | `TODO(orchestrator)`: link to the package artifact and its SHA-256 |
| Experimental build | `TODO(orchestrator)`: build-windows run number, version string, commit id |
| Windows compile (MSVC /W4 /WX) | `TODO(orchestrator)`: result of the final full round |
| Automatic tests | `TODO(orchestrator)`: counts per suite (core tests Linux and Windows, GPU tests on the software renderer, QML tests, kit dry run) |
| Independent reviews | `TODO(orchestrator)`: verdict per stream and any open finding |
| Rollback | Package 7's experimental build is installed beside it, unchanged (`PushFPS-slipstream-p7`) |

**Three downloads, one after another** (your message of 00:27 UTC). Each holds only whole, reviewed parts:

| Download | Sections below | Teams' work in it |
|---|---|---|
| 8A | 2, 4, 5, and the clean-up and fallbacks of 7 | restoration, pipeline, picture profiles, test kit |
| 8B | 8A + 1, 6 | + mouse and keyboard (core and page), presentation and audio, upstream fixes, chiaki-ng patches |
| 8C | 8B + 3 | + the neural layer, its runtime check and the installer question |

`TODO(orchestrator)`: per download: build run, version string, test counts, review verdicts, link.

### 1. Mouse and keyboard (input)

- **Download.** 8B.
- **What it is.** A *Mouse & Keyboard* page with an on/off switch, key bindings, a first-start wizard and
  calibration. Three new input profiles: **Ultra Low Latency**, **Adaptive Precision** and **Custom**, each
  with its own walking, aiming, sprinting and driving settings. They turn each mouse reading into a
  stick position at once: no smoothing, no waiting for the next reading (only Custom can add a
  smoothing window, and it starts at 0 = off). A game calibration you made is
  used by every new profile. A tilt cap (`maxTilt`, from xcontrol's idea) stops the stick at a set
  angle. A self-check moves Adaptive Precision or Custom back to Ultra Low Latency if they measure
  slower, and says why (`fallback=translate-slower` or `queue-older`). `Ctrl+Alt+Shift+Z` always
  gives the mouse back.
- **How to turn it on.** The *Mouse & Keyboard* page (preferences `kbmEnabled`, `kbmInputProfile`,
  `kbmInputCustom`, `kbmContextOverride`). The kit uses `VEYRA_EXP_RP_KBM=1` and
  `VEYRA_EXP_RP_KBM_INPUT_PROFILE=ultra-low-latency|adaptive-precision|custom|legacy`.
- **Default.** Off. When you switch it on, the profile is `legacy` (exactly today's behaviour) until you
  pick another.
- **EXPECTED (reasoned, not measured).** The new profiles drop the old profile's 8 ms averaging window,
  so a mouse move should reach the pad state without that wait. Whether you can feel it is unknown.
- **MEASURED.** Nothing yet.
- **Limits.** The statistics are software times inside the PC, never input-to-photon. The GTA VI key
  layout comes from published button tables and is untuned and unverified in the game. A faster
  button-press experiment, `VEYRA_EXP_RP_KBM_EVENT_SEND=1` (a press is sent when it arrives, not at the
  next poll), is off by default and has no kit arm yet.
- **Kit session.** `-Session input` (arms `i-legacy`, `i-ultra`, `i-adaptive`; lines
  `[slipstream-input]` and `[slipstream-input-poll]`).

### 2. Restoration (compression clean-up and 1440p)

- **Download.** 8A.
- **What it is.** A clean-up stage for the compressed 1080p stream. It runs once per real frame at the
  stream's size, **before** any upscaler, neural layer or frame generation. Its parts: debanding
  (ported from libplacebo), deblocking, de-ringing and grain removal (our own), with stricter
  protection for text and thin HUD lines. After it, the final write is rounded with a fixed pattern that
  keeps black exact, so smooth gradients do not band again. Five **picture profiles** at the top of the
  quick setup: `original`, `clean-1440`, `enhanced-1440`, `max-fidelity`, `ultra-low-latency`.
- **How to turn it on.** Pick the `clean-1440` or `enhanced-1440` profile (preference `pushProfile`), or
  set the preference `pushClean` (0 to 100; 50 is the recommended strength). Fine control:
  `pushCleanHud` (text protection, on), `pushCleanParts` (which parts run: by default everything but
  deblocking, because deblocking softened HUD text in the team's Linux bench on drawn test pictures;
  `docs/RESTORATION.md`).
- **Default.** Off: `pushClean` = 0 (no pass at all), `pushProfile` = `none` (nothing read or written).
- **EXPECTED (reasoned, not measured).** Cleaning at 1080p before the upscale costs less than cleaning a
  1440p or 4K picture, and the upscaler no longer enlarges blocks and bands. How much cleaner it looks,
  and what it costs on your card, the kit must show.
- **MEASURED.** Nothing yet.
- **Limits.** The strength 50 was chosen on drawn test pictures with a software encoder, not on game
  footage or the PS5's encoder. Above 50 it smooths fine texture. HDR uses the same thresholds and was
  never looked at on an HDR screen. Its GPU cost is unknown. The shader ran only on the software
  renderer in CI, never on a GPU. No Lanczos scaler yet. The `max-fidelity` profile is your reference
  (Model M to 4K) and has no clean-up. In 8A and 8B, `enhanced-1440` drops its neural stage and says so.
  The profiles promise no quality: compare with `original` yourself.
- **Kit session.** `-Session visual` (arms `v-clean-dlss`, `v-clean-nis`, `v-clean-neural-dlss` against
  `v-original`, `v-dlss`, `v-dlss-k`; lines `[slipstream-clean]`, `[slipstream-quality]`). Then your own
  eyes: in 8A, START-HERE step 3 makes two comparison clips (profile `original`, then `clean-1440`).

### 3. Neural layer (optional, experimental)

- **Download.** 8C.
- **What it is.** The layers shown as "DLSS 5 NR" / AI picture enhancement. They apply NVIDIA's neural
  rendering model to the decoded video. This is video processing without the game's motion vectors or
  depth: **it is not NVIDIA's game-integrated DLSS 5.** The app now uses NVIDIA's runtime file only after
  checking that it is an NVIDIA-signed original. If your runtime choice was never changed and the default
  file is missing, it uses the signed original instead. (The likely reason you saw "This package lacks
  the runtime components": the default slot's file is left out by the installer. Not checked on your PC.)
  Settings → Components and licenses names the exact reason when it cannot run, and the log line
  `[nr-runtime-status]` says the same. A **conservative** look uses half of RTX Remix's published
  defaults (tone 0.15, structure 0.35).
- **How to turn it on.** Answer **yes** to the installer's one question (`Install-Slipstream.cmd
  -NeuralRuntime Yes`), then pick `enhanced-1440` or switch on AI picture enhancement.
- **Default.** Off. The installer keeps NVIDIA's file only on an explicit yes.
- **EXPECTED (reasoned, not measured).** No speed claim. A lower internal size (720 or 900 lines) should
  cost less than 1080 or 1440.
- **MEASURED.** Nothing yet. Your reference settings did not use it.
- **Limits.** RTX 50 cards only (NVIDIA's statement). On HDR it is unverified, and the interface says so.
  The licence questions are open (see `docs/OPEN_SOURCE_REVIEW_2026-11-20.md` and
  `docs/NEURAL_RENDERING_RUNTIME.md`). Automatic size on cards other than the one upstream verified only
  with `VEYRA_EXP_NR_AUTO_CAPABILITY=1`, and never for a PS5 stream.
- **Kit session.** `-Session neural` (four internal sizes, with and without the upscaler) and the neural
  arms of `-Session visual`. Without the file every neural arm says "not installed: skipped".

### 4. Frame generation

- **Download.** 8A (the profiles' rules).
- **What it is.** No new generator. What changed: a picture profile chooses the largest multiplier at
  or under 120 FPS and your screen's refresh, and keeps your method; 180 FPS only as your explicit,
  experimental choice; the duplicate-frame detector stays off. The planner (section 5) shrinks image
  stages first, so frame generation keeps its GPU time.
- **How to turn it on.** As before in the interface, or through a profile (target 120 FPS).
- **Default.** Unchanged.
- **EXPECTED (reasoned, not measured).** None claimed.
- **MEASURED.** Only your reference: NVIDIA VFG 2x, 120 FPS submitted, 93.2 FPS 1 % low (your reading).
- **Limits.** Submitted FPS counts generated frames and is not proof of new motion. Ghosting shows only
  to your eyes.
- **Kit session.** `-Session framegen` (every method at 2x, and 3x marked experimental; real and
  generated frames counted apart).

### 5. GPU pipeline

- **Download.** 8A.
- **What it is.** A tested planner (`include/veyra/engine/PushPipelinePlan.h`) that decides the stage order
  and removes repeated work: no upscale above your screen unless you ask (`above-display`), no second
  clean-up beside a cleaning upscaler (`double-clean`), no second sharpening (`double-sharpen`), and with
  measured costs the most expensive image stage is reduced first (`gpu-budget`). If an NVIDIA stage is
  missing it falls back to FSR, then NIS. New measurements: `[slipstream-nr]` (time per stage, copies,
  video memory) and a picture probe (`[slipstream-quality]`). The answer to "why was DLSS SR Model M so much
  cheaper than RTX Video SR" is in `docs/NEURAL_RENDERING.md`: the two numbers do not measure the same
  thing, because RTX Video SR's includes two to four extra full-picture passes.
- **How to turn it on.** The planner acts only through a picture profile. The statistics need
  `VEYRA_EXP_RP_STATS=1`; the probe is for the kit only. `VEYRA_EXP_PIPE_IDLE_NR_FLOW=1` skips one pass
  nobody reads (the motion field resized for a neural layer that is not there).
- **Default.** The graph itself is unchanged; the one pass above is skipped only with its switch.
- **EXPECTED (reasoned, not measured).** The largest saving is not processing at 4K for a 1440p screen
  (2.25 times fewer pixels after the upscaler); the 1440 profiles do that.
- **MEASURED.** You reported RTX Video SR "consumes substantial processing time" on the RTX 5070, and your
  reference reads 9.3 ms processing time. Which RTX Video SR level you ran is not recorded.
- **Limits.** The other copies and repeated passes found in the graph are listed, not removed (a wrong
  picture is worse than a saved pass). The skip switch has no kit arm yet.
- **Kit session.** `-Session visual` (graph and stage times per arm), `-Session neural`.

### 6. Network, audio and the Remote Play library

- **Download.** 8B.
- **What it is.** Five changes from newer chiaki-ng as small patches (`0005` to `0009`), plus our own
  counters for the resend buffer (`0010`, log only). **Hardware decoding kept** through an isolated stream
  error instead of switching to software decoding for the rest of the connection. Audio **own
  clock**: audio follows the local picture delay instead of the video timestamps, which fall behind.
  **One-rectangle window region** while a menu or badge is over the picture, to test the slow first
  seconds. A **scan-out audit** (`[slipstream-scanout]`) counting which frames the screen statistics can
  date. From Veyra: the trigger jitter filter. SmartLink is unchanged.
- **How to turn it on.** `VEYRA_EXP_RP_AUDIO_OWN_CLOCK=1`, `VEYRA_EXP_PRESENT_RECT_REGION=1`,
  `VEYRA_EXP_RP_DECODE_KEEP_HW=1`, `VEYRA_EXP_RP_CHIAKI_SEND_BUFFER=1` (patch 0009),
  `VEYRA_EXP_RP_TRIGGER_FILTER=1`. No interface switch.
- **Default.** All five off.
- **EXPECTED (reasoned, not measured).** With the own clock, the audio delay should stay flat instead of
  growing, with fewer hard resets. With the one-rectangle region, the first seconds might be as fast as
  the rest, if the region's shape is the cause.
- **MEASURED (yours, Wi-Fi, Package 7).** Audio target 249 to 537 ms and 3 to 10 hard resets a minute on
  the default path (`BENCH2`). Present to screen: P50 7.3 to 8.6 ms for the first 1 to 3 seconds, then 1.0
  to 1.4 ms, in all 12 runs (`BENCH2`); 9.1 to 9.9 ms for whole runs on 19 November (`BENCH1`).
- **Limits.** Lip sync with the own clock rests on arrival times. The region hides a strip of picture
  while a menu is up. Hardware decoding kept, the larger resend buffer and the trigger filter have no kit
  arm yet. Nothing was tested on a console.
- **Kit session.** `-Session audiofix`, `-Session pacing`, `-Session network` (SmartLink), `-Session builds`.

### 7. Hardware-aware behaviour (NVIDIA, AMD, Intel)

- **Download.** 8A (clean-up, upscaler fallback); 8C (neural parts).
- **What it is.** New decisions use what the card reports, not its name. The clean-up is a plain compute
  shader for every GPU maker. Without DLSS a profile's upscaler becomes FSR, then NIS; without the signed
  NVIDIA runtime the neural stage is dropped and the rest of the profile runs. The automatic neural size
  no longer names a card model when it refuses.
- **How to turn it on.** Nothing to do.
- **Default.** Unchanged on NVIDIA. Automatic neural size keeps upstream's one verified configuration as
  its default gate.
- **EXPECTED (reasoned, not measured).** AMD and Intel get the clean-up, NIS or FSR, PushFlow and FSR
  frame generation. All of it is reasoned only.
- **MEASURED.** Nothing: no AMD or Intel card was used.
- **Kit session.** `v-clean-nis` in `-Session visual`; `f-pushflow-*`, `f-fsr-*` in `-Session framegen`.

### On by default: fixes, not features

Where a test switch restores the old behaviour, it is named.

- Neural runtime: the untouched default resolves to the verified signed original (reason: the feature
  was unusable although its signed file was installed). A runtime you picked yourself is never changed.
- Two stability fixes for the neural runtime from Veyra: a runtime file without an import name table
  no longer crashes the loader (no switch), and the runtime is shut down before it is unloaded
  (`VEYRA_TEST_RP_LEGACY_NR_TEARDOWN=1` restores the old order).
- Controller queue: a state that waited too long is dropped alone, never answered with a release
  (Veyra's rule; `VEYRA_TEST_RP_LEGACY_INPUT_EXPIRY=1` or `VEYRA_TEST_RP_LEGACY_TIMING=1` restores the
  old one).
- chiaki-ng patches `0005` to `0008` (memory and bounds faults, the login PIN kept out of logs).

`TODO(orchestrator)`: confirm this list against the final merge.

### Remaining limitations

- Nothing in Package 8 has run on a graphics card, a PS5, or your PC. Every value above under
  "EXPECTED" is a guess from the code.
- The kit has run only as a dry run on GitHub's Windows machine (made-up log lines).
- The neural layer: licence questions open; RTX 50 only; HDR unverified; video only, not game-integrated.
- The GTA VI key layout is untuned; Rockstar's position on mouse and keyboard is unknown.
- The clean-up strength (50) was chosen on drawn test pictures, not game footage; no Lanczos scaler yet.
- One repeated GPU pass can be skipped, behind a switch; the rest of the repeated work found is
  documented only.
- The duplicate-frame detector stays off by your order.
- Veyra 2.2.0 and PushFPS 3.2.10 were not merged: HDR and colour fixes wait for the next merge.
- Wi-Fi results stay "diagnostic only"; a wired-LAN run is still needed for any latency conclusion.
- Input-to-photon needs a camera (procedure section 2).
