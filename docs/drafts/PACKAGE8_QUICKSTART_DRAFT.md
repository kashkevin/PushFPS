# DRAFT: Package 8 in five steps

`TODO(orchestrator)`: release name (8A, 8B or 8C), download link and SHA-256 of the zip.

1. **Unpack** the zip ("Extract All"). Open a Command Prompt in that folder (not as administrator).

2. **Install:** `Install-Slipstream.cmd` (10 to 20 minutes). It also updates an earlier 8A or 8B in
   place; your PS5 pairing and settings stay. Only 8C asks a question: keep NVIDIA's neural runtime?
   - **Yes:** kept only if it is an NVIDIA-signed original; the neural layer can then run (RTX 50).
   - **No:** everything else works; the neural arms say "not installed: skipped".
   To answer in advance: `Install-Slipstream.cmd -NeuralRuntime Yes` (or `No`).
   Unknown-publisher warning: "More info", then "Run anyway".
   Not paired yet? Pair once in any copy (`docs\SETUP.md`, section 3).

3. **First tests.** Start the game, stand somewhere quiet, run one command at a time:
   - **8C:** `Run-First-Test.cmd -Session input` (12 minutes), then `-Session visual` (22 minutes).
   - **8B:** `-Session input`, then `-Session pacing` (6 minutes).
   - **8A:** `-Session visual`, then your own eyes (START-HERE step 3: two comparison clips), then
     `-Session builds` (15 minutes).

   *input:* do what the lines starting with `>>>` say: mouse, keys, then the DualSense.
   *visual:* stand still with detail in view. For your HDR reference, pick H.265 HDR in the PS5
   connect dialog first (optional).

4. **Send back** each session's zip (`RESULT.txt` shows on screen). Interrupted? Run the same
   command again.

5. **Back to Package 7:** nothing to undo. Start
   `%LOCALAPPDATA%\Programs\PushFPS-slipstream-p7\PushFPS.exe`. `Rollback-Slipstream.cmd` switches the
   test settings file off; `Uninstall-Slipstream.cmd` removes every test copy (type YES). Your own
   PushFPS stays untouched.
