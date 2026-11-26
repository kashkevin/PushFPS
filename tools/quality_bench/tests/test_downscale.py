# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""The bench's downscale filters against the vendored s12-pace C++ reference (g++) and known answers."""
import shutil

import numpy as np
import pytest

from qb import downscale as ds, native, scenes, tf

needs_gxx = pytest.mark.skipif(shutil.which("g++") is None, reason="g++ not available")


@needs_gxx
@pytest.mark.parametrize("filt,area", [("p16", False), ("area", True)])
def test_port_equals_vendored_header(filt, area):
    rng = np.random.default_rng(0)
    sig = tf.Signal(True)
    img = sig.encode(rng.uniform(1, 1000, (48, 96)))
    mine = ds.downscale(img, sig, filt, quant=False)
    ref = native.downscale_ref(img, 64, 32, area, True)
    assert np.abs(mine - ref)[3:-3, 3:-3].max() < 1e-9   # interior: the reference clamps, the bench wraps


def test_exact_area_conserves_light_and_flat_fields():
    sig = tf.Signal(True)
    flat = sig.encode(np.full((6, 48), 1000.0))
    for f in ds.FILTERS:
        assert np.allclose(sig.decode(ds.downscale(flat, sig, f)), sig.decode(flat[:4, :32]), rtol=2e-3)
    sc = scenes.Scene(48, 6, scenes.lines(10.0, [(21.25, 1, 300.0)]), scenes.still(), qy=1)
    o = sig.decode(ds.downscale(sig.encode(sc.render(0.0), quant=False), sig, "exact", quant=False))
    assert (o[2] - 10.0).sum() == pytest.approx(290.0 / 1.5, rel=1e-6)


def test_package16_drops_a_one_pixel_stroke_in_every_third_column():
    sig = tf.Signal(True)
    peaks = []
    for k in range(3):
        sc = scenes.Scene(48, 6, scenes.lines(0.0, [(21 + k, 1, 300.0)]), scenes.still(), qy=1)
        peaks.append(sig.decode(ds.downscale(sig.encode(sc.render(0.0)), sig, "p16"))[2].max())
    assert min(peaks) < 2.0 and max(peaks) > 60.0   # S12_PACE_REVIEW2: 44 nits mean, 1 nit in the worst column
