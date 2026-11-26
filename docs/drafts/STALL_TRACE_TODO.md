# Record-Stall-Trace.ps1: held back (review S10_STALL_REVIEW1, verdict C: REJECT for this package)

The draft (docs/drafts/Record-Stall-Trace.ps1) is NOT in the test kit and is not to be sent. Before it can be:
1. `wpr -start` sits outside the `try`: in Windows PowerShell 5.1 with `$ErrorActionPreference = 'Stop'`, a stderr line from
   `& wpr.exe ... 2>&1` is a terminating error, so the NT Kernel Logger can be left running (128 MB of non-paged memory) with
   no `wpr -cancel`. The same pattern at the `-stop`, `-cancel` and `tracerpt` calls. Fix: `$ErrorActionPreference =
   'Continue'` around native calls, start inside the `try`, check `wpr -status` before starting.
2. Closing the window with X skips `finally`: print "if you close this window, run wpr -cancel" and check for a running
   session at the next start.
3. It waits only for `[capture-stall] kind=gap`, which Package 15 does not write: on his install it waits 25 minutes and
   saves nothing. Use the Package 15 detector (Collect-Stall-Evidence.ps1 v2: [capture-timing] deficit, [capture-callback]
   late callbacks, [live-anchor] leads) or the quarter-hour model's predicted times.
4. The USB providers (USBXHCI, UCX, USBHUB3) are enabled with no level or keyword limits: per-transfer events on a 4K
   capture stream add work to the very path under study. Restrict them to errors and power events.
5. The 16 MB event-collector ring probably wraps within seconds at that rate (event rate not measured), so the stall second
   can be overwritten before the save at +5 s. Poll every 0.5 s, save 2 s after the stall, size the kernel ring for >= 15 s.
6. The .etl holds every process's image path and command line: private; say so in the script and in the notes.
