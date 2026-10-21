"""
One Euro filter (Casiez, Roussel and Vogel, CHI 2012).

A first-order low-pass filter whose cutoff frequency rises with the speed of the signal: a still head is
smoothed heavily, where jitter would be visible, and a turning head is followed with little lag, where lag
is what the operator feels. Time steps come from the sample timestamps, so the behaviour does not depend on
the rate at which samples arrive (the headset sends one head-pose packet per rendered frame, 72-120 Hz).

Parameters:
    min_cutoff  cutoff at rest, Hz; lower = smoother when still
    beta        cutoff increase per unit of speed (Hz per unit/s); higher = less lag when moving
    d_cutoff    cutoff of the speed estimate itself, Hz
"""

import math
from typing import Optional


def _alpha(cutoff_hz: float, dt_s: float) -> float:
    tau = 1.0 / (2.0 * math.pi * cutoff_hz)
    return 1.0 / (1.0 + tau / dt_s)


class OneEuroFilter:
    def __init__(self, min_cutoff: float, beta: float, d_cutoff: float):
        if min_cutoff <= 0 or d_cutoff <= 0 or beta < 0:
            raise ValueError("One Euro filter needs min_cutoff > 0, d_cutoff > 0 and beta >= 0")
        self.min_cutoff = min_cutoff
        self.beta = beta
        self.d_cutoff = d_cutoff
        self.x: Optional[float] = None   # filtered value
        self.dx = 0.0                     # filtered speed, units/s

    def reset(self):
        self.x = None
        self.dx = 0.0

    def __call__(self, value: float, dt_s: float) -> float:
        if self.x is None:
            self.x = value
            self.dx = 0.0
            return value
        raw_dx = (value - self.x) / dt_s
        self.dx += _alpha(self.d_cutoff, dt_s) * (raw_dx - self.dx)
        cutoff = self.min_cutoff + self.beta * abs(self.dx)
        self.x += _alpha(cutoff, dt_s) * (value - self.x)
        return self.x
