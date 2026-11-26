# PushWarp, PushFPS's own frame generator: what a later package could let you try (draft for the owner)
Labels: SIMULATED = worked out on a computer with made-up pictures, no graphics card. TESTED = run on Microsoft's software
renderer (WARP) in the automatic checks. Nothing here has run on your PC or any real graphics card, and no full PushFPS
build contains this version yet.

1. **Today it would not start on your PC.** PushWarp gets its motion from AMD FidelityFX optical flow, and on your PC that
   part failed its start-up check in Package 16. We found the likely cause (the test picture it was checked with) and
   changed it, but that fix has not run on your card. If the check fails again, choosing PushWarp turns frame generation
   off, says why, and offers "Back to NVIDIA VFG" in one click.
2. **What it is for.** Smooth, even in-between frames without ghost copies of the crosshair. It works like NVIDIA VFG (the
   real frame, then the made-up ones). It is not about delay. Nothing chooses it for you.
3. **What a comparison would offer.** Same settings, NVIDIA VFG against PushWarp, about a minute each: how the picture
   looks (copies of the crosshair and HUD text when you turn, edges where things cross), how even the motion is, and how
   hard the graphics card works. The quality test can keep the last result of each and show both in one table.
4. **Both in-between frames made the same way.** At 3X PushWarp makes the frame at 1/3 and the one at 2/3 with the same
   method from the same motion (TESTED). NVIDIA VFG at your current quality setting (level 5, "Medium + Low") makes the
   1/3 frame with its Low model and the 2/3 frame with Medium: your own log shows 1.87 ms against 4.16 ms for the two
   (research note SETTINGS_AUDIT section 9). Quality level 1 uses Medium for both.
5. **Crosshair and HUD** (SIMULATED and TESTED, not seen in a game): a still crosshair, HUD text or minimap ring over a
   moving picture is found from the two real frames and kept in place, so the moving picture around it should no longer
   pull copies of it along. On made-up pictures most of the copies went away. Limits:
   - A see-through crosshair is only half handled (about half the copies remain).
   - A crosshair that changes between two frames (firing, a hit marker) is not found in that moment.
   - Nothing is found in the first moment the camera starts to move.
   - Exclusion boxes and HUD Lock work as before.
6. **Thin fast things** (SIMULATED): a thin object that moves further than its own width each frame (a pole, a wire, a
   far car) used to vanish from the in-between frames; that is fixed as far as PushWarp's motion can see the object. The
   motion comes in blocks of 32 pixels at 4K, so a much thinner object often has no motion of its own: it still vanishes
   from the in-between frames and shows only in the real ones.
7. **Fast turns** (SIMULATED): PushWarp used to give up and repeat the picture above about 480 px of movement per real
   frame at 4K; the limit is now about 960 px. On made-up fast turns that looked clearly better than repeating.
8. **How it compares** (SIMULATED, made-up pictures): slightly worse than a learned interpolator on ordinary motion,
   clearly worse at thin railings with cars behind them and in flashes and fades. Repeating patterns (a fence, a grille,
   rows of trunks) can confuse its motion over a large part of the picture. Not measured on game footage or in HDR on a
   card.
9. **What your log would show** (one line per second, the same names for both generators): which generator ran; the
   generator's GPU time; how fast the picture moved; how many pixels were kept in place as overlay; frames shown on
   time, late or repeated.
10. **Lower delay** (the "Lower delay" row, predicted frames): unchanged and opt-in; no more work on it in this round.
