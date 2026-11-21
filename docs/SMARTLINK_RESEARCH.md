# SmartLink research (sprint 4, research stream)

Status: code reading and literature reading only. Nothing in this document is a measurement of this
product. Paths without a prefix are in the pinned chiaki-ng tree (`/home/claude/vendor/chiaki-ng`, commit
`0e16950165f0`, unpatched; the product applies `scripts/remoteplay/patches/0001..0004` on top). Product paths
are relative to the `exp/rc4` tree. Line numbers are those of these trees on 20 Nov 2026.

## A. PS5 Remote Play protocol: what the client can and cannot control

### A1. Bitrate

**How it is requested.** Once, at stream start, inside the encrypted launch spec JSON sent in the BIG message:
`"network":{"bwKbpsSent":%u,"bwLoss":0.001000,"mtu":%u,"rtt":%u,...}` (`lib/src/launchspec.c:23-29`), filled from
`connect_info.video_profile.bitrate` (`lib/src/streamconnection.c:1009`), the Senkusha MTU (`:1001`) and the Senkusha
average RTT in ms (`:1002`, `rtt_us/1000`). `bwLoss` is a fixed constant (`launchspec.c:25`). PS5-only extras: the
codec, SDR/HDR and `"adaptiveStreamMode": "resize"` (`launchspec.c:75-84`). Only one resolution is offered, with
`"score":10` (`launchspec.c:12-22`). chiaki-ng presets: 360p 2000, 540p 6000, 720p 10000, 1080p 15000 kbit/s
(`lib/src/session.c:97-115`). The product sets the rate from `request.video.bitrateKbps`, turns
`video_profile_auto_downgrade` off, `packet_loss_max=0.05`, `enable_idr_on_fec_failure=true`
(`src/remoteplay/ChiakiBackend.cpp:266-268`).

**Can any message change it during a session?** Not in anything chiaki-ng sends. The client-to-console messages in
the pinned library are: BIG (launch spec), STREAMINFOACK, CONTROLLERCONNECTION, STREAMINFO (microphone header; it
explicitly leaves `congestion_control_interval` unset, `streamconnection.c:1159-1166`), DISCONNECT, HEARTBEAT,
CORRUPTFRAME (`:1263-1282`), IDRREQUEST (`:1285-1302`), the 200 ms congestion packet (below), feedback (controller
state) and microphone audio. The protobuf schema (`lib/protobuf/takion.proto`) also names messages chiaki-ng never
sends: `DebugOption` type `BITRATE` with `BitratePayload { toggle, bitrate_change (float) }` (`takion.proto:221-250`),
`FPSCHANGE { fps }` (`:143-145`), `CLIENTMETRIC` (per-stage video/audio timing statistics, `:174-211`) and
`SERVERSETTINGS { idr_period, send_periodic_timestamp }` (`:335-338`). Whether a retail PS5 honours a client
`DebugOption BITRATE` or `FPSCHANGE`, and in which direction FPSCHANGE flows, is **not verified**: no open client
sends them, and sending an unknown/debug message to the console risks a disconnect. Treat them as research items for
a separate, opt-in hardware experiment, never as a SmartLink control.

**CONNECTIONQUALITY (console -> client).** Payload `ConnectionQualityPayload { target_bitrate uint32,
upstream_bitrate uint32, upstream_loss float, disable_upstream_audio bool, rtt double, loss uint64 }`
(`takion.proto:285-292`). chiaki-ng only logs it at VERBOSE (`streamconnection.c:693-708`) and uses its arrival to
close a measuring window: it computes `measured_bitrate` from the bytes and frames received since the previous report
(`chiaki_stream_stats_bitrate` = bytes x 8 x fps / frames, `lib/src/frameprocessor.c:30-35`) and resets the counter.
Units: none are documented in the source. `target_bitrate` is very probably kbit/s (same scale as `bwKbpsSent`, by
analogy only: not verified); `rtt` is a double whose unit is **not verified** (the owner's on-screen "2-79 ms" suggests
ms or s scaled by the GUI; check against the Senkusha RTT on hardware); `loss` is a 64-bit count, `upstream_loss` a
fraction. **Sending interval: unknown** — it is chosen by the console; nothing in the client requests or sets it (the
`congestion_control_interval` field that might relate to it is left unset by the client, `streamconnection.c:1166`).
The product reads only `target_bitrate`, and only when the opt-in `VEYRA_TEST_PS5_QUALITY_TRACE` turns on chiaki's
VERBOSE log (`src/remoteplay/ChiakiBackend.cpp:146-149,256-257`). VERBOSE also hexdumps every Takion data message
(`streamconnection.c:685-686`), so it is not a cheap always-on source. **Recommendation:** a small patch
(`0005`, stream "net") that hands the decoded payload to a callback (all six fields + receive timestamp), so SmartLink
gets the console's RTT, loss and target rate without VERBOSE logging. The interval then becomes measurable.

**`adaptive_stream_index` and stream profiles.** Every video packet carries a 3-bit index (`av[2] >> 5`,
`lib/src/takion.c:1768`, v7 at `:1906`) into the list of resolutions/headers the console announced in STREAMINFO
(`chiaki_video_receiver_stream_info`, `lib/src/videoreceiver.c:93-110`). On a change chiaki-ng logs "Switched to
profile", sends the new header to the decoder callback and re-parses it (`videoreceiver.c:126-143`). The client asks
for one resolution and `adaptiveStreamMode: resize`; the console decides when to switch. The client has no message
to choose an index. It can only influence the choice indirectly through what it reports (congestion packets and
corrupt-frame reports) and through the start bitrate.

**The 200 ms congestion report.** A thread wakes every 200 ms (`lib/src/congestioncontrol.c:5,18`), reads and
resets the packet statistics (`:24`) and sends `{received, lost}` as 16-bit counts (`:35-39`). Loss is measured as:
for each completed video frame, expected units (source + FEC) minus received units (`frameprocessor.c:199-204`),
plus a sequence-gap estimate for audio (`lib/src/packetstats.c:51-75`). Frames of which not a single packet arrived
are not counted here (they surface only as a corrupt-frame report). `packet_loss_max` clamps the reported fraction:
if measured loss exceeds it, `lost` is rewritten to `total x packet_loss_max` (`congestioncontrol.c:28-34`). The
product uses 0.05. Effect: the console never sees more than 5 % loss, so it is kept from reacting to short bursts as
hard as it otherwise would (presumably by lowering its target rate or changing profile; the console's reaction is
**not verified** — it is Sony's closed encoder logic).

**What chiaki-ng's own settings expose:** resolution preset, FPS (30/60), bitrate, codec (H.264/HEVC/HDR),
`packet_loss_max`, `enable_idr_on_fec_failure`, `video_profile_auto_downgrade` (`lib/include/chiaki/session.h:76-92`).
All are fixed at connect time.

**Conclusion: live bitrate change — not supported by any known client message; only indirectly** (the console adapts
on its own from the congestion report and corrupt-frame reports; the client can bias that only by what it reports).

Safest supported alternatives, ranked:

| Alternative | How | Risk |
|---|---|---|
| Choose the next session's start rate from measured link quality (`suggestedStartKbps`) | Record per-route the achieved rate vs requested, late/lost packets, RTT spread; suggest a rate for the next start; the user (or "Automatic") accepts it | Low. Changes nothing mid-session. Risk is only a too-low suggestion lowering picture quality: cap the step, never go below the measured sustained rate |
| Leave the in-session adaptation to the console and observe it | Read `target_bitrate` and the profile index live (patch 0005) and show "network-limited" when the console has reduced the rate | None (read-only) |
| Reconnect with another rate | Tear down and restart with a lower/higher `bwKbpsSent` | High: several seconds of black picture, a fresh Senkusha/handshake, possible "Remote Play already in use"; only on explicit user request, never automatic during play |
| Shape what is reported (`packet_loss_max`) | Lower the clamp so the console reacts less to Wi-Fi bursts, or raise it so it reacts more | Medium: under-reporting real congestion keeps the console sending too much, which turns loss into queueing delay and more loss. The value is fixed per session (`congestioncontrol.c:46-50`); a live change would need a patch. Not recommended without a hardware A/B |
| Debug `BITRATE` / `FPSCHANGE` messages | Send an undocumented message | Unknown; possible disconnect. Research only, opt-in, never shipped by default |

### A2. Loss recovery

- **FEC.** Per frame, the header gives `units_in_frame_total` and `units_in_frame_fec` (video: 11-bit total-1 and 10-bit
  FEC count, `takion.c:1745-1747`; audio packs source and FEC unit counts into nibbles, `lib/include/chiaki/takion.h:56-57`).
  The **console chooses the ratio**; the client cannot request one. The client sees it per frame
  (`frame_processor->units_fec_expected`, `frameprocessor.c:76-79`) — a cheap SmartLink signal ("FEC share" and
  "units recovered by FEC"). Decoding is Reed-Solomon-style erasure decoding over the units (`lib/src/fec.c`,
  `chiaki_fec_decode`, called at `frameprocessor.c:237-245`). A frame is flushed as soon as source + FEC units
  received >= source units expected (`lib/include/chiaki/frameprocessor.h:67-71`, `videoreceiver.c:181-187`), or when
  the first packet of the next frame arrives (`videoreceiver.c:145-158`).
- **Retransmission.** None for audio/video. Only Takion *data* (control) messages are resent: 200 ms timeout, 25 tries,
  16-entry buffer (`lib/src/takionsendbuffer.c:12-15`; upstream later raised the buffer to 1024, section B).
- **Reorder queue.** Video packets go through a 64-entry reorder queue (`takion.c:46`); audio bypasses it and is
  delivered immediately (`takion.c:1655-1667`). When the head is missing the queue waits
  `TAKION_AV_REORDER_TIMEOUT_US` = 16 ms (`takion.c:47`), then skips to the first buffered packet; a burst that keeps
  advancing within one gap shares one timeout budget (`takion.c:937-1024`). The receive loop wakes for the
  remaining wait (`takion.c:1035-1058,1138-1151`). The product's patch 0004 makes this wait configurable and live
  (`chiaki_veyra_transport_set_av_reorder_timeout_live`) and adds counters for gaps, timeouts, skipped packets, late
  arrivals and lateness buckets (`scripts/remoteplay/patches/0004-chiaki-transport-tuning.patch`).
  Note: on a link that does not reorder, every wait is pure added delay for a lost packet; on one that does, a short
  wait turns reordering into loss.
- **Missing frames / units.** If a frame index jumps, chiaki-ng sends CORRUPTFRAME(start,end) for the missing range
  (`videoreceiver.c:160-168`). If FEC fails, it sends CORRUPTFRAME again and, with `enable_idr_on_fec_failure`, an
  IDRREQUEST; then it **skips P-frames until an I-frame arrives** (`videoreceiver.c:197-235,247-261`). With the option
  off, a frame with failed FEC is not passed to the decoder (`succ=false`, `:243`), so the decoder never sees a
  damaged frame. For a P-frame whose reference is missing, chiaki-ng rewrites the slice header to point at an older
  frame it does have (up to 16 back) and marks it `recovered` (`videoreceiver.c:264-292`); if none exists the frame is
  dropped and counted lost. The console's answer to CORRUPTFRAME is presumably a recovery (intra-refresh/reference)
  frame; chiaki-ng handles an incoming CORRUPTFRAME only by logging (`streamconnection.c:709-712`). Missing units
  inside a frame that FEC cannot rebuild therefore mean: frame dropped (never decoded with errors), plus a request.
  Pinned weakness fixed upstream: if the IDR request or the IDR itself is lost, the picture stays frozen (section B,
  `8134f45`).

### A3. Round-trip time available to the client

| Source | When | Meaning, units |
|---|---|---|
| Senkusha echo test | Once before the stream: 10 pings, average (`lib/src/senkusha.c:34,252,385`); fallback 1 ms if Senkusha is skipped (`lib/src/session.c:642-644`) | Application-level UDP echo, microseconds; sent to the console in the launch spec (ms). Patch 0004 exposes it as `CHIAKI_VEYRA_STAT_SENKUSHA_RTT_US` |
| CONNECTIONQUALITY `rtt` | Live, console-chosen interval (unknown) | The console's own estimate, double, unit not verified. Only live RTT available |
| Takion data acks | Live, but only for reliable control messages (heartbeats, feedback history), resend timer 200 ms | Could be timed by a patch (send time -> ack); not done in chiaki-ng; resolution limited by how often reliable data is sent. Not verified as useful |
| Video timing | Live | Not an RTT: one-way arrival jitter (inter-arrival vs frame cadence) is what the client can measure itself, and it is what GCC-style estimators use (section E) |

### A4. Audio

Codec Opus (`packet->codec == 5`, `lib/src/audioreceiver.c:87-91`); rate, channels and frame size come from the
console's audio header (`lib/src/audio.c:15-21`); 10 ms (480 samples at 48 kHz) is typical, **not verified** from
the source (docs/AUDIO.md says the same). Each audio packet carries N source units plus M redundancy units, where the
redundancy units are **copies of earlier frames** (frame index = `packet->frame_index - fec_units_count + i`,
`audioreceiver.c:121-135`): packet-level redundancy, not Opus in-band FEC. The decoder is called with `decode_fec=0`
(`lib/src/opusdecoder.c:95`), so Opus LBRR data, if the console encodes any, is not used (not verified whether it
does). Jitter buffer: 8 frames, playback starts after 3 (`audioreceiver.c:9-10,188-198`); a missing frame is
concealed by Opus PLC (`opus_decode` with a NULL buffer, `opusdecoder.c:94-95`) once 3 newer frames are buffered
(`audioreceiver.c:217-246`). There is no clock-drift handling or time-stretching in chiaki-ng; the product's
`AudioLatencyController` / WASAPI sink handles latency (docs/AUDIO.md).

### A5. Constrained by the PS5 protocol vs. what we can improve independently

| Area | Constrained by the protocol (console decides) | We can improve on the client |
|---|---|---|
| Bitrate | In-session rate and profile switch; FEC overhead included | Start rate for the next session; what we report (clamp); showing "network-limited" |
| Resolution / FPS | Fixed at launch; console may switch announced profile | Choose at launch from link class |
| FEC | Ratio per frame | Read it; measure recovered vs failed frames |
| Retransmission | None for A/V | — (only control messages are resent) |
| Reorder wait | — | Fully ours (patch 0004): adaptive 2/4/8/16 ms from measured lateness |
| Jitter buffering (video) | Console send pacing | Ours: decode/present queue depth, pacing |
| Audio buffering / concealment | Packet redundancy layout | Ours: prefill, target delay, PLC, time-stretch |
| Frame concealment | Recovery frame after CORRUPTFRAME / IDR | Display-side: repeat last frame or (offline research only) extrapolate; faster IDR re-request |
| Pacing | Encoder cadence | Present timing, VRR, latency governor |
| Input rate | Console consumes feedback packets | Feedback send cadence (8-200 ms, `lib/src/feedbacksender.c:8-9`) and history resends |

## B. chiaki-ng library changes after the pinned commit

The newer clone (`/home/claude/refsrc/streetpea_chiaki-ng`, HEAD `43728af`, 3 Nov 2026) contains the pinned commit;
`git diff 0e16950..HEAD --stat -- lib` touches 25 files (+1682/-157), mostly certificate (AIA), PSN holepunch/STUN
and HTTP hardening. Commits that touch networking, audio or recovery (`git log 0e16950..HEAD -- lib`):

| Commit (date) | What | Adopt? | Risk |
|---|---|---|---|
| `8134f45` (30 Oct) | While waiting for a requested IDR, re-send IDRREQUEST once a second (`VIDEO_IDR_RETRY_INTERVAL_MS 1000`); before, a lost request or lost IDR froze the picture with audio running | **Yes, first.** The product runs with `enable_idr_on_fec_failure=true`, so it has exactly this freeze path on Wi-Fi | Low: one extra small reliable message per second only while frozen. 1 s is long for a game; SmartLink could use a shorter retry (e.g. 2-3 x RTT, bounded), not verified on hardware |
| `3e1a24d` (30 Oct) | Reliable Takion send buffer 16 -> 1024 entries | Yes | Low; memory only. After a stall, control messages (feedback, IDR/corrupt reports) are no longer lost untracked |
| `727917c` (30 Oct) | Reorder queue: mark dropped entries unset (use-after-free fix) | Yes (correctness) | Very low. Check patch 0004, which uses the queue's drop path |
| `e5f1dd8` (22 Oct) | Do not push into the Takion send buffer after the receive loop ended | Yes (crash fix at disconnect) | Low |
| `bc397a7`, `c938844`, `bfbb296`, `4382c1c` (Oct) | Feedback history: one event per packet, numbered, last packet resent, header count fixed; fixes buttons staying dead after a lost history packet | Compare with product patch `0003-chiaki-feedback-history.patch`, which already sends one event per packet; whether it also covers the numbering/resend fixes is **not verified** | Medium: input path; needs a hardware button test |
| `0212445` (30 Oct) | Heartbeat replies on a timer over PSN | Only for remote (PSN) play | Low |
| `15098e4` (2 Nov) | Microphone: constant 32 kbit/s Opus, each packet carries the new frame plus the two before (redundancy) | Only if the microphone is used | Low |

Nothing in the newer clone adds a live bitrate control, reads CONNECTIONQUALITY beyond logging, changes the 16 ms
reorder wait or the audio jitter buffer (`git diff 0e16950..HEAD -- lib/src/streamconnection.c lib/src/audioreceiver.c`
is empty in the stat above).

## C. Moonlight (moonlight-common-c `94c65fd`, moonlight-qt `28629dc`)

Read with `git show HEAD:<path>` in `/home/claude/refsrc/moonlight-stream_*`.

- **Video FEC and loss reports.** Each frame is split into Reed-Solomon blocks; the host announces the FEC percentage
  per packet (`src/RtpVideoQueue.c:704-711`). The client reports per-frame FEC status (data/parity received, missing
  before highest, FEC percentage) to the host (`reportFinalFrameFecStatus`, `RtpVideoQueue.c:89-105`) and a loss
  report every 50 ms (`src/ControlStream.c:316,1660-1701`).
- **Speculative reference-frame invalidation (most transferable idea).** If the number of packets already missing in
  a frame exceeds the parity available, the frame cannot be rebuilt, so the client reports it lost immediately instead
  of waiting for a timeout (`RtpVideoQueue.c:205-227`). This is done only while no out-of-order packets have been seen
  from this host for a cooldown (`SPECULATIVE_RFI_COOLDOWN_PERIOD_US` = 300 s, `:15,161-176`); any out-of-order packet
  turns it off, and a wrong prediction also turns it off (`:228-240`). **Transfers to PS5:** the client knows
  `units_in_frame_total` / `units_in_frame_fec` for every frame (A2), so "missing units > FEC units" can be decided
  as soon as the gap is known, and on a link that has shown no reordering the reorder wait for that frame can be cut
  to zero and CORRUPTFRAME sent at once. Model idea; must be shown on hardware (recovery time, not more recovery frames).
- **IDR vs reference invalidation.** The depacketizer drops frames until an IDR or a reference-invalidated frame
  (`src/VideoDepacketizer.c:95-160,544-590`). The PS5 has no exposed RFI message; CORRUPTFRAME is the analogue.
- **Connection status.** Frame loss per sample period: "poor" at >= 30 % in one period or >= 15 % in two consecutive,
  "okay" again at <= 5 % (`ControlStream.c:140-142,519-534`). A simple, hysteretic two-threshold classifier: a good
  template for SmartLink's `Profile`/`Regime` hysteresis.
- **Audio.** Audio RTP queue with its own RS FEC (4 data + 2 parity shards), 10 ms out-of-sequence wait
  (`src/RtpAudioQueue.h:11-18`); a missing packet is passed to the decoder as NULL, i.e. Opus PLC (`src/AudioStream.c:167`).
  moonlight-qt keeps at most about 50 ms in SDL's output queue, waiting (up to 100 x 1 ms) before queuing more
  (`app/streaming/audio/renderers/sdlaud.cpp:112-125`), and drops samples for a window after a slow audio reinit
  (`app/streaming/audio/audio.cpp:168-178,243-251`). No time-stretching: latency is bounded by the cap, not by rate control.
- **Frame pacing.** The pacer keeps at most 3 queued frames (`app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp:22`);
  on each vsync it drops down to 1 queued frame, or tolerates up to 3 if the queue length was at most 1 at any point in
  the last 500 ms of history (`pacer.cpp:241-275`). Transfers directly (display side, protocol independent): drop excess
  frames only when the queue stays long, not on one burst.
- **What does not transfer:** Moonlight's RTSP/ENet control protocol, its host FEC negotiation, LTR acks and RFI
  messages, and its host-side bitrate settings: the PS5 offers none of these.

## D. Sunshine (`23faefde`, host side; explains host behaviour only, not transferable as code)

- FEC percentage is a host setting (default 20 %, `src/config.cpp:817`, range 0-100 `:1866`), applied per frame in
  up to 4 FEC blocks; FEC is skipped for abnormally large frames (`src/stream.cpp:1685-1712`). The client's requested
  bitrate is reduced by the FEC share, audio and ~500 kbit/s overhead before it reaches the encoder
  (`src/rtsp.cpp:1207-1230`). If the PS5 does something similar (not verified), a 15 Mbit/s request would encode
  noticeably below 15 Mbit/s, and a static scene encodes far below the cap anyway (BENCH1: ~4-5 Mbit/s in a menu).
- Pacing: Sunshine spreads a frame's packets at about 80 % of 1 Gbit/s in 1 ms groups and sends in batches under
  64 KB (`src/stream.cpp:1736-1760,1846-1862`). Large keyframes therefore still arrive as a dense burst; on Wi-Fi a
  burst of this kind is where queueing and loss concentrate. The PS5's own pacing is unknown (not verified).
- Loss statistics from the client are only logged by Sunshine (`stream.cpp:1205-1219`); IDR and reference-invalidation
  requests drive the encoder (`:1221-1235`). Suggests (by analogy only) that a host's reaction to loss reports may be
  limited to recovery frames, which is consistent with the PS5 still having its own CONNECTIONQUALITY `target_bitrate`.

## E. WebRTC: reusable algorithms (BSD-3-Clause + PATENTS grant)

| Piece | What it does | Smallest piece worth reusing here |
|---|---|---|
| GCC delay-based estimator ([draft-ietf-rmcat-gcc-02](https://www.ietf.org/proceedings/96/id/draft-ietf-rmcat-gcc-02.txt) §5; [trendline_estimator.cc](https://webrtc.googlesource.com/src/+/main/modules/congestion_controller/goog_cc/trendline_estimator.cc)) | Groups packets sent within 5 ms (burst merging after an outage, §5.2); per group, one-way delay variation d = inter-arrival - inter-departure; the draft filters it with a scalar Kalman filter (§5.3), current WebRTC with an exponentially smoothed accumulated delay (coefficient 0.9) and a least-squares slope over a window; over-use when the scaled trend stays above an adaptive threshold (initial 12.5, clamped 6-600, k_up 0.0087 / k_down 0.039 in code; K_u 0.01 / K_d 0.00018 in the draft; no update when the excess is > 15 ms) | **The trendline slope + adaptive threshold as a "queue is building" detector** on video packet arrivals. Without send timestamps, use the frame cadence (frame index / 60 Hz, or the stream's measured rate) as the departure clock. This is receive-only and cheap. It cannot change the console's rate, but it can feed `Regime=Congested` and `networkLimited`. Model idea; needs hardware validation |
| GCC loss-based controller (draft §6) | < 2 % loss: increase; 2-10 %: hold; > 10 %: decrease by (1 - 0.5 p) | Thresholds as **reference points for `suggestedStartKbps`** for the next session |
| Video jitter estimator ([jitter_estimator.cc](https://webrtc.googlesource.com/src/+/main/video/timing/jitter_estimator.cc)) | Kalman filter of frame delay variation vs frame-size change, plus a noise term; estimate = size-based part + 2.33 x noise std dev - 30 ms (floor 1 ms); outliers above 15 std devs fed back as clamped; frames shrinking sharply treated as congestion samples; adds 10 ms "OS jitter" | **Separating size-caused delay (keyframes, big frames take longer to arrive) from network noise.** For SmartLink this means: do not widen buffers because of keyframe arrival time; widen only on unexplained (noise) delay. The 2.33 sigma (~99th percentile) rule is the right shape for a "late frame" threshold |
| NetEQ ([g3doc](https://webrtc.googlesource.com/src/+/HEAD/modules/audio_coding/neteq/g3doc/index.md); [delay_manager.cc](https://webrtc.googlesource.com/src/+/main/modules/audio_coding/neteq/delay_manager.cc)) | Target delay from packet inter-arrival statistics measured in playout ticks (so clock drift is included); an "underrun optimizer" picks a quantile of the relative arrival delay with a forgetting factor (start target 80 ms, `kStartDelayMs`); reordering can only raise the target; operations Normal / Accelerate / Preemptive expand (time-stretch when the filtered buffer level is above/below target) / Expand (PLC) / Merge | **Quantile-with-forgetting target delay + time-stretch to reach it** (instead of dropping or inserting silence). This is the standard answer to "audio latency must not accumulate" and "pops after a stall". Quantile and forget-factor defaults are in the field-trial config, **not verified** here. Stream "audio" owns the implementation |
| Opus decoder ([API](https://opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html)) | NULL packet = PLC; `decode_fec=1` uses in-band FEC from the *next* packet; PLC/FEC frame size must be a multiple of 2.5 ms | Only PLC is usable (A4: the console's in-band FEC use is unknown). Use the Merge idea: cross-fade the first real frame after concealment |

Licence: WebRTC is BSD-3-Clause with an additional patent grant (header of each file above); compatible with GPL-3.0;
reuse would need the BSD notice in `THIRD_PARTY_NOTICES.md`. Re-implementing the algorithm in our own words from the
IETF draft needs no notice (the draft is an IETF document; cite it).

## F. Research (2019+) relevant to a client-only receiver

| Work | Finding | Usable by a client-only receiver? |
|---|---|---|
| Guo et al., *BLADE: Adaptive Wi-Fi Contention Control for Next-Generation Real-Time Communication*, NSDI 2026 ([page](https://www.usenix.org/conference/nsdi26/presentation/guo-fengqian)) | Wi-Fi tail latency in cloud gaming comes from short packet-delivery droughts caused by contention control; adapting contention windows cut tail latency > 5x and stalls > 90 % (their tests) | No (changes Wi-Fi MAC behaviour of transmitters). Supports the model: spikes are short droughts followed by bursts, not loss |
| *Zhuge* (Meng et al.), SIGCOMM 2022 ([pdf](https://chensun-me.github.io/papers/zhuge-sigcomm22.pdf), [summary](https://blog.apnic.net/2022/11/01/reducing-the-control-loop-in-congestion-control/)) | Predicts queueing at the Wi-Fi AP and shortens the feedback loop so the sender reacts before the queue grows | No (runs on the AP). Shows that sender-side reaction alone is too slow for Wi-Fi spikes: a receiver can mostly only absorb them |
| Wang et al., *Pudica*, NSDI 2024 ([pdf](https://www.usenix.org/system/files/nsdi24-wang-shibo.pdf)) | Cloud-gaming congestion control that keeps the bottleneck queue near empty using frame delays and probing; lower average and tail frame delay | Server-side. The client part is only per-frame feedback (which the PS5 protocol does not carry) |
| Meng et al., *Hairpin*, NSDI 2024 ([page](https://usenix.org/conference/nsdi24/presentation/meng)) | Deadline-aware mix of FEC and retransmission; ~40 % less bandwidth, ~32 % fewer deadline misses (their production numbers) | No (needs sender retransmission; PS5 A/V has none) |
| Cheng et al., *GRACE*, NSDI 2024 ([arXiv](https://arxiv.org/pdf/2305.12333)) | Neural codec trained to decode with missing packets | No (needs a different encoder) |
| Rudow et al., *Tambur*, NSDI 2023 ([page](https://usenix.org/conference/nsdi23/presentation/rudow)) | Streaming codes recover bursty loss with less redundancy than per-frame FEC | No (sender side) |
| Xu & Claypool, *Frame Display Time Variation*, MMSys 2024 ([pdf](https://web.cs.wpi.edu/~claypool/papers/frame-variation-mmsys-24/paper.pdf)) | Player QoE is explained mostly by the standard deviation of frame display time (adj. R^2 0.92); size of an interruption matters more than how often | **Yes**: the metric to optimise and report is frame-time spread / largest gap, not mean latency. Supports trading a few ms of buffer for fewer large gaps only on links that have large gaps |
| Lindström, Wetterberg, Carlsson, UCC 2020 ([pdf](https://www.ida.liu.se/~nikca89/papers/ucc20.pdf)) | Latency hurt players more than loss; frame-age spikes cause decode backlogs that hurt after the spike | **Yes**: after a burst, drain backlog (drop to the newest frame) rather than play every late frame |
| Hirose et al., *IFRVP*, 2025 ([arXiv](https://arxiv.org/pdf/2503.23185v1)) | Receiver-side next-frame prediction from the last two frames, 70-130 fps at small sizes on a desktop GPU | Research only (directive §3): low resolution, prediction errors; never shown as game state |

Not found: a peer-reviewed study that predicts Wi-Fi latency spikes from client-side signals far enough ahead to act
(seconds). Treat "prediction" claims accordingly (section I).

## G. Wi-Fi on Windows

**Documented causes of periodic spikes.** Windows' WLAN service and drivers scan for networks in the background
while connected; Microsoft documents a per-client switch for it: "Background scan can only be disabled when the
interface is in the connected state", it is disabled "if at least one client disables it" and re-enabled
automatically after a disconnect (`wlan_intf_opcode_background_scan_enabled`,
[WLAN_INTF_OPCODE](https://learn.microsoft.com/en-us/windows/win32/api/wlanapi/ne-wlanapi-wlan_intf_opcode)); the same page
documents `wlan_intf_opcode_media_streaming_mode` (enabled while at least one client asks). Scans the OS makes on its
own are described under *Implicit Scan Operations*
([driver docs](https://learn.microsoft.com/it-it/windows-hardware/drivers/network/implicit-scan-operations); content
not read in full: not verified). Adapter-vendor forums report periodic ping spikes on Intel AX2xx adapters
([Intel community](https://community.intel.com/t5/Wireless/AX210-Periodic-Lag-Spike/m-p/1567109)) — user reports, not
vendor documentation. A program that queries Wi-Fi scan results itself (`WlanScan`, `WlanGetAvailableNetworkList`)
also triggers scans: SmartLink must never call them.

**Signals a normal program can read** (no administrator rights are documented as needed;
[WlanQueryInterface](https://learn.microsoft.com/windows/win32/api/wlanapi/nf-wlanapi-wlanqueryinterface)):
`WlanOpenHandle` -> `WlanEnumInterfaces` -> `WlanQueryInterface` with
`wlan_intf_opcode_rssi` (LONG, dBm), `wlan_intf_opcode_channel_number` (ULONG), `wlan_intf_opcode_statistics`
(`WLAN_STATISTICS`: driver counters), `wlan_intf_opcode_realtime_connection_quality`
(`WLAN_REALTIME_CONNECTION_QUALITY`: `dot11PhyType`, `ulLinkQuality` 0-100 mapping linearly to -100..-50 dBm,
`ulRxRate`, `ulTxRate`, MLO link count and per-link info;
[struct](https://learn.microsoft.com/en-us/windows/win32/api/wlanapi/ns-wlanapi-wlan_realtime_connection_quality);
minimum Windows version not stated there: not verified, probably recent Windows 11 only), and
`wlan_intf_opcode_current_connection` (`WLAN_CONNECTION_ATTRIBUTES`: signal quality, PHY type, rx/tx rate, but also the
network name and BSSID). Since Windows 11 24H2, `WlanQueryInterface` with `current_connection` and the scan functions
need location consent and return `ERROR_ACCESS_DENIED` without it
([Wi-Fi access and location changes](https://learn.microsoft.com/en-us/windows/win32/nativewifi/wi-fi-access-location-changes)).
**Use `realtime_connection_quality` (or `rssi` + `channel_number`) and never `current_connection`**: it avoids the
location prompt and never touches the network name/BSSID (privacy rule). Fall back to "signal unknown" (negative value).
Wired vs Wi-Fi: the product already has a route class (`LinkKind` in `ConnectionHealth.h`).

**Remedies that are system or adapter settings — advice for the owner only, never applied by the program:**
use Ethernet (the documented best case); move the PC to 5/6 GHz and closer to the access point; adapter driver
"roaming aggressiveness" / power saving (vendor settings; vendor docs not checked: not verified). Disabling background
scan or enabling media streaming mode via `WlanSetInterface` is a documented per-client, self-reverting API, but it
changes Wi-Fi behaviour for the whole system while set; under the brief's rule ("never change system settings") it is
listed here as **advice / a separate owner decision**, not a SmartLink action.
Precedent: moonlight-common-c turns `wlan_intf_opcode_media_streaming_mode` on for every Wi-Fi interface during a
stream (`src/PlatformSockets.c:933-975`, `git show HEAD:`), and Sunshine does the same on the host
(`src/platform/windows/misc.cpp:1199-1256`). Microsoft's page does not say what the mode changes inside the driver
(not verified; commonly understood to reduce background scanning and power saving). If the owner wants it, it should be
a separate, visible, off-by-default switch with its own hardware A/B, outside SmartLink's "detect and advise" scope.

## H. Licences

| Source | Licence | Reuse in this GPL-3.0 program | Notice needed |
|---|---|---|---|
| chiaki-ng (lib) | AGPL-3.0-only with OpenSSL exception (`SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL`, e.g. `lib/src/congestioncontrol.c:1`) | Yes, in the Chiaki-facing part (already AGPL-3.0); upstream commits can be cherry-picked as patches | Keep headers; patch files under `scripts/remoteplay/patches`; corresponding source zip (already shipped) |
| moonlight-common-c | GPL-3.0 (`LICENSE.txt`) | Yes (GPL-3.0 compatible; into the AGPL part too, GPL-3/AGPL-3 combination is permitted by both) | Copyright notice + record in `THIRD_PARTY_NOTICES.md`; `nanors` RS library has its own licence (not checked: not verified) |
| moonlight-qt | GPL-3.0 (`LICENSE`) | Yes | As above |
| Sunshine | GPL-3.0 (`LICENSE`) | Yes, but nothing worth copying (host side) | As above |
| WebRTC (trendline, jitter estimator, NetEQ) | BSD-3-Clause + PATENTS grant | Yes | BSD notice in `THIRD_PARTY_NOTICES.md` if code is copied; none if only the algorithm is re-implemented |
| IETF draft-ietf-rmcat-gcc | IETF Trust document | Algorithm reuse: yes | Cite |
| Opus (libopus) | BSD-3-Clause | Already used via chiaki-ng | Existing |
| Papers (section F) | Copyright of the authors | Ideas only, no code | Cite |
| Microsoft WLAN API docs | Documentation | Calls only | None |

## I. Recommendations for SmartLink

Ranked separately for the two goals. Evidence levels: **measured by others** (published or upstream-reported),
**model** (our simulator or code reading shows a mechanism), **idea**. None is measured on this product.

### I1. Lowest latency on wired

| # | Recommendation | Evidence | Component | Hardware run must show |
|---|---|---|---|---|
| 1 | On a link with no reordering seen, use the shortest reorder wait (2 ms), and decide "frame unrecoverable" as soon as missing units > FEC units; then skip the wait and send CORRUPTFRAME at once (Moonlight's speculative RFI with its out-of-order cooldown) | measured by others (Moonlight in production); model for PS5 | `SmartLinkController` -> `reorderWindowMs`; stream "net" (patch hook for FEC counts) | Wired: zero late-arrival packets at 2 ms over a long session; time from first missing unit to recovered picture shorter than with 16 ms; no rise in recovery frames requested |
| 2 | Everything else "leave" on a clean link: no added audio target, no buffer, no rate change | model (brief's ZERO-delay rule) | `SmartLinkController` | Wired-clean: SmartLink apply vs off gives identical `pushfpsMs`, present-to-scanout and audio delay within noise |
| 3 | Adopt upstream `8134f45` (IDR re-request) + `3e1a24d` + `727917c` + `e5f1dd8` | measured by others (upstream bug reports); code reading | chiaki patch set (stream "net") | A forced loss burst never leaves the picture frozen > 1 s; no crash at disconnect |
| 4 | Read CONNECTIONQUALITY via a callback (patch 0005) instead of VERBOSE logging; log `rttMs`, `targetKbps` | code reading | `Sample` fields; stream "net" | Interval of the message; agreement of its `rtt` with Senkusha RTT (gives the unit) |

### I2. Stability on Wi-Fi

| # | Recommendation | Evidence | Component | Hardware run must show |
|---|---|---|---|---|
| 1 | Widen the reorder wait only from measured lateness (patch 0004 lateness buckets): pick the smallest of 2/4/8/16 ms that would have caught ~99 % of late packets over the last seconds; widen fast, narrow slowly | model (existing `ReorderStudy`/`ReorderQueueModel`); shape from WebRTC jitter estimator (2.33 sigma) | `SmartLinkController.reorderWindowMs` | Fewer reorder timeouts and skipped packets per minute at an added wait whose P95 is below one frame |
| 2 | Audio: quantile-with-forgetting target delay plus time-stretch (NetEQ) rather than fixed prefill; cross-fade after PLC; never let the target grow without a measured reason, decay it back | measured by others (NetEQ in WebRTC); model | `audioTargetMs`; stream "audio" | Audio gaps/pops per minute down, audio delay P95 not growing over 30 min |
| 3 | After a stall, present the newest frame and drop the backlog (do not replay late frames); keep the pacer tolerant to one burst (Moonlight's 500 ms queue history) | measured by others (UCC 2020 finding; Moonlight pacer) | governor/present path (stream "control") | Shorter time-to-live-picture after a gap; no rise in frame-time standard deviation |
| 4 | Report `networkLimited` when stutter coincides with arrival gaps (not GPU), so graphics are not lowered for a network problem; report `linkHeadroom` when measured rate and arrival spread allow the requested rate | model | `networkLimited`, `linkHeadroom` -> governor | On Wi-Fi spikes the governor does not lower upscaling/frame generation; on a clean link it does not either |
| 5 | Next-session start rate (`suggestedStartKbps`) from sustained rate, late/lost share and RTT spread, using GCC loss thresholds (< 2 % fine, > 10 % too high) as reference points; the user accepts it | measured by others (GCC thresholds) as reference; idea for PS5 | `suggestedStartKbps`, `SmartLinkProfile` | A run at the suggested rate has fewer stutter events/min than at 15 Mbit/s, with image quality judged by the owner |
| 6 | Read signal (`realtime_connection_quality` or `rssi`) once per second, only as a profile hint ("Weak signal" advice), never as a trigger for buffer changes alone | idea | `SmartLinkProfile`, advice ids | Correlation of low signal with arrival gaps on the owner's PC |
| 7 | Advise (do not apply) Ethernet / 5-6 GHz / closer AP when `Regime=PeriodicSpikes` or `WeakSignal` persists | documented (section G) | advice ids (stream "auto") | Advice appears only on the unstable profile, never on wired |

### I3. Can instability be predicted early enough to act?

- **What the signals support.** The client sees: per-packet arrival times, per-frame unit/FEC counts, reorder lateness,
  CONNECTIONQUALITY (console RTT/loss/target, interval unknown), and Wi-Fi signal/rate (1 s polling). None of these
  gives seconds of warning for a single Wi-Fi drought: the literature locates the cause in MAC contention and AP
  queues (BLADE, Zhuge), which the client cannot observe, and the fixes that work act at the AP or sender.
- **What is predictable** is the *regime*, not the next spike: periodic spikes (e.g. background scanning) recur with a
  stable period, weak signal persists, congestion shows a rising one-way delay trend (GCC trendline) for hundreds of ms
  before loss. Simple statistics catch these: inter-arrival quantiles with forgetting, the trendline slope with an
  adaptive threshold, an autocorrelation/period check of gap times, and a hysteretic two-threshold classifier
  (Moonlight). Acting on the regime (keep a wider reorder/audio margin while it lasts) is what SmartLink can do.
- **Learned model.** Justified only if, on the simulator's labelled traces *and* on recorded hardware traces, it beats
  the statistics on stall-risk prediction (e.g. precision at equal recall) by a margin larger than run-to-run noise,
  at negligible CPU. With the few, coarse features available and the absence of hardware traces, the expected
  benefit is small; a tiny logistic model over the same features is the most that is reasonable. Recommendation:
  ship statistics; keep the learned model as a simulator comparison until hardware traces show a gain.
