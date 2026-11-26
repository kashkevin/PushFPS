# Settings effect: requested / applied / executing (stream s12-settings)

Status: DRAFT for Package 17. Written 26 Nov 2026 by the second session of the stream. The first session's review documents
(S12_*_REVIEW*, SETTINGS_AUDIT) were lost; the table below was rebuilt by reading the code, not recovered. Nothing here was run
on a GPU: every test named is SIMULATED (pure functions, `tests/remoteplay/SettingsEffectTests.inc`, built in CI) or a QML
interface test (`tests/qml/quick/tst_push_settings_effect.qml`, built on the Windows runner; it passed in the `slipstream/dev5`
build, 727de002, for an older state of the stream; the state after the audit below has not been through a full build yet).

## What the three words mean

- requested: the switch or choice the owner made (settings, presentation settings, or the stored choice).
- applied: the engine accepted it: `snapshot.applied` / the settings revision the facts were measured with (`PushEffectFacts::revision`,
  `[settings] Applied revision=... changed=[...]`). The interface hides stale reasons while `applying` is true.
- executing: a MEASUREMENT of the last window (about 10 s, a young window shows the last completed one): generated frames presented,
  GPU stage samples (GPU timestamps) produced in this window, a counter of recorded dispatches, or the driver's own answer.
  Where nothing measures a setting the row says `unknown` (asked, not seen) or `not-measurable` (no counter exists), never `run`.
  One honest limit: the two "dispatch counters" (HUD lock composes, Edge Aegis adapt dispatches) count commands RECORDED by the
  engine thread; GPU completion is not read back. They prove the pass was part of the frames' command lists, not its GPU time.

Where it shows: log line `[settings-effect]` (at a change, then every 10 s; `name=asked/run|idle:reason`), the "running now" block
(`PushRunningNow.qml`), the grey status lines under rows (`PushEffectNote.qml`), all from the same facts
(`include/veyra/engine/PushSettingsEffect.h`, filled in `EngineController.cpp`, published as `snapshot.pushEffect`, read by
`QmlPlayerBridge::pushSettingsEffect()` in `src/ui/bridge/FrameGeneration.cpp`).

## Audit table (one row per `PushEffectSetting`)

"Test" = rule covered by a SIMULATED unit test in `SettingsEffectTests.inc` (rules) and/or the QML test (wording / colour).

| Row | Requested reads | Executing source (what must be true to read "run") | Kind of source | Test |
|---|---|---|---|---|
| multiplier | FG on and multiplier > 1 | generated frames presented in the window > 0 (`flow.counters.generatedPresented`); reason from the per-pair multiplier after each rule (adaptive, cap, Smoothness Mode); `pairs-skipped` when fewer generated frames were shown than the pairs offered give | measured (presented-frame counters) | rules, pairs-skipped, young window |
| adaptive | `fgAdaptive` | the engine chose a lower multiplier than the ceiling AND generated frames were shown. CHANGED in this session: before, the choice alone was enough | engine decision + presented frames | adaptive cases, reason order |
| pacingRepair | `pacingRepair` | frames re-timed > 0 (`PacingRepair::stats().repaired`) | counter of the engine's own action | yes |
| dedup | content mode with duplicate removal, capture source | frames observed by the duplicate remover in the window > 0 (`content-clock` when none repeated; effective, shown grey) | counter of frames examined | yes |
| hudLock | `hudLock` asked and the graph state is 1 | HUD lock compose dispatches recorded in the window > 0 | dispatch counter (recorded, not GPU time) | yes |
| vfgQuality | NVIDIA VFG is the method | every generated-frame position at the running multiplier produced GPU samples in the window (Fg1..Fg4 stage timestamps) and frames were shown | GPU timestamps | yes (incl. stopped stage) |
| vfgAutoRegulate | switch on | pairs that ran the faster model in the window > 0 (`vfgFastPairs`) | counter of the engine's own action | yes (every `needs-*` reason) |
| strictCadence | switch on | never `run`: live sources never use it (`file-playback-only`), file playback has no counter (`not-measurable`) | none (stated as not measured) | yes |
| flow (flow group) | always "on" (stored choices) | optical-flow stage GPU samples in the window > 0 | GPU timestamps | yes |
| edgeAegis | `fgMotionRefine` | CHANGED in this session: the refine's own adapt dispatches recorded in the window (`EnhanceGraph::flowRefineDispatches()`) > 0, with a flow-using method that estimates its own motion excluded. Before, a flow stage that ran was taken as proof the refine ran | dispatch counter (recorded) | yes (flow ran, refine not: `unknown`) |
| nvidiaLowLatency | presentation `nvidiaLowLatency` | the driver's answer to `NvAPI_D3D_GetSleepStatus` (read every 2 s) is "on" | the driver's report | yes |
| lowQueue | presentation `enabled` | CHANGED in this session: never `run`. The value shown before (`queueDepth` 1) is `PresentSink::queueLatency()`, which is derived from PushFPS's own pacing flag (the success of `SetMaximumFrameLatency`), not read back from DXGI: that was a configuration echo. Now `already-by-vsync` under Vsync, otherwise `not-measurable` | none | yes |
| vrr | `variableRefresh` | display sync is Automatic AND the driver last reported VRR active for the window's display (`displayVrr`) | the driver's report | yes |
| enhancement | AI picture enhancement layers on | NR stage GPU samples in the window > 0 (`works-at-lower-size` when it works below the output size) | GPU timestamps | yes |
| smoothness (NEW) | a Smoothness Mode target is set | it lowered the multiplier the other rules left and generated frames were shown; a target already met reads `nothing-to-choose` | engine decision + presented frames | yes |
| outputCap (NEW) | an output cap in force | it chose a lower multiplier and frames were shown; a cap that changed nothing reads `not-measurable` (how often frames are shown has no counter) | engine decision + presented frames | yes |

Smoothness and outputCap appear in the log line and in the bridge's `rows` map; there is no grey note under their controls yet.

## What the audit found (this session)

1. Two rows derived "executing" from configuration or from an inference: lowQueue (the app's own pacing flag echoed back) and
   edgeAegis (flow ran, so "refine ran"). Both are fixed as described in the table. Adaptive was tightened (needs shown frames).
2. Skipped pairs (item 4 of the task): `skippedPairs` was NOT lost in `[capture-timing]`: the field, its counter
   (`windowSkippedPairs`) and `triage.py`'s use of it are identical to `origin/ck/rel16`. What was missing was the same count next to
   the readout. `[settings-effect]` now carries `skippedPairs=` (admission skips on a capture source in the window, the same decision),
   the bridge exposes it as `skippedPairs`, and `pairs-skipped` (fewer generated frames shown than the pairs offered give) keeps its rule.
   The `[capture-timing]` line itself was not changed and has no test of its own in this stream.
3. Controls: all 14 original rows have a control. No placeholder rows were found in `qml/Veyra` (a search for placeholder / TODO wording
   found only text-field placeholders). Settings that affect the capture-card path and had a control but no readout row of their own:
   Smoothness Mode and the output cap (rows added, see table). Still without a row: display sync (Vsync / Automatic / Tearing: the
   presenter's own choice, no measurement), Lower delay (has its own state machine and `[fg-health]` / `[capture-timing]` fields), the capture card's
   own low-latency mode, SR / colour / restoration passes (GPU stages exist in `GpuStage`; no row). Not added: a row needs a measurement
   source, and inventing one was not done.

## G-SYNC and NVIDIA low latency wording (item 3)

What the program can observe: the driver's VRR state for the display (`displayVrr`, level 4 = reported active; his Package 15/16 logs
showed it never active in windowed mode), and the driver's `NvAPI_D3D_GetSleepStatus`. What it cannot observe: that the screen really
refreshes with the picture, or any latency change. The app's "NVIDIA low latency" is a per-frame Reflex-style `NvAPI_D3D_SetSleepMode` /
Sleep bounded on a capture card (the game does not run on this PC); it is not the control panel's "Low Latency Mode"
(`docs/NVIDIA_LOW_LATENCY_2026-10-28.md`). Texts changed: the variable refresh help (`ProPresentationRows.qml`), the hint of
`vrrHintText` (`FrameGeneration.cpp`: it said "the screen follows the picture" at driver level 3 and 4), the presenter's status detail
(`VideoPresenter.cpp`), the G-SYNC rows and restart rows and the NVIDIA low latency help (`PushQuickSetup.qml`, new help in
`NvidiaLowLatencyEditor.qml`), and the grey notes (`PushEffectNote.qml`). Unchanged: `[vrr-profile]` / `[present-path]` log lines.

## Open

- A full Windows build with the QML tests and the install test of this state has not run (ask the orchestrator for `slipstream/dev5`).
- `outputCap` / `smoothness` have no grey note in the interface yet.
- `hudLock` and `edgeAegis` "run" mean "recorded"; a GPU-time reading would need a stage timer for those passes.
