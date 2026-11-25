# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Stream "ownfg" (review 1, SHOULD 8): PushWarp's [push-warp] lines (self-check verdict, the play-size run, "unavailable"
reasons, the memory line, the too-slow exit) are kept by the kit's three allow-lists, which stay identical."""
import pathlib
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import report  # noqa: E402

ROOT = HERE.parent.parent


class OwnFrameKit(unittest.TestCase):
    def test_push_warp_lines_are_kept(self):
        for line in ("[INFO ] [push-warp] ready 3840x2160 flow 960x540 format=28 selfCheck passed: I(2/3) matches the CPU reference",
                     "[WARN ] [push-warp] PushWarp unavailable (over the start-up budget of 3000 ms)",
                     "[WARN ] [push-warp] PushFPS frame too slow on this card: its I took 4.10 ms (P95 of 20 in play)"):
            self.assertTrue(report.ALLOWED.search(line), line)

    def test_three_lists_identical(self):
        for name in ("scripts/remoteplay/testkit/collect-diagnostics.ps1", "scripts/remoteplay/testkit/run-tests.ps1"):
            text = (ROOT / name).read_text(encoding="utf-8")
            self.assertIn("'" + report.ALLOWED.pattern + "'", text, name)


if __name__ == "__main__":
    unittest.main()
