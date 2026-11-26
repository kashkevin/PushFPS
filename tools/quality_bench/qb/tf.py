# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Transfer functions: SMPTE ST 2084 (PQ, HDR10), a simple SDR curve, code quantisation, ICtCp and delta E ITP.

Everything in the chain is carried as CODE VALUES (0..1) exactly as the capture card delivers them (PQ codes for HDR10,
gamma codes for SDR) and decoded to nits only where light is integrated (the eye) or where a filter decodes.
"""
from __future__ import annotations

import numpy as np

M1, M2 = 2610.0 / 16384.0, 2523.0 / 4096.0 * 128.0
C1, C2, C3 = 3424.0 / 4096.0, 2413.0 / 4096.0 * 32.0, 2392.0 / 4096.0 * 32.0
SDR_WHITE_NITS = 200.0      # SDR model: code 1.0 = 200 nits, gamma 2.2 (a label for "SDR", not his monitor's setting)
SDR_GAMMA = 2.2


def pq_encode(nits):
    """Nits (0..10000) -> PQ code 0..1."""
    y = np.clip(np.asarray(nits, dtype=np.float64) / 10000.0, 0.0, 1.0)
    p = y ** M1
    return ((C1 + C2 * p) / (1.0 + C3 * p)) ** M2


def pq_decode(code):
    """PQ code 0..1 -> nits."""
    p = np.clip(np.asarray(code, dtype=np.float64), 0.0, 1.0) ** (1.0 / M2)
    return 10000.0 * (np.maximum(p - C1, 0.0) / (C2 - C3 * p)) ** (1.0 / M1)


def sdr_encode(nits):
    return np.clip(np.asarray(nits, dtype=np.float64) / SDR_WHITE_NITS, 0.0, 1.0) ** (1.0 / SDR_GAMMA)


def sdr_decode(code):
    return SDR_WHITE_NITS * np.clip(np.asarray(code, dtype=np.float64), 0.0, 1.0) ** SDR_GAMMA


def quantize(code, bits):
    q = float((1 << bits) - 1)
    return np.round(np.clip(code, 0.0, 1.0) * q) / q


class Signal:
    """HDR10 (PQ, 10-bit codes) or SDR (gamma 2.2, 8-bit codes, white 200 nits)."""

    def __init__(self, hdr: bool):
        self.hdr = bool(hdr)
        self.bits = 10 if hdr else 8
        self.name = "HDR10" if hdr else "SDR"

    def encode(self, nits, quant=True):
        c = pq_encode(nits) if self.hdr else sdr_encode(nits)
        return quantize(c, self.bits) if quant else c

    def decode(self, code):
        return pq_decode(code) if self.hdr else sdr_decode(code)

    def quant(self, code):
        return quantize(code, self.bits)

    def code_step(self):
        return 1.0 / float((1 << self.bits) - 1)


# ---- ICtCp (ITU-R BT.2100) and delta E ITP (ITU-R BT.2124) -------------------------------------------------------------
_RGB2LMS = np.array([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], dtype=np.float64) / 4096.0
_LMS2ICTCP = np.array([[2048, 2048, 0], [6610, -13613, 7003], [17933, -17390, -543]], dtype=np.float64) / 4096.0


def ictcp(rgb2020_nits):
    """Linear BT.2020 RGB in nits (..., 3) -> ICtCp (..., 3) (PQ variant)."""
    lms = np.asarray(rgb2020_nits, dtype=np.float64) @ _RGB2LMS.T
    return pq_encode(lms) @ _LMS2ICTCP.T


def delta_e_itp(rgb_a, rgb_b):
    """delta E ITP = 720 sqrt(dI^2 + dT^2 + dP^2), T = 0.5 Ct, P = Cp. About 1 = a just-noticeable difference."""
    a, b = ictcp(rgb_a), ictcp(rgb_b)
    d = a - b
    return 720.0 * np.sqrt(d[..., 0] ** 2 + (0.5 * d[..., 1]) ** 2 + d[..., 2] ** 2)


def hue_angle_deg(rgb2020_nits):
    c = ictcp(rgb2020_nits)
    return np.degrees(np.arctan2(c[..., 2], 0.5 * c[..., 1]))
