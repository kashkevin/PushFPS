# Why the PS5 stream looks smeared, and what can be fixed without adding input delay

DRAFT verdict of stream "clarity", 21 November 2026, written on branch `exp/s5-clarity` from Package 8C1
(`exp/rel-8c`, 10430c57). Internal codename Slipstream. Answer to the owner's tenth message.

**How to read every claim.** Each one carries a tag:

- **[measured-on-owner-PC]**: a number from the owner's own logs (kit runs of 19 and 20 November, evidence
  BENCH1 / BENCH2: Wi-Fi, SDR, no upscaler, no frame generation). Software counters only, never photons.
- **[code]**: read in our source at 10430c57 (file and function named).
- **[published]**: a public source, linked in "Sources" at the end. "Press" means a journalist's report,
  not the vendor's own statement.
- **[estimate]**: our arithmetic or reasoning. It can be wrong.
- **[unknown]**: not known; the measurement that settles it is named.

Nothing in this document was run on Windows, on a GPU or on a PS5 by us. The other team working now
(stream "smooth", `exp/s5-smooth`, docs/drafts/SMOOTHNESS_ANALYSIS.md) covers frame-generation pacing and
the 4K working size; this document refers to it and does not repeat it.

## The six answers

**1. Why does the stream look smeared?** Mostly because of what arrives, then because of what we add in
motion.

- The picture that arrives is a 1920x1080, 4:2:0, H.265 stream made by the PS5's real-time encoder
  [code][measured-on-owner-PC]. In the owner's runs the console asked for at most 15 Mbit/s actually
  sent a median of 4.2-6.7 Mbit/s and never more than 9.6 Mbit/s in any second (2,378 one-second windows with video; PC on Wi-Fi, SDR stream, mostly a still scene)
  [measured-on-owner-PC]. That is about 0.03-0.05 bits per pixel per frame [estimate]: a very small
  budget, and a real-time encoder spends it worst exactly where the picture moves.
- The game itself is soft before Sony encodes it: Digital Foundry (via press) reports GTA VI on the base
  PS5 renders at 1440p with Rockstar's own TAA and a "softer spatial upscale", at a 30 FPS target that drops
  into the high 20s [published]. So each new picture is shown twice in the 60 FPS stream, and it was already
  temporally filtered by the game.
- On top of that, three of our stages work across time or interpolate, and can add smear in motion:
  DLSS Super Resolution on video runs with zero camera jitter and motion taken from optical flow on
  compressed video (or zero motion, when the SR motion source is set to zero) [code], which NVIDIA's own guide names as the main source of DLSS artifacts when the
  vectors are wrong [published]; frame generation makes 1 of every 2 shown frames at 2x and 5 of every 6 at
  6x by interpolation [code]; the neural layer keeps its own history [code].
- With an HDR stream, RTX Video Super Resolution and the neural layer **do nothing in bright areas**: their
  change is faded out between about 150 and 250 nits and is zero above (shaders `HdrVideoSrRestore.hlsl`,
  `ParityDecode.hlsl`, function `HdrRestore` in `HdrColor.hlsli`) [code]. There the picture is the 1080p
  stream scaled with a bilinear filter. In a sunny GTA scene that may be much of the screen [unknown: the
  share of pixels above 200 nits is not measured]. This alone explains "even with RTX Video SR and neural
  rendering it looks blurry" for an HDR stream.
- Not causes worth chasing: hardware decoding (exact by the standard), the chroma upsampler (bilinear, correctly
  sited: `ReconstructChroma` in `YuvToLinearRgb.hlsl`, log `[remoteplay-sampling] fine=true`), the YUV-to-RGB
  conversion (FP16) [code]. The 4K-then-1440p path costs GPU time and can alias thin lines (one bilinear
  sample per output pixel in `PresentBlit.hlsl`) but is not what makes a picture soft [code][estimate].

**2. What can actually be fixed?** Four things, none of which can add detail Sony's encoder discarded:

- Ask the console for more bits. Our connect dialog already offers 5 to 100 Mbit/s (`kPs5Bitrates`,
  `src/ui/bridge/RemotePlay.cpp`) [code]; the owner's runs used 15 [measured-on-owner-PC]. Whether the PS5
  honours a higher request, and whether it helps when the console already sent less than 15, is
  **[unknown]** and is the first measurement of the clarity arms (section 9). Sony itself raised the PS Portal's
  1080p bitrate in March 2026 ("1080p High Quality mode", "a higher bitrate compared to the default 1080p
  Standard mode") [published]; press reports 15 to 25 Mbit/s [published, press]. So the console's encoder
  can do more than 15 for at least one client.
- Do not run the stages that do nothing or smear: no RTX Video SR and no neural layer on an HDR stream; test
  DLSS SR against a single-pass spatial scaler (NIS, or our presenter's own filter) for smear in motion.
- Scale once, to the screen's size: 1080p to 1440p directly, not 1080p to 4K and back down (stream smooth's
  `VEYRA_EXP_RP_SMOOTH_SR_DISPLAY`; restoration doc section 4). Cheaper, and nothing is thrown away.
- Game settings: GTA VI has a "Display" category with "Visual Effects" [published]; whether it offers motion
  blur, film grain or depth of field switches on PS5 is [unknown]. Each one that exists and is turned off is
  free sharpness in motion at zero latency.

**3. What is fundamentally limited by Sony's stream?** Resolution (1080p is the protocol's highest preset),
chroma (4:2:0), the codec (H.264 or H.265, no 4:4:4 profile in the protocol) and the console's rate
control [code][measured-on-owner-PC]. Everything the encoder discarded is gone; an upscaler, a clean-up
filter or a neural network can only guess it back, and guesses that change from frame to frame shimmer.

**4. Can clarity be improved significantly without increasing input delay?** **Not significantly through Remote
Play.** Modestly, yes: a higher bitrate (if honoured), one well-filtered 1080p-to-1440p scale and no smearing
stages should give a cleaner, steadier picture at the same latency [estimate]. "Crystal clear" at 1440p is not
reachable from a 1080p 4:2:0 stream at 5-10 Mbit/s (the rate measured in mostly still scenes over Wi-Fi; a moving
scene on Ethernet, or a higher request, may deliver more: the clarity arms measure it): that is a limit of the input,
not of our pipeline [estimate]. A higher bitrate is also not literally free: a bigger frame takes longer on the wire, about 0.25 ms
per frame at 15 Mbit/s and 1.7 ms at 100 Mbit/s on a gigabit link [estimate: arithmetic], more on Wi-Fi.
Frame generation by interpolation can never be zero added delay: it shows each real frame at least one real
frame interval later (16.7 ms at 60 pictures/s, 33 ms for a 30 FPS game with repeated pictures removed)
[code: `pushFgPairDelayMs`].

**5. Is a capture card realistically worth considering?** It is the **only** route to a picture without Sony's
compression: the PS5 can output 1440p over HDMI (since September 2022; no VRR at 1440p) [published], so a
capture card would deliver an uncompressed 1440p picture of a game that renders at 1440p. On latency, Digital
Foundry measured Remote Play (PS Portal, and the PC app "similar") at 4 to 5 frames, 66-83 ms, behind the PS5
on a TV [published, via a forum summary]; a capture card replaces the encoder, the network and the decoder
with a frame or so of capture buffering [estimate]. So it is plausible that capture is **not slower** than
Remote Play. But no credible, independent, recent measurement of capture-to-photon through a Windows app was
found (section 8). **On the evidence available, buying one is not recommended**; the camera test in section 9
decides it, ideally with a card bought where it can be returned.

**6. Best achievable quality and latency on this hardware?**

- Lowest latency with the best picture: **the PS5 straight into the monitor** (1440p output). Zero added
  delay by definition; it gives up everything on the PC side, including frame generation [published][estimate].
- Through Remote Play, latency first (expected, to be confirmed by the clarity arms): wired, H.265 (HDR
  or SDR, see below), the highest bitrate the console actually honours, frame generation off, one spatial
  scale 1080p to 1440p (our presenter's filter or NIS), clean-up only if the probe shows less blocking.
  Picture: a faithful, steady 1080p picture, not a sharp 1440p one.
- If the owner accepts the frame-generation delay for smoothness (his priority 3): VFG at a 1440p working size
  as stream smooth recommends; it adds the pair delay above and nothing else [code].
- HDR or SDR stream: with HDR the AI stages skip bright areas (answer 1). With an SDR stream RTX Video SR works
  everywhere but HDR is lost. Which looks better on his OLED is a by-eye A/B; the clarity arms include both.

## 1. The evidence that exists today

All [measured-on-owner-PC]: kit runs of 19 November 20:00-20:40 ET (`/home/claude/evidence/e1/x`) and of
20 November 19:41-19:56 UTC (`p7-a`, `p7-b`). PC on Wi-Fi; stream 1920x1080 at 60, HEVC, SDR (`hdr=0`), requested
15000 kbit/s (`[slipstream-config] bitrateKbps=15000`); decoder D3D12VA (`[remoteplay-decode] actual=D3D12VA`);
no upscaler, no frame generation. Scene on 19 November: GTA, standing still outside (BENCH1); on 20 November not
recorded.

| Quantity | Value | Log field |
|---|---|---|
| Video bitrate actually delivered, 1-s windows, 19 Nov (n = 1,797) | p10 2.2, **median 4.2**, p90 5.6, max 8.9 Mbit/s | `[remoteplay-progress] videoMbps` |
| same, 20 Nov presentation runs (n = 397) | p10 3.1, **median 6.7**, p90 7.4, max 9.1 Mbit/s | same |
| same, 20 Nov audio runs (n = 184) | p10 3.15, **median 6.7**, p90 7.9, max 9.6 Mbit/s | same |
| Decoded surface | DXGI format 103 = NV12 (8-bit 4:2:0), 1920x1080 | `[remoteplay-texture] format=103` |
| Chroma reconstruction | bilinear at the stream's chroma siting | `[remoteplay-sampling] fine=true chromaLocation=1` |
| Frame arrival -> decoder output ready (n = 112 windows of 5 s) | median of P50s 0.29 ms, of P95s 0.39 ms | `[slipstream-latency] arrivalToDecodedP50/P95` |
| Engine pickup -> Present() called | median of P50s 1.05 ms | `pickupToPresentCallP50` |
| Present() call itself | median of P50s 0.29 ms | `presentCallP50` |
| Present -> scan-out (DXGI statistics; only dated frames) | median of P50s 1.3 ms (0.7-8.2), of P95s 4.7 ms | `presentToScanoutP50/P95` |
| Frame arrival -> scan-out | median of P50s 4.7 ms, of P95s 10.5 ms | `arrivalToScanoutP50/P95` |

What these do NOT say: `videoMbps` is the size of the assembled video frames per second (`SessionInbox::snapshot`,
`src/remoteplay/SessionInbox.cpp`) [code]: the encoder's output, without FEC and packet headers; it says nothing
about what the console was allowed to send, because the console's own target (`consoleTargetKbps`) is logged only
with `VEYRA_EXP_RP_HEALTH=1`, which was off [code]. "Decoder output ready" is when the decoder returned the frame to
the CPU; the GPU's decode work is waited for on the GPU (`EnhanceGraph::process`, decoder fence) and has no timestamp
of its own [code]. Nothing here includes the console (input to picture change in the game, encoding) or the
network's travel time. The PS-button test of BENCH1 (about 556 ms) is dominated by the console's own menu
response and cannot compare a few milliseconds (BENCH1, BENCH2).

## 2. Stage by stage, on the owner's settings

His settings as he reported them (docs/VISUAL_REFERENCE.md): PS5 Remote Play 1080p60, H.265 HDR, wired Ethernet
since 20 November; his reference configuration DLSS SR model M to a 4K target, VFG 2x; and in his tenth message
RTX Video SR and the neural layer as well. Which of these were on together in the play he describes is [unknown].

| # | Candidate cause | What our code does there | What published sources say | Verdict | The measurement that settles it |
|---|---|---|---|---|---|
| A | **Sony's compression and delivered bitrate** | Requests `bitrateKbps` from the profile (dialog: 5-100 Mbit/s; owner 15) in the launch spec (`bwKbpsSent`, chiaki-ng `launchspec.c`); `video_profile_auto_downgrade=false` (`ChiakiBackend.cpp`) [code] | chiaki-ng's slider goes to 100 Mbit/s and its default is 15 "to match the official remote play app's default" [published: chiaki-ng docs]; press: chiaki "allows the console to stream at basically any bitrate" (2023) [published, press]; Sony raised Portal's 1080p rate in March 2026 [published] | **Main cause** [estimate]; what is measured is the rate, not the cause: 4.2-6.7 Mbit/s median delivered at 15 requested, still scenes, Wi-Fi [measured-on-owner-PC]. Whether a higher request is honoured: [unknown] | Clarity arms `c-15`, `c-30`, `c-50`, `c-100` (section 9) in a high-motion scene: `[remoteplay-health] consoleTargetKbps` (what the console targets) and `[remoteplay-progress] videoMbps` median and max (what it sent), with the probe's `[slipstream-quality] stage=raw blockiness / detail`. Honoured = target follows the request AND videoMbps median and max rise |
| B | **Stream resolution** | 1920x1080 is the highest preset (`CHIAKI_VIDEO_RESOLUTION_PRESET_1080p`, chiaki-ng `session.h`; our dialog: 720p or 1080p) [code] | chiaki-ng's own settings offer 360p-1080p for the PS5 [published: chiaki-ng]; Sony's support page names "resolution" without values [published]; no source found for 1440p or 4K over Remote Play | **Hard limit**: 1080p in, 1440p screen = 1.33x per axis, nothing to recover the missing pixels | none needed |
| C | **H.265 encode and decode** | D3D12VA hardware decode, zero copy into the graph [code][measured-on-owner-PC] | A hardware decoder reproduces the encoder's pictures exactly (decoding is specified bit-exactly by the standard) [estimate] | **Decode: not a cause.** Encode: part of A. H.265 is already the better of the two codecs on offer | none needed; `[remoteplay-decode] fallback=false` confirms hardware |
| D | **4:2:0 chroma** | NV12 (SDR) / P010 (HDR) surfaces; chroma bilinear at the right siting when "fine sampling" is on (`ReconstructChroma`) [code][measured-on-owner-PC] | chiaki-ng's docs point to chroma subsampling as the reason 1080p looks better than 720p even on an 800p screen [published] | **Hard limit**, visible on saturated edges (red text, neon, foliage against sky). No 4:4:4 in the protocol: the codec choices are H.264, H.265, H.265 HDR only (`ChiakiCodec`) [code] | none needed |
| E | **Our scaling and filtering, incl. 4K-then-down** | Default SR target 4K; the presenter shrinks 3840x2160 to 2560x1440 with ONE bilinear sample per pixel (`PresentBlit.hlsl` `FineSample`, footprint > 1). Without SR: 1080p to 1440p by Catmull-Rom clamped to the 2x2 neighbourhood, in linear light [code] | NVIDIA recommends model M for half resolution per axis, K for higher-resolution modes (S5_NR_RESEARCH.md) [published] | **Minor for softness**, real for GPU cost and thin-line aliasing. Stream smooth's `SR_DISPLAY` switch removes the 4K intermediate | Visual arms `v-dlss` (1440p target) against `v-reference` (4K target) by eye and `detail`; stream smooth's `sm-sr4k-fg3` / `sm-sr-display` |
| F | **Neural / clean-up smoothing** | Neural layer: an 8-bit proxy at 1080 lines by default, its change composited back at the work size; in HDR its change is gated to zero above about 250 nits (`ParityDecode.hlsl` `HdrRestore`); not verified for HDR at all (planner note `neural-hdr-unverified`). Clean-up: off by default; when on, smooths texture fainter than 2-4 code values by design (docs/RESTORATION.md section 1) [code] | NVIDIA's DLSS 5 neural rendering takes motion vectors, depth, jitter and a mask from a game engine; on video it gets none of that (S5_NR_RESEARCH.md) [published] | **Likely contributor when on**; unverifiable without a picture | Visual `v-neural`, neural session `nn-*`: `[slipstream-quality] stage=output detail` against `stage=raw`, flicker, and by eye |
| G | **Temporal reconstruction (DLSS SR, VSR, FG)** | DLSS SR: zero jitter (`EvalDesc` jitter left 0), constant depth, motion from optical flow when `srUsesFlow()` (the SR motion source), otherwise zero motion, auto exposure, sharpness 0 (`EnhanceGraph.cpp` `runSr`, `DlssSrBackend.cpp`). RTX Video SR: spatial (no history), 8-bit; in HDR gated off above about 150-250 nits (`HdrVideoSrRestore.hlsl`). VFG: interpolation between two real frames at the work size [code] | DLSS "emulates a higher sample rate" from sub-pixel jitter, wants at least 16 jitter phases, and "incorrect or poor precision motion vectors are the most common cause of visual artifacts" [published: DLSS guide 310.5.0] | **Likely the "smear in motion"**: DLSS on unjittered video can only blend in time; VFG frames are interpolations. VSR is spatial: it cannot smear in time, but in HDR it does little | `v-dlss` against `v-original` and `v-clean-nis` (spatial) by eye in fast motion and `flicker`; `q-quality` (frame quality test: every second frame withheld and compared) for generated frames |
| H | **HDR / colour conversion** | P010 -> BT.2020 PQ -> linear FP16 scRGB (1.0 = 80 nits); final write PQ R10G10B10A2 or FP16 [code] | none needed | **Not a softness cause**, except via the HDR gates of F and G (RTX Video SR and neural layer skip bright areas) | `c-hdr-*` arms of the clarity arms: `[slipstream-quality] hdr=1` raw against output |
| I | **Frame-generation artifacts** | Generated frames are 50 % of shown frames at 2x, 83 % at 6x; at a 4K work size VFG may skip whole pairs (stream smooth C1/C4) [code] | NVIDIA's video frame generation interpolates between two real frames and copies the current frame at a shot change (S5_NR_RESEARCH.md) [published] | **Contributor in motion**, owned by stream smooth (pacing) | `q-quality`; stream smooth's `[slipstream-smooth]` |

## 3. Facts asked for in the brief

| Fact | Answer | Tag |
|---|---|---|
| Bitrates PS5 Remote Play allows | The protocol carries a requested bandwidth (`bwKbpsSent` in the launch spec). chiaki-ng's slider: 2-100 Mbit/s for the PS5; defaults 2 / 6 / 10 / 15 Mbit/s for 360p / 540p / 720p / 1080p. Our dialog: 5-100 Mbit/s; our validator 1-100 Mbit/s | [code][published: chiaki-ng] |
| Bitrate delivered at 1080p | 4.2-6.7 Mbit/s median, 9.6 Mbit/s max, at 15 requested, Wi-Fi, SDR, mostly a still scene | [measured-on-owner-PC] |
| Is a higher request honoured? | Not known. Press (2023): chiaki "allows the console to stream at basically any bitrate"; Sony's own client got a higher 1080p rate in 2026. No measurement exists of what a PS5 sends at 30, 50 or 100 Mbit/s requested | [published, press][unknown] |
| The console's own target | The PS5 reports `target_bitrate` in its connection-quality messages; chiaki-ng logs it at verbose level only; our patch 0004 keeps it and `[remoteplay-health] consoleTargetKbps` prints it, after its unit is decided (`ConsoleQuality.h`). Never logged on the owner's PC yet | [code] |
| 1440p over Remote Play | No. The protocol's presets end at 1080p; no source mentions a higher one. (The PS5's HDMI output does offer 1440p, without VRR) | [code][published] |
| 4:4:4 over Remote Play | No. The protocol's codec field offers H.264, H.265 and H.265 HDR; the decoded surfaces on the owner's PC are NV12 (4:2:0); a 4:4:4 stream would decode to another format | [code][measured-on-owner-PC] |
| Chroma format | 4:2:0: 8-bit NV12 in SDR (measured), 10-bit P010 in HDR (code) | [measured-on-owner-PC][code] |
| PS5 HDR over Remote Play | Needs the PS5 itself to output HDR, so it must be connected to an HDR display (chiaki-ng's docs) | [published] |

## 4. What each possible change costs in latency

| Change | Added input delay | Tag |
|---|---|---|
| Higher bitrate request | Wire time per frame: about 0.25 ms at 15 Mbit/s, 0.8 ms at 50, 1.7 ms at 100 on gigabit Ethernet (frame bits / link rate); keyframes several times that; more on Wi-Fi. Console encode time: [unknown] | [estimate] |
| SR target 1440p instead of 4K | None added; less GPU work per frame | [code][estimate] |
| Spatial scale (presenter, NIS) instead of DLSS SR | None added; GPU time is inside the same frame | [code] |
| RTX Video SR (Video Effects families) | Its own CUDA round trip inside the frame (docs/NEURAL_RENDERING.md section 2) | [code] |
| Clean-up | One frame in, one frame out, no history: GPU time only | [code] |
| Frame generation (any interpolating method) | At least one real-frame interval: 16.7 ms at 60, 33 ms at 30 unique pictures/s | [code] |

GPU time inside a frame becomes latency only when it delays the present; with the frame arriving every 16.7 ms
and a GPU span well under that, it does not queue [estimate]. Per-stage GPU times are in `[slipstream-sr] gpuP95=`,
`[slipstream-clean]` and, once merged, stream smooth's `[slipstream-smooth] gpu<Stage>P95Ms`.

## 5. Low-latency improvements the owner asked to be evaluated

| Approach | Assessment | Tag |
|---|---|---|
| Better bitrate negotiation | The protocol has one knob, the requested bandwidth, already exposed up to 100 Mbit/s. Nothing else to negotiate: resolution and chroma are fixed. The clarity arms measure whether the knob works | [code][unknown] |
| Better hardware decoding and presentation | Decoding is already hardware, zero copy, and exact. Presentation is stream "present"'s work (Package 8B) | [code][measured-on-owner-PC] |
| Single-pass high-quality scaling | Exists twice: the presenter's clamped Catmull-Rom (1080p to 1440p, no SR) and NIS. A Lanczos-3 port is described in S5_SRC_VIDEO.md; it would be a little sharper than the clamped cubic, with ringing risk. Not worth building before the session shows the spatial path wins | [code][estimate] |
| GPU compression-artifact reduction | Exists: the clean-up stage (deband ported from libplacebo, own deblock / dering). It removes blocks and bands; it cannot add detail | [code] |
| Lightweight spatial reconstruction | NIS (any GPU) and RTX Video SR (NVIDIA; SDR only in practice, see the HDR gate) exist | [code] |
| Avoid redundant GPU processing | Already listed and partly switched by streams pipeline and smooth: 4K intermediate, motion estimated twice, repeated pictures processed twice (docs/NEURAL_RENDERING.md section 4) | [code] |
| More efficient GPU scheduling | Stream smooth's area | |
| Low-latency capture | Section 8 | |

Open-source projects read for this (study files): chiaki-ng (protocol, bitrate options), moonlight-qt and Sunshine
(presentation, statistics), libplacebo (deband, dither, scaling). None of them can raise what the PS5 sends beyond
the requested bandwidth.

## 6. HDR: why the AI stages give up in bright areas

RTX Video SR's NVIDIA interface takes 8-bit RGBA only (`src/ngx/VideoSrBackend.cpp`), so in HDR the graph feeds
it an SDR "proxy" of the picture: linear light divided by a 203-nit reference, compressed softly above 0.75
(`HdrProxy`), and adds back only the CHANGE the upscaler made, on top of a bilinear upscale of the HDR picture
(`HdrRestore`). The change is multiplied by a gate that is 1 below a proxy value of 0.75 and falls to 0 at 0.995
[code]. In nits of the brightest colour channel [estimate: from the formula]: full effect up to about 150 nits,
15 % at 200 nits, 0.5 % at 250 nits, nothing from about 300 nits. The neural layer's HDR path uses the same
`HdrRestore` [code]. The gate exists so that highlights are never clipped or shifted in colour by an SDR network;
the price is that a bright HDR scene is mostly bilinear. DLSS SR does not have this gate (it takes linear FP16
with the HDR flag) [code].

## 7. Interpretation limits

- The delivered bitrate was measured in mostly still scenes over Wi-Fi. A still scene needs few bits; a
  variable-rate encoder sends less than it may. It does not prove that the console ignores the request; the clarity
  session must use a moving scene (driving fast, panning across foliage) [estimate].
- Every image metric of the probe is no-reference: there is no pristine original of a Remote Play picture, and
  sharpening raises "detail" without making the picture truer (S5_NR_RESEARCH.md B3) [published]. The final judge of
  smear is the owner's eye on a moving scene, with the comparison clips.

## 8. Capture cards: realistic assessment

**What a capture card changes.** The PS5's HDMI output goes into the card instead of a screen; the PC gets
uncompressed pictures: no Sony encoder, no network, no decoder [estimate]. With the PS5 set to 1440p output
(available since 2022, no VRR at 1440p) [published] the picture would be the game's own 1440p, scaled by nobody.

**What PushFPS already supports** [code] (its upstream, Veyra, was built for capture cards):
`src/source/CaptureCardSource.cpp` (DirectShow capture with a sample grabber; uncompressed NV12, P010 / P016, YUY2
and its variants, I420 / YV12, RGB; MJPEG decoded on worker threads), native paths for Blackmagic DeckLink
(`DeckLinkCapture.cpp`) and Magewell (`MagewellCapture.cpp`), AVerMedia and Elgato HDR controls; a buffer policy
(`CaptureBuffer.h`: automatic = 2 driver buffers at 1080p and below, 3 above; "minimum" = 1); and a log line that
measures how old a frame already is when the driver hands it over (`[capture-driver-age] p50Ms= p95Ms=
driverBuffers=`). The frame then goes through the same graph and presenter as a Remote Play frame.

**Formats and links** [published: manufacturer claims]. Elgato 4K X (USB 3.2 Gen 2, 10 Gbit/s): SDR up to 1440p240,
HDR up to 1440p60; uncompressed up to 4K60, MJPEG above. Elgato 4K Pro (PCIe 2.0 x4): HDR up to 1440p240. Elgato 4K S
setup page: "lag-free passthrough at up to 4K60 HDR". An uncompressed 1440p60 P010 picture is about 5.3 Gbit/s
[estimate: 2560 x 1440 x 1.5 samples x 2 bytes x 60], inside a 10 Gbit/s USB link and a PCIe x4 slot.

**HDCP.** The PS5 has its own switch: Settings > System > HDMI > HDCP. Turn it off while the PS5 is connected
directly to a screen; the menu cannot be reached through the card while HDCP is on (Elgato's PS5 set-up page)
[published: manufacturer]. Whether
games are affected: Elgato's page lists none; streaming-video apps on the PS5 commonly refuse to play without HDCP
[unknown: no Sony page read].

**Latency: what is measured, what is claimed.**

| Source | What it says | Kind |
|---|---|---|
| Digital Foundry, PS Portal review (Nov 2023), via a ResetEra summary | Remote Play 4-5 frames, 66-83 ms behind the PS5 on a TV; PS5 wired; on-screen timer; the PC app "similar" | independent measurement [published] |
| Same review, Russian press summary | 60-80 ms on the Portal | press of the same measurement [published] |
| EposVox, OBS forum capture-card documentation (2019) | Method: Leo Bodnar 1080p60 lag tester, preview full screen, monitor's own lag subtracted; per-card values only in chart images (not readable here); text: Elgato 4K60 Pro "hitting 200ms" in Elgato's own app | independent measurement, old cards, numbers not extractable [published] |
| A user on X (2020, Leo Bodnar tester) | Elgato 4K Capture Utility 146 ms, OBS 62 ms ("5 frames" apart); card and whether the monitor's lag was subtracted not stated in the excerpt | individual measurement, method partly unknown [published] |
| alive-project.com (Oct 2026) | OBS preview: 4K X about 60 ms, 4K S about 43 ms; photographed screens; the author gives a margin of error of about 0.1 s | measurement with an error larger than the differences [published] |
| AVerMedia (2020) | "lowest preview latency of any capture card on the market at only 35ms" (Live Gamer 4K) | manufacturer claim [published] |
| Elgato | "lag-free" passthrough; no capture-latency figure | manufacturer claim [published] |

**What can be said honestly.**

- The application matters as much as the card: the same capture showed 146 ms in a vendor's app and 62 ms in OBS
  in one individual's test [published]. Through PushFPS the extra is our pipeline and presenter, measured at about
  5 ms from frame arrival to scan-out in the owner's Remote Play runs [measured-on-owner-PC], plus the driver age,
  which `[capture-driver-age]` would measure [code].
- A capture card replaces encode + network + decode + Remote Play's jitter handling with HDMI capture and one or two
  driver buffers (16.7 ms each at 60 Hz) [estimate]. Remote Play costs 66-83 ms over a TV by DF's measurement
  [published]. So capture is plausibly not slower than Remote Play, possibly faster [estimate]. **No credible
  measurement found shows it for a current card on a PC like his.**
- PCIe against USB: PCIe cards DMA straight into memory; USB 3 cards move uncompressed frames over a 5-10 Gbit/s
  link, which at 1440p60 is a few milliseconds per frame [estimate]; no measurement comparing the two was found.
- The honest zero-latency reference is **the PS5 straight into the monitor**: nothing is added, the picture is the
  console's own 1440p; it costs frame generation and every PC-side processing step (the console's 30 FPS stays 30)
  [estimate]. A card's passthrough port gives the same picture to the monitor, but then the PC processes a copy
  nobody sees.
- **Recommendation: do not buy on the current evidence.** If the owner wants to try, the decisive test is a camera
  test (section 9, step 4) of three arms on the same monitor: `direct-ps5`, the current Remote Play arm, and the
  card through PushFPS; buy where it can be returned.

## 9. Measurement plan (a repeatable baseline on the owner's PC)

Internal software times and true controller-to-photon latency are kept apart: the first comes from log lines,
the second only from a camera or a light sensor.

1. **Internal stage baseline** (every arm of every kit session already logs it; kit option `-Health` adds network):
   - input: `[slipstream-input]` (mouse and keyboard meter), `[remoteplay-timing] input=` (pad schedule);
   - network: `[remoteplay-health] consoleRttMs` (the console's round trip), `frameJitterMs`, `lossPercent`
     (with `VEYRA_EXP_RP_HEALTH=1`); the console's response and encoding are NOT visible to the PC;
   - decode: `[slipstream-latency] arrivalToDecodedP50/P95` (CPU-side; GPU decode has no timestamp);
   - enhancement: `[slipstream-sr] gpuP50/gpuP95`, `[slipstream-clean]`, `[slipstream-nr]`, stream smooth's
     `[slipstream-smooth] gpu<Stage>P95Ms` once merged;
   - frame generation: the pair delay (`pushFgPairDelayMs`), `[live-fg-admission]`, `[fg-drops]`;
   - presentation: `[slipstream-latency] pickupToPresentCall*, presentCall*, presentToScanout*, arrivalToScanout*`.
2. **Clarity arms** (this branch; arms.csv set `clarity`, run inside the visual session's plan:
   `Run-First-Test.cmd -Session visual -Arms c-15,c-30,c-50,c-100`, then `-Arms c-hdr-15,c-hdr-50` with the PS5
   outputting HDR): the same moving scene for every arm (driving fast, panning across foliage), one app start per arm,
   two rounds, the probe for 20 s, health on, no processing. The visual page's new `stream rate` line gives per arm the
   requested rate, the console's target (`[remoteplay-health] consoleTargetKbps`), the delivered median and max
   (`[remoteplay-progress] videoMbps`) and loss; the `quality raw` line gives `blockiness banding detail`. Rule: a higher
   request is "honoured" when the console's target follows it and the delivered median and max rise by at least 20 %
   against `c-15`; it "helps" when, in addition, raw blockiness falls by 10 % or more with loss unchanged. Watch the
   comparison clips of the best arm against `c-15` too: the metrics are no-reference.
3. **Picture paths**: the existing visual and neural sessions (`v-original`, `v-dlss`, `v-dlss-k`,
   `v-clean*`, `v-neural`, `v-reference`, `nn-*`) and stream smooth's `sm-sr4k-fg3` / `sm-sr-display`,
   run at the bitrate the clarity arms found best; judged by eye in fast motion with comparison clips.
4. **Controller to photon** (camera at 240 fps or a light sensor; templates `camera-template.csv`,
   `sensor-template.csv`): arms `direct-ps5`, the clarity arms's best arm, the same with VFG 2x, and, only if a
   card is borrowed or returnable, `capture-card` (any arm name is accepted). 30 presses or more per arm. Same
   button, same scene, same monitor. Only this step can say "no added latency".

## 10. Small changes on this branch

- A test-only override of the requested bitrate and codec for one connection (`VEYRA_TEST_RP_BITRATE_KBPS`,
  `VEYRA_TEST_RP_CODEC`, `include/veyra/remoteplay/ClarityOverride.h`, applied in `ps5Connect` after the profile is
  saved, so the saved choice never changes; unset = no change). Needed because the bitrate and codec live in the
  saved console profile and the kit must not edit it. Logged as `[remoteplay-clarity]`.
- The six clarity arms, the `stream rate` line on the visual page, and their dry-run fixture.
- A clip mode for identical-frame comparisons (`VEYRA_TEST_RP_CLIP`, kit `-Clip`) and the seven A-F arms (section 12).
- Nothing that changes the picture or the default request: the evidence does not yet support a default change. The
  dialog already offers 5-100 Mbit/s, so if the arms show that a higher request is honoured and helps, the owner can
  choose it himself today; a new default would follow only after that measurement.

## 11. Inventory rows for the clarity area (master directive section 2)

Classes: working = implemented and working; defective = implemented but defective; feasible = missing and feasible;
experimental; unsupported = unsupported by the protocol; not worthwhile. "Latency" = expected effect on input delay.
Nothing here is hardware-validated.

| Item | Class | Evidence | Latency |
|---|---|---|---|
| Hardware HEVC decode, zero copy into the graph | working | [measured-on-owner-PC] `[remoteplay-decode] actual=D3D12VA fallback=false`; [code] | none |
| Chroma reconstruction at the stream's siting (bilinear) | working | [measured-on-owner-PC] `[remoteplay-sampling] fine=true`; [code] `ReconstructChroma` | none |
| YUV to linear FP16, HDR10 kept through the graph | working | [code] `YuvToLinearRgb.hlsl`, final PQ write | none |
| Presenter 1080p to 1440p: clamped Catmull-Rom (no SR) | working | [code] `PresentBlit.hlsl` `FineSample` | none |
| Presenter shrink of a 4K work picture: one bilinear tap | defective (aliasing, wasted work) | [code]; fix = SR target 1440p (stream smooth's `SR_DISPLAY`) | none; less GPU |
| RTX Video SR on an HDR stream | defective by design: no effect above about 200-250 nits | [code] `HdrRestore` gate (section 6) | GPU time only |
| Neural layer on an HDR stream | defective by design (same gate), unverified for HDR | [code] `ParityDecode.hlsl`; planner note `neural-hdr-unverified` | GPU time only |
| DLSS SR on video (no jitter, flow motion, constant depth) | experimental: may smear in motion | [code]; [published] DLSS guide 310.5.0 | GPU time only |
| Compression clean-up (deband ported, deblock / dering own) | experimental, off by default | docs/RESTORATION.md | GPU time only, one frame |
| NIS single-pass spatial scale, any GPU | working (NIS is upstream-tested in CI on WARP) | CLAUDE.md table; `NisGpuTests.cpp` | GPU time only |
| Requested bitrate above 15 Mbit/s | feasible (dialog offers 5-100); effect [unknown] | section 3; clarity arms `c-*` | +0.25 to 1.7 ms wire time per frame on gigabit [estimate] |
| 1440p, 4K or 4:4:4 over Remote Play | unsupported | [code] presets end at 1080p; codec field has no 4:4:4 | n/a |
| Lanczos-3 1080p to 1440p (libplacebo-style, 12 taps) | not worthwhile until the clarity arms show the spatial path wins (the clamped cubic is close) | S5_SRC_VIDEO.md 2.3 [estimate] | none |
| Sharpening (RCAS / CAS style) after the scale | not worthwhile on this source: it amplifies compression noise (AMD: noisy passes belong after upscaling "to avoid those noisy components being amplified") | [published] AMD FSR page | none |
| Frame generation (VFG) for smoothness | working, owned by stream smooth; never zero added delay | [code] `pushFgPairDelayMs` | +1 real-frame interval (16.7 / 33 ms) |
| HDMI capture source + PS5 control by Remote Play | working in code (upstream's main use; `PS5 手柄控制` controller-only session) | [code] `CaptureCardSource.cpp`, `RemotePlaySessionSource.cpp` controller-only branch | [unknown]: needs the camera test |
| Delivered-bitrate and console-target readout in the visual page | working in code (this branch) | `measures.csv` group `stream rate` | none (log lines) |

## 12. The A-F comparison on the same frames (master directive section 10)

Live Remote Play runs never show the same frames twice, so this branch adds a clip mode: `run-tests.ps1 -Session visual
-Arms cl-a-raw,cl-b-nis,cl-c-vsr,cl-d-neural,cl-e-vsr-neural,cl-f-clean-nis,cl-f-dlss -Clip <file>` starts every arm on
the same recorded file (`VEYRA_TEST_RP_CLIP`) from its start (a position remembered by an earlier arm is not resumed;
the run logs `step=0 name=clip state=done reason=ok start=0`) with the visual session's plan (settle 8 s, probe 20 s,
wait 40 s) [code]. An arm whose session request is not its own bitrate or codec is set aside as "test override not
applied" (section 10 switches).

| Arm | Path | Master directive |
|---|---|---|
| `cl-a-raw` | decoded picture, no processing (the presenter's clamped cubic to the screen) | A |
| `cl-b-nis` | one spatial scale to 2560x1440 (NIS) | B |
| `cl-c-vsr` | RTX Video SR only (streaming family) to 2560x1440 | C |
| `cl-d-neural` | the neural layer only, conservative look | D ("NVIDIA noise reduction / neural enhancement") |
| `cl-e-vsr-neural` | both | E |
| `cl-f-clean-nis`, `cl-f-dlss` | clean-up + NIS; DLSS to 2560x1440 | F |

How to make the clips (one per scene: standing still, camera turning, distant buildings, foliage, water, night, fine
text such as the map or phone screen): play through Remote Play with every enhancement off and record with PushFPS's
recorder for at least 90 s. The recorder's standard rate is about 0.3 bits per pixel [code: CLAUDE.md "Recording
files"], about 37 Mbit/s at 1080p60 [estimate], several times what the PS5 delivered (4-10 Mbit/s), so the
re-encode adds little to the console's own loss [estimate]; the recorded clip is the reference for every arm.

What can be concluded, honestly:
- Each arm's probe compares its output with the raw picture of the SAME frames (`[slipstream-quality] stage=raw` and
  `stage=output`) [code]. Across arms the frames are the same clip at the same plan offsets, matched to within a few
  frames of timing jitter, not bit-identical frame sets [estimate].
- There is no pristine reference: the game's own frame before Sony's encoder does not exist on the PC. The metrics
  measure change against the clip, never recovered truth. Higher "detail" can be sharpening or invention; only the
  comparison clips (split view, by eye, in motion) can tell detail that belongs to the scene from detail that does
  not, and nothing can bring back what the encoder removed.
- Not built: verdict lines against `cl-a-raw` (the visual verdict rule is written against `v-original`; the A-F page
  shows each arm's numbers side by side without a verdict word).

## 13. Low-latency image processing (master directive section 11)

| Approach | What exists or was found | Verdict |
|---|---|---|
| Better single-pass spatial scaling | presenter's clamped Catmull-Rom; NIS; FSR 1 EASU is edge-adaptive, two passes, 0.2-0.5 ms at 1440p on 2021 cards [published, AMD]; libplacebo's Lanczos-3 [S5_SRC_VIDEO] | test B against A first; build nothing new unless B wins clearly |
| Edge-aware scaling | NIS and FSR 1 EASU are edge-adaptive [published] | covered by B |
| Compression artifact reduction | clean-up stage (single frame, no history) [code]; NVIDIA VFX Artifact Reduction is tuned for H.264 and takes float planar BGR through CUDA (S5_NR_RESEARCH) | F1; no NVIDIA SDK path added |
| Chroma upsampling | already sited bilinear [code]; better (joint, luma-guided) filters exist in video players [estimate] | not worthwhile: 4:2:0 loss is in the source |
| Selective / adaptive sharpening | RCAS / CAS exist (MIT); AMD warns noise is amplified by passes before or around the scaler [published] | not worthwhile on a 5-10 Mbit/s source; would add halos on blocks |
| Low-cost deblocking, texture-preserving denoise | clean-up's deblock (off in the default parts) and grain parts [code] | experimental; judged in F1 |
| Colour conversion | correct matrices, range, PQ [code] | nothing to fix |
| Alternative NVIDIA paths | RTX Video SR families, DLSS SR; DLSS 5 neural rendering has no public SDK (S5_NR_RESEARCH) | C, D, E, F2 cover them |

No heavyweight model is proposed: nothing found can restore detail removed by the encoder, and every learned method
in reach either needs engine data it does not get (DLSS 5) or blends in time (DLSS SR).

## 14. Input latency: a credible method (master directive section 13)

| Stage | How it is measured | Kind |
|---|---|---|
| Input device | the device's own rate (DualSense over USB reports at 250 Hz; our pad poll 4 ms, `[remoteplay-timing] input=`) | [code] |
| Input transmission | `[slipstream-input] eventToSendMsP95` (event to the packet handed to the network); network flight not visible | internal timestamp |
| PS5 game processing, video encoding | not visible to the PC | camera only |
| Network transport | `[remoteplay-health] consoleRttMs` (the console's round trip), `frameJitterMs` | internal, indirect |
| Decode | `[slipstream-latency] arrivalToDecodedP50/P95` (CPU side; the GPU decode is waited on the GPU) | internal timestamp |
| Enhancement | `[slipstream-sr] gpuP95`, `[slipstream-clean]`, `[slipstream-nr]` | GPU timestamps |
| Frame generation | pair delay `pairDelayMs` in `[slipstream-fg]` | internal, by construction |
| Presentation | `pickupToPresentCall*`, `presentCall*`, `presentToScanout*` | internal + DXGI statistics |
| Display response | the same monitor in every arm: cancels in differences | camera only |

Button to photon: a phone at 240 frames/s (4.2 ms per camera frame), on a stand, the button and the screen in one
picture; count camera frames from the press to the first changed screen; at least 30 presses per arm, arms interleaved,
same scene, same monitor; templates `camera-template.csv` / `sensor-template.csv`; `run-tests.ps1 -PackOnly` turns
them into tables. Arms: `direct-ps5` (the reference), the current Remote Play setting, each candidate change, and a
capture card if one is on loan. With a per-press spread of about 26 ms (BENCH1, PS button) 30 presses give a standard
error of about 4.7 ms per arm and 100 presses about 2.6 ms [estimate: 26 / sqrt(n)]: a difference under about 10 ms
needs 100 presses per arm. The PS button includes the console's menu response; an in-game action with an instant
visible result (GTA: firing a weapon) is better, but a 30 FPS game adds up to 33 ms of random phase per press, which
is why the press count matters. Internal timestamps are never presented as button-to-photon latency.

## 15. HDMI capture: passthrough and capture preview kept apart (master directive section 17)

- **Passthrough latency** (the card's HDMI out to the monitor) is what the PS5's own picture costs through the card:
  Elgato calls it "lag-free" [published: manufacturer]; it is the `direct-ps5` reference with the card in the cable.
  Nothing on the PC side (frame generation, enhancement) applies to it.
- **Capture-preview latency** (the card into the PC, through PushFPS, to the monitor) is what matters for using the
  PC's processing: card buffering + driver buffers (`[capture-driver-age]`) + our pipeline (about 5 ms arrival to
  scan-out on the owner's PC for Remote Play frames [measured-on-owner-PC]) [code][estimate].
- **Input path with a card:** the DualSense straight into the PS5 (no PC in the input path), or PushFPS's controller-only
  Remote Play session ("PS5 手柄控制"), which sends the pad and mouse-and-keyboard input over the network while the
  picture comes from the card and drops the console's keepalive video (`RemotePlaySessionSource.cpp`) [code].
- **1080p60 and 1440p60:** the PS5 outputs both; VRR is not available at 1440p [published]. USB 3.2 Gen 2 (10 Gbit/s)
  carries uncompressed 1440p60 P010 (about 5.3 Gbit/s) [estimate]; a USB 5 Gbit/s card would need MJPEG or NV12 at
  lower rates, which PushFPS decodes or uploads (`CaptureCardSource.cpp`) [code].
- **Costs:** a card's price is not researched here (retail prices change; no source read) [unknown].
- Verdict unchanged (section 8): plausible, not proven; decide by the camera test with `direct-ps5`, Remote Play and the
  card's capture preview on the same monitor.

## Sources

Published (read 21 November 2026):

- chiaki-ng documentation, configuration page (bitrate, HDR, chroma subsampling), and its release notes (default
  bitrate "to match the official remote play app's default"); pinned source `0e16950165f0`
  (`lib/src/session.c` presets, `lib/src/launchspec.c` `bwKbpsSent`, `gui/src/qml/SettingsDialog.qml` slider 2-100):
  read from the pinned checkout `/home/claude/vendor/chiaki-ng` (`docs/setup/configuration.md`,
  `docs/updates/releases.md`); project https://github.com/streetpea/chiaki-ng
- PlayStation Blog, PS Portal 1080p High Quality mode, 17 March 2026:
  https://blog.playstation.com/2026/03/17/new-playstation-portal-software-update-adds-1080p-high-quality-mode-and-ux-refinements/
- Notebookcheck (press), 21 March 2026, "from 15 to 25 Mbit/s":
  https://www.notebookcheck.net/PS-Portal-is-becoming-more-appealing-Sony-finally-fixes-one-of-its-biggest-shortcomings.1256141.0.html
- Wccftech (press), 27 November 2023, Portal bitrate and chiaki "basically any bitrate":
  https://wccftech.com/playstation-portal-issues-bitrate-limitations/
- PlayStation support, PS Remote Play on PC and Mac (settings, 5 / 15 Mbit/s recommendations):
  https://www.playstation.com/support/games/playstation-remote-play-on-pc-and-mac/
- TechPowerUp (press, citing Digital Foundry), 18 November 2026, GTA 6 on PS5:
  https://www.techpowerup.com/355004/gta-6-reportedly-plays-best-on-ps5-pro-xbox-series-s-struggles?cp=2
- Rockstar Support, GTA VI settings, 18 November 2026:
  https://support.rockstargames.com/articles/4jUHNdP22SmaKSF8DWqhXC/how-to-change-grand-theft-auto-vi-settings
- NVIDIA DLSS Programming Guide, revision 310.5.0, December 2025 (mirror):
  https://primebuild-pc.github.io/guida-completa-nvidia-dlss-4.5/DLSS_Programming_Guide_Release.pdf
- AMD FidelityFX Super Resolution 1 page (passes, cost, MIT licence, noise warning): https://gpuopen.com/fidelityfx-superresolution/
- AMD FidelityFX Super Resolution 3 manual (jitter, reactive mask): https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-upscaler/
- Tom's Guide (press), 8 September 2022, PS5 1440p output and the VRR caveat: https://www.tomsguide.com/news/ps5-update-1440p-output
- Elgato, Game Capture 4K S with PS5 (HDCP path, passthrough):
  https://www.elgato.com/us/en/explorer/products/capture/how-to-set-up-game-capture-4k-s-with-playstation-5-and-playstation-5-pro/
- Elgato, 4K X against 4K Pro (interfaces, formats):
  https://www.elgato.com/us/zh/explorer/products/capture/what-is-the-difference-between-elgato-game-capture-4k-x-and-4k-pro/
- ResetEra summary of Digital Foundry's PS Portal review (latency 66-83 ms):
  https://www.resetera.com/threads/playstation-portal-df-tech-review-decent-hardware-but-does-remote-play-cut-it.788895/
- Shazoo (press), 27 November 2023, same review: https://shazoo.ru/2023/11/27/150475/60-80-ms-zaderzka-i-dostoinoe-zelezo-razbor-playstation-portal-ot-digital-foundry
- EposVox, capture card documentation, OBS forum, 2019:
  https://obsproject.com/forum/resources/capture-card-documentation-latency-decode-modes-formats-more.777/
- User measurement on X (2020; read from the search result's excerpt, the page refused automated reading):
  https://x.com/pkns_marine/status/1265162762885320704
- alive-project.com, 14 October 2026: https://alive-project.com/en/streamer-magazine/article/19722/
- AVerMedia on X (2020), Live Gamer 4K claim: https://x.com/AVerMedia/status/1261723110618943488?lang=en

Earlier study files used (same folder as the brief): S5_SRC_VIDEO.md, S5_SRC_STREAM.md, S5_SRC_CHIAKI.md,
S5_NR_RESEARCH.md, BENCH1.md, BENCH2.md.

Not found or not readable: a Sony statement of Remote Play's bitrates or resolutions; any measurement of what a PS5
sends at a request above 15 Mbit/s; a recent independent capture-to-photon measurement of a current capture card
through a Windows app; Digital Foundry's original article (read through two summaries).
