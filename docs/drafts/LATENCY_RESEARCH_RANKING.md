# Lower frame-generation delay: what was researched and how the candidates rank (24 November 2026)

Three research reports were written for Package 15 (NVIDIA Streamline / Reflex / Optical Flow SDK; AMD FidelityFX frame
generation, Windows presentation and the GPU path at 4K; frame generation models and prediction). This is their
synthesis. Labels: SIMULATED = the scheduling simulator; estimate = arithmetic from published or upstream figures.
**Nothing here is measured on the owner's PC.** PushFPS receives captured video: it has no depth buffer, no engine motion
vectors and no future game state, and every candidate was checked against that.

Delay below = the capture card's callback -> the real frame's Present returned, 30 FPS shown as 90 FPS (3X), 4K.
At 3X the real frame's delay is: its own processing + the first generated frame + a wait + margins. The wait is 22.2 ms
when both generated frames are interpolated (today), 11.1 ms with one predicted frame (Package 15's "Lower delay"), and
about 0 with two predicted frames.

| # | Candidate | Expected effect | Picture risk | Branch / owner | Status |
|---|---|---|---|---|---|
| 1 | One predicted frame, STAY-IN schedule; untrusted pixels keep the real picture; whole frame = real picture when more than half of the moving pixels fall back (seam-aware verdict, leading-edge warp and Catmull-Rom sampling NOT built) | ~31 -> ~20-22 ms (grid arithmetic 11.1 ms; SIMULATED gain about 9 ms, 37.6 -> 28.3 ms on the simulator's own scale); schedule switches 0.5-0.8 a minute (SIMULATED) | one frame in three predicted; picture unknown | Package 15 | in Package 15, off by default |
| 2 | Display path: overlays drawn inside the swap chain so the window region stays one rectangle (Independent Flip, VRR) | readout unchanged; on screen possibly -6 to -8 ms (correlation in earlier logs) | none | Package 15 | in Package 15, on |
| 3 | Reflex Sleep kept, ended only when it keeps running past the capture sample's arrival | removes a possible delay tail (0 ms if it never overran) | none | Package 15 | in Package 15, on |
| 4 | Blit ahead, Present at the deadline (AMD's order) | steadier present-to-screen; ~0.1 ms + queue wait | none | Package 15 (off by default) | in Package 15, off |
| 5 | Shared-motion interpolator: motion once per pair at reduced size, cheap full-size warp for I(2/3) AND P, replacing VFG's run | fg1 6.6 -> ~1-3 ms: ~14-20 ms (estimate), the only route to ~15 ms on the grid with one predicted frame | I becomes our own warp instead of NVIDIA's model: unknown, must be compared | next experimental branch | next |
| 6 | NVOF hardware flow as a second estimator / trust signal for the predicted frame (bidirectional, temporal hints on) | 0 ms directly; fewer bad regions | lowers risk | after #5 | later |
| 7 | Merged colour pass, upload overlap, capture copy removal | 0.3-1.5 ms (estimate) | none if bit-exact | later | later |
| 8 | Fully causal (two predicted frames) | a few ms | highest; literature: video-only prediction clearly worse than interpolation | simulator only | evaluate, opt-in at most |

Rejected as incompatible with captured video (verified): Streamline DLSS-G and its pacer, Blackwell flip metering, Reflex
frame limiter / Reflex 2 Frame Warp, FSR 3 / XeSS frame generation, NVOF FRUC (interpolation only, no D3D12), GPUDirect
for Video, batching two VFG timesteps (batch size 1), beam racing, shorter uneven slots.

## The main findings

- Every interpolating frame generator found (DLSS frame generation and multi frame generation, FSR 3, XeSS-FG, AFMF,
  Lossless Scaling, NVIDIA Video Frame Generation, RIFE and its relatives) holds the newest real frame until the frames
  generated before it have been shown. Reflex, Anti-Lag and XeLL shorten a game's render queue, which captured video does
  not have. Only prediction removes the wait.
- The extrapolators that match interpolation in published work use engine depth and motion vectors. The one published
  comparison of a video-only predictor (DMVFN in the GFFE paper) scores clearly worse than interpolation, so a fully predicted schedule cannot be offered as "no visible loss".
- A stable ~15 ms with one predicted frame needs processing + first generated frame + margins of about 4 ms. NVIDIA Video
  Frame Generation makes one frame per run (batch size 1) and costs about 6-7 ms at 4K by the upstream figure, which gives
  about 20 ms. The design that could make it cost about 1-3 ms (our estimate) is "motion once per pair at reduced
  size, then cheap full-size warps"; the same motion can feed the predicted frame. Estimate 14-20 ms; its picture quality
  against NVIDIA's model on 4K game footage is unproven and has to be compared (PushQualityProbe and by eye).
- The readout stops when Present returns. It is not input-to-photon latency and cannot be compared with LDAT or
  PresentMon's PC latency. What happens after Present (Independent Flip or composition, VRR) is logged as `[present-path]`.
