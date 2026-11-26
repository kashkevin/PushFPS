# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Offline end-to-end quality bench (stream s12-bench).

SIMULATION OF A MODEL CHAIN. Synthetic 4K scenes -> frame generation (none / ideal / block-motion MODEL / PushWarp's CPU
reference) -> 3840 -> 2560 downscale (Package 16's one bilinear tap / s12-pace's area filter / an exact linear-light area
filter) -> display holds at 179.952 Hz (ideal / a model of the composed desktop) -> an observer (tracking or fixating eye).
It shows mechanisms and their sizes relative to each other. It is NOT a measurement of the owner's screen, capture card,
graphics card or NVIDIA's generator. Every number it prints is SIMULATED.
"""
