# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""The bench's downscale filters against the vendored s12-pace C++ reference (g++) and known answers."""
import shutil

import numpy as np
import pytest

from qb import downscale as ds, native, scenes, tf

needs_gxx = pytest.mark.skipif(shutil.which("g++") is None, reason="g++ not available")


@needs_gxx
@pytest.mark.parametrize("filt", sorted(ds.HEADER_KERNELS))
def test_port_equals_vendored_header(filt):
    rng = np.random.default_rng(0)
    sig = tf.Signal(True)
    img = sig.encode(rng.uniform(1, 1000, (48, 96)))
    mine = ds.downscale(img, sig, filt, quant=False)
    ref = native.downscale_ref(img, 64, 32, ds.HEADER_KERNELS[filt], 1)
    assert np.abs(mine - ref)[4:-4, 4:-4].max() < 1e-9   # interior: the reference clamps, the bench wraps


def test_every_kernel_runs_and_keeps_flat_fields_and_clamp_bounds():
    sig = tf.Signal(True)
    flat = sig.encode(np.full((6, 48), 1000.0))
    rng = np.random.default_rng(1)
    img = sig.encode(rng.uniform(1, 1000, (12, 48)))
    for f in ds.Q2_FILTERS:
        assert np.allclose(sig.decode(ds.downscale(flat, sig, f)), sig.decode(flat[:4, :32]), rtol=2e-3), f
    for f in ("catmull_rom_ar", "lanczos2_ar", "area_sharp_ar"):
        lin = sig.decode(img)
        out = sig.decode(ds.downscale(img, sig, f, quant=False))
        assert out.max() <= lin.max() + 1e-6 and out.min() >= lin.min() - 1e-6


def test_anti_ringing_removes_overshoot_but_plain_kernel_rings():
    sig = tf.Signal(True)
    sc = scenes.Scene(48, 6, scenes.lines(50.0, [(14.25, 14, 1000.0)]), scenes.still(), qy=1)
    c = sig.encode(sc.render(0.0))
    plain = sig.decode(ds.downscale(c, sig, "catmull_rom", quant=False))[2].max()
    clamped = sig.decode(ds.downscale(c, sig, "catmull_rom_ar", quant=False))[2].max()
    assert plain > 1000.5 and clamped <= 1000.0 + 1e-6


def test_cost_proxy_orders_filters():
    assert ds.cost("p16")["fetches"] < ds.cost("exact")["texels_2d"] < ds.cost("catmull_rom")["texels_2d"]


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
