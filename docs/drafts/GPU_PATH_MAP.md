# The GPU path of one frame-generation pair at 3X, 4K (stream "gpupath", 24 Nov 2026)

Scope: the owner's twenty-fifth message ("unnecessary GPU synchronization, processing dependencies, and opportunities
for parallel execution"). Base: exp/s6-lagfix. Setup assumed: 4K capture card, a 30 FPS game with duplicate removal
(T = 33.4 ms per pair), VFG (NVIDIA Video Frame Generation through CUDA) at 3X, SR and NR off, HUD Lock and exclusion
off, independent presentation queue (every non-XeSS chain), smooth pacing (DXGI maximum latency 1).

Labels: **code** (read here, file:line at commit e07a0cd5 of exp/s6-gpupath; EngineController.cpp lines are exp/s6-lagfix's,
unchanged), **tested** (unit / WARP in CI), **estimate** (arithmetic with a stated assumption), **hypothesis**.
Nothing was run on a GPU; no number below is a measurement. Byte sizes at 3840x2160: NV12 12.4 MB, RGBA8 33.2 MB,
RGBA16F 66.4 MB. Copy-time estimates assume about 400 GB/s of effective video-memory bandwidth (read + write counted)
and about 25 GB/s over PCIe; both are assumptions, not his card's figures.

Threads: **CB** capture callback thread (driver); **EN** engine owner thread (pickup, `EnhanceGraph::process`, every
present); **VW** VFG worker thread (CUDA submissions, `VfgBackend.cpp` `Impl::work`). GPU: **GQ** the graph's direct
queue (`CommandSlotRing`, 16 slots in a live session: `EngineController.cpp:594-599`, fence = `context_.fence()`); **CS** VFG's CUDA stream; **PQ** the presenter's own
direct queue (3 slots, own fence); DXGI flip-model swap chain.

## 1. One pair, in order

Pair (A, B): B is picked up at t = 0. v0 / v1 / v2 = graph-fence values of the base list, g1's list, g2's list.
p1 / p2 = VFG producer-fence values; c1 / c2 the completion values (equal to p1 / p2 today).

| # | Thread / queue | Event | file:line | Waits for | Needed at that moment? | Cost at 4K |
|---|---|---|---|---|---|---|
| 1 | CB | `selectCaptureUploadTarget`: writes the driver sample into the frame's upload buffer, or, if the GPU still holds that buffer (`gpuBusy`, a non-blocking fence read), into CPU planes | `CaptureUploadFrame.h` `selectCaptureUploadTarget` | nothing (no CPU wait) | - | CPU copy 12.4 MB (upstream-measured 1.3 ms) |
| 2 | EN | Pickup gate: no new frame while the scheduler holds 2 batches, a frame slot is leased, or **the previous pair's whole group is not GPU-complete** (`pendingCompletions` non-empty); waits in `waitLive` (≤ 1 ms slices on the arrival event and `gpuReadyEvent`) | `EngineController.cpp:2078-2107`, `1584` | the previous pair's v2 (its last list) | Only as overload protection: with base + 2 x fg ≈ 15-20 ms < 33.4 ms the previous group is done before the next content frame exists (estimate) | 0 in steady state; > 0 only when a group exceeds T |
| 3 | EN | `process`: CPU wait for `uploadFences_[parity]` (the same-parity pair two frames ago) | `EnhanceGraph.cpp:3004` | v2 of pair n-2 | No: always complete because of row 2 | 0 |
| 4 | GQ | GPU `Wait(presentationFences_[parity])`: the presenter's blit of pair n-2's real frame has read `videoFrame_[parity]` | `EnhanceGraph.cpp:3008` | PQ, ~1-2 pairs ago | Yes (this list rewrites that texture) | 0 (long done) |
| 5 | EN | `ring_.acquireNext`: CPU wait if the slot's previous list is still running | `EnhanceGraph.cpp:3024` → `CommandSlotRing.cpp:153-161` | the list 16 submissions ago | Only if more than 16 lists are in flight; the pickup gate keeps one pair's lists in flight (base + 2 at 3X; +1 per NR layer, VFX SR pass, NVOF split): never in his setup | 0 (counted: `ring.cpuWaitCount`, `[frame-flow] slotWait*`) |
| 6 | EN → GQ | **Base list**: upload copy (`CopyTextureRegion` from the upload buffer, `:3262` / packed `:3108`), YUV → RGBA16F (`:3273`), colour, clean-up, (SR / NR / NVOF would split here: `:3379`, `:3504`+`:3513` queue wait on NVOF, `:3984`), final write to `videoFrame_[parity]` RGBA8 (`:4174`), **VFG capture**: copy `videoFrame_[parity]` → CUDA input buffer `inputs[parity]` + shot-change test, 1 group (`:4260`, `VfgBackend.cpp:589-601`), exclusion compose (off, `:4270`), HUD Lock update (off, `:4272`); `ExecuteCommandLists` + `Signal(v0)` (`:4274`, `CommandSlotRing.cpp:214-217`) | as listed | row 4 | yes | PCIe 12.4 MB ≈ 0.5 ms; conversion and copies: 66 MB write + 33 MB final write + 33 MB VFG copy ≈ 0.3-0.5 ms of memory traffic (estimate) |
| 7 | EN → GQ | `vfgBackend_->generate(sub 1)` → `handoff`: GQ `Signal(producer, p1)`, job queued, GQ `Wait(completion, c1)`; no CPU wait | `EnhanceGraph.cpp:4357`, `VfgBackend.cpp:422-433` | - | - | host µs |
| 8 | VW → CS | `cudaJob`: `cuWaitExternalSemaphoresAsync(p1)`, bind A/B (new pair), `NvVFX_Run` (async), `cuSignalExternalSemaphoresAsync(c1)`; CPU never waits | `VfgBackend.cpp:320-380` | CS waits on the GPU for p1 = the base list | yes (both inputs) | **fg1's model time** (upstream-measured +6.6 ms per source frame at 4K, VFG Medium) |
| 9 | EN → GQ | g1's list: copy CUDA output `outputs[parity]` → `genFrame_[parity]` (predicated on the shot-change result), compose (off); submit, `Signal(v1)` | `EnhanceGraph.cpp:4358-4367`, `VfgBackend.cpp:628-661` | GQ waits on the GPU for c1 (row 7) | yes | 33 MB copy ≈ 0.17 ms (estimate) |
| 10 | EN | `previewCheckpoint` (`presentDuringProcess`): presents an older frame that is due now (not with HDR output) | `EnhanceGraph.cpp` after each submit; `EngineController.cpp:1604` | - | - | - |
| 11 | EN → GQ | sub 2: GQ `Signal(producer, p2)` (queued **after** `Wait(c1)` and g1's list), `Wait(completion, c2)` | `VfgBackend.cpp:422-433` | - | - | - |
| 12 | VW → CS | `cudaJob` sub 2: `cuWait(p2)`, Run, `cuSignal(c2)` | `VfgBackend.cpp:320-380` | **p2, i.e. c1 + GQ wake-up + g1's copy back** | **No** (finding A1): sub 2 needs only the inputs (behind p1); the stream already orders it after Run 1 | CS idles one D3D12 round trip per later subframe (`VEYRA_TEST_VFG_GPU_PROFILE` `handoffIdle`; unmeasured on his PC) |
| 13 | EN → GQ | g2's list: copy `outputs[parity+2]` → `genFrame_[parity+2]`; `Signal(v2)` | `EnhanceGraph.cpp:4358-4367` | c2 | yes | ≈ 0.17 ms |
| 14 | EN | `uploadFences_[parity] = v2` (`:4470`); upload buffer released (`:4481`): **was v2, now v0** (change 2); real frame's presentation wait (`:4489`): **was v2, now v0** with VFG (change 1) | `EnhanceGraph.cpp:4470-4491` | - | - | - |
| 15 | EN | Batch queued: `SetEventOnCompletion(lease fence, gpuReadyEvent)` for v0, v1, v2; the scheduler polls `lease->ready()` (`GetCompletedValue`) and sleeps in `waitLive` | `EngineController.cpp:3062`, `3110-3200` | - | - | CPU wake-up latency of the event (unmeasured) |
| 16 | EN → PQ | Present g1 at its deadline: PQ `Wait(graph fence, v1)`, `presentationRing.acquireNext` (CPU wait only if the blit 3 presents ago is still running), blit 3840x2160 → back buffer (1440p), `Signal`, DXGI `Present` (capacity: waitable object / maximum latency 1, checked before by `presentationReady()`) | `VideoPresenter.cpp:277`, `280`, `507`, `522` | v1 | yes | blit reads 33 MB, writes 14.7 MB ≈ 0.1 ms (estimate) |
| 17 | EN → PQ | Present g2: PQ `Wait(v2)`, blit, Present | same | v2 | yes | same |
| 18 | EN → PQ | Present B: PQ `Wait(graph fence, presentationReadyFence(slot, false))` = **v2 before, v0 now (VFG)**; blit; `presentationSubmitted` (row 4 of pair n+2 waits for it) | `VideoPresenter.cpp:277`, `518`; `EnhanceGraph.h` `presentationReadyFence` | before: g2's copy back; now: B's own lists | **No** (finding B1): with VFG no list after the base list touches `videoFrame_[parity]` | 0 when g1 and g2 were shown (PQ is FIFO: B's blit follows g2's anyway); when they were not shown: g1 + g2 GPU time, about 2 x fg at 4K (estimate) |
| 19 | EN | `pollCompletions` → `resolveGeneration` → `pendingCompletions.erase`: the pickup gate (row 2) opens | `EngineController.cpp:1420-1500` | v2 (CPU poll) | as row 2 | - |

Other backends, for completeness (code): the DLSS path records each generated frame in one GQ list that reads
`videoFrame_[parity]` (`fgBackend_->evaluate`, `:4424`) and copies a 4-byte status to a readback buffer that the CPU
maps (no wait: `resolveFrame` reads it after the fence); PushFlow packs, runs ONNX Runtime on GQ (`pushFlow_->infer`,
`:4302`) and resolves in two lists per subframe; AMF queues on its own queue with texture fences (`:4330`). All three
keep the real frame's wait at the pair's last list.

## 2. Findings

### A. CPU or GPU waits that serialise work the GPU could overlap

- **A1 (code):** row 12. VFG's later subframe waits for a producer value signalled after the previous subframe's copy
  back. Nothing it reads or writes needs that: inputs are behind p1, its output buffer was last read two pairs ago, and
  the CUDA stream orders the Runs. Built as the experiment `VEYRA_EXP_RP_VFG_BACK_TO_BACK=1`, **off**: chained, g1's
  copy back on GQ shares the GPU with Run 2 on another context, so g1 (on the critical path) may be ready later. g2
  and the group's span would be earlier by one round trip. Only his PC decides.
- **A2 (code):** row 2. The next pair waits for the previous pair's whole group, not only for its base work. At 3X 4K
  with the group well under 33.4 ms this costs nothing (estimate); it costs exactly when a group exceeds T. The rule is
  EngineController's (stream lagfix owns it): "never stack a second frame behind one the GPU is still enhancing". Not
  changed.
- **A3 (code):** no CPU waits for the GPU inside a pair: VFG hands off through shared fences only (rows 7-13); ring
  slots are reused only after the pickup gate; `waitForFenceValue` (row 3) is always satisfied.

### B. Non-critical work in front of the first generated frame

- **B1 (code), changed:** row 18, the real frame's presentation-queue wait covered both generated frames. Not in front
  of g1, but it tied the real frame's display to generated frames' GPU work in exactly the pairs where those frames are
  not shown: a probe pair during `LiveFgRejectionHold` (default on cards: "pairs admitted while held expire their
  generated frames", real frame shown when ready) and a generated frame dropped at its deadline.
- **B2 (code):** in his setup the base list before p1 holds only what g1 needs (final write, the CUDA input copy, the
  shot-change test of 1 thread group). The real frame's exclusion compose and HUD Lock update (rows 6, both off) would
  sit before p1; the HUD Lock update cannot move after g1 (g1's compose reads its result). Comparison / raw-frame
  references (two full-frame copies, `:1650`, `:3308`, `retain` in the NR section) run only while comparing or
  recording raw frames. Not changed: nothing to gain in his setup.
- **B3 (code):** the real frame's own fence (v0) includes the 33 MB copy into CUDA's buffer; the real frame is shown
  ≈ 22 ms later, so this delays nothing.

### C. Copies that exist only for the hand-off

- **C1 (code):** the copy of frame A is NOT repeated: `capture` copies only B into `inputs[parity]`; A is still in
  `inputs[1-parity]` from the previous pair (`cudaJob` binds `SrcImage0 = inputs[1-parity]`). The "reuse A" step the
  brief suggested already exists.
- **C2 (code):** what remains per pair is one copy in (33 MB) and one copy back per generated frame (33 MB each).
  Removing the copy in would mean the final write (row 6) writing straight into the shared buffer (a new UAV-buffer
  path in the blit shader) and the copy back would mean the presenter sampling a buffer: new HLSL that cannot be
  compiled here and a different resource type for every consumer (recorder, HUD sampler, quality probe). Not built;
  ≈ 0.17 ms each (estimate).
- **C3 (code), changed:** the capture upload buffer was released at v2; a callback that wanted it while g1/g2 ran wrote
  CPU planes and the graph copied 12-25 MB again on the engine thread. Released at v0 now.

### D. Parallel execution on another queue

- **D1 (hypothesis):** the upload copy (row 6, 12.4 MB over PCIe, ≈ 0.5 ms) could run on a copy queue as soon as the
  callback has written the buffer, before pickup. It needs a texture per in-flight frame, a cross-queue fence per frame
  and submission from the callback thread or a new thread. Not built blind.
- **D2 (code):** VFG's model already runs on its own engine (CUDA), overlapped with nothing of the same pair by
  necessity (g1 needs the base list). Presentation blits run on PQ, overlapped with GQ work.

### E. Idle GPU between bursts

- **E1 (hypothesis, unchanged):** the GPU works ≈ 15-20 ms per 33.4 ms pair and idles the rest; whether clocks drop
  between bursts is a driver matter. No system setting from code. Visible as `gpuFg1P95Ms` minus `gpuFg1P50Ms`.

## 3. Changes (exp/s6-gpupath)

| Change | Removes | Why it cannot hurt | Test | Restore | Default |
|---|---|---|---|---|---|
| Real frame's PQ wait = its own lists' fence (VFG, no HUD Lock / exclusion / present-sink FG) | a GPU wait for g1+g2 before B's blit, in pairs whose generated frames are not shown | lowers a waited fence only; PQ is FIFO so no reorder; the texture is final at v0 (no later list touches it, conditions in `PushGpuPath.h`) | unit (FIFO model: never later, never reordered, 400 random sequences; decision table); WARP `gpu-path-real-wait` (CI) | `VEYRA_TEST_GPU_REAL_WAITS_GROUP=1` | on |
| Capture upload buffer released at v0 | the callback's fallback to CPU planes and the graph's second CPU copy (12-25 MB) | lowers a waited fence only; the only reader is the base list's copy | unit (decision) | `VEYRA_TEST_UPLOAD_LATE_RELEASE=1` | on |
| VFG later subframes chained to p1 | CS idle round trip per later subframe | - (can delay g1: see A1) | unit (decision) | unset | **off** (`VEYRA_EXP_RP_VFG_BACK_TO_BACK=1`) |

Every start logs `[gpu-path] realFrameWait=own-lists|pair-group uploadRelease=base-list|pair-group`. Benefit of all
three: unmeasured.

## 4. What only his PC can decide

| Question | Reading |
|---|---|
| How often B's blit waited for unshown generated frames | `EnhanceGraph::Metrics::realPresentOwnFence` against `[live-fg-hold] heldProbes`, `dropDeadlineFps`; real-frame present lateness in those pairs (`presentLateP99Ms`) |
| Whether the upload fallback ever happens | `uploadCpuCopyFps` (expected 0 either way in steady state) |
| Whether chaining VFG subframes helps or hurts g1 | `gpuFg1P50/P95Ms`, `gpuFgP50Ms`, `VEYRA_TEST_VFG_GPU_PROFILE` (`handoffIdle`) with and without `VEYRA_EXP_RP_VFG_BACK_TO_BACK=1` |
| Whether a copy-queue upload (D1) is worth building | `gpuColorP50Ms` (includes the PCIe copy) |
| Whether the GPU starts cold after idle (E1) | `gpuFg1P95Ms - gpuFg1P50Ms` |

## 5. The capture-card path at 4K in detail (owner's scope from 23 Nov 22:51 ET: capture card only)

Remote Play paths were not changed by this stream beyond what both paths share (the real frame's wait applies to any
VFG pair; the upload release only to capture upload buffers).

| Step | What happens (code) | CPU wait / copy on the critical path? | Verdict |
|---|---|---|---|
| Callback, claim | `CaptureCardSource.cpp:689-694`: takes the staging frame under the lock, copies OUTSIDE it | no wait | fine |
| Callback, copy | `writeCaptureFrame` (`:329-342`): one memcpy per plane of the driver sample into the staging frame's upload buffer (`CaptureUploadFrame.h`), I420 interleaved here; 64x36 proxy sampled from the driver sample | the one unavoidable CPU copy (12.4 MB NV12; upstream-measured 1.3 ms); the driver's buffer is not GPU-visible | nothing to remove |
| Callback, repeat test | `compareCaptureSample` before publish, the reference copy AFTER publish (`CommitAfterPublish`, `:688`) | a new picture exits early once `max(toleranceFor()+1, kDecidedOver)` components differ (`PictureCompare.h:90-101`); a repeat reads the whole picture but is not shown as a new pair | fine for new pictures; `[capture-callback] entryToCopiedMs` measures it |
| Publish | `std::swap(stagingFrame, pendingFrame)` under the lock, `SetEvent(frameEvent)` (`:759-786`) | no | fine |
| Mailbox | three upload-backed frames: reader's, pending, staging (`:522-548`); a replaced pending frame becomes the next staging frame | the reader's frame comes back to the callback only after the next `read()`, which the pickup gate allows only after the pair's whole group: the upload buffer is always free by then, so `gpuBusy` is false with either release point (expected `uploadCpuCopyFps` = 0) | change 2 is a safety margin, not a measurable gain in steady state |
| Pickup | engine wakes on `frameEvent` (`waitLive`), gate of row 2 | only when a group exceeds T | lagfix's rule, unchanged |
| GPU upload | `CopyTextureRegion` from the upload buffer at the start of the base list | ≈ 0.5 ms over PCIe at 4K NV12 (estimate) on the critical path of g1 | D1: a copy queue could take it off; not built blind |
| CUDA hand-off in | one 33 MB copy of B per pair; A reused from the previous pair | ≈ 0.17 ms (estimate) | C2: not removable without new shaders and buffer consumers |
| CUDA hand-off out | one 33 MB copy per generated frame, predicated on the shot-change test | ≈ 0.17 ms each (estimate) | C2 |
| Between Runs | CUDA idles one round trip before Run 2 (A1) | not on g1's path | experiment `VEYRA_EXP_RP_VFG_BACK_TO_BACK=1`, OFF (below) |
| Real frame's blit | waited for g2's copy back | only in pairs whose generated frames are not shown | change 1, ON |

**Can the back-to-back experiment be shown safe?** Not without his GPU. Correctness (same pictures, every hazard
covered) follows from the code (PushGpuPath.h, point 3) and its decision is unit-tested; the open question is time: with
chaining, g1's copy back (graph queue, D3D12 context) and Run 2 (CUDA context) are both ready at once, and how the
driver shares the GPU between two contexts decides whether g1 is ready later. Today Run 2 cannot start before g1's
copy back is done, so g1 never competes. The deciding measurement, same scene, 4K, 3X, VFG, ≥ 60 s per arm, both arms
twice: `gpuFg1P50Ms` / `gpuFg1P95Ms` (must not rise), `gpuFgP50Ms` (expected to fall by the round trip),
`[vfg-gpu] handoffIdleMeanMs` with `VEYRA_TEST_VFG_GPU_PROFILE=1` (expected near 0 with chaining), and
`dropDeadlineFps` (must not rise). It stays off until such a run says g1 does not move.
