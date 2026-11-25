# Open-source review, 20 November 2026 (Package 8)

You asked us to study proven open-source projects before writing more code of our own, and to reuse
what fits. This is the short report: what we read, what we took, what we left out, and why.

**How to read it.** "Ported" means their code, placed in our files, with their copyright notice kept.
"Re-implemented" means our own code that follows a rule they publish; nothing was copied. All of it
was read as source on Linux. None of these projects was built or run, and nothing below has run on a
graphics card or a PS5. Every statement about our code is **as committed at 01:25 UTC on
21 November** on the team branches (`exp/s5-*`).

`TODO(orchestrator)`: replace "as committed at 01:25 UTC" with the final merge commit, and add the
final commit id of each port (team branch heads then: chiaki `99337661`, restore `8cab0023`, present
`cdc32ad5`, input `a73f432b`, upstream `ce4514c2`, nrrun `9f7046d2`).

## What we studied

| Project (commit studied) | Licence | What we looked at | What we reused, and where | What we did not take, and why | Risk |
|---|---|---|---|---|---|
| **chiaki-ng**, the PS5 Remote Play library. Our pin `0e16950165f0`, read up to `43728af2e639` (91 commits later) | AGPL-3.0 with the OpenSSL exception, as today | Packet recovery, connection stability, input sending, audio receive | **Ported** five upstream fixes as patches `0005` to `0009` in `scripts/remoteplay/patches/`: a freed-packet fault, a send-buffer fault when a connection drops, buffer bounds at session start, the login PIN kept out of logs, and a larger resend buffer (off: `VEYRA_EXP_RP_CHIAKI_SEND_BUFFER=1`) | Moving the pin (more to redo, no known gain at home). The trigger-effects change (alters every connect request; untested on a console). Internet (PSN) and microphone code we do not use | Low for `0005`-`0008`. Medium for `0009`, hence the switch |
| **libplacebo**, a video renderer. `89b04663135b` | LGPL-2.1-or-later (may be used inside GPL-3.0) | Debanding, dither, scalers, anti-ringing, the order of steps | **Ported** its debanding and random generator into `include/veyra/engine/PushClean.h` and `shaders/PushClean.hlsl`, and its fixed ordered dither (rounding that keeps black exact) into `shaders/ScaleBlit.hlsl`. Its order is ours too: clean first at the stream's size, then upscale | Lanczos scaler: not done yet. Error diffusion, peak detection, film grain, temporal dither, frame mixing: they need history or metadata, or flicker. It has **no** deblocking, de-ringing or denoising: those parts of the clean-up are our own | Low at default strength; a high strength flattens dark texture |
| **moonlight-qt**, a game-stream client. `2dc63d1f1385` | GPL-3.0 | Frame queues, audio timing, swap chain, overlays, statistics | **Re-implemented** three rules, each off by default: audio timed from its own arrival, not from video timestamps (`include/veyra/sink/AudioOwnClock.h`, `VEYRA_EXP_RP_AUDIO_OWN_CLOCK=1`); a video window that is not cut into shapes (`include/veyra/engine/PushCoverRegion.h`, `VEYRA_EXP_PRESENT_RECT_REGION=1`); exact counting of frames reaching the screen (`include/veyra/engine/PushScanoutAudit.h`) | Its frame pacer (ours already holds fewer frames). Its five-buffer swap chain. Its vblank thread (no fixed vblank under variable refresh). Zero copy chosen by GPU maker | Own clock: lip sync rests on arrival times. Region: a strip of picture is hidden while a menu is up |
| **Sunshine**, a stream host. `de1e671e815c` | GPL-3.0 | Capability detection, connection monitoring, congestion | No code. Its rule, learn what the hardware can do by **trying it**, never from a list of models, is the rule behind the automatic neural-layer size (`pushNrAutoState` in `include/veyra/engine/PushNrRuntime.h`) | Packet pacing, error-correction choice, loss feedback: the sender's job, and the PS5 is the sender | None |
| **xcontrol**, mouse to stick. `c1921818b709` | MIT | Mouse-to-stick maths, timers, smoothing | One idea **re-implemented**: a cap on stick tilt, `maxTilt` in `include/veyra/remoteplay/KbmInputProfile.h` | Its smoothing filter, always on, which adds delay. Speed-dependent gain | None taken |
| **PadForge**, mouse to stick. `fa8a76ebebae` | MIT | The same | Nothing. Its dead-zone and curve formula is the one we already had | Its spring filter (38 ms time constant in its source). Its inner dead zone, which drops slow aim. Its calibration, which measures the hand, not the game | None taken |
| **rtx-upscale**, video clean-up. `ba2a8c431f63` | **No licence file** | Clean-up network, RTX upscaling steps | Nothing copied. One note kept: which NVIDIA quality levels include artifact removal | All code and model weights: no licence; frames go through the CPU; 8-bit only | Legal risk if copied; none as used |
| **Veyra 2.1.5-2.2.0 and PushFPS to 3.2.10**, our upstreams | GPL-3.0 | Neural-layer, input, HDR and pacing changes since our base | **Ported** two stability fixes for the neural runtime (`src/ngx/DlssNrRuntimeAdapter.cpp`, `src/pipeline/EnhanceGraph.cpp`) and a trigger jitter filter (`include/veyra/remoteplay/TriggerFilter.h`, off: `VEYRA_EXP_RP_TRIGGER_FILTER=1`). **Re-implemented** the controller-queue expiry rule (`include/veyra/remoteplay/ControllerQueue.h`). The two fixes and the expiry rule are **on**; a `VEYRA_TEST_RP_LEGACY_*` switch restores the old behaviour | A full merge (47 conflicting files). HDR peak and SDR code-value changes: they change the picture and need your screen. Predictive frame generation: barred | Low. The trigger filter changes the values sent, hence off |

Upstream has nothing new that makes the neural layer cheaper.

## What the studies changed in Package 8

1. The clean-up stage uses libplacebo's proven debanding and dither instead of an invention.
   Deblocking, de-ringing, grain removal and HUD protection are our own; deblocking is off by default.
2. Mouse input ports nothing: both mouse projects smooth the stick, and smoothing is delay. Our
   immediate path stays, plus one cap.
3. Audio got a new experiment after moonlight-qt's rule. Your logs of 20 November (Wi-Fi) showed
   sound falling up to half a second behind on the default path.
4. The slow first seconds on screen in those logs have a suspect: our own rounded cut-out in the video
   window. One switch tests it.
5. Five library fixes arrive as small patches instead of a risky version jump.

## Licences and attribution

- Every port is recorded in `THIRD_PARTY_NOTICES.md` (libplacebo, chiaki-ng, Veyra). Each ported
  part keeps the original notice above it.
- xcontrol, PadForge (MIT): nothing ported, so no notice is owed. The idea's source is named in
  `docs/KEYBOARD_MOUSE.md`.
- moonlight-qt, Sunshine: nothing copied. Sources are named in
  `docs/PS5_REMOTE_PLAY_EXPERIMENTS.md`.
- **NVIDIA's neural runtime (`nvngx_dlssnr.dll`), facts.** NVIDIA has published no public SDK or
  licence for it: partner developers only. It ships inside partner games and RTX Remix. We never
  commit, download or modify it. The app uses only a copy it has checked as an NVIDIA-signed
  original. The installer keeps it only on your explicit yes.
- **Open questions.** We found no NVIDIA text that lets another program load or pass on this file,
  and that includes the signed copy in upstream's package. Whether any terms cover it is unknown.
  No verdict is given here, and this is not legal advice. Details: `docs/NEURAL_RENDERING_RUNTIME.md`.

`TODO(orchestrator)`: confirm the merged `THIRD_PARTY_NOTICES.md` holds the three port entries and
the NVIDIA entry.

## Not done, recommended later

- A Lanczos-3 scaler with anti-ringing (libplacebo's published filter): a faithful 1080p-to-1440p
  step for every GPU maker.
- Overlays drawn inside the swap chain (moonlight-qt's way), if the region experiment confirms the
  cause.
- A dedicated input poll thread with fixed deadlines, only if logs show late polls.
- Asking NVIDIA's runtime itself whether the card and driver support it, as NVIDIA's open RTX Remix
  code does.
- chiaki-ng's trigger-effects change and Veyra's HDR fixes, with the next Veyra merge.

`TODO(orchestrator)`: Windows compile and test results for the ports; independent review verdicts.
