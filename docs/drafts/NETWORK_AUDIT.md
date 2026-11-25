# PS5 stream network and receive path: audit (stream "network", 21 Nov 2026)

Branch `exp/s5-network` from Package 8C1 (`exp/rel-8c`, 10430c57). Internal codename Slipstream. Answers the owner's
eleventh, twelfth and thirteenth messages and sections 2-9 and 14-16 of his master directive, for the network,
packet-recovery and decoder parts. Picture-quality root causes beyond the network are stream "clarity"'s
(`docs/CLARITY_VERDICT.md` on `exp/s5-clarity`); frame-generation pacing is stream "smooth"'s.

**Tags.** [code] = read in our source at 10430c57 (file, function). [chiaki] = read in the pinned chiaki-ng library,
commit `0e16950165f06e5c3291537c2eeba6e852be7120` (paths `lib/src/...`, browsable at
https://github.com/streetpea/chiaki-ng/tree/0e16950165f06e5c3291537c2eeba6e852be7120/lib/src), plus our patches
`scripts/remoteplay/patches/0001-0010`. [published] = public document, linked. [estimate] = our arithmetic.
[unknown] = not known; the log line that settles it is named. [measured-on-owner-PC] = his BENCH1 / BENCH2 logs
(Wi-Fi, one setup, software counters). **Nothing here was run on Windows, a GPU or a PS5 by this stream.**

Status: sections 1, 2, 4-7 are the audit (code reading). Sections 3, 8 and 9 describe what this branch then built
(`[slipstream-net]`, the dominant-cause rule, the Network Diagnostics panel, the kit rows); section 11 is the Linux
impairment model's result; section 12 the answers on blur and what his PC must measure. Built = compiled by g++ / MSVC
in CI for the portable parts only; the Windows wiring is blind code until a full Windows round builds it.

## 0. Short answers

1. **Are packets actually missing?** Not known for his wired LAN. His only logs are Wi-Fi. Every counter needed to
   tell exists or is added by this stream (section 3): `[slipstream-net]` separates packets that never arrived, packets
   that arrived too late and were given up, frames FEC repaired, frames lost, and queue drops in our own code.
2. **Can network loss make the picture blurry?** Not directly [code][chiaki]. A frame the library cannot complete is
   never decoded: it is dropped, a corrupt-frame report and a keyframe request go to the console, and our ingress
   discards every following frame until the keyframe (`videoreceiver.c` `chiaki_video_receiver_flush_frame`;
   `VideoIngress::push`, `recoverLocked`). Loss shows as a **frozen or skipped picture**, not as smear. Two indirect
   routes from the network to softness exist and are now observable: (a) the console lowers its encoder bitrate when
   it is told about loss (`congestioncontrol.c`, every 200 ms; its own target is `consoleTargetKbps`); (b) the console
   may switch to a smaller announced resolution (`adaptiveStreamMode: resize` in `launchspec.c`; profile switch in
   `videoreceiver.c`) [chiaki]. Whether a PS5 does either on a clean LAN is [unknown] (fields `consoleTargetKbps`,
   `resolution`, `resolutionChanges` of `[slipstream-net]`).
3. **Is FEC working as well as it can?** Yes in its mechanism [chiaki]: Reed-Solomon (Cauchy, jerasure) over the
   units of one frame, attempted the moment as many units as source units have arrived, no extra wait
   (`frameprocessor.h` `chiaki_frame_processor_flush_possible`, `frameprocessor.c` `chiaki_frame_processor_fec`).
   The FEC ratio is the console's choice per frame (`units_in_frame_fec`), a client cannot change it. The only wait
   in front of FEC is the reorder queue (16 ms upstream; switches exist). How often FEC repairs a frame on his LAN is
   [unknown]: `fecRecovered`, `fecFailed`, `fecRecoveryPct` in `[slipstream-net]`.
4. **Retransmission?** The PS5 Remote Play protocol as implemented by chiaki-ng has **no retransmission request for
   video** [chiaki]: AV packets are not acknowledged (only the reliable Takion data channel has acks and a resend
   buffer, `takionsendbuffer.c`). The supported recovery messages are the corrupt-frame report
   (`stream_connection_send_corrupt_frame`) and the IDR request (`stream_connection_send_idr_request`). Nothing else
   is used or invented here.
5. **Is the route direct?** The library connects straight to the console's address: no hole punching, relay or PSN
   path is used by our app (`ChiakiBackend.cpp` leaves `holepunch_session` null; LAN sessions only) [code]. Whether
   that address is on this PC's own subnet (no router between) is checked by `assessRoute` (`ConnectionHealth.h`,
   on-link prefix of a local adapter) and, new, by the system's own route choice for that address
   (`GetBestRoute2`: on-link or through a gateway) in `[slipstream-net] route=`.
6. **Does GPU load delay network reception?** By construction it should not [code]: reception (chiaki's Takion
   thread), decode submission (our session owner thread) and the GPU work (the engine thread) are three threads; the
   engine never takes a lock the Takion thread needs. Two couplings remain: the owner shares the inbox lock with the
   Takion thread (section 6), and CPU contention under full GPU load can delay the two network threads, which run at
   normal priority while the engine thread has MMCSS (`EngineController.cpp` `MmcssScope`). Measured by
   `[slipstream-net] netToDecodeP95Ms` / `netToDecodeP99Ms` with frame generation and the neural layer on against
   off in the same kit session (section 9).

## 1. The receive path, socket to screen

| # | Stage | Thread | Buffer / queue | What is dropped here, and when | Longest wait | Tag |
|---|---|---|---|---|---|---|
| 1 | NIC ring, driver, IP | kernel | NIC receive descriptors (driver setting) | ring full: counted by the adapter (`MIB_IF_ROW2.InDiscards`) | interrupt moderation of the NIC [unknown] | [published] |
| 2 | UDP socket (Takion, port 9296, connected UDP) | kernel | `SO_RCVBUF` 102 400 B (`TAKION_A_RWND`, `takion.c` 41, 348-349); 1 MiB with `VEYRA_EXP_RP_LAN_PROFILE` / `VEYRA_EXP_RP_RCVBUF` | buffer full: newest datagram dropped; on Windows only visible system-wide (`MIB_UDPSTATS.dwInErrors`, section 4) | none unless the Takion thread is late; 102 400 B is about 54 ms of a 15 Mbit/s stream, 8 ms of 100 Mbit/s [estimate] | [chiaki][estimate] |
| 3 | receive loop `takion_thread_func` | chiaki Takion | one 1500 B `malloc` per datagram, `realloc` to size; per datagram a new WSA event (`stoppipe.c` `chiaki_stop_pipe_select_single`: `WSACreateEvent`, `WSAEventSelect`, wait, `WSACloseEvent`) | a datagram above 1500 B fails `recv` (the console's MTU probe stops at 1454, `senkusha.c` 265) | infinite wait when no reorder gap is open, else until the gap's deadline (ms granularity) | [chiaki] |
| 4 | decrypt, parse (GMAC key position) | Takion | none | malformed / bad MAC: dropped, library error line | 0 | [chiaki] |
| 5 | video reorder queue | Takion | 64 packets (`TAKION_AV_VIDEO_REORDER_QUEUE_SIZE_EXP 6`) | duplicate (`reorderqueue.c` "received twice"), older than the queue head, overflow (oldest given up), gap not closed within the reorder wait (16 ms, `TAKION_AV_REORDER_TIMEOUT_US`; 2 ms with the LAN profile; 2-16 ms adaptive) | **the reorder wait**: every packet behind a missing one waits up to the wait | [chiaki] |
| 6 | frame processor | Takion | one frame, units of one frame (`unit_slots`) | units of an older frame ("received old frame packet") | 0: flushed when source units arrive, or by FEC as soon as `received >= source units`; an incomplete frame is closed when the next frame's first packet comes | [chiaki] |
| 7 | FEC (Reed-Solomon, Cauchy matrix, jerasure, `fec.c`) | Takion | in place | FEC failure: frame dropped, corrupt-frame report, keyframe request (`enable_idr_on_fec_failure=true`, `ChiakiBackend.cpp` 302), P-frames skipped until an I-slice | 0 | [chiaki][code] |
| 8 | video callback `ChiakiBackend::videoCallback` | Takion | copy of the access unit (`PaddedBytes::copy`); inbox lock | not a valid Annex B unit (`callbackRejected`) | the inbox lock (held by the owner's `snapshot()`, section 6) | [code] |
| 9 | `VideoIngress` (inside `SessionInbox`) | Takion pushes, owner pops | 4 frames, 8 MiB, 100 ms age; 16 frames within 100 ms with `VEYRA_EXP_RP_BACKLOG` | frame-index gap, `framesLost>0`, `referenceRecovered`, a fifth frame, a frame older than 100 ms, size other than 720p/1080p, config change: **drop all, wait for a keyframe** (`recoverLocked`) | 100 ms (safety ceiling, not a target) | [code] |
| 10 | decode submit `RemotePlaySource::read` / `submitPacket` | session owner | one `AVPacket` copy; FFmpeg D3D12VA, `thread_count=1` | send/receive error: flush, keyframe request; first failure falls back to software unless `VEYRA_EXP_RP_DECODE_KEEP_HW` | CPU submission only: the GPU decode finishes behind a fence the engine waits on (GPU-side) | [code] |
| 11 | decoded queue `ready_` | owner | 4 frames | above 4: submission refused | 0 | [code] |
| 12 | hand-off `latest_` (one slot) or `DeliverySlots` (`VEYRA_EXP_RP_DELIVERY`) | owner -> engine | 1 (2) | an unread frame is replaced (`skipped_`, `mailboxOverwrites`) | 0 | [code] |
| 13 | engine import (`EnhanceGraph.cpp` ~3090) | engine | the decoder's own surface, sampled in place | none | GPU-side `Queue::Wait` on the decoder's fence: **zero-copy**, no CPU wait | [code] |
| 14 | enhancement, FG, present | engine | stream "smooth" / "present" | `[slipstream-smooth]`, `[slipstream-cadence]` | their documents | [code] |

Copies of the compressed bytes per frame: chiaki unit `memcpy` into the frame buffer and `memmove` to join units
(`frameprocessor.c`), our callback copy, the `AVPacket` copy (plus the codec header in front after a reset). At
15 Mbit/s a frame is about 31 KB, at 100 Mbit/s about 210 KB [estimate]: four copies are microseconds of CPU per
frame. **Not worthwhile to remove** (classification below). The decoded picture is never copied on the CPU.

Decoder reorder delay: `AV_CODEC_FLAG_LOW_DELAY` is not set (`RemotePlaySource::openDecoder`) [code]. FFmpeg holds
pictures only when the stream signals reordering; whether the PS5's H.265 stream has `sps_max_num_reorder_pics = 0`
is [unknown]: a held picture would show as a network-to-decode time near one frame period in `netToDecodeP50Ms` (new) or
`decodeP95Ms` (health line), against about a millisecond when nothing is held [estimate].

## 2. Recovery: FEC, reorder, keyframes, and how long each can wait

| Mechanism | Rule | Longest added wait | Who decides | Tag |
|---|---|---|---|---|
| Reorder wait | a missing packet holds every later packet until it arrives or the wait runs out; then given up | 16 ms (2 ms LAN profile, 2-16 ms adaptive, SmartLink apply 2/4/8/16) | client | [chiaki][code] |
| Early loss (`VEYRA_EXP_RP_EARLY_LOSS`) | give a gap up at once when its frame already misses more units than it has FEC units | removes the wait in that case | client | [code] |
| FEC | MDS Reed-Solomon: any `k` of `k+m` units rebuild the frame; tried at once when `k` arrived | 0 | console sets `m` per frame | [chiaki] |
| Corrupt-frame report | sent when a frame is lost or FEC fails; over the reliable channel (resent after 200 ms if not acked, `takionsendbuffer.c`) | n/a | supported message | [chiaki] |
| Keyframe (IDR) request | at FEC failure (library), at an ingress recovery (owner, at most every 500 ms), after 1 s / 3 s without a decoded frame (`StreamRecovery.h`); first repeat earlier with `VEYRA_EXP_RP_IDR_RETRY_MS` | the console's turnaround [unknown] + the keyframe's own transfer | supported message | [chiaki][code] |
| Reference-frame substitution | the library can point a P-frame at an older reference it has (`videoreceiver.c` `chiaki_bitstream_slice_set_reference_frame`) | 0, but artifacts | **disabled in effect by us**: `referenceRecovered` forces a keyframe (`VideoIngress::push`) | [chiaki][code] |
| Video retransmission | does not exist in the protocol | - | - | [chiaki] |

Can recovery finish before the display deadline? FEC: yes, by construction (no wait). A keyframe: no; the picture
holds the last good frame for at least the console's turnaround plus a frame [estimate]; [unknown] on a PS5:
`[remoteplay-lan] attempt=...` `recoveries`, and the time from `[remoteplay-recovery] request keyframe` to the next
decoded frame. Nothing in the path can wait indefinitely: the reorder wait has its ceiling, the ingress its 100 ms age
rule, the owner its 1 s / 3 s / 6 s recovery steps [code].

Packet counts and what they mean (the master directive: "do not label missing sequence numbers as confirmed loss"):

- `congestionLost` (patch 0004, summed 200 ms windows) counts units a frame was missing when the next frame began,
  **plus audio sequence gaps** (`packetstats.c`: pushed by `chiaki_packet_stats_push_seq`, counted in `chiaki_packet_stats_get`) [chiaki]. At the audio sequence's
  16-bit wrap `seq_max - seq_min` is computed in `int` and stored in `uint64_t`, so one window can add about 2^64
  (`packetstats.c:71-72`, found in review); the library is not changed: `[slipstream-net]` and the health line mark a
  window in which the cumulative loss counter went down or rose by 2^62 or more `counterWrap=1` and give no loss
  figures for it; a real heavy loss (more lost than received) stays a loss.
  It also **includes** packets that arrived but were given up by the reorder wait (`reorderLateArrivals`),
  units of a frame that arrived after it was closed, and packets dropped by this PC's socket buffer. So
  `[slipstream-net]` reports `missing` (that count), `late` (arrived after being given up) and
  `missingNotLate = missing - late` as the **upper bound** of packets that never reached the library; whether any of
  those were this PC's socket buffer is bounded by `udpInErrors` (section 4).
- Duplicates are dropped by the reorder queue without their own counter [chiaki]; patch 0004's `reorderDropped`
  holds duplicates, overflow drops and late arrivals together. `[slipstream-net] dropOther = reorderDropped - late`
  is therefore "duplicates or queue overflow" [estimate of the split].
- `fecRecovered` / `fecFailed` are frames, not packets (patch 0004) [code]. "FEC packets received" per packet is
  not counted (it would need a patch line in `frameprocessor.c`; not worthwhile while frames are counted).

## 3. Counters: what exists, and the gaps against the owner's list

Existing lines (all [code]): `[remoteplay-transport]` every 1 s (callbacks, library packet window, errors, send
buffer), `[remoteplay-progress]` every 1 s (received, decoded, fps, `videoMbps`, ages, queue, `waitingIdr`,
`idrRequests`), `[remoteplay-health]` every 5 s with `VEYRA_EXP_RP_HEALTH=1` (loss %, frame jitter RFC 3550 style,
reorder, late, FEC, keyframes, decoder resets, ingress depth, mailbox overwrites, Mbit/s, fps, route, console
CONNECTIONQUALITY), `[remoteplay-lan]` per attempt (ingress accepted / dropped / recoveries / backlog),
`[slipstream-delivery]`, `[slipstream-latency]` (per-stage times arrival -> present -> scan-out), `[slipstream-scanout]`,
`[slipstream-cadence]`, `[slipstream-smooth]`.

| Owner's item | Before this stream | Now (`[slipstream-net]`, `VEYRA_EXP_RP_STATS=1`, every 5 s) |
|---|---|---|
| requested / actual / min / max / rolling bitrate | requested at connect; `videoMbps` 10 s rolling; console target | `requestedKbps`, `kbps` (window), `kbpsMin1s` / `kbpsMax1s` (one-second windows), `consoleTargetKbps` |
| codec, resolution, fps | connect log, decoder-open log | `codec`, `resolution`, `resolutionChanges`, `fps` |
| UDP packets received / missing / late / out of order / duplicates | library counters, health line only | `packets`, `missing`, `late`, `missingNotLate`, `reordered`, `dropOther` (duplicates or overflow) |
| jitter | per frame, health line | `jitterMs` (RFC 3550 per frame; Takion has no sender timestamp: per-packet jitter is impossible) [chiaki] |
| arrival bursts | none | `burstMax` (most frames completed within one 4 ms slot in the window); `gapMaxMs` (longest gap between complete frames) |
| FEC recovery attempts / success | frames, health line | `fecRecovered`, `fecFailed`, `fecRecoveryPct` |
| unrecoverable | `fecFailed`, frames lost | `framesLost` (library), `recoveries` (ingress, each a keyframe wait) |
| socket receive drops | none | `udpInErrors` (system-wide UDP datagrams not delivered; an upper bound for our socket, see section 4), `nicDiscards` / `nicErrors` (the route's adapter) |
| application queue drops | ingress `dropped` per attempt | `ingressDropped`, `ingressPeak`, `mailboxOverwrites` |
| incomplete / corrupted frames, decoder drops | `frameDrops`, `decoderResets` | `framesLost` (never decoded), `recoveries` (each a keyframe wait; decoder failures land here too), `waitingIdrMs` (time without a decodable picture) |
| decode and presentation queue depth, buffer occupancy | ingress depth, peak | `ingressPeak`; presentation: `[slipstream-latency]` / `[slipstream-delivery]` |
| network-to-decode latency, P50/P95/P99 | `ingressWaitMeanMs`, `decodeP95Ms` | `netToDecodeP50/P95/P99Ms` (complete frame handed over by the library -> decoder returned the picture: queue wait plus decode submission) |
| frame age at display, GPU / render drops, repeated source frames | `[slipstream-latency]`, `[slipstream-smooth]`, `[slipstream-cadence]` | unchanged (not duplicated); joined by the bottleneck verdict |
| route facts | health line `route=`, `routeClass=` | `route` (`on-link` / `gateway` / `unknown`, from the system's route), `routeClass`, `family=ipv4` |

## 4. Socket, kernel and adapter (read-only on Windows)

- Socket receive buffer: 102 400 B by default [chiaki]. The library asks for it with `SO_RCVBUF` before connecting;
  the LAN profile asks for 1 MiB and logs what the system took (`[remoteplay-lan] effective receiveBufferEffective=`)
  [code]. The advertised `a_rwnd` stays 0x19000.
- Kernel-level drops of one socket are not exposed by Windows per socket [unknown: no documented per-socket
  counter found]. What is documented: `MIB_UDPSTATS.dwInErrors` "The number of erroneous datagrams received. This
  number does not include the value contained by the dwNoPorts member" (GetUdpStatisticsEx,
  https://learn.microsoft.com/en-us/windows/win32/api/udpmib/ns-udpmib-mib_udpstats), whose MIB definition is
  "received UDP datagrams that could not be delivered for reasons other than the lack of an application at the
  destination port" (RFC 4113 `udpInErrors`, https://www.rfc-editor.org/rfc/rfc4113) [published]. A full socket
  buffer is such a reason; whether Windows counts it there is [unknown] (one test settles it: a run with
  `VEYRA_EXP_RP_RCVBUF=102400` and the receive thread deliberately stalled, not planned). The counter is system-wide:
  **zero during a window proves no UDP receive drop on this PC; non-zero is an upper bound, shared with every other
  program**. Adapter: `MIB_IF_ROW2.InDiscards` "inbound packets which were chosen to be discarded even though no
  errors were detected" and `InErrors` (GetIfEntry2,
  https://learn.microsoft.com/en-us/windows/win32/api/netioapi/ns-netioapi-mib_if_row2) [published]. Both are read
  once per 5 s on the owner thread, only with `VEYRA_EXP_RP_STATS=1` [code, new].
- Receive-queue occupancy: `FIONREAD` on a datagram socket returns "the total number of bytes available to read"
  (https://learn.microsoft.com/en-us/windows/win32/winsock/winsock-ioctls) [published]. Sampling it would need a
  library patch on the Takion thread (one ioctl per packet or per N packets). Not done: `udpInErrors` = 0 plus a
  normal `ingressWait` already shows the socket is not the bottleneck; classified "missing and feasible, low value".
- Receive batching / URO: Windows 11 24H2 can coalesce UDP receives in the NIC, but "If a socket doesn't opt-in to
  URO, then the stack resegments the receives for that socket"
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-rsc-offload) [published]. The Takion
  socket does not opt in [chiaki], so it sees single datagrams. At 1 300 (15 Mbit/s) to 8 600 (100 Mbit/s) packets
  a second [estimate] the per-packet cost (event object, `malloc`, decrypt) is far below a frame period: opting in
  would need a rewrite of the library's receive loop for no expected latency gain. **Not worthwhile.**
- MTU / fragmentation: the library sets don't-fragment and measures the path MTU with Senkusha (576-1454,
  `senkusha.c`), so video packets are not fragmented on the LAN [chiaki].
- IPv4 / IPv6: the library resolves IPv6 only for a literal containing ':' (`session.c` 230-238); discovery and our
  route check are IPv4 (`Discovery.cpp`, `parseIpv4`) [chiaki][code]. A PS5 found by discovery is reached over IPv4.
- DSCP / QoS: the library marks nothing; on a switched LAN without congestion a mark changes nothing [estimate];
  Windows ignores application DSCP without a QoS policy (documented behaviour of `IP_TOS` on Windows is not relied
  on here). Not changed: **unsupported without a system policy, not worthwhile on an idle LAN.**
- Adapter power management, interrupt moderation, RSS, offloads, energy-efficient Ethernet: system settings. This
  stream reads nothing of them in the app and changes nothing. The kit's diagnostics collector already records the
  adapter's advanced properties read-only (`collect-diagnostics.ps1`) [code]; the owner decides on any change.

## 5. Threads, locks and the GPU

| Thread | Priority | Waits on | Locks it takes | Tag |
|---|---|---|---|---|
| chiaki Takion (receive, reorder, FEC, video callback) | normal; above normal with `VEYRA_EXP_RP_THREAD_PRIORITY` | socket event, reorder deadline | inbox mutex (one push per frame) | [chiaki][code] |
| chiaki feedback sender, congestion control | normal (feedback raised with the switch) | own timers | library mutexes | [chiaki] |
| session owner (pad send, decode submit, statistics) | normal; above normal with the switch | `WakeSignal` (a frame, a pad state) or 4 ms | inbox mutex (pop, snapshot, IDR), session mutex | [code] |
| engine (enhancement, FG, present) | MMCSS ("Games"/"Pro Audio" task, `MmcssScope`) | frame event, swap chain | session mutex (take the frame), never the inbox mutex | [code] |

- **Inbox lock coupling** [code]: the owner calls `SessionInbox::snapshot()` on every loop pass (`sessionSnapshot()` in
  `RemotePlaySessionSource::run`); `snapshot()` walks up to four 10 s sample queues, allocates and sorts the decode
  times under the mutex the Takion thread needs to hand over each frame. At 60 frames a second that is a few hundred
  values [estimate: tens of microseconds per pass]. It cannot hold reception for a frame period; it can add a few
  microseconds to a frame's hand-over. Classified "implemented, minor cost; not worthwhile to change without a
  measurement". `[slipstream-net] netToDecodeP99Ms` would show it if it mattered.
- GPU saturation reaches the network threads only through the CPU: the engine's driver work and MMCSS priority can
  delay normal-priority threads on a loaded CPU. The decoder runs on the video engine, sampled by the 3D queue behind
  a fence: a busy 3D queue delays *use* of a decoded picture, not its reception [code][estimate]. Whether FFmpeg's
  D3D12VA submission waits for a surface the engine still reads (surface pool pressure) is [unknown]:
  `netToDecodeP99Ms` with frame generation on against off (arm nd-load against nd-base).
- Existing experiments for this: `VEYRA_EXP_RP_THREAD_PRIORITY=1` (above normal, never real time),
  `VEYRA_EXP_RP_TIMER_1MS=1` [code]. Each can be benchmarked alone (kit arms, section 9).

## 6. Bitrate and encoder feedback

- Requested once in the launch spec (`bwKbpsSent`, `launchspec.c` 24; `info.video_profile.bitrate`,
  `ChiakiBackend.cpp` 300); choices 5-100 Mbit/s (`kPs5Bitrates`, `RemotePlay.cpp` 15) [chiaki][code]. No client
  message changes the rate during a session [chiaki]. Stream "clarity"'s test-only override
  `VEYRA_TEST_RP_BITRATE_KBPS` / `VEYRA_TEST_RP_CODEC` sets it per kit arm; this stream reuses it, adds no second one.
- Feedback the protocol supports: the congestion report every 200 ms (received / lost, loss clamped to
  `packet_loss_max` = 5 %, `congestioncontrol.c`), corrupt-frame reports, IDR requests [chiaki]. The console answers
  with its own `target_bitrate` in CONNECTIONQUALITY (patch 0004 keeps it: `consoleTargetKbps`) [code]. Telling the
  console less loss than measured to keep the bitrate up would be misreporting: not done.
- Owner's measured delivery: median 4.2-6.7 Mbit/s of 15 requested, never above 9.6 Mbit/s in a second, Wi-Fi
  [measured-on-owner-PC, from CLARITY_VERDICT.md]. Whether delivery is the encoder's choice (scene complexity, its
  rate control) or a reaction to loss reports is told apart by `consoleTargetKbps` against `kbps` and `missing` in
  the same window: target near the request with low delivery and no loss = the encoder sends what the scene needs;
  target falling after `missing > 0` = the console reacting to loss [estimate of the rule; no PS5 data].

## 7. Optimization inventory (master directive section 2, network / decoder / FEC rows)

Latency impact: what a change does to input-to-photon delay, by reasoning [estimate], never measured here.

| Item | Class | Evidence | Latency impact |
|---|---|---|---|
| Direct LAN connection, no relay / hole punch | already implemented and working | `ChiakiBackend.cpp` connect info; `assessRoute` | none |
| FEC (Reed-Solomon per frame, tried at once) | already implemented and working | `frameprocessor.c` | none: no wait |
| Duplicate suppression | already implemented and working | `reorderqueue.c` "received twice" | none |
| Bounded queues everywhere, 100 ms ceiling, newest-wins hand-off | already implemented and working | `VideoIngress`, `LatestMailbox` | none |
| Zero-copy decoded surface to the engine | already implemented and working | `EnhanceGraph.cpp` import with GPU-side fence wait | none |
| Corrupt-frame report + IDR on loss | already implemented and working | `videoreceiver.c`, `StreamRecovery.h` | recovery time is the console's |
| Reorder wait 16 ms on every gap | implemented; costly behind every lost packet | `takion.c`; Linux models `reorder_sim_results.json`, `impairment_study_results.json` (section 11: hold P99 about 15 ms at 0.1 % loss, no frame saved) | up to 16 ms per lost packet for the frames behind it; switches `LAN_PROFILE` (2 ms), `REORDER_ADAPTIVE`, `SMARTLINK=apply`, `EARLY_LOSS` exist, all off; arms nd-wait2 / nd-adaptive decide on his LAN |
| Reorder queue index wrap drops packet 0 | implemented but defective (upstream bug) | `reorderqueue.c` add; switch `VEYRA_EXP_RP_REORDER_WRAP_FIX` | none |
| Ingress expiry race (frame stamped after the owner's clock read called expired) | implemented but defective | `VideoIngress::expiredLocked`; `VEYRA_EXP_RP_EXPIRY_FIX` | removes a needless keyframe wait |
| A burst of more than 4 frames drops the queue | implemented, harsh | `VEYRA_EXP_RP_BACKLOG` (decode through, 100 ms ceiling) | trade: brief catch-up vs a keyframe freeze |
| Receive buffer 100 KiB | implemented; size unproven either way | `RCVBUF` switch with the LAN profile | none on a clean LAN; fewer drops if the Takion thread stalls |
| Receive / owner thread priority, 1 ms timer | experimental | switches exist | tail only |
| Diagnostics separating loss kinds, socket / adapter drops, route via the system | missing and feasible: **done here** | section 3 | none (5 s aggregation, off by default) |
| Pipeline bottleneck verdict | missing and feasible: **done here** (`StreamBottleneck.h`) | section 8 | none |
| Video retransmission request | unsupported by protocol | chiaki: none exists | - |
| Changing the FEC ratio or packet size | unsupported by protocol (console side) | `units_in_frame_fec` from the console | - |
| Under-reporting loss to keep the bitrate | unsupported (misreporting) | `congestioncontrol.c` | - |
| Accepting the library's reference substitution instead of a keyframe | experimental, not built | artifacts likely; no evidence | shorter freeze, damaged picture |
| Removing compressed-data copies | not worthwhile | ~31-210 KB per frame [estimate] | microseconds |
| URO / receive batching, DSCP, socket `FIONREAD` sampling | not worthwhile (now) | section 4 | none expected |
| Jitter (playout) buffer for video | not worthwhile | no evidence of frame-interval jitter on a LAN; any buffer adds its depth to every frame; the queues already decode at once | would add delay by design |
| Network Performance Mode (profiles) | not built: no measurement supports a choice yet | section 10 | - |

## 8. The dominant-cause verdict (`include/veyra/remoteplay/StreamBottleneck.h`)

A pure rule over one 5 s window: evidence in, one cause out, every cause whose sign was seen, and the numbers that
decided it. A window with no packet and no frame (outage, stall, keyframe wait) is `unknown`, never `none`.
Precedence, local and certain first: `socket-or-adapter` (frames lost, packets never arrived, AND this PC's UDP
receive errors or adapter discards are at least a tenth of the packets that never arrived; fewer are only a sign),
`receive-thread` (reorder waits that ran out lasted >= 2 ms longer than asked, >= 4 of them: the Takion thread woke
late, from CPU load or the default timer granularity of the library's millisecond wait; arm `nd-timer` tells them
apart), `app-drops` (our ingress dropped complete frames while the library lost
nothing), `network-loss` (frames lost, the missing packets never came), `packet-recovery` (frames lost although
their packets came late; or frames came unevenly while reorder waits ran out for packets that never came: the wait
held them for nothing), `decoder` (decode P95 >= 0.8 frame), `jitter`, then with the engine's evidence
`frame-generation` (FG over budget), `enhancement` (GPU >= 95 % with an upscaler / the neural layer),
`gpu-processing` (>= 2 % of decoded frames replaced before the engine took them, or GPU >= 95 %), `presentation`
(fewer real frames presented than decoded), and last `encoder` (nothing above, and the console delivered < 60 % of
the request: the picture is what the encoder sent). It never names the network when its counters are clean, and never
names the GPU, decoder or encoder while frames are lost to the network (tests in `NetDiagnosticsTests.inc`).
Thresholds are provisional (not calibrated on a PS5). The log line's verdict has network and decoder evidence only;
the panel's adds the engine's (`pushLiveStats`: GPU load, FG budget, real presents, decoded rate).

## 9. What his PC must measure (kit; unverified until he runs it)

- Every measured kit session now shows a "network reception" line per arm from `[slipstream-net]` (the kit sets
  `VEYRA_EXP_RP_STATS=1` for every arm): framegen and neural therefore compare reception with VFG / the neural layer on
  and off **on the same network, interleaved**.
- `-Session framegen -Arms nd-base,nd-wait2,nd-rcvbuf,nd-adaptive,nd-priority,nd-timer,nd-backlog,nd-expiry,nd-load`:
  one existing switch per arm against `nd-base` (the 2 ms wait alone: `LAN_PROFILE=1` + `RCVBUF=102400`; the 1 MiB buffer
  alone: `LAN_PROFILE=1` + `REORDER_US=16000`; priority; 1 ms timer; backlog; expiry fix), and `nd-load` (upscaler + neural
  + VFG 2x, no network switch) against `nd-base`. No new switch was needed for this.
- First question, a 5-minute clean `nd-base` on the cable: `missing`, `late`, `missingNotLate`, `udpInErrors`,
  `nicDiscards`, `fecFailed`, `framesLost`. **All zero = the network is not the problem; stop network work (master
  section 19) and look at `kbps` / `consoleTargetKbps` (the encoder) and stream "clarity"'s arms.** Read-only adapter
  facts (power management, interrupt moderation, RSS, offloads) come from the kit's `collect-diagnostics.ps1`.

## 10. Network Performance Mode

Not built, and no profile set is proposed as a default. What the evidence so far supports (section 11, a model): on a
link without reordering, the 2 ms wait is never worse and removes the hold behind every lost packet; on a link whose
packets arrive late (`late` > 0) the 2 ms wait loses frames that 16 ms saves, and the adaptive wait covers both. If his
wired run shows `missing` > 0 with `late` = 0, the candidate "Ultra-Low Latency" set is `VEYRA_EXP_RP_LAN_PROFILE=1`
`VEYRA_EXP_RP_REORDER_ADAPTIVE=1` (existing switches, off by default); "Maximum Stream Stability" would be the library's
16 ms wait plus `VEYRA_EXP_RP_BACKLOG=1`, and it adds up to 16 ms to frames behind a gap: never automatic. Until a run
of `nd-wait2` / `nd-adaptive` on his LAN says so, the profiles would be labels without measurements.

## 11. Impairment model (Linux simulation, not a PS5 measurement)

`tools/remoteplay_latency_bench/impairment_study.cpp` (results `impairment_study_results.json`, 5 seeds x 120 s,
impairment 30-90 s, the same bytes on a second run) puts made-up packet arrivals through the library's reorder-queue
rule (the model `reorder_library_check.sh` compares with the C source), FEC as "any 24 of 27 packets of a frame"
(the console's real FEC ratio is not known here) and a 3 ms decoder, for each existing receive choice on the same
arrivals. 36 000 frames per cell, 18 000 of them in the impaired window. "Hold" = hand-over minus the moment the
frame's 24th packet reached the socket.

| Condition | Frames lost (any choice) | Library default 16 ms: hold P95 / P99, stutters | 2 ms wait (LAN profile): hold P95 / P99, stutters | Verdict named (16 ms / 2 ms) |
|---|---|---|---|---|
| no loss | 0 | 0 / 0 ms, 0 | 0 / 0 ms, 0 | none / none |
| 0.1 % loss | 0 | 0 / 14.9 ms, 8 | 0 / 1.1 ms, 0 | packet-recovery / none |
| 0.5 % loss | 3 | 14.9 / 15.9 ms, 118 | 1.1 / 2.2 ms, 3 | packet-recovery / none (+ network-loss where a frame was lost) |
| 1 % loss | 5 | 15.5 / 31.5 ms, 398 | 2.1 / 3.1 ms, 2 | packet-recovery / none |
| 3 % loss (diagnostic) | 110 (16 ms), 87 (2 ms) | 32.7 / 48.3 ms, 1676 | 3.1 / 5.0 ms, 57 | network-loss |
| bursts of ~6 packets, ~1/s | 191 | 0 / 0 ms, 111 | 0 / 0 ms, 111 | network-loss |
| 1 % duplicated | 0 | 0 / 0 ms, 0 | 0 / 0 ms, 0 | none |
| reordering 1 % 1-6 ms + jitter | 0 | 2.8 / 4.8 ms, 0 | 1.7 / 2.3 ms, 0 | none |
| path stalls 5-20 ms in 1 % of frames | 0 | 0 / 0 ms, 7 | 0 / 0 ms, 7 | jitter |
| loaded-PC proxy (receive thread 3 ms late, decode 9 ms, 0.1 % loss) | 0 | 0 / 14.9 ms, 6 | 0 / 4.1 ms, 0 | packet-recovery / receive-thread |

The adaptive wait from 2 ms equals the 2 ms wait in every row; early loss changes nothing here (single losses never
exceed a frame's FEC). The decoder queue never dropped (0 ingress / backlog recoveries in every cell). What the model
says, and no more:

- **Independent loss up to 1 % is repaired by FEC under every choice**: 0-5 frames of 18 000 lost. Nothing a client
  setting does changes how many frames are lost there.
- **The library's 16 ms wait turns each lost packet into a 15 ms hold of the frames behind it** (head-of-line
  blocking: FEC can only run when the later packets are released). That is the "stutter" column. The 2 ms wait removes
  it without losing one more frame. This is the only change in this study that reduced damaged picture delivery (holds
  and stutters) — in the model; whether his LAN loses packets at all is unknown.
- **Bursts longer than FEC lose the same frames whatever the wait**: only recovery speed matters (keyframe turnaround,
  the console's).
- The verdict named the cause the condition was built with in every impaired interval, and `none` in clean ones
  (except, with the 16 ms wait only, up to 5 intervals right after a loss window, where the frame-jitter peak still
  remembers the holds).
- Not modelled: the console's real pacing and FEC ratio, Windows scheduling, a real GPU load (the proxy is a delayed
  thread, not a GPU), Wi-Fi block-ack (section in PS5_REMOTE_PLAY_EXPERIMENTS.md, sprint 4).

## 12. Blur: network, compression, scaling or neural processing? (as far as code and simulation can tell)

- **Network loss: not a source of blur in this pipeline** [code]. A frame that cannot be completed is never decoded;
  the picture freezes on the last good frame until a keyframe (`framesLost`, `recoveries`, `waitingIdrMs`). Loss shows
  as hitches and freezes, not as smear. Two indirect network routes to a softer picture exist: the console lowering
  its bitrate after loss reports (`consoleTargetKbps` against `requestedKbps` with `missing` > 0 in the same window),
  and the console switching to a smaller announced resolution (`resolution`, `resolutionChanges`). Both are logged
  now; whether a PS5 does either on a clean wired LAN is unknown.
- **PS5 compression**: the strongest candidate on his measured Wi-Fi runs (4.2-6.7 Mbit/s median delivered of 15,
  [measured-on-owner-PC] via CLARITY_VERDICT.md); the dominant-cause rule names it `encoder` when the network is clean
  and delivery stays under 60 % of the request. Stream "clarity" owns the bitrate and codec arms.
- **Scaling and neural processing**: outside the network path; stream "clarity" (A-F picture paths) and "smooth".
- **What his PC must show**: a wired `nd-base` with `missing` / `late` / `framesLost` at 0 and `bottleneck=encoder` or
  `none` means the blur is not the network's; `kbps` far below `requestedKbps` with `consoleTargetKbps` near the request
  means the encoder sends what the scene needs at that request (clarity's higher-bitrate arms decide whether more helps).

## 13. Not done

- A per-frame trace with one id from the first packet to Present: the first packet's arrival is not visible outside the
  library (it would need a line in chiaki patch 0004's frame ring) and the present side belongs to streams "present" /
  "smooth" (`[slipstream-latency]` already has arrival -> decoded -> pickup -> present -> scan-out per frame, aggregated).
  `[slipstream-net]` correlates by window instead: loss, recoveries and keyframe waits in the same 5 s as
  `[slipstream-cadence]` / `[slipstream-delivery]` stutter.
- Socket receive-queue occupancy (`FIONREAD`) and a per-socket drop counter: would need a library patch on the Takion
  thread; `udpInErrors` = 0 already rules the socket out in a window.
- No new behaviour switch: every receive-side change worth comparing already exists as one (section 7); the evidence
  says compare them, not add more.
