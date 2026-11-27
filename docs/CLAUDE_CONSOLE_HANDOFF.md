# Slipstream (PushFPS) development handoff for a new Claude Code session

Last updated: 22 November 2026, about 08:40 UTC, by the Claude Code (Console) session that finished Package 12
(first written about 01:45 UTC by the orchestrating Claude session). **State (25 Nov 2026): Packages 11 to 16 are delivered;
the own frame generator is in work for the next package (section 0).** This file is the recovery point: a fresh Claude Code session with no access to the earlier
conversation should be able to resume from it alone. Each section marks what is **verified** (built, tested,
reviewed, with a run link) and what is **planned or incomplete**.

"Slipstream" is the internal codename of a private derivative of PushFPS (DosukaSOL/PushFPS, built on Veyra by
Likely7; GPL-3.0, the Chiaki remote-play part also AGPL-3.0) for low-latency PS5 Remote Play and HDMI
capture-card frame generation on the owner's PC: RTX 5070, Ryzen 7 8700F, 32 GB, 2560x1440 180 Hz OLED (G-SYNC
compatible), Windows 11, PS5 and PC on wired Ethernet. The owner is a beginner developer: give short, plain,
action-oriented answers, complete working builds with honest footnotes, and never claim hardware results he has not
measured himself.

Read the repository's own `CLAUDE.md` first (project rules: translations keyed by the Simplified Chinese source
string, QML loaded from disk, never commit NVIDIA/AMD/Intel SDKs or runtimes, keep upstream mergeable). This file adds
the Slipstream workflow on top.


> **UPDATE 23 November 2026, ~18:00 UTC (orchestrating session, resumed at the owner's request): development is no longer
> paused.** Read section 0 first; sections 1-9 below are the record up to Package 12.

> **UPDATE 26 November 2026 (Claude Code Console session, second takeover): read section 0-A first.** The orchestrating
> (Claude Desktop) session ended at its usage limit after Package 16 with five streams in work. Section 0-A is the
> recovered state and the way work is organised now; section 0 and below are the record up to Package 16.

## 0-A. After Package 16: recovery on 26 November 2026 and Package 17 (Console session)

### 0-A.1 What was recovered, and from where

- **Nothing was running.** Last push 26 Nov 03:41 UTC (`ck/s12-hud`); no workflow run in progress or queued; no pull
  requests; no agents alive. Recovery read every remote branch (117), the Actions run list since Package 16 and the
  failing job logs.
- **The private `exp/*` branches and the private review / research documents are NOT available** (they lived on the
  orchestrating session's machine; no bundle is on the owner's PC). Not recoverable: `S12_HUD_REVIEW1`,
  `S12_PACE_REVIEW2`, `S11_OWNFG_REVIEW2`, `SETTINGS_AUDIT`, `P16_REAL_LOG_ANALYSIS`, `OBJECTIVE_COMPARISON`,
  `HUD_GHOSTING`. The public `ck/*` snapshots (squashed code, one commit on `main`) are the only source. Outstanding
  review items were reconstructed from code, drafts, tests and CI, not from those documents.
- **Common base of every stream below: the tree of `ck/rel16` `a126d619`** (= Package 16's code before its package
  text). The snapshots share no git history, so merge with an explicit base:
  `git merge-tree --write-tree --merge-base=origin/ck/rel16 <a> <b>` (26 Nov: settings + pace conflict only in the three
  kit log allow-lists, `collect-diagnostics.ps1`, `run-tests.ps1`, `report.py`, where each stream appends its log tag).

| Stream | Newest public state at recovery | Its CI at recovery | Last full Windows build |
|---|---|---|---|
| s12-settings (requested / applied / executing readout, settings audit) | `ck/s12-settings` `d3c29fd0` | checks, remoteplay-checks, gpu-quick green | `slipstream/dev5` `727de002` (older state), run 45746321588: everything passed up to the last step ("Lower delay" end-to-end on WARP, the same step Package 16 shipped with failing) |
| s12-pace (on-screen cadence, frame-content probe, 4K -> 1440p downscale, 2X duplicate fix) | `ck/s12-pace` `e4d2067e` **plus** the `VideoPresenter` out-of-line constructor / destructor that existed only on `slipstream/dev6` `8781755b` | checks, remoteplay-checks, gpu-quick green | `slipstream/dev6`, run 45746477289: compiled; **two QML tests failed** in `tst_push_quick_setup.qml` (`test_steady_detail_is_off_by_default_and_a_preference`, `test_unfocused_notice_switch_is_a_preference`: actual `undefined`) |
| s12-hud (HUD Guard: crosshair / HUD kept out of frame generation; opt-in inside Live HUD Lock) | `ck/s12-hud` `547dfa1c` (the last commit before the cut-off: the aim-point fix) | checks, remoteplay-checks, gpu-quick green; e2e-quick green on `ck/e2e-s12-hud` `8dd08a69` (older state) | none of its own |
| ownfg (own generator "PushWarp"; separate from Package 17) | `ck/ownfg` `83209dd6` | checks, remoteplay-checks green; gpu-quick green on `ck/gpu-ownfg` `0c3bdfea` | `slipstream/dev4` `b05832fd` (older state), run 45740796232: failed only at the same last end-to-end step |
| bench (offline quality bench, `tools/quality_bench/`) | `ck/bench` `7e413012` | checks, remoteplay-checks green (the bench itself never ran in CI) | not applicable (tools only) |
| ffxcheck (the FidelityFX start-up self-check's test picture) | `ck/ffxcheck` `8f96aa92` | green; gpu-quick green on `ck/gpu-ffxcheck` | none |
| First Package 17 integration attempt (settings + pace + hud, older states) | `slipstream/dev3` `592a90f0` | - | run 45745028541: **did not compile** (`std::unique_ptr<PushFrameThumb>` deleted where the type is incomplete, in `EngineController.cpp` / `EngineControllerImage.cpp`; fixed afterwards on dev6 only) |

- `ck/gpu-*` and `slipstream/dev*` lanes fail the `checks` workflow's "Translations complete" step by construction
  (those lanes carry the catalogue without the streams' new strings); the plain `ck/<stream>` lanes pass it.
- A push / CI timeline of every lane since Package 16 can be rebuilt from the Actions API (`/actions/runs`).

### 0-A.2 How work is organised now (owner's PC)

- The owner's instruction of 26 Nov: work independently, specialist teams in separate worktrees with independent
  reviewers, continue Package 17 from the existing work, keep the own generator off Package 17's critical path, never
  touch his installed application or interrupt his play, no hardware claims from simulations or CI.
- His priorities, in order: frame-generation quality and motion clarity; crosshair / HUD ghosting; smooth, even
  displayed frames; functional, truthful settings; 90 FPS visibly better than 60; downscaling without needless detail
  loss; 2-3 ms less delay only where it costs none of the above (Package 16 reads about 30-33 ms at 3X; aggressive
  delay reduction is no longer the first objective). Setup: 3840x2160 HDR10 capture of a 30 FPS game, 2X and 3X,
  2560x1440 180 Hz OLED.
- **His PC has no C++ compiler, cmake, pytest, numpy or `gh`.** C++ compiles and runs only in GitHub Actions. Python 3.14
  (standard library) is there. It is his gaming PC: nothing heavy runs locally.
- Local layout (beside the main clone, whose branch is left alone): one git worktree per stream in a `PushFPS-wt`
  folder next to the clone, on local branches `work/s12-settings`, `work/s12-pace`, `work/s12-hud`, `work/ownfg`,
  `work/bench`, and `work/handoff-p17` for this file. Each work branch sits on top of its `ck/<stream>` head, so a
  check is a fast-forward push `git push origin HEAD:ck/<stream>`; `ck/gpu-<stream>` (WARP GPU tests) and
  `ck/e2e-<stream>` take forced pushes (scratch lanes). Full builds: the integration owner pushes to a
  `slipstream/dev*` lane with the lane-only CI commit (take `.github/workflows/build-windows.yml` from an existing
  dev lane: it adds the `slipstream/**` trigger).
- Lanes: `slipstream/dev3` = Package 17 integration, `dev5` = settings, `dev6` = pace, `dev4` = own generator.
- Team briefs, the CI helper and downloaded logs are private orchestration files outside the repository; they are
  not needed to resume (this section and each stream's `docs/drafts/*.md` are).

### 0-A.3 Team assignments started 26 November 2026 (state: IN WORK unless a line below says otherwise)

1. **Settings** (`work/s12-settings`): every "executing" value from a runtime measurement, never from configuration
   (else "not measured"); audit table in `docs/drafts/SETTINGS_EFFECT.md`; missing controls; G-SYNC and NVIDIA low
   latency wording limited to what the program observes; skipped-pair reporting restored; QML tests; the install test
   made to load the new screens.
2. **HUD Guard** (`work/s12-hud`, draft `docs/drafts/HUD_GUARD.md`): confirm the aim-point fix; protection over
   less-textured backgrounds without erasing moving objects; soft crosshair edges; HDR10 / changing crosshair / fast
   camera / dark background / subtitles / thin objects scenes; opt-in inside Live HUD Lock; the console's own crosshair
   pixels kept; the graphics-queue wait of the guard's update + pre-clean reviewed (alternatives only if provably not
   later).
3. **Pace and downscale** (`work/s12-pace`, draft `docs/drafts/PACE_DOWNSCALE.md`): the two QML failures; an unbiased
   on-screen pacing measure for 2X against 3X; the accepted 2X duplicate fix kept under test; the linear-light (HDR
   correct) 3840 -> 2560 filter and sharper variants compared; `[frame-probe]` without false alarms in slow camera
   movement; window-occlusion checks off the presentation thread.
4. **Own generator** (`work/ownfg`, drafts `OWN_FG_DESIGN.md`, `OWN_FG_OWNER_DRAFT.md`): NOT part of Package 17. Never
   selected automatically; NVIDIA VFG and PushWarp both selectable. Root causes of "67 distinct frames of 90" and of
   the 32-px motion block limit; HUD Guard interface; repeating patterns; HDR10; slow camera movement.
5. **Bench** (`work/bench`, drafts `QUALITY_BENCH.md`, `OBJECTIVE_COMPARISON.md`): the bench run in CI (a Linux
   workflow on `ck/bench*`); 30 / 60 / 90, old against new downscale, PushWarp against the block-motion MODEL (NVIDIA's
   generator cannot run offline); results in four classes that are never mixed: simulation, software-renderer tests,
   real GPU measurements (only the owner's logs), display measurements (none exist).

**State at 26 Nov ~17:40 UTC: the teams were interrupted by an API billing error ("credit balance is too low";
the own-generator team stopped first). Their first batches are UNREVIEWED and partly failing CI. Resume from these
heads; do not treat any of it as accepted.**

| Stream | Local branch head | Pushed to | CI on that head | Note |
|---|---|---|---|---|
| settings | `work/s12-settings` `39d5e8d9` (one commit on `d3c29fd0`: executing only from measurements for low queue / Edge Aegis / adaptive, skipped pairs in `[settings-effect]`) | `ck/s12-settings` | `checks` #481 FAILED (run 45868596236: read which step; most likely translations or QML syntax); remoteplay-checks #448 was still running | fix `checks` first |
| pace | `work/s12-pace` `df86edb2` (only the recovery commit) | not pushed | - | **uncommitted work in the worktree** (PushDownscale.h, PushOnScreenCadence.h, VideoPresenter.*, PresentBlit.hlsl, PushGpuTests.cpp, QuickSmokeTests.cpp, OnScreenCadenceTests.inc, new `tools/pace_downscale/`): never compiled; keep it (commit as WIP before anything else) |
| hud | `work/s12-hud` `03795b79` (one commit on `547dfa1c`: aim anchor over flat pictures, scene matrix) | `ck/s12-hud` | remoteplay-checks #446 FAILED (run 45868463293); `checks` green | the new rule or its tests do not pass the core tests yet |
| ownfg | `work/ownfg` `04829397` (one commit on `83209dd6`: reference refinement rules, duplicate tests) | `ck/gpu-ownfg` only | not checked on `ck/ownfg` | CPU reference only, never compiled by the core-test lane |
| bench | `work/bench` `73fd6765` (workflow `quality-bench.yml`, data-driven downscale kernels) | `ck/bench` | quality-bench #2 green on `f955fd98` (run 45867581929: the first time the bench ran in CI; its artifact holds the report); #3 on `73fd6765` was running | `OBJECTIVE_COMPARISON.md` not written yet |

### 0-A.3b Third takeover, 26 Nov 2026 ~18:50-21:30 UTC (state: read this before the tables above)

How this session had to work (it matters for the next one):
- A session started in one worktree can only reach the others after the owner adds `C:\Users\kashh\PushFPS-wt` as a
  working directory. Background sub-agents are refused the shell; **foreground** sub-agents work. CI is read without `gh`
  from `https://api.github.com/repos/kashkevin/PushFPS/actions/runs?branch=<lane>&per_page=4`, `.../runs/<id>/jobs` and
  `.../check-runs/<job id>/annotations` (the workflows repeat error and FAIL lines as annotations; job logs need a sign-in).
  `origin` is `kashkevin/PushFPS`, not the upstream project.
- **Windows trap in `scripts/i18n/extract.py`:** it sorts paths case-insensitively on Windows, so a catalogue regenerated
  on this PC attributes some strings to a different `"src"` than Linux CI does and `checks` fails its byte comparison
  (this was `checks` #481: `src/ui/bridge/Preferences.cpp` instead of `src/ui/QmlPlayerBridge.cpp`). After regenerating,
  revert `"src"`-only changes, or sort by `as_posix()` when comparing.
- `remoteplay-checks` takes 35-45 minutes when it passes (the kit dry runs); a failing run ends in 5-10.

| Stream | Head (local = pushed) | CI on that head | What happened |
|---|---|---|---|
| settings | `45173758` on `ck/s12-settings` | `checks` #484, `remoteplay-checks` #450 green | catalogue attribution fixed. Open review item: the `smoothness` / `outputCap` rows are in the log line and the bridge only (the draft says so); no note in the interface |
| pace | `c8e53472` on `ck/s12-pace` and `ck/gpu-s12-pace` | `checks` #489, `remoteplay-checks` #455, `gpu-quick` green | the interrupted team's uncommitted work was committed as found (`ca1e1f96`), then: a broken `printf` in `OnScreenCadenceTests.inc`, exact sums in the spread estimators, a slow-pan test for `[frame-probe]`, the MSVC annotation filter, `docs/drafts/PACE_DOWNSCALE.md` (new). The two QML failures: the test mock lacked `pushSteadyDetail` / `pushUnfocusedNotice`, already added by the batch (verified only by a full build). Open: downscale kernel table never run (no numbers, sharpest variant not chosen); window-occlusion checks off the presentation thread not started |
| hud | `0d176349` on `ck/s12-hud` and `ck/gpu-s12-hud` | g++ / sanitizers green on `55b2b282`; MSVC and `gpu-quick` on `0d176349`: read them | the aim anchor protected nearly a whole still textured picture (4 failing tests): now only over a flat surround (`anchorFlatMax`), and off for a pair when anything changes in the aim window + 64 px (`anchorMargin`, state bit 28); scans behind explicit shader branches; scene numbers printed as CI notices. Known limits pinned by tests and in `HUD_GUARD.md`: soft-edged crosshair fringe slightly worse than raw (+3 codes), HDR highlights 87 % exact, dark / fast-camera numbers never read. **Unknown: whether the anchor ever arms on real pictures with grain** |
| ownfg | `04829397` on `ck/ownfg`, `ck/gpu-ownfg` | green on `ck/gpu-ownfg` (#447, `gpu-quick` #47) | not worked on (not part of Package 17) |
| bench | `73fd6765` on `ck/bench` | `quality-bench` #3 green | not worked on; `OBJECTIVE_COMPARISON.md` still unwritten (its artifact needs a sign-in to download) |

**FINAL STATE of that session (27 Nov 2026, about 01:30 UTC). Package 17 is prepared, NOT packaged:**

| What | Where | State |
|---|---|---|
| settings | `work/s12-settings` = `ck/s12-settings` `269671fb` | green; + the install check's variable clash (`$qmldir` overwrote `$qmlDir`: PowerShell names ignore case; builds #104 / #105 failed there) |
| pace | `work/s12-pace` = `ck/s12-pace` `6d9ecc50` | green |
| hud | `work/s12-hud` = `ck/s12-hud` = `ck/gpu-s12-hud` `d0aebca5` | + the busy scan only for pixels with `run >= anchorFrames`; parity and core tests passed inside build #107 and `remoteplay-checks` #477; read the lanes' own runs |
| integration | `work/rel-17` = `ck/rel17` `853ea0c1` (snapshots: settings `c7e7ccc9`, pace `1f94f3f1`, hud `f1ad7331`, ffxcheck `031c623f`) | `checks` #523, `remoteplay-checks` #477 green |
| full build | `slipstream/dev3` `f6b30e87` (integration + lane-only CI commit), **build-windows #107, run 45920935731** | every step passed (MSVC /W4 /WX, unit, WARP GPU, going-live, MP4, both QML suites, packaging, install test, artifact upload) **except the last: "Lower delay" end-to-end 435 passed / 17 failed** (Package 16's #92: 564 / 5) |
| package source | `work/package-17` = `ck/package-17` `264281a1` = the built tree + package text only (`labels/17.txt`, `CHECKED.txt` filled, status section, label 17, documents) | `test_slipstream_package.py` passes; no `<angle brackets>` left |

**UPDATE, about 03:10 UTC: `e2e-quick` #45 (HUD Guard off, same tree) finished: 507 passed / 13 failed, the
"pipeline keeps up" check passes and "Lower delay" enters again. So the cause IS HUD Guard's per-real-frame work on the
software renderer; `CHECKED.txt`, `17.txt` and the status section say so (`work/package-17` = `ck/package-17`, newest
commit "Package 17 text: the end-to-end finding is HUD Guard's work on the software renderer"). What it costs on a
graphics card is still unmeasured. The 13 left are timing comparisons, exit kinds and the `ownerHdr` format check, which
failed in all three runs of that lane (not explained; not compared with Package 16's lane). The decision that remains is
the owner's: ship HUD Guard on with Live HUD Lock as built (the text warns and names the undo line), or make it opt-in /
cheaper first (then rebuild and re-read the end-to-end counts).**

**The finding as first written (before #45 finished):** with the owner's settings ("Lower delay" off, 3X,
Smoothness Mode 90, Live HUD Lock, HDR10) the software renderer no longer keeps up since this package: 0.41-0.68
generated frames per real frame where the test asks for 0.9 (`e2e-quick` on `ck/e2e-rel17-gate`: #43 438 / 25, #44
469 / 18 after the busy-scan change; HUD Guard's own end-to-end test 70 / 0 in both). Likely cause: HUD Guard's work in
every real frame (it is ON whenever Live HUD Lock runs, which the owner uses); WARP does it on the CPU. **Not proven:
`e2e-quick` #45 on `ck/e2e-rel17-gate-nohud` (run 45923810826, the same tree with
`VEYRA_TEST_RP_LEGACY_HUD_GUARD=1` in the workflow, a scratch lane) decides it; read its annotations
(`.../actions/runs/45923810826/jobs`, then the check-run's annotations: the notices carry the PASS / FAIL counts and the
first FAIL lines).** If it passes like Package 16, HUD Guard is the cause and the choice is: ship it on with the warning
that is in `17.txt`, ship it off by default, or make it cheaper first. If it still fails, look at the s12-pace parts that
are on by default (frame probe, extra frame-statistics readings: `VEYRA_TEST_RP_LEGACY_FRAME_PROBE`, `_STATS_READINGS`).
Update `CHECKED.txt` / `17.txt` / the status section with the answer before packaging.

**To package (needs `gh` or a token; this session had neither):**
`git push origin 4f802db1:refs/heads/archive/package-16`; make the package commit on Package 16's
(`git commit-tree "work/package-17^{tree}" -p origin/slipstream/package -m "Package 17 package source"`), push it to
`slipstream/package` (fast-forward), cancel the runs that push starts, then
`gh workflow run slipstream-package.yml -R kashkevin/PushFPS --ref slipstream/package -f package_label=17 -f experimental_run=45920935731 -f experimental_branch=slipstream/dev3`.
If code changes first, rebuild on `slipstream/dev3` and use that run instead.

Not done in this session: a second independent review of the fixes and a review of the merged package (said in
`CHECKED.txt`); the settings notes for `smoothness` / `outputCap`; pace's kernel table and the occlusion check; anything
on ownfg and bench. CI now repeats what stops the install test and the PASS / FAIL counts of the end-to-end steps as
annotations (`build-windows.yml`, `e2e-quick.yml`).

**Later the same session (to about 23:00 UTC), superseded where the table above differs:**
- Pace is now `6d9ecc50` (`ck/s12-pace`): the `present-shrink` WARP test allows 3 codes of 1023 for the two unclamped
  negative-lobe kernels in HDR10 (build-windows #102 and #103 read 2.02 for Lanczos-2 at 1.5x where `gpu-quick` on another
  machine stayed within 1.5; the filter itself was not changed). HUD's MSVC job is green on `0d176349` (#461).
- The motion-check correction (`ck/ffxcheck` `8f96aa92`, snapshot `031c623f`) is merged into the integration too: it is
  planned content and was in none of the three streams. No conflicts.
- Full builds on `slipstream/dev3`: #102 / #103 stopped at that one WARP check; **#104 (`28eeabbf`, run 45898482220)
  compiled with MSVC /W4 /WX and passed the unit tests, the WARP GPU tests, going-live, MP4 and BOTH QML suites (so the
  two `tst_push_quick_setup.qml` failures are gone), then failed in the install test with a bare exit code**. The step
  now repeats what stops it as an annotation (`work/rel-17`); #105 is the build to read.
- **Package branch `work/package-17` = `ck/package-17`** (rel17 worktree; switch branches there with `git checkout`):
  the integration + Package 16's package text (`1bccf71f` = snapshot of `ck/package-16`) + `labels/17.txt`, the Package 17
  section of `docs/SLIPSTREAM_STATUS.md`, default label 17, the three stream drafts in `DOC_FILES`, and a drafted
  `CHECKED.txt` whose build facts are still in `<angle brackets>` (the packager refuses to package until they are filled).
  `python -B packaging/slipstream/test_slipstream_package.py` passes. After a good build: merge `work/rel-17` into it,
  fill `CHECKED.txt`, then the package commit goes on `origin/slipstream/package` (`4f802db1`, keep it as
  `archive/package-16` first) as in section 6.
- **The package run itself needs a signed-in GitHub call** (`gh workflow run slipstream-package.yml ...` or the REST
  dispatch, and cancelling the builds a push to `slipstream/package` starts). This PC has no `gh` and the session had no
  token: that step is the owner's, or needs him to provide a way to authenticate.
- Honest gaps for the package text: the fixes made in this session's reviews were not reviewed a second time and the
  merged package was not reviewed as a whole (`CHECKED.txt` says so).

**Integration:** `work/rel-17` = `ck/rel17`: the three streams as snapshot commits on `ck/rel16` (`95ad6e0e`
settings, `63527bf4` then `1f94f3f1` pace, `98a8393a` hud), merged with real merge commits, so later stream commits merge the same way
(`git commit-tree <stream>^{tree} -p <previous snapshot of that stream>`, then `git merge`). Conflicts were only lists both
sides append to: the three kit allow-lists, `PushLegacy.h` / `LegacyTests.inc` (order: `HudGuard17`, `HudGuardClean17`, then
the four s12-pace parts; `kPushLegacyFirstAfter16 = HudGuard17`), the `gpu-quick.yml` case list, one member each in
`EngineController.h` / `EnhanceGraph.h`, the `CoreTests.cpp` includes. A script confirmed that every line a stream added
is present. Catalogue: 4411 entries, 0 missing, 0 placeholder problems. Full build: `slipstream/dev3` `d2fd02f7` (the
integration + the lane-only CI commit); the lane's old head is kept as `archive/592a90f0`.

### 0-A.4 Package 17: what it is to contain and what remains before packaging

Planned content: the settings readout, the on-screen (`[cadence]`) and frame-content (`[frame-probe]`) checks, the
accepted 2X fix, the improved downscale, crosshair protection (HUD Guard, opt-in), the motion-check correction. Not in
it: the own generator.

Steps (none done at the time of writing; update this list as they complete):
1. Each stream: fixes, CI green on its `ck/` lanes, an independent review by a reviewer who did not write it, fixes,
   second look.
2. Integration branch `work/rel-17` = `ck/rel16` tree + settings + pace + hud with the explicit merge base; verify per
   file that every line each side added is present once; translations (`extract.py --check` 0 / 0).
3. Full strict Windows build on `slipstream/dev3` (MSVC /W4 /WX, unit, WARP GPU, QML, install test). The end-to-end
   "Lower delay" step's known timing failures on WARP must be compared with Package 16's build #92 (564 passed / 5
   failed) and stated, not hidden.
4. Package text `packaging/slipstream/labels/17.txt`, `CHECKED.txt`, `docs/SLIPSTREAM_STATUS.md` section, default label
   in `slipstream-package.yml`; integration review; `archive/package-16` = `4f802db1` before `slipstream/package`
   moves; package run as in section 6.
5. Nothing is hardware-verified until the owner's own log and eyes say so.

## 0. Since Package 12: Package 12A and the "delay90" stream

**Package 16 (delivered 25 Nov 2026, ~14:30 UTC, by the orchestrating session; verified from GitHub):**
https://github.com/kashkevin/PushFPS/actions/runs/45633587347 (artifact `Slipstream-test-package-16-2026-11-25-34`,
1416 files, 596.5 MB, inner zip SHA-256 `0828b8d36f96a63ace0a89e120db8436e22002132af68c6b725d4442e64a11b4`; package and
verify jobs passed; expires 23 Feb 2027). Experimental build #92 (run 45625070013, `slipstream/dev3` `dd24a030`,
`archive/dd24a030`): every step passed **except the last** (the "Lower delay" end-to-end engine test on WARP: 564 checks
passed, 5 failed, all timing comparisons or the harness's own set-up; no device removal). That was shipped on purpose,
stated in CHECKED.txt and the label; the owner's rule: software-GPU timing gates are no proof of real performance.
Package source `slipstream/package` `4f802db1` (parent `129c1377` = Package 15, kept as `archive/package-15`); public
tree `ck/package-16` `03c54a56`; private `exp/pkg-16` `41244bde` = `exp/rel-16` `d9b65e9c` + packaging commits.
- **Why Package 15 did nothing on his PC (from his own log, first hardware data of the capture path):** "Lower delay"
  never ran (`lowDelay state=not-here refusal=smoothness-mode predictedPairs=0`): our own exclusion with Smoothness Mode
  (his 90 FPS target) and Live HUD Lock. Measured there: readout 31 ms = picture 4.7 + first generated frame 2.0 +
  schedule wait 22.1 + margin 2.2; Present to vblank about 10 ms more; a pair of capture stalls after every quarter hour.
- **What Package 16 changes:** "Lower delay" (opt-in row; reset to OFF once) runs with Smoothness Mode and HUD Lock /
  exclusion boxes; P's motion from AMD FidelityFX optical flow behind a start-up self-check on the user's card (Package
  15's FP64 GPU DIS removed the device on WARP and is off); VFG refusal ladder; tolerant entry / exit; fault memory
  (`runtime_local\lowdelay-flow-check.txt`: `failed` after a failed check or two device removals in a row while P ran).
  Always on: `[fg-health]` / `[present-path]` / `[capture-timing]` additions, "to screen" lines, the stall witness
  (`[capture-stall]`, `[capture-stall-cause]`, private `[stall-witness]`). Undo: `VEYRA_TEST_RP_LEGACY_PACKAGE15 = 1`
  (interface lines, witness, reset, the row's schedule; NOT the always-on log figures).
- **Reviews:** `REL16_REVIEW` (7 text MUSTs, 4 code SHOULDs: all fixed), `REL16_DELTA_REVIEW` of those fixes (2 new
  MUSTs: fixed in `7fa0539f`, `0330933e`, `06b5a43d`, `d9b65e9c`; NOT reviewed a third time).
- **Nothing in it is hardware-verified.** After he installs: read his log (read-only, lightly, he may be playing) for
  `[push-predict] flow source=fidelityfx selfCheck`, `[fg-sched] lowDelay` (state, predictedLate, exits, aloneMode),
  `[capture-timing]` delayWaitMs / predictGpuMs, `[fg-health]` late / poll / toScreen, `[capture-stall-cause]`,
  `[vrr-profile] start-state`. No delay figure may be promised to him before his log shows one.
- **Own frame generator ("PushWarp", branch `exp/s11-ownfg`, for the package after 16):** own I(t) frames from one
  FidelityFX field per pair; a `FrameGenerationBackend::PushWarp` choice next to NVIDIA VFG (A/B at identical settings,
  `generator=` on the evidence lines); own-frame source inside the predicted schedule; a causal schedule B, P, P as a
  labelled experiment. Review 2 (`S11_OWNFG_REVIEW2`): accept after MUST 1-5 (it must never be selected automatically,
  e.g. by "Optimise for my PC"; cost fields must mean the same for both generators; never built as the app yet: first
  full build on `slipstream/dev4`). Fact to keep straight: in the standard schedule (I, I, B) PushWarp cannot shorten the
  22 ms schedule wait; only the predicted and causal schedules can lower his 30-32 ms.


**Package 12A (delivered 23 Nov 2026 by the Console session; verified from GitHub):**
https://github.com/kashkevin/PushFPS/actions/runs/45151071112 (artifact `Slipstream-test-package-12A-2026-11-23-26`,
1381 files, 596.2 MB, inner zip SHA-256 `17cebfaf5b5f4ebaac536949e05321731661674af9668a931184d1823dbf0adc`; package and
verify jobs passed; expires 21 Feb 2027). **The same program as Package 12** (experimental build #66, `archive/c41755fe`);
only the test kit changed: the `fglat` arm set (`-Session smoothness -Pass 120 -Arms flt-off,flt-fg2,flt-fg2-phase,
flt-fg2-hold,flt-fg2-grace,flt-fg2-headroom,flt-fg2-ull,flt-fg2-ull-prio,flt-fg2-ull-low`, about 25 min), the kit keeps
`[present-priority]` and clears `VEYRA_TEST_PRESENT_*`. Package source `slipstream/package` `436510ae` (commits
`1c25c05c`, `4f2540a2`, `436510ae` on top of Package 12's `4781f4d4`; Package 12's source kept as `archive/package-12`);
checked by remoteplay-checks #207 and checks #227 (`ck/fglat2`). Nothing in it has run on the owner's hardware.

**Private branch matching what shipped:** `exp/rel-12a` (`ad547044`) = `exp/rel-12` + the public commits `1471053e`,
`ec38b34f`, `1c25c05c`, `4f2540a2`, `436510ae` applied as one commit. New work starts from it.

**Owner's nineteenth message (23 Nov, 12:23 ET):** reduce the frame generation delay while keeping a steady 90 FPS.
His setup: "resolution setting" 3840x2160, game 30 FPS, target 90 FPS (3X), reported delay about 30 ms, screen
2560x1440 at 180 Hz, RTX 5070. Asked: is 4K processing, buffering or scheduling part of the delay; verify the real
processing size. Not yet known: Remote Play or capture card, and which 4K setting he means (asked).

**Verified in code (no hardware):**
- `include/veyra/pipeline/ResolutionPlan.h`: `p.fg=p.output=p.base`. With the upscaler target at 4K (the default) or a
  4K capture, NR, frame generation and the output work at 3840x2160; the presenter scales that into the 1440p window.
- `VEYRA_EXP_RP_SMOOTH_SR_DISPLAY=1` (Package 9) lowers the SR target to the box covering the monitor, only when SR
  enlarges; a source larger than the display has no reduction.
- The delay readout is capture callback / decoded picture -> Present per real frame. At 3X from 30 FPS the inherent
  interpolation wait is 22.2 ms; with the upstream-measured VFG Medium cost at 4K on an RTX 5070 (6.6 ms) and the
  margins that is about 30 ms, his reading (arithmetic, not a measurement). At 1440p: about 26-27 ms by estimate.
  Under 22.2 ms needs fewer interpolated frames (2X = 16.7 ms + the frame's GPU time, 60 FPS) or predicted frames
  (research branch only; the owner's rule keeps them out of the engine until the interpolating path is measured).

**OWNER'S FIRM RULE (twentieth message, 23 Nov 2026, 12:43 ET): 4K (3840x2160) is fixed.** Targets: 4K, a 30 FPS game,
a stable 90 FPS, the delay (now about 30 ms) as low as it can go on an RTX 5070. Preserve 4K picture quality; reduce
the delay through scheduling, buffering, GPU utilisation and frame generation efficiency; keep even pacing; never lower
the resolution, switch frame generation off or lower picture quality to improve a number; lower sizes only as labelled
diagnostic comparisons; be open about hardware limits; no claimed improvement without measurements; keep all earlier
builds and features. **Never recommend 1440p (a 1440p capture format, SR target 2K, FIT_DISPLAY, SMOOTH_SR_DISPLAY) as a
solution, in any text.**

**Stream `delay90` (author finished 23 Nov; review and build running):** private `exp/s6-delay90` head `ede71dfd`
(public snapshot `handoff/delay90` `3596d678`), commits 925a0cfd, 02a83fd1, 32c1d086, 70388830, d5983b32, ede71dfd on
`exp/rel-12a`. Built (unverified on Windows; nothing measured):
- `[slipstream-sizes]` (always on; `include/veyra/engine/PushSizes.h`, `PushSizesHook.h`): the real size of every stage.
- With `VEYRA_EXP_RP_STATS=1`, new `[slipstream-smooth]` fields: pickupWait / baseObs / fg1Obs / interpWait / margin /
  realLate, `partsSumP50Ms` against `delayToCallP50Ms`, `gpuFg1P50/P95Ms`, `uploadCpuCopyFps`.
- Switches, all off: `VEYRA_EXP_RP_UPLOAD_EARLY_RELEASE=1` (capture upload buffer released at the base list's fence),
  `VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY=high` (`[graph-priority]`), `VEYRA_EXP_RP_FIT_DISPLAY=1` (DIAGNOSTIC only; shares
  SMOOTH_SR_DISPLAY's path). At 3X use `VEYRA_EXP_RP_FG_ULL=hold,phase,grace` (the headroom part can step 3X down).
- Kit sets `d90` (Remote Play, needs `VEYRA_EXP_RP_STREAM_DEDUP=1`, set by the arms) and `d90-capture`, about 30 min each,
  `-Pass 180`; `docs/drafts/DELAY_90.md`. remoteplay-checks job limit 45 -> 60 minutes.
- Finding (code + upstream figures, an estimate): his ~30 ms is within a few ms of the floor at 4K (22.2 ms + base +
  fg1); removable without changing the picture: about 0-3 ms on a capture card, 4-7 ms on Remote Play.
- Checks: 995/995 core tests (plain, TSan, ASan+UBSan), 270 kit tests, remoteplay-checks #213 and checks #234 passed.

**PACKAGE 13 DELIVERED (23 Nov 2026):** https://github.com/kashkevin/PushFPS/actions/runs/45263495378 (artifact
`Slipstream-test-package-13-2026-11-23-28`, 1382 files, 596.2 MB, inner zip SHA-256
`e5ef19ff48d5a1dc30a229af1dca6eddde9944ed34a3eff407fa4e8b13c27805`; package and verify jobs passed; expires 21 Feb
2027). Four copies: original 2.1.5-pushfps.1, stable .7, experimental .70, previous (Package 7) .33.
- Experimental build: build-windows #70, run 45255513074, tree `cd1ac414` (archived `archive/cd1ac414`), every step
  passed; remoteplay-checks #218 (995/995 core tests plain, ASan+UBSan and MSVC; kit dry runs) and checks #239 passed.
- Reviews: delay90 ACCEPT (upload early release traced reader by reader; defaults unchanged apart from the always-on
  `[slipstream-sizes]` line); six fixes made (`64a0b793`); second look ACCEPT with one package-text fix (no comment
  after an ini value), made.
- Sources: private `exp/rel-13` `248f35ac` (= `exp/s6-delay90` `64a0b793` + package commits). Public
  `slipstream/package` `b8a7cc2f` (a fast-forward on Package 12A's `436510ae`, which is also kept as
  `archive/package-12A`); `ck/package-13` `e3f2d441` holds the same tree; `handoff/delay90` is the stream snapshot.
- Nothing in it has run on the owner's hardware. His steps (START-HERE): install; `slipstream.ini` with
  `VEYRA_EXP_RP_STATS = 1`, 3 minutes of normal play, Collect-Diagnostics; the d90 / d90-capture arm set (about 30 min,
  `-Pass 180`); the feel test with `VEYRA_EXP_RP_FG_ULL = hold,phase,grace`, `VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY = high`,
  `VEYRA_EXP_RP_UPLOAD_EARLY_RELEASE = 1`.

**OPEN, TOP PRIORITY: Package 13 reads about 32 ms instead of about 30 ms and "feels noticeably laggier" on the owner's
PC (messages 22-24, 23 Nov, 19:05-19:26 ET).** His instruction: we own the fix; do not ask him to run diagnostics, edit
files or switch versions; investigate the code, compare 12A and 13, fix, validate by tests, independent review and
Windows builds, deliver a reviewed package that only needs installing, explain what changed, and never call it fixed
before his PC confirms. 4K, 30 -> 90, image quality, even pacing stay fixed. His PC cannot be read from the orchestrating
session (folder access refused three times).
- Audit (independent reviewer, code only): with no switch set, #70's frame path equals 12A's (new: a once-a-second size
  check and one counter). The readout is a 1 s mean over real frames (arrival -> Present returned), whole ms. Candidates:
  our own switches from Package 13's START-HERE steps 2 and 4 (`VEYRA_EXP_RP_STATS`, `FG_ULL=hold,phase,grace`,
  `GRAPH_QUEUE_PRIORITY=high`), a leftover `FG_ULL=1` from 12A, the neural runtime answer at install, different conditions.
- In progress (two specialists): `exp/s6-lagfix` (from `exp/rel-13`): a scheduling simulator in the core tests driving
  the real decision code at 4K-like costs, fixes so that no FG_ULL part or priority switch can ever be worse than the
  default (or the part is made inert), and a plain install made safe against leftover test lines.
  `exp/s6-laghunt` (from `exp/rel-13`): the installer and the neural runtime question, what STATS turns on (stream
  delivery path), the always-on additions once more, and what the kit may leave behind in the copy he plays with.
- Parked (not shipped): `exp/s6-ab13`, the fifth-copy A/B package (12A's program beside 13's). Finished part:
  `tools/remoteplay_latency_bench/triage.py` (reads a diagnostics zip: switches accepted, frames shown / dropped, GPU
  span against the pair interval, stalls; splits sessions by build).
- **Owner's twenty-fifth message (23 Nov, 22:09 ET):** restoring 12A's behaviour is not the goal; improve the frame
  generation architecture itself (the fragile spots of the default 4K path, alternative scheduling methods, unnecessary
  GPU synchronisation, dependencies, parallel execution). Priorities in order: no lag / stutter; stable 90 from 30; full
  4K quality; delay below ~30 ms. Package 14 must need nothing but installing; simulated and hardware-verified results
  must be told apart. Orchestrator's rule for the teams (`agents/S5_DIRECTIVE25_FORWARD.txt`): in the experimental copy
  a fix of a demonstrated default-path defect is ON by default only if the simulator and tests show it never worse in
  every cost set, it changes no picture, a `VEYRA_TEST_*` switch restores the old behaviour, the start-up log names it,
  and an independent reviewer accepts it.
- Results so far (code and unit tests only): `exp/s6-laghunt` `87b4703c` done: a neural layer could switch itself back on
  after the runtime file appeared (fixed), STATS on a PS5 stream delayed one frame every 5 s (fixed), the sizes line
  write capped; the two-slot Newest delivery equals the mailbox (tested); the kit leaves nothing behind. In review.
  `exp/s6-lagfix` (head `cd91fedf` at this writing): scheduling simulator (`tests/remoteplay/FgScheduleSim.h`), every
  FG_ULL part and the graph queue priority made inert, a plain install takes no frame-path switch from slipstream.ini,
  `[experiment-switches]` start-up line; four fragile spots of the default path found (admission lock-out near the GPU
  limit, the learned phase jumping to ~63 ms after skip runs, no arrival smoothing on a PS5 stream, the card's rejection
  hold on a stream). Now: fixing those four, and a simulated comparison of alternative schedules
  (`docs/drafts/FG_SCHEDULING_NEXT.md`). `exp/s6-gpupath` (new, from `exp/s6-lagfix`): map of every GPU / CPU wait and
  copy for one pair at 3X, removal of unnecessary synchronisation, WARP tests.
- **Owner, 23 Nov 22:51 ET: "dont worry about remote play only capture card".** He plays through his HDMI capture card
  (4K capture, 30 FPS game, 3X = 90 FPS). All new work targets the capture-card path; Remote Play only has to keep
  working unchanged.
- State after a server restart (04:00 UTC 24 Nov; all three teams resumed, branches were clean): `exp/s6-lagfix`
  `454d9e63`: FG_ULL parts and graph queue priority inert; slipstream.ini allow-list; `PushFgSchedule.h` with
  `VEYRA_EXP_RP_FG_SCHED` parts robustAdmit (ends the admission lock-out: near-limit card SIMULATED skips 11.2 -> 0/s,
  generated 34.8 -> 59.7/s, longest gap 62 -> 33 ms, real P95 61 -> 47 ms, real MEAN 35.6 -> 42.5 ms) and
  phaseBehindWarmup (real P95 61 -> 45 ms), both still OFF; being judged for default-on on capture cards by the owner's
  priority order (stutter, stable 90, quality, delay). `docs/drafts/FG_SCHEDULING_NEXT.md`: SIMULATED comparison of
  schedules; "one predicted frame" takes ~11.1 ms off a real frame's delay (design only, nothing built).
  `exp/s6-gpupath` `7037a6ed`: `docs/drafts/GPU_PATH_MAP.md`; a real frame's presentation waits for its own lists
  instead of the group's (VFG; restore `VEYRA_TEST_GPU_REAL_WAITS_GROUP=1`); upload buffer released after the base list
  by default (restore `VEYRA_TEST_UPLOAD_LATE_RELEASE=1`); `VEYRA_EXP_RP_VFG_BACK_TO_BACK=1` (off).
  `exp/s6-laghunt` `bcd3b16c`: neural-layer fix redesigned as a marker (review had rejected the first version).
- 24 Nov ~05:00 UTC, after a second restart: `exp/s6-gpupath` finished at `00cd668d` (in independent review);
  `exp/s6-lagfix` at `fcfc5c40` (robustAdmit and gridEarly ON by default for capture cards, two content-cadence repairs
  for a 60 Hz capture of a 30 FPS game; hand-back pending); `exp/s6-laghunt` at `2947d286` (marker follows its layer;
  final check pending).
- **Owner, 23 Nov 23:39 ET: he needs LESS than 30 ms at 90 FPS, and Package 13 made it worse.** Plan told to him:
  Package 14 first (lag fix, install only), then **Package 15: the "one predicted frame" schedule** as a switch in the
  app, off by default: per pair I(2/3), the real frame B, P(B + 1/3); SIMULATED about 11 ms less delay (about 19-20 ms
  instead of 30), one shown frame in three predicted. Started: its GPU half on `exp/s7-predict` (from `exp/s6-gpupath`;
  `VEYRA_EXP_RP_FG_SCHEDULE=onepredicted`, `include/veyra/pipeline/PushPredict.h`, `docs/drafts/PREDICT_GPU.md`); the
  scheduling half follows the design in `docs/drafts/FG_SCHEDULING_NEXT.md` section 3 (on `exp/s6-lagfix`).
- 24 Nov ~07:00 UTC: all three streams reviewed ACCEPT and merged: **`exp/rel-14` `72a77a9d`** = `exp/s6-lagfix`
  `ae47e446` + `exp/s6-gpupath` `00cd668d` + `exp/s6-laghunt` `2947d286` (merge verified line by line; 1042 core tests,
  276 kit tests pass locally). Default-on for capture cards (SIMULATED / tested, not built, not hardware-verified):
  robustAdmit (admission cannot lock out), gridEarly (a skipped pair's real frame on the earliest grid slot), two
  ContentDedup cadence repairs, the real frame waiting for its own lists (VFG), the upload buffer released after the
  base list, the neural-layer marker; always-on steadiness counters in `[capture-timing]` (onGrid1msPct, shownPct ...).
  Owner's acceptance number (23:59 ET): 99 % of intervals on the 11.1 ms grid and 99 % of frames shown; SIMULATED it
  holds in comfortable / tight / drift / thermal sets and falls short where the GPU spikes (96.8-98.8 % on grid).
  Review fixes in progress ON `exp/rel-14`: every restore switch in the `VEYRA_TEST_RP_LEGACY_*` family plus the master
  `VEYRA_TEST_RP_LEGACY_PACKAGE13=1`, a stop for robustAdmit under sustained bimodal load, inert kit arms relabelled.
  Package 15 (lower delay): GPU half written on `exp/s7-predict` `1eb56824` (never run on a GPU; its shaders and WARP
  tests first run in the next full build); the scheduling half is next.
- **PACKAGE 14 DELIVERED (24 Nov 2026):** https://github.com/kashkevin/PushFPS/actions/runs/45350919356 (artifact and
  inner zip SHA-256: see section 0's log line below; package and verify jobs passed). Experimental build: build-windows
  #73, run 45346138947, tree `dfc0a73b` (`archive/dfc0a73b`); remoteplay-checks #248 (1047/1047 core tests plain,
  ASan+UBSan, MSVC), checks #271. Build #72 had failed at link (three PushNeural function bodies deleted by `2947d286`;
  restored in `exp/rel-14` `b0fc6e01`). Private source `exp/rel-14`; public `slipstream/package` `c68c6b60` (fast-forward
  on Package 13's `b8a7cc2f`, kept as `archive/package-13`); `ck/package-14` holds the same tree. START-HERE: install
  only. Default-on parts and their `VEYRA_TEST_RP_LEGACY_*` undo lines (master `VEYRA_TEST_RP_LEGACY_PACKAGE13 = 1`):
  see `docs/SLIPSTREAM_STATUS.md`, Package 14. Nothing hardware-verified; nothing is called fixed.
- **Next: Package 15, the lower-delay schedule ("one predicted frame").** GPU half `exp/s7-predict` `1eb56824`;
  scheduling half, the app switch and the simulator comparison in progress on `exp/s7-lowdelay` (from `exp/rel-14` +
  `exp/s7-predict`). Acceptance: 99 % steadiness not worse than Package 14's default, switches between schedules included.
- **Status 24 Nov 2026, afternoon (owner on Package 14: 31 ms at 90 FPS, 4K; asks for ~15 ms, stable, no quality loss,
  no manual testing; capture card only).** `exp/s7-lowdelay` head `51c79850`: the "Lower delay" row applies with one
  click (graph rebuild, `EnhancementSettings::lowDelay`), both GPU-review MUST fixes (the warp divides by trust; a
  failed flow counts as fallback), the simulator's content-step metric and the three-way comparison
  (`tools/remoteplay_latency_bench/lowdelay_three_way.cpp`). First Windows build of the branch passed (build-windows
  run 45415550329 on `dc93f898`). **Decision:** stay in the low-delay schedule once in; the predicted frame is always
  shown as composed (untrusted pixels held at the real frame); the verdict (to become seam-aware) only feeds a
  sustained exit; undo `VEYRA_TEST_RP_LOWDELAY_LEAVE_ON_UNUSABLE=1`. Remaining before packaging: scheduling review
  B-F, GPU SHOULD items, the always-on delay split, second-look reviews. The row stays off by default.
  **Research (private, `/home/claude/out/research/`, in the orchestration zip):** `AMD_FG_WINDOWS_PRESENT_GPU.md`,
  `NVIDIA_STREAMLINE_REFLEX_NVOF.md`, `FG_TECH_SURVEY.md`, synthesis `RANKING.md`. Conclusions: every interpolator holds
  the newest real frame, only prediction removes the wait; Streamline DLSS-G, Reflex 2, FSR/XeSS FG, NVOF FRUC are
  incompatible with captured video; ~15 ms on the grid needs a first generated frame of ~1-3 ms: motion once per pair
  at reduced size feeding full-size warps for both generated frames, replacing VFG's run (estimate 14-20 ms, picture
  quality unproven) = the next experimental branch after Package 15. **`exp/s8-present`** (from `exp/rel-14`):
  R1 in-picture readouts drawn inside the swap chain so the video window stays one rectangle (`c614deee`; earlier logs
  correlated a complex region with 7-9 ms present-to-vblank and VRR off), R2 always-on `[present-path]` and 10 s
  `[fg-health]` (`1ba379e0`), R11 the NVIDIA low-latency Sleep measured against the sample's arrival and bounded on a
  capture card (`f7a9b49a`), R3 blit ahead / Present at the deadline (in progress). Needs independent review, then
  merges into Package 15. Nothing hardware-verified.
- **PACKAGE 15 (24 Nov 2026, evening): release tree `exp/rel-15`** = `exp/s7-lowdelay` `6aaaa4c6` + merge of
  `exp/s8-present` `106c9e96` (merge `6922ecba`, nine keep-both / union conflicts, verified line by line) + packaging.
  Strict build: build-windows #79, run 45467487654 on `slipstream/dev3` `b15c7410` (`archive/b15c7410`): MSVC, WARP
  (`predict`, `predict-hdr10`, `video-overlay*`, `blit-ahead-order`), QML (`tst_push_lowdelay` 16/16,
  `tst_push_in_picture` 8/8), install test green; remoteplay-checks #273 (1107/1107), checks #298. Public
  `slipstream/package` `129c1377` (fast-forward on Package 14's `c68c6b60`, kept as `archive/package-14`);
  `ck/package-15` same tree. **DELIVERED:** https://github.com/kashkevin/PushFPS/actions/runs/45475568705 (package and
  verify jobs passed), artifact `Slipstream-test-package-15-2026-11-24-32`, 596,332,046 bytes, artifact SHA-256
  `38211f8b6b1ed1643f8cda5cd7ea1e4a84d1f0028af10b0aaf51b572521ec704`. Nothing hardware-verified; nothing called fixed.
  **In it:** A "Lower delay" row (OFF; one click; stay-in policy; whole-frame fallback above one half; sustained exit
  45/60; late-frame watch; VFG index-2-only refusal handled; undo of the policy
  `VEYRA_TEST_RP_LOWDELAY_LEAVE_ON_UNUSABLE`), B in-picture readouts (ON, `VEYRA_TEST_RP_LEGACY_VIDEO_OVERLAY`),
  C bounded NVIDIA low-latency Sleep (ON, `VEYRA_TEST_RP_LEGACY_REFLEX_SLEEP`), D delay split + `[present-path]` +
  `[fg-health]` (ON, display / log only), E blit ahead (OFF, `VEYRA_EXP_RP_PRESENT_BLIT_AHEAD=1`). Master undo for B
  and C: `VEYRA_TEST_RP_LEGACY_PACKAGE14=1`. **Reviews** (private, `/home/claude/out/reviews/`):
  `S7_LOWDELAY_REVIEW2.md`, `S8_PRESENT_REVIEW1.md`, `S8_PRESENT_REVIEW2.md`, `REL15_REVIEW.md` (merge accepted; ten
  false or overstated sentences of the start text corrected). **Honest state:** expected readout with the row on
  roughly 20-22 ms (schedule arithmetic 11.1 ms, SIMULATED gain about 9 ms), not the ~15 ms asked for; the VFG
  "frame index 2 alone" request has never run on a GPU; the 99 % on-grid target is missed in 5-7 of 16 simulated cost
  sets (Package 14: 8); near or over the GPU limit some pairs are later than Package 14. **Left for the next branch:**
  the shared-motion interpolator (motion once per pair at reduced size, full-size warps for both generated frames,
  replacing VFG's run: the only route found to ~15 ms on the grid; estimate 14-20 ms), seam-aware verdict, leading-edge
  search, Catmull-Rom sampling, NVOF as a second estimator, the two-predicted schedule in the simulator, R5 merged
  colour pass; notes N3 (`LEAVE_ON_UNUSABLE` read as set for "0" in the environment) and N5 (comment
  `PushLowDelay.h:97`) from the release review.
- **FIRST HARDWARE DATA (24 Nov 2026, 18:20 ET): Package 15's "Lower delay" never ran on the owner's PC.** He connected
  the experimental copy's folder to the orchestrator's chat (read-only: logs only; nothing on his PC may be modified,
  no settings, no restarts, no heavy tests). His log: `[fg-sched] lowDelay state=not-here refusal=smoothness-mode ...
  predictedPairs=0` for the whole session. He uses **Smoothness Mode with target 90** (that is his 90 FPS) and **Live
  HUD Lock**; Package 15 excludes the mode with both (review-2 SHOULD 4 and `PushPredict.h:81`). NVIDIA was never asked
  for a predicted pair. Measured on his card (software timers): readout 31 ms = picture 4.7 + first generated frame 2.0
  (VFG at 3840x2160 HDR10: about 2 ms per frame, not the 6.6 ms assumed) + schedule wait 22.1 + margin 2.2; Present ->
  vblank median about 10 ms, P95 14-17 (Package 14: 20.5), windowed, VRR never active, HDR on; capture is 3840x2160 at
  30 Hz without repeated frames; all frames shown, 95-98 % of intervals within 1 ms of the grid (target 99 %).
  **Now in work** (branches from `exp/rel-15`): `exp/s10-lowdelay2` (the mode WITH Smoothness Mode and HUD Lock /
  exclusion boxes; entry / exit rule that survives isolated skips; predicted-frame cost logged and kept off the real
  frame's path; VFG refusal fallback; a row that reports measured state), `exp/s10-display` (why 1.7 refreshes after
  Present, why fullscreen ended after 2 s, VRR, the 4.7 + 2.2 ms before generation, grid precision), `exp/s9-e2e`
  (end-to-end engine test on the Windows build machine with a test-only fake VFG and the owner's exact configuration:
  the gate for the next package), `exp/s9-ownfg` (offline picture-quality harness and architecture comparison: today /
  hybrid / all-own / fully causal, with his measured costs). Rule adopted: no delay projections to the owner; only his
  next log counts. Lesson: every earlier figure came from a scheduling simulator that never ran the real chain.
- **25 Nov 2026, 03:00 UTC: DO NOT ENABLE "Lower delay" ON PACKAGE 15.** Two independent runs on the Windows build
  machine's software renderer (the fix branch's start-up self-check, and the end-to-end engine test on Package 15's own
  code) executed the predicted frame's motion pass (GPU DIS flow) for the first time anywhere: both ended with the D3D12
  device removed. On the owner's PC it never ran (the mode was blocked) and his switch is off. Decision: the predicted
  frame takes its motion from an estimator upstream runs in normal operation (AMD FidelityFX optical flow); GPU DIS for
  comparison only; nothing with the mode ships before the end-to-end test runs predicted pairs without a removed device.
  State of the branches (all from `exp/rel-15`): `exp/s10-lowdelay2` `4d97b879` (mode with Smoothness Mode and HUD Lock,
  tolerant entry / exit rule, predicted-frame cost by stage, VFG refusal ladder, measured row text, containment of the
  flow self-check; reviewed: accept with changes, fixes in work); `exp/s10-display` `091668db` (reviewed and fixed:
  "to screen" figures, late-Present and poll instrumentation, `[capture-stall]`, `VEYRA_TEST_RP_LEGACY_PACKAGE15`,
  opt-in `VEYRA_EXP_RP_PRESENT_BEFORE_POLL` / `VEYRA_EXP_RP_FULLSCREEN_NO_REGION`); `exp/s9-e2e` `bf3cb6f2` (test-only
  fake VFG, replay as a capture card, the owner's configuration; fast workflow `e2e-quick.yml` on `ck/e2e*`; reproduces
  his log on Package 15); `exp/s9-ownfg` (offline picture-quality harness, architecture comparison; in work);
  `exp/s10-stall` (new). **Separate issue, owner's order of 24 Nov 21:51 ET:** a pair of capture stalls (92-158 ms)
  a few seconds after every quarter hour of the wall clock, 6.3 h of logs, across a restart; cause outside PushFPS,
  unknown; to be identified with evidence (an unprivileged in-program witness in the next package, a read-only
  collector he may start, an Event Tracing session only with his permission), never masked by buffering.

**Owner's twenty-first message (23 Nov, 15:01 ET):** the 22.2 ms interpolation wait is a limitation of the current
approach, not of the hardware; never describe it as a hardware limit. **Next investigation, after Package 13 is
tested:** other frame generation scheduling approaches that shorten that wait at 4K and an even 90 FPS (the research
branch `research/predictive-fg` and `docs/PREDICTIVE_FRAME_GENERATION_RESEARCH.md` in the private bundle are the
starting point: showing the real frame earlier with predicted frames after it).

---

## 1. Where everything lives

**Public repository:** https://github.com/kashkevin/PushFPS (a fork of DosukaSOL/PushFPS). Never push to
`DosukaSOL/PushFPS`.

**Private development branches (`exp/*`) were never pushed**, by design: they carry private planning and review
documents that must not be public. Their *code* is public as squashed snapshots (section 1.2). The full private
history exists in two places outside GitHub:

- the orchestrating session's machine (lost if that session ends), and
- a git bundle the owner was sent with this handoff: `slipstream-private-branches.bundle` (restore: section 7,
  step 5b). A second file, `slipstream-orchestration.zip`, holds the private task briefs, review rules and state log.
  **Neither may ever be committed to the public repository.**

### 1.1 Public branches with a meaning

| Branch | What it is | Head |
|---|---|---|
| `main` | upstream PushFPS as forked; do not touch | `ed317efb` |
| `slipstream/package` | source of the latest **released** test package (Package 12, incl. its package text and filled `CHECKED.txt`; its parent is Package 11's `9c5e80e8`) | `4781f4d4` |
| `archive/package-11` | Package 11's package source (kept before `slipstream/package` moved to Package 12) | `9c5e80e8` |
| `archive/package-10` | Package 10's package source | `40c66eef` |
| `slipstream/dev2` | the exact tree Package 10's experimental build was compiled from | `8658fd34` |
| `archive/8658fd34` | permanent copy of that tree (one `archive/<sha>` per released build) | `8658fd34` |
| `slipstream/dev3` | second CI lane (vcpkg cache warm); built Package 11 (#64, `4462a475`) and Package 12 (#66, `c41755fe`) | `c41755fe` |
| `archive/4462a475` | the tree Package 11's experimental build (#64) was compiled from | `4462a475` |
| `archive/c41755fe` | the tree Package 12's experimental build (#66) was compiled from | `c41755fe` |
| `handoff/console` | this file, on top of Package 10's source | see section 9 |
| `handoff/rel-11` | public snapshot of the Package 11 candidate (`exp/rel-11`) | `f7e0241e` |
| `handoff/rel-12` | public snapshot of the Package 12 candidate (`exp/rel-12`); same tree as `c41755fe` | `811f4ddb` |
| `ck/package-12` | Package 12 build tree + the integration review's kit fix (`1471053e`), checked by checks #222 and remoteplay-checks #204 | `1471053e` |
| `handoff/<stream>` | public snapshot of each unfinished stream at the time of this handoff (section 4) | section 4 |
| `ck/<stream>` | per-stream remote-check snapshots made by `ck.sh` (may be older than `handoff/<stream>`) | — |
| `slipstream/rc2`, `slipstream/baseline` | **never push to these** (old fixed checkpoints) | — |

Builds: a push to `slipstream/**` runs `build-windows.yml` (the full Windows build, about 50-70 min with a warm cache;
a new branch name is cold: about 105 min). Branches outside `slipstream/**` and `main` only run `checks.yml` and,
on matching paths, `remoteplay-checks.yml`.

### 1.2 How the public snapshots are made

A private branch is turned into a public one by squashing it and deleting the private paths (`.claude`,
`SKILLS_INVENTORY.md`, `docs/*REVIEW_2026*`, `docs/ROADMAP_AND_PROJECTIONS_*`, …), and by adding `slipstream/**`
to the build trigger. A new session without the private scripts can do the same by hand: never push a branch that
contains those paths.

---

## 2. Released packages (verified: built, tested in CI, independently reviewed)

None of these has been validated on the owner's RTX 5070 / PS5 except where his own evidence is quoted.
Each package installs side by side; **Package 7 is the rollback** (its own copy stays installed).

| Package | What | Download (GitHub Actions run, Artifacts) | Experimental build run |
|---|---|---|---|
| 12 (latest) | Frame generation delay (`VEYRA_EXP_RP_FG_ULL`, off by default; `-ull` kit arms) and the "DLSS 5" stutter (`VEYRA_EXP_RP_NR_ADAPT`, off by default; four-way `nrp-*` kit arms), on top of Package 11 | https://github.com/kashkevin/PushFPS/actions/runs/45002383758 — `Slipstream-test-package-12-2026-11-22-24`, 1381 files, 596.2 MB, zip SHA-256 `6e857b215cf1f8f4a26164a05ed5202efd621786846af6a34a6d41dd379f86cd` | 44980732479 (build-windows 66, `c41755fe`) |
| 11 | HDMI capture card frame generation; GTA V-style mouse and keyboard | https://github.com/kashkevin/PushFPS/actions/runs/44979515518 — `Slipstream-test-package-11-2026-11-22-22`, zip SHA-256 `569d543aa3d9657f83e57f36d00090587b4745ef5a57771e6e447561b1674e02` | 44975871790 (build-windows 64, `4462a475`) |
| 10 | Sound follows real frame arrival (fixes up to 1.5 s audio lag), stale-clock expiry fix, frame-cap fit, PS5 frame-rate log, matched picture captures, `retest` session | https://github.com/kashkevin/PushFPS/actions/runs/44956440633 — `Slipstream-test-package-10-2026-11-21-20`, zip SHA-256 `64b5a318b044e3d186e834ba62544fa276fd50f001c84895aa3e38ba70cc0f19` | 44951360667 (build-windows 62, `8658fd34`) |
| 9 | Smoothness Mode, clarity tools (bitrate test), network diagnostics panel | https://github.com/kashkevin/PushFPS/actions/runs/44903164942 | 44899669488 (`9c008f40`) |
| 8C1 | 8C + neural-runtime verification fix | https://github.com/kashkevin/PushFPS/actions/runs/44877749832 | 44874996812 (`11af5a8d`) |
| 8C / 8B / 8A | Package 8 rolling releases (restoration, profiles; adaptive mouse/keyboard; neural layer) | runs 44871437854 / 44870117294 / 44867667892 | 44868744683 / 44867771740 / 44864340774 |
| 7 | rollback package | build 44780265264 | — |

Package 10's four copies: original 44401154647, stable 44445529795, experimental 44951360667, previous (Package 7)
44780265264. Package 12's four copies (package run 45002383758, verified in its log): original 44401154647
(`0af79be4`), stable 44445529795 (`9940e2e3`), experimental 44980732479 (`c41755fe`, version 2.1.5-pushfps.66),
previous (Package 7) 44780265264 (`6fe95fa4`); the verify job passed for all four (GitHub runner, not a real PC).
Artifacts expire on GitHub: Package 11 on 20 Feb 2027 03:11 UTC, Package 12 on 20 Feb 2027 08:20 UTC.

**Owner's measured facts (his Package 9 evidence, wired, SDR, FG off):** bitrate requests are honoured (median
9.7 / 19.0 / 30.4 / 44.8 Mbit/s delivered for 15 / 30 / 50 / 100 requested, max 60.8); zero network loss in all eight
runs; the console sent 57.5-59.4 frames/s at 15-50 Mbit/s and ~59.9 at 100; his panel ran at 119.998 Hz (not 180);
audio lag grew to the 1.5 s cap at lower rates (fixed in Package 10, unverified on his PC).

---

## 3. Package 11: HDMI capture card + mouse and keyboard (delivered 22 Nov 2026)

**DELIVERED (22 Nov 2026):** Package 11 download: https://github.com/kashkevin/PushFPS/actions/runs/44979515518
(artifact `Slipstream-test-package-11-2026-11-22-22`, 1379 files, 596.1 MB, inner zip SHA-256
`569d543aa3d9657f83e57f36d00090587b4745ef5a57771e6e447561b1674e02`; package and verify jobs passed). Experimental build:
build-windows #64, run 44975871790, tree `archive/4462a475`; package source `slipstream/package` `9c5e80e8`
(private `exp/rel-11`). Nothing in it has run on the owner's hardware. The sections below are the history.

### 3.1 Stream `capture` (frame generation from an HDMI capture card)

- Private branch `exp/s6-capture`; public snapshot `handoff/capture` (section 4). Base: Package 9 + `exp/s6-fps`.
- **Done, accepted:** assessment `docs/drafts/CAPTURE_FG.md` (60->120 at 2X achievable; 30->120 at 4X with VFG
  Low/Medium or the DLSS path, not VFG High; 180 is a stretch; inherent delay of a real frame (M-1)/M x T + the first
  generated frame's GPU time, e.g. 25 ms at 4X for a 30 fps game; DLSS FG/MFG cannot apply to captured video),
  `[slipstream-capture]` statistics (`PushCaptureStats.h`), `VEYRA_TEST_RP_CAPTURE=1` kit autostart (never connects to
  the PS5), a sanitised card line, `VEYRA_EXP_RP_CAPTURE_DEDUP_SPAN` (off), kit session `capture` (13 arms,
  `-Pass 120/180`). Defaults unchanged.
- **Done at this handoff:** `exp/rel-10` (Package 10) merged into `exp/s6-capture` (commit `a3ed26a6`, 14 conflicted
  files, almost all kit lists both sides appended to). Its public check snapshot `ck/capture` `0be983f5`: Linux and
  checks/QML passed, and the Windows kit and diagnostics dry runs of every session passed (remoteplay-checks run
  44970077156, checks run 44970077213). **Verified by the orchestrator:** in all 57 changed files the merge equals base
  `8b8b7d86` plus both sides' changes; the 8 files that differ only combine lines both sides edited (session lists,
  ValidateSet, MEASURE_SESSIONS: retest before capture) and renumber the capture row to 10. Left as it was:
  measures.csv has 4 label pairs that appear in two sessions (capture row beside the Remote Play row, never on the
  same page), already there before the merge.
- **Unverified:** no Windows build of the capture changes yet; the owner has not named his capture card (ask him).
- Capture-card **audio**: no capture-audio defect has been found or reported; audio sync for capture uses the existing
  WASAPI input path (AverMedia audio helper exists). Treat as untested.

### 3.2 Stream `kbm` (GTA V-style mouse and keyboard, camera feel)

- Private branch `exp/s6-kbm`; public snapshot `handoff/kbm`. Base: Package 9.
- **Done, accepted:** audit `docs/drafts/KBM_AUDIT.md`; opt-in camera fixes (report-timed speed, a stop rule that never
  holds a stick past one send period, 8-bit stick steps); GTA V-style PC preset `kbmGtaPcProfiles` (button
  "GTA V 风格"), contexts on foot / aiming / vehicle / menu, R = Square (GTA V's PC reload + light melee; GTA VI's reload
  button is not in any published table: "check in game"), F leaves the vehicle keys; `VEYRA_EXP_RP_KBM_CONTROL=1`
  (keyboard/mouse over the existing controller-only Remote Play connection, for use beside a capture card);
  `VEYRA_EXP_RP_KBM_TIMING=report`; kit arms `i-gta-pc`, `i-report`. A test proves the old profiles produce byte-identical
  output (old-vs-new digest over 8 x 200,000 events).
- **Done at this handoff:** the last two fixes, commit `03ffbb87` (the 8-bit remainder is dropped only when the motion
  turns by more than 90°, with a test that fails on the old rule; reload wording: if reload is another button in the
  game, put R on it). `ck.sh kbm --all` passed every check on it, including the Windows kit dry run; 123 keyboard and
  mouse tests pass. **Still to do:** merge on top of the capture integration.
- **Not done (by design):** moving mouse polling off the interface thread (would need a message-only window redesign);
  measured instead in the kit (poll gap P95/P99).

### 3.3 To finish Package 11

**State at ~02:55 UTC 22 Nov:** steps 1-3 below are done. `exp/rel-11` = `bcf4e29a` (capture merge `a3ed26a6`, kbm merge
`11e814ed` verified line by line with 0 lines lost, package commit `bcf4e29a`). Public build lane `slipstream/dev3`
`4462a475`: strict build https://github.com/kashkevin/PushFPS/actions/runs/44975871790 (build-windows #64: **passed**), remoteplay-checks
#192 passed, checks #207 passed. `CHECKED.txt` filled (`exp/rel-11` head after it), tree archived as `archive/4462a475`,
package source pushed to `slipstream/package` (`9c5e80e8`); package run
https://github.com/kashkevin/PushFPS/actions/runs/44979515518 started. Integration review: ACCEPT with wording fixes in `labels/11.txt`, all made (`8a669ab4`, text only; the build's
tree `4462a475` has the same code). Then: fill `CHECKED.txt`, archive the tree as
`archive/4462a475`, package with `-f package_label=11 -f experimental_run=44975871790 -f experimental_branch=slipstream/dev3`.


1. Capture's merge of Package 10 is committed (`a3ed26a6`); wait for `ck/capture`'s Windows kit dry run and verify the
   merge line by line.
2. `exp/rel-11` = that merge; then merge `exp/s6-kbm` (or have its author merge `exp/rel-11` into it), same verification.
3. Package text: `packaging/slipstream/labels/11.txt` (start from `labels/10.txt`), `CHECKED.txt` back to its three
   `<…>` placeholders until the build is known, `docs/SLIPSTREAM_STATUS.md` section, `slipstream-package.yml` default
   label, translations (`python3 -B scripts/i18n/extract.py --check` must say 0 missing / 0 placeholder problems).
4. Independent integration review (line-multiset composition over every touched file; defaults; package text).
5. Strict Windows build on `slipstream/dev2`, then package (section 6).

---

## 4. Package 12: frame-generation latency + "DLSS 5" stutter (delivered 22 Nov 2026)

**DELIVERED (22 Nov 2026, ~08:22 UTC), finished by the Claude Code (Console) session:** Package 12 download:
https://github.com/kashkevin/PushFPS/actions/runs/45002383758 (slipstream-package #24; artifact
`Slipstream-test-package-12-2026-11-22-24`, 1381 files, 596.2 MB, inner zip SHA-256
`6e857b215cf1f8f4a26164a05ed5202efd621786846af6a34a6d41dd379f86cd`; package and verify jobs passed). Nothing in it has
run on the owner's hardware. Record (verified from the GitHub runs and logs):

- **Experimental build:** build-windows #66, run 44980732479, tree `c41755fe` (= `handoff/rel-12` tree), every step
  passed: MSVC /W4 /WX build, unit tests, controller test on a virtual gamepad, NIS/comparison GPU tests on WARP,
  27 Qt test suites / 394 QML tests (0 failed), install test. remoteplay-checks #201: 986/986 core tests on Linux
  (plain and ASan+UBSan) and 986/986 with MSVC, kit/diagnostics dry runs passed. checks #218 passed.
  Archived as `archive/c41755fe`.
- **Integration review** (independent, Sonnet, line-multiset composition of P11 `archive/4462a475` -> P12 over all 29
  touched files against `handoff/fglat` and `handoff/nrperf`): **ACCEPT WITH FIXES.** No line lost or duplicated,
  defaults unchanged with nothing set (ULL and NR_ADAPT gates read and confirmed), no unconditional hot-path work,
  kit durations match the text (smoothness 36 min, capture 42 min, four-way NR set 14 min), i18n `--check` 0 missing /
  0 placeholder problems, no forbidden content, `AGENTS.md` unchanged. Note for future reviews: `handoff/fglat` predates
  capture's Package 10 merge, so a naive `handoff/capture..handoff/fglat` diff shows ~2,500 "deleted" lines that are
  not real losses.
  - Fixed (should-fix): the kit's log allowlist lacked `[fg-ull]` (`fg-(validity|drops)` -> `fg-(validity|drops|ull)`
    in `run-tests.ps1`, `collect-diagnostics.ps1`, `report.py`), so the evidence dropped the lines FG_LATENCY.md tells
    the reader to look for; new test `test_the_ull_lines_are_copied_by_the_kit` (commit `1471053e`). Kit scripts only:
    the package takes them from the package source, so PushFPS.exe stays exactly build #66. Checked by checks #222
    and remoteplay-checks #204 (986/986 core tests plain and ASan+UBSan; 236 kit tests; Windows kit dry runs).
  - Left as they are (minor): SLIPSTREAM_STATUS.md wording of the stream reviews; NR_STUTTER.md / FG_LATENCY.md refer
    to research files that are not in the tree (`S5_NR_RESEARCH.md`, `PREDICTIVE_FRAME_GENERATION_RESEARCH.md`); a
    `VEYRA_EXP_RP_FG_ULL` value of 64+ characters turns nothing on without an `understood=false` log line (safe;
    fixing it needs an engine change and a rebuild).
- **Package source:** `CHECKED.txt` filled (commit `ec38b34f` on the local work branch; paragraphs 4-5 are Package 11's,
  unchanged); package commit `4781f4d4` = that tree with Package 11's `9c5e80e8` as parent (a fast-forward of
  `slipstream/package`, no force push; Package 11's source also kept as `archive/package-11`). The push started
  build-windows #67 and remoteplay-checks #205 (cancelled, as the procedure says), checks #224 (passed) and
  slipstream-package #23 (the usual push-triggered failure at "Download the builds", as #19 and #21 before it).
  Package run: dispatched with `package_label=12`, `experimental_run=44980732479`, `experimental_branch=slipstream/dev3`.

**Earlier status ("~04:30 UTC 22 Nov", from the orchestrating session; its "~" times run ahead of GitHub's clock,
e.g. build #66 started 03:27 UTC, so trust run IDs and commits over those times):** both streams reviewed ACCEPT and merged on top of Package 11 in `exp/s6-fglat`
(merges `5d0e02bd`, `aacf94c8`, verified line by line); `exp/rel-12` = `7f15198e` adds the package text
(`labels/12.txt`). Public `handoff/rel-12`; strict build https://github.com/kashkevin/PushFPS/actions/runs/44980732479 on
`slipstream/dev3` `c41755fe` (running); integration review running. Then: fill `CHECKED.txt`, archive the tree as
`archive/c41755fe`, keep Package 11's package source as `archive/package-11` (`9c5e80e8`), package with
`-f package_label=12 -f experimental_run=44980732479 -f experimental_branch=slipstream/dev3`.

### 4.1 Stream `fglat` (lowest-latency frame generation)

- Private branch `exp/s6-fglat` (from `exp/s6-capture`); public snapshot `handoff/fglat`.
- **Done (code reading, not yet reviewed):** `docs/drafts/FG_LATENCY.md`. The three largest sources:
  1. inherent: (M-1)/M x T + first generated frame's time + 0.5-1.5 ms margins (30 fps: 16.7 / 22.2 / 25.0 / 27.8 ms at
     2/3/4/6X; 60 fps: 8.3 / 11.1 ms at 2/3X);
  2. **avoidable "dead phase"**: a pair whose generated frames are skipped still shows its real frame at the full FG
     phase (~16.7 ms on Remote Play, because `LiveFgRejectionHold` only runs with card timing or
     `VEYRA_EXP_RP_ADAPTIVE_PHASE`);
  3. avoidable: Remote Play's fixed phase T + base_P95 instead of the learned one (~4.3 ms at 60->120; ~12.7 ms for a
     30 fps game with stream dedup at 2X).
  Next: VSync back-pressure at the VRR ceiling at 180 output (0-5.6 ms, estimate): 120 before 180.
- **Done at this handoff (author's report; not yet independently reviewed; no Windows exe build):** head `b13d4db4`.
  Switch `VEYRA_EXP_RP_FG_ULL` (`1`/`all`, or a list of `hold`, `phase`, `headroom`, `grace`; anything else turns
  nothing on; may be set in `slipstream.ini`); unset = unchanged behaviour. Logic in
  `include/veyra/engine/PushFgLatency.h`, small hunks in `EngineController.cpp`, `PushSmoothHook.h`, `PushSmoothness.h`.
  New `[slipstream-smooth]` fields only with `VEYRA_EXP_RP_STATS=1` (`realAge*`, `addedEstMs` = an estimate,
  `realHold*`, `phaseMs`/`phaseLearned`, `firstReady*`, `presentLate*`, `deadPhaseFps`, `fallbackFps`). Kit arms
  `sm-fg2-ull`, `sm-fg3-ull`, `cap-fg4-ull`, `cap-fg6-ull` (smoothness session 30 -> 36 min, capture 36 -> 42 min).
  Extrapolation verdict (section 7 of the draft): no engine prototype (NVIDIA VFG only interpolates; the only usable
  flow is the DLSS-path backend's; it cannot beat FG off for responsiveness). No new strings, no quick-setup row.
  Checks: `ck/fglat` (#172 on `ca8804bc`) passed every job incl. the Windows kit dry run; local 941/941 tests (plain,
  TSan, ASan+UBSan). One known false alarm: the EngineController syntax check against `ed317efb` shows the same
  diagnostics as its base. Known limit: admission still allows 10 ms of lateness, so under overload `grace` drops frames
  the GPU already made. **Review: ACCEPT** (independent reviewer, 22 Nov): defaults unchanged with the switch unset; no
  part can add latency; the hold part adds two uneven intervals per hold on PS5 streams (stated in the draft). The three
  minor findings are fixed in `e3fcaccb` (`fallbackFps` now counts only frames a hold showed early; the drain resets the
  ULL hold; the docs say the kit arms turn on all four parts at once, so each part, e.g. `VEYRA_EXP_RP_FG_ULL=hold`,
  needs its own arm before it can become a default). Its checks passed (ck/fglat `a92c347a`; the kit dry run passed on
  the previous run and this commit does not touch the kit). Not built on Windows yet.

### 4.2 Stream `nrperf` ("DLSS 5" stutter)

- Private branch `exp/s6-nrperf` (from `exp/s6-capture`); public snapshot `handoff/nrperf`.
- **Done (code reading, not yet reviewed):** `docs/drafts/NR_STUTTER.md`. The "DLSS 5" option is NVIDIA's
  `nvngx_dlssnr.dll` 310.8.0.0 (NGX feature 18), called through the DLL's own exports (no public SDK, no Streamline),
  on the decoded picture with optical flow and a constant depth; real frames only, after SR and before FG, in series on
  the graph queue; 1920x1080 inference by default. Likely cause (mechanism from code, size an estimate ~5.5 ms per
  inference): the live engine takes the next decoded frame only after the previous pair's whole GPU work; when base +
  FG passes 16.68 ms, frames are replaced in the one-slot mailbox and late frames lose their generated frames ->
  2X/1X alternation and FgCostSettling holds (plus fglat's dead phase). By estimate Enhanced 1440p at 2X fits; 3X/180,
  SR to 4K, VFG High and nrSize 1440/native do not.
- **Finished by its author:** head `66193080` (Package 10 merged in through `exp/s6-capture`), tree clean. Placement
  study: keep the current order; the amount of inference per pair is the lever, not placement. Checks on `66193080`:
  Linux, MSVC core tests, /W4 /WX headers, PowerShell parse, Python and lint passed; local 934/934 core tests (plain,
  TSan, ASan); the Windows kit dry run passed on `5494a27b` and was still running on `66193080`. Not done (by design):
  no live NR size cap, no adaptive size, no new log line. Needs the RTX 5070 and PS5 for the four-way runs and to judge
  the adaptive arm's picture by eye. **Review: ACCEPT**; its two doc fixes are made (`634c787d`:
  the motion pass at level 0 adds its GPU time to the phase; the four-way set takes ~14 min).
- Earlier status: tree clean (no uncommitted work). The NR inference-reuse controller is behind
  `VEYRA_EXP_RP_NR_ADAPT` (off by default; its own cost at level 0 is one flow-sized motion pass per real frame and
  about 100-175 MB of textures), with the four-way kit arms and the placement study in `NR_STUTTER.md`. No Windows exe
  build yet.

### 4.3 Dependencies

- Package 12 depends on Package 11's integration: `fglat` and `nrperf` branch from `exp/s6-capture` (which contains
  `exp/s6-fps`), so they must be merged after Package 11 and will meet the same kit-list conflicts.
- `fglat` owns the frame-generation phase fix; `nrperf` must not build a second one.
- Both extend existing diagnostics (`[slipstream-smooth]`, `[slipstream-capture]`); no duplicate fields.

---

## 5. Known issues, warnings and open items

**Known issues (verified):**
- After using the Ultra Low Latency picture profile together with the lowest-latency switch, check Smooth pacing.
- An even 90 FPS is not possible from a 60 FPS game with whole multipliers; 2X (up to 120) can be even on a 120 Hz or
  VRR screen.
- `VEYRA_EXP_RP_CLOCK_RATE=1` must not be paired with frame generation (it can bring back 3X under a 120 cap).
- GitHub may still serve one old commit by its hash (`f267a036` on the former `ck/capture`) whose test file used the
  owner's first name as an example; the branch was overwritten. Only GitHub support can purge it sooner.

**Pending reviews:** none. Package 11's and Package 12's integration reviews are done (ACCEPT; Package 12 with the
kit fix in section 4).

**Hardware verification still needed (none of it has run on his PC):** everything in Packages 10-12. His next runs, as
listed in Package 12's START-HERE.txt (each step ends with one zip to send back):
1. `Install-Slipstream.cmd` (answer YES to the NVIDIA neural runtime question to test the "DLSS 5" option).
2. Frame generation delay: `Run-First-Test.cmd -Session smoothness -Pass 120`, then Windows at 180 Hz and `-Pass 180`
   (about 36 min each; arms ending in `-ull` are the new low-latency frame generation).
3. "DLSS 5" stutter, four ways: `Run-First-Test.cmd -Session smoothness -Pass 120 -Arms
   nrp-base,nrp-nr,nrp-fg,nrp-nr-fg,nrp-nr-fg-adapt` (about 14 min), then `-Pass 180`; he says which runs stutter.
4. Still from Packages 10 and 11: `-Session retest` (about 25 min), `-Session input` (about 29 min; first "Apply GTA
   V-style keys"), and with a capture card `-Session capture -Pass 120` / `-Pass 180` (about 42 min each, once with a
   30 FPS game and once with a 60 FPS game).
5. Ten minutes or more of normal play for lip sync, then `Collect-Diagnostics.cmd`.
6. He still has to name his capture card.

**Must not be done:**
- Never push to `DosukaSOL/PushFPS`, `slipstream/rc2` or `slipstream/baseline`; never push tags; never edit
  `AGENTS.md`; never overwrite an `archive/*` branch; `slipstream/package` is overwritten only with the next package's source, after the previous
  one is kept as `archive/package-<label>` (Package 10: `archive/package-10` = `40c66eef`).
- Never commit NVIDIA binaries or SDK files, the private bundle, the orchestration zip, the owner's evidence, his
  name, console registration data or credentials. Never put private documents in a public branch.
- Never turn on an unvalidated feature by default. A default change is allowed only for a demonstrated defect whose fix
  provably cannot add latency, with tests and a `VEYRA_TEST_*` switch that restores the old behaviour.
- Never claim RTX 5070, PS5 or capture-card validation from CI, WARP or simulations.
- No automated aiming, recoil compensation, gameplay automation or anti-cheat circumvention; no external adapters,
  modified firmware, admin requirements, registry or router changes.
- Do not recommend buying capture hardware; do not present video processing as NVIDIA's game-integrated DLSS 5.
- Do not change Package 7 or any released package artifact.

---

## 6. How to build and package (verified procedure)

1. **Translations:** fill new strings in `i18n/catalog.json` (zh-TW, en, ja by hand; other languages may start as the
   English text), then `python3 -B scripts/i18n/extract.py --check` -> 0 missing, 0 placeholder problems. Teams leave
   their new strings in `i18n/pending/<stream>.json`.
2. **Local checks (Linux):** core tests:
   `g++ -std=c++20 -fsyntax-only -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -Iinclude tests/remoteplay/CoreTests.cpp`
   and the kit tests `python3 -B -m pytest -q tools/remoteplay_latency_bench/ packaging/slipstream/test_slipstream_package.py tools/clarity_frames`.
3. **Windows build:** push the public (squashed, private paths removed) branch to `slipstream/dev2` (or `dev3`). This
   runs `build-windows.yml`: MSVC /W4 /WX, unit tests, D3D12 WARP GPU tests, strict QML tests, the install test.
   `remoteplay-checks.yml` runs the Linux/MSVC core tests and the kit dry runs. A release needs all of them green
   (not a "trial" build).
4. **Fill `packaging/slipstream/CHECKED.txt`** (three paragraphs: the build number and run id; the CI results; the
   reviews; no `#` characters, lines <= 100 columns), archive the built tree as `archive/<sha>`.
5. **Package:** push the package commit to `slipstream/package`, cancel the build runs that push starts, then
   `gh workflow run slipstream-package.yml -R kashkevin/PushFPS --ref slipstream/package -f package_label=<label> -f experimental_run=<build run id> -f experimental_branch=slipstream/dev2`.
   The run assembles four copies (original, stable, experimental, previous = Package 7) and verifies them; it refuses
   any NVIDIA runtime file and any unfilled `CHECKED.txt`. Report the artifact name and the inner zip's SHA-256 from
   the log line `Slipstream-test-package-<label>-<date>-<n>.zip: … sha256 …`.

**Without the `gh` CLI (the owner's Windows PC, 22 Nov 2026):** git pushes work through Git Credential Manager as
`kashkevin`; its token has the `repo` and `workflow` scopes, so the same steps work through the REST API with curl
(read the token with `git credential fill`, never print it): job logs `GET /repos/kashkevin/PushFPS/actions/jobs/<id>/logs`,
cancel `POST .../actions/runs/<id>/cancel`, package `POST .../actions/workflows/slipstream-package.yml/dispatches` with
`{"ref":"slipstream/package","inputs":{"package_label":"12","experimental_run":"44980732479","experimental_branch":"slipstream/dev3"}}`.
Local checks on that PC: no g++ (C++ only in CI) and no pytest; run the kit tests with
`python -B -m unittest discover -s tools/remoteplay_latency_bench` and `-s packaging/slipstream`. Instead of overwriting
`slipstream/package`, a new package commit can be made with `git commit-tree <tree> -p origin/slipstream/package` so
the push is a fast-forward (done for Package 12).

---

## 7. Recovery checklist for a fresh Claude Code session

1. **Authenticate with GitHub:** `gh auth status` (if not logged in: `gh auth login`, account with push rights to
   `kashkevin/PushFPS`). Never use another account's tokens.
2. **Clone:** `git clone https://github.com/kashkevin/PushFPS.git && cd PushFPS` (or open an existing clone and
   `git remote -v`: the push remote must be `kashkevin/PushFPS`).
3. **Fetch:** `git fetch --all --prune` and list `git branch -r | grep -E 'handoff/|slipstream/|archive/|ck/'`.
4. **Read:** this file on `origin/handoff/console`, then `CLAUDE.md`, `docs/SLIPSTREAM_STATUS.md`, and the drafts of
   the stream you resume (`docs/drafts/*.md` on its `handoff/<stream>` branch).
5. **Inspect without overwriting:**
   a. Public only: create a local branch from the snapshot, e.g. `git switch -c work/fglat origin/handoff/fglat`; read
      `git log -1` and `git diff origin/slipstream/package...HEAD --stat`. Never reset or force-push a public branch.
   b. With the owner's private bundle (preferred, keeps history and private docs): `git bundle verify
      slipstream-private-branches.bundle`, then `git fetch slipstream-private-branches.bundle 'refs/heads/exp/*:refs/heads/exp/*'`.
      Keep `exp/*` local only; publish only squashed copies without private paths.
6. **Resume the right stream:** order of priority: finish Package 11 (section 3.3), then review and finish `fglat` and
   `nrperf` (section 4), merge them on top of Package 11, then Package 12 the same way. Work in a separate branch or
   worktree per stream; never edit another stream's branch.
7. **Tests and reviews:** run the local checks (section 6, step 2). Every stream needs an independent review by a
   reviewer who did not write it (defaults unchanged with nothing set; latency never added; tests meaningful;
   claims honest), then the reviewer's fixes and a second look. After merging streams, an integration review checks
   that every line each side added is present once (compare each file against base + each side's added lines).
8. **Next Windows build:** section 6, steps 3-5. Tell the owner the download link, the artifact name, the SHA-256,
   what changed by default, what was tested where, and the shortest test steps; nothing is validated until he runs it.

---

## 8. Next recommended engineering actions

**Development is paused (owner's instruction, 22 Nov 2026) until he sends his PC and PS5 test results.** Do not start
new features, rebuilds or new packages before that.

1. ~~Package 11~~: delivered 22 Nov 2026 (section 3).
2. ~~Review `fglat` and `nrperf`, merge onto Package 11, Package 12~~: delivered 22 Nov 2026 (section 4).
3. When the owner sends evidence: read the Package 12 smoothness passes (`-ull` arms against their non-ULL partners:
   `addedEstMs`, `deadPhaseFps`, `[fg-ull] held=` lines, cadence) and the four-way `nrp-*` runs with his notes on
   which runs stutter; then the Package 10 evidence below.
4. When the owner sends Package 10 evidence: read `retest` (bitrate, audio delay per run, decoder-reset causes,
   picture captures with `tools/clarity_frames/compare.py`) and the two smoothness passes (which target is even on his
   panel); only then consider default changes.
5. Ask the owner which capture card he bought before any device-specific work.
6. Optional, only if the owner asks: the three minor review notes in section 4 (doc wording, missing research-file
   references, the ULL over-long-value log line).

---

## 9. This handoff's own record

Branch `handoff/console` holds this file on top of Package 10's source. Snapshot branches made at this handoff:
see the table below (updated with each milestone).

| Public snapshot branch | Private source branch | Private head | Public head |
|---|---|---|---|
| `handoff/capture` | `exp/s6-capture` (includes Package 10) | `a3ed26a6` | `fa00456f` |
| `handoff/kbm` | `exp/s6-kbm` | `03ffbb87` | `95483276` |
| `handoff/fglat` | `exp/s6-fglat` | `e3fcaccb` | `d952d6af` |
| `handoff/nrperf` | `exp/s6-nrperf` (includes Package 10) | `634c787d` | `0911f502` |
| `handoff/rel-11` | `exp/rel-11` (Package 11 candidate) | `8a669ab4` | `f7e0241e` |
| `handoff/rel-12` | `exp/rel-12` (Package 12 candidate) | `7f15198e` | `811f4ddb` |
| `slipstream/package` | Package 12 (public commits only: `c41755fe` + `1471053e` + `ec38b34f`, made as `4781f4d4` on top of `9c5e80e8`) | — | `4781f4d4` |
| `archive/package-11` | `exp/rel-11` (Package 11, after its `CHECKED.txt`) | — | `9c5e80e8` |
| `archive/package-10` | `exp/rel-10` (Package 10) | `93587fc5` | `40c66eef` |

The Package 12 kit fix and `CHECKED.txt` were made on public branches only (no private `exp/*` commit for them): if
`exp/rel-12` is restored from the bundle, apply `1471053e` and `ec38b34f` to it to match what shipped.

Each public head is one squashed commit on top of `main` (`ed317efb`) with the private paths removed and the
`slipstream/**` build trigger added, so `git diff origin/main origin/handoff/<stream>` shows the whole stream. The
four stream snapshots are not built (their names do not match `slipstream/**`); to build one, push it to
`slipstream/dev2` or `slipstream/dev3`.

Not on GitHub (only on the orchestrating machine and in the bundle sent to the owner): the `exp/*` history, the task
briefs, review rules, the owner's directives and the state log, and the owner's evidence files.

### Milestone log

- 25 Nov 2026 ~14:30 UTC: **Package 16 delivered** (run 45633587347, SHA-256 `0828b8d3…e64a11b4`; build #92 with its
  end-to-end step failed 564 / 5, stated in the package). `archive/package-15` = `129c1377`, `archive/dd24a030`.
  Own generator: review 2 fixes in progress on `exp/s11-ownfg`, first full build on `slipstream/dev4`.

- 22 Nov 2026 ~01:50 UTC: handoff created. All four teams committed with clean trees (no WIP); capture's Package 10
  merge committed; nothing merged into Package 11 yet.
- 22 Nov ~02:00 UTC: `exp/rel-11` fast-forwarded to `a3ed26a6`; kbm is merging it. fglat review ACCEPT, minor fixes
  in progress.
- 22 Nov ~04:30 UTC: Package 11 delivered (run 44979515518). Package 12 merged (`aacf94c8`), package text `7f15198e`,
  build 44980732479, integration review started.
- 22 Nov ~04:05 UTC: Package 11 build #64 passed; package run 44979515518 started.
- 22 Nov ~03:15 UTC: Package 11 integration review ACCEPT, text fixes `8a669ab4`; nrperf fixes `634c787d`; Package 12
  merges (rel-11 + nrperf into `exp/s6-fglat`) started.
- 22 Nov ~02:55 UTC: kbm merged (`11e814ed`, verified); Package 11 package text `bcf4e29a`; strict build 44975871790 on
  `slipstream/dev3`; integration review started. nrperf review ACCEPT, two doc fixes in progress.
- 22 Nov ~02:30 UTC: nrperf finished `66193080`; review started; `handoff/nrperf` refreshed.
- 22 Nov ~02:20 UTC: fglat review fixes `e3fcaccb`; `handoff/fglat` refreshed.
- 22 Nov ~02:10 UTC: capture's Package 10 merge verified line by line; all its checks green.
- 22 Nov 03:44 UTC (Console session, times from GitHub): repository cloned on the owner's PC; build-windows #66
  running; all public snapshots consistent with the handoff (P11 build tree = package tree except text; P12 build tree
  = `handoff/rel-12`; fglat/nrperf files identical to their snapshots). The orchestrating session's Package 12
  integration review result was never recorded, so a new one was run.
- 22 Nov 04:05 UTC: build-windows #66 passed; remoteplay-checks #201 passed 04:08 UTC.
- 22 Nov ~07:20 UTC: `archive/package-11` = `9c5e80e8` created; Package 12 integration review (Sonnet) ACCEPT WITH
  FIXES; kit fix `1471053e` pushed to `ck/package-12`; `archive/c41755fe` created.
- 22 Nov ~08:15 UTC: checks #222 and remoteplay-checks #204 passed on `1471053e`; `CHECKED.txt` filled; package commit
  `4781f4d4` pushed to `slipstream/package` (fast-forward); #67/#205 cancelled; slipstream-package #24 dispatched.
- 22 Nov 08:22 UTC: **Package 12 delivered** (run 45002383758, SHA-256 `6e857b21…379f86cd`). Development paused until
  the owner's hardware results arrive.
