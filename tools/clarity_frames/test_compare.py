#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Tests of compare.py on made-up captures (synthetic pictures, never a PS5 frame)."""
import json, os, sys, tempfile, unittest
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare

def capture(folder, stem, raw, output=None, screen=None, hdr=0, dither=0, fine=None):
    meta = {"schema": 1, "stamp": "20261121T183000", "probe": 1, "pair": 0, "pts100ns": 1000, "hdrMode": hdr, "ditherStep": dither, "presentFine": fine,
            "raw": None, "output": None, "screen": None}
    for part, data, enc in (("raw", raw, "linear-float16"), ("output", output, "srgb8"), ("screen", screen, "srgb8")):
        if data is None: continue
        np.save(os.path.join(folder, f"{stem}-{part}.npy"), data)
        meta[part] = {"file": f"{stem}-{part}.npy", "width": data.shape[1], "height": data.shape[0], "dtype": str(data.dtype), "encoding": enc}
    with open(os.path.join(folder, stem + ".json"), "w") as f: json.dump(meta, f)

def picture(h=48, w=64, seed=1):
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:h, 0:w]
    base = 0.5 + 0.4 * np.sin(xx / 3.0) * np.cos(yy / 5.0)
    img = np.stack([base, base * 0.8, 1 - base], axis=-1) + rng.normal(0, 0.02, (h, w, 3))
    return np.clip(img, 0, 1).astype(np.float16)

class Compare(unittest.TestCase):
    def test_identical_and_rounding_and_changed_at_one_to_one(self):
        with tempfile.TemporaryDirectory() as d:
            raw = picture()
            exact = compare.to_codes8(raw.astype(np.float64))
            capture(d, "p1-f0", raw, exact)
            off = exact.copy(); off[0, 0, 0] = np.uint8(min(255, int(off[0, 0, 0]) + 1))
            capture(d, "p1-f1", raw, off)
            changed = np.clip(exact.astype(np.int16) + 3, 0, 255).astype(np.uint8)
            capture(d, "p1-f2", raw, changed)
            r = {x["capture"]: x for x in compare.analyse(d)}
            self.assertEqual(r["p1-f0"]["output"]["verdict"], "identical")
            self.assertEqual(r["p1-f1"]["output"]["verdict"], "identical within rounding")
            self.assertEqual(r["p1-f1"]["output"]["codesDiffering"], 1)
            self.assertTrue(r["p1-f2"]["output"]["verdict"].startswith("differs"))

    def test_dither_on_gives_no_verdict(self):
        with tempfile.TemporaryDirectory() as d:
            raw = picture()
            noisy = np.clip(compare.to_codes8(raw.astype(np.float64)).astype(np.int16) + 2, 0, 255).astype(np.uint8)
            capture(d, "p1-f3", raw, noisy, dither=1.0 / 255.0)
            r = compare.analyse(d)[0]
            self.assertEqual(r["output"]["verdict"], "dither on: up to ±2 codes expected (no verdict)")
            self.assertEqual(r["output"]["maxCodeDiff"], 2)

    def test_enlargement_against_references(self):
        with tempfile.TemporaryDirectory() as d:
            raw = picture(48, 64)
            lanczos = compare.to_codes8(compare.resample(raw.astype(np.float64), 96, 72, compare.k_lanczos3, 3))
            blurred = compare.to_codes8(compare.resample(raw.astype(np.float64), 96, 72, compare.k_linear, 1))
            capture(d, "p2-f0", raw, lanczos, blurred)
            png = os.path.join(d, "png"); os.makedirs(png)
            r = compare.analyse(d, png)[0]
            self.assertEqual(r["output"]["kind"], "enlarged")
            self.assertEqual(r["output"]["psnr"]["lanczos3-linear"], float("inf"))
            self.assertAlmostEqual(r["output"]["detailRatio"], 1.0, places=6)
            self.assertLess(r["screen"]["detailRatio"], 1.0)                     # bilinear is softer than Lanczos-3
            self.assertEqual(r["screen"]["psnr"]["bilinear-linear"], float("inf"))
            self.assertIn("presenter-model-fine", r["screen"]["psnr"]); self.assertIn("presenter-model-bilinear", r["screen"]["psnr"])   # kernel unknown
            self.assertTrue(os.path.isfile(os.path.join(png, "p2-f0-screen.png")))
            with open(os.path.join(png, "p2-f0-raw.png"), "rb") as f: self.assertEqual(f.read(8), b"\x89PNG\r\n\x1a\n")
            text = compare.report([r])
            self.assertIn("enlarged to 96x72", text)

    def test_fine_sample_never_leaves_the_neighbourhood(self):
        raw = picture(16, 16).astype(np.float64)
        out = compare.fine_sample(raw, 24, 24)
        self.assertGreaterEqual(out.min(), raw.min() - 1e-12)
        self.assertLessEqual(out.max(), raw.max() + 1e-12)

    def test_presenter_interpolates_srgb_codes_from_the_captured_output(self):
        # a 1-texel black / white checker: interpolating gamma-encoded codes gives a darker in-between than linear light
        h, w = 16, 16
        yy, xx = np.mgrid[0:h, 0:w]
        raw = np.repeat(((xx + yy) % 2).astype(np.float64)[..., None], 3, axis=2).astype(np.float16)
        out = compare.to_codes8(raw.astype(np.float64))
        for fine in (True, False):
            with tempfile.TemporaryDirectory() as d:
                screen = compare.presenter_model(out, 24, 24, fine)
                capture(d, "20261121T183000-p4-f0", raw, out, screen, fine=fine)
                r = compare.analyse(d)[0]
                self.assertEqual(r["output"]["verdict"], "identical")
                self.assertEqual(r["screen"]["psnr"]["presenter-model"], float("inf"))
                self.assertLess(r["screen"]["meanLumaVsLinear"], -5.0)            # darker than a gamma-correct scale
                self.assertIn("on sRGB codes", compare.report([r]))

    def test_hdr_and_bad_names_are_not_judged(self):
        with tempfile.TemporaryDirectory() as d:
            raw = picture()
            capture(d, "p3-f0", raw, compare.to_codes8(raw.astype(np.float64)), hdr=2)
            with open(os.path.join(d, "p3-f1.json"), "w") as f:
                json.dump({"schema": 1, "hdrMode": 0, "raw": {"file": "..\\x.npy"}}, f)
            r = {x["capture"]: x for x in compare.analyse(d)}
            self.assertIn("HDR capture", r["p3-f0"]["note"])
            self.assertIn("no raw picture", r["p3-f1"]["note"])

    def test_json_has_no_infinity(self):
        self.assertEqual(compare.finite({"a": [float("inf"), 1.0]}), {"a": [None, 1.0]})

if __name__ == "__main__":
    unittest.main(verbosity=1)
