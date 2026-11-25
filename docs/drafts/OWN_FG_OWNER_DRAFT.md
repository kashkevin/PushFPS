# PushWarp, PushFPS's own frame generator: what the next package would let you try (draft for the owner)
Labels: SIMULATED = offline, synthetic pictures, CPU. TESTED = on Microsoft's software renderer (WARP) in CI. BUILT = in
PushFPS.exe, started by the install test. HARDWARE-VERIFIED = none: nothing has run on your card.

1. **The switch.** Frame generation menu: "PushWarp" next to NVIDIA VFG, marked experimental (BUILT, TESTED). Nothing
   chooses it for you: not "Optimise for my PC", not a fallback. If it cannot start, frame generation turns off, the row
   says why and offers "Back to NVIDIA VFG" in one click.
2. **What the plain A/B answers.** Same settings, VFG vs PushWarp: picture, GPU cost and pacing. Not delay: the real frame
   is still shown after both generated frames, so the delay changes only by the generator's own cost (about 2 ms per
   frame for VFG in your log); the 22 ms wait of two displayed frames stays with either generator.
3. **What answers delay.** The "Lower delay" row: source NVIDIA vs PushFPS (one predicted frame after the real one), and
   the experimental "Full prediction" (the real frame first, two predicted frames). How much delay this saves on your PC
   is not known yet.
4. **Picture, as far as we know** (SIMULATED): PushWarp's in-between frame is about 0.6 dB below a learned interpolator on
   average, about 4 dB below at thin railings with cars behind, about 3 dB below in flashes and fades. Full prediction's
   frames are about 1.8 and 3.9 dB below. Not measured on game footage or in HDR on a card.
5. **What the row shows.** Which source makes the frame, "PushFPS makes it now" only while it runs, and plain reasons
   when it steps back or stops (unreliable predictions, too late, too slow on this card).
6. **What your log will show** (one line per second, the same names for both generators): `generator=nvidia-vfg` or
   `generator=pushwarp`; `gpu_fgBatch` / `gpu_fg1` (the same span for both); `flowMs`; `motionP50Px` / `motionP95Px`
   (how fast the picture moved, while PushWarp runs); `ownIGpuMs` (PushWarp's stages); frames shown on time, late or
   repeated. One minute of each generator in one session is enough to compare them.
7. **Fast turns** (SIMULATED): PushWarp used to repeat the whole picture above about 480 px of movement per real frame
   at 4K. The limit is now about 960 px; in synthetic fast turns of 500-900 px that is 1-5 dB better than repeating,
   with no whole-picture repeats. How it looks in a real game is unknown until your log (`motionP95Px`) or a capture.
