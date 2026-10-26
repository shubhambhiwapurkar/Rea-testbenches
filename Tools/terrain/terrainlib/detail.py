"""Deterministic sub-30 m detail synthesis for upsampled DEMs.

Cubic upsampling of a 30 m DEM to 2-5 m only produces smooth surfaces. This module adds
plausible high-frequency relief while keeping the real low-frequency signal intact:

1. Multi-octave noise at wavelengths below what the 30 m DEM resolves (default 64 m down to
   ~2.5 pixels). Rocky terrain (steep) gets *ridged* noise with amplitude ~ 5 % of the
   wavelength (fractal, self-similar roughness); gentle terrain (snow, glacier, meadows)
   gets low-amplitude smooth noise whose high octaves are damped (snow smoothing).
   Every octave uses a rotated lattice so no axis-aligned artefacts appear.
2. The noise is high-passed (Gaussian, sigma ``lowpass_sigma_m``) so it has no mean shift.
3. Thermal erosion (Musgrave et al. 1989, "The synthesis and rendering of eroded fractal
   terrains") for a few hundred iterations. The talus angle is *relative to the real slope*
   (base slope + margin, never below ``talus_min_deg``) so real cliffs survive while synthetic
   over-steep micro-relief collapses into scree aprons.
4. Low-frequency restore: out = eroded + lowpass(original - eroded). Anything longer than
   ~60 m wavelength is exactly the original DEM again, so peaks and valleys do not move.

All randomness comes from ``numpy.random.default_rng(seed)``: same seed, same terrain.
"""

from __future__ import annotations

import math
import time
from dataclasses import dataclass
from typing import Callable

import numpy as np
from scipy import ndimage

from .analysis import slope_deg, smoothstep


@dataclass
class DetailParams:
    seed: int = 1953
    strength: float = 1.0               # global multiplier on noise amplitude
    max_wavelength_m: float = 64.0      # longest synthesised wavelength (~2x DEM posting)
    min_wavelength_px: float = 2.5      # shortest wavelength in pixels
    rock_amp_per_wavelength: float = 0.05   # std of rock noise = 5 % of its wavelength
    soft_amp_per_wavelength: float = 0.012  # std of snow/meadow noise at max wavelength
    rock_slope_lo_deg: float = 25.0
    rock_slope_hi_deg: float = 50.0
    erosion_iterations: int = 200
    erosion_rate: float = 0.5
    talus_min_deg: float = 33.0          # angle of repose of loose scree
    talus_margin_deg: float = 10.0       # extra steepness allowed over the real slope
    lowpass_sigma_m: float = 15.0


def _rotated_value_noise(shape: tuple[int, int], spacing_px: float, rng: np.random.Generator,
                         chunk_rows: int = 512) -> np.ndarray:
    """Smooth (cubic B-spline) value noise with a random rotation; unit standard deviation."""
    rows, cols = shape
    angle = rng.uniform(0.0, 2.0 * math.pi)
    ca, sa = math.cos(angle), math.sin(angle)
    lattice_n = int(math.ceil(math.hypot(rows, cols) / spacing_px)) + 8
    lattice = rng.uniform(-1.0, 1.0, size=(lattice_n, lattice_n))
    coeffs = ndimage.spline_filter(lattice, order=3, mode="grid-wrap")
    centre = lattice_n / 2.0
    out = np.empty(shape, dtype=np.float32)
    xs = (np.arange(cols, dtype=np.float64) - cols / 2.0) / spacing_px
    for r0 in range(0, rows, chunk_rows):
        r1 = min(r0 + chunk_rows, rows)
        ys = ((np.arange(r0, r1, dtype=np.float64) - rows / 2.0) / spacing_px)[:, None]
        u = ca * xs[None, :] - sa * ys + centre
        v = sa * xs[None, :] + ca * ys + centre
        out[r0:r1] = ndimage.map_coordinates(coeffs, [v, u], order=3, mode="grid-wrap",
                                             prefilter=False)
    out -= out.mean()
    out /= max(float(out.std()), 1e-6)
    return out


def _ridged(n: np.ndarray) -> np.ndarray:
    """Ridged transform: sharp crests where the noise crosses zero; unit std, zero mean."""
    r = 1.0 - np.minimum(np.abs(n) * 0.5, 1.0)
    r = r * r
    r -= r.mean()
    r /= max(float(r.std()), 1e-6)
    return r


def octave_wavelengths(px: float, p: DetailParams) -> list[float]:
    out, lam = [], p.max_wavelength_m
    while lam >= p.min_wavelength_px * px:
        out.append(lam)
        lam *= 0.5
    return out


def synthesize_noise(h: np.ndarray, px: float, p: DetailParams, log: Callable[[str], None]) -> np.ndarray:
    """Slope-weighted fractal noise (metres), already high-passed."""
    rng = np.random.default_rng(p.seed)
    rock = smoothstep(slope_deg(h, px), p.rock_slope_lo_deg, p.rock_slope_hi_deg)
    rock = ndimage.gaussian_filter(rock, 2.0)
    detail = np.zeros(h.shape, dtype=np.float32)
    for lam in octave_wavelengths(px, p):
        t0 = time.time()
        n = _rotated_value_noise(h.shape, lam / px, rng)
        rock_amp = p.rock_amp_per_wavelength * lam
        soft_amp = p.soft_amp_per_wavelength * lam * math.sqrt(lam / p.max_wavelength_m)
        detail += p.strength * (rock * rock_amp * _ridged(n) + (1.0 - rock) * soft_amp * n)
        log(f"    noise octave {lam:5.1f} m: rock std {rock_amp:.2f} m, soft std {soft_amp:.2f} m"
            f" ({time.time() - t0:.1f}s)")
    detail -= ndimage.gaussian_filter(detail, p.lowpass_sigma_m / px)
    return detail


def thermal_erosion(h: np.ndarray, talus_tan: np.ndarray, px: float, iterations: int,
                    rate: float) -> np.ndarray:
    """4-neighbour thermal erosion; material above the per-pixel talus slope slides downhill."""
    h = h.astype(np.float32, copy=True)
    t = (talus_tan * px).astype(np.float32)  # max stable height difference to a neighbour
    rows, cols = h.shape
    e_r = np.zeros_like(h); e_l = np.zeros_like(h); e_d = np.zeros_like(h); e_u = np.zeros_like(h)
    for _ in range(iterations):
        dx = h[:, :-1] - h[:, 1:]   # >0: left higher than right
        dy = h[:-1, :] - h[1:, :]   # >0: upper (north) higher than lower
        np.maximum(dx - t[:, :-1], 0.0, out=e_r[:, :-1])
        np.maximum(-dx - t[:, 1:], 0.0, out=e_l[:, 1:])
        np.maximum(dy - t[:-1, :], 0.0, out=e_d[:-1, :])
        np.maximum(-dy - t[1:, :], 0.0, out=e_u[1:, :])
        total = e_r + e_l + e_d + e_u
        biggest = np.maximum(np.maximum(e_r, e_l), np.maximum(e_d, e_u))
        # move half the largest excess (scaled by rate), split in proportion to each excess
        k = np.divide(rate * 0.5 * biggest, total, out=np.zeros_like(total), where=total > 0)
        out_r, out_l, out_d, out_u = e_r * k, e_l * k, e_d * k, e_u * k
        h -= out_r + out_l + out_d + out_u
        h[:, 1:] += out_r[:, :-1]
        h[:, :-1] += out_l[:, 1:]
        h[1:, :] += out_d[:-1, :]
        h[:-1, :] += out_u[1:, :]
    return h


def add_detail(h: np.ndarray, px: float, p: DetailParams,
               log: Callable[[str], None] = print) -> np.ndarray:
    """Full detail pipeline; returns a new float32 array (input untouched)."""
    t0 = time.time()
    h0 = h.astype(np.float32, copy=False)
    log(f"  detail: octaves {octave_wavelengths(px, p)} m, seed {p.seed}")
    noisy = h0 + synthesize_noise(h0, px, p, log)
    if p.erosion_iterations > 0:
        base_slope = ndimage.gaussian_filter(slope_deg(h0, px), 1.0)
        talus = np.clip(base_slope + p.talus_margin_deg, p.talus_min_deg, 80.0)
        t1 = time.time()
        noisy = thermal_erosion(noisy, np.tan(np.radians(talus)), px, p.erosion_iterations,
                                p.erosion_rate)
        log(f"    thermal erosion: {p.erosion_iterations} iterations ({time.time() - t1:.1f}s)")
    sigma = p.lowpass_sigma_m / px
    out = noisy + ndimage.gaussian_filter(h0 - noisy, sigma)
    log(f"  detail done in {time.time() - t0:.1f}s; |out-in| mean "
        f"{float(np.abs(out - h0).mean()):.2f} m, max {float(np.abs(out - h0).max()):.2f} m")
    return out.astype(np.float32)


def summit_correction(h: np.ndarray, px: float, row: int, col: int, target_m: float,
                      radius_m: float = 250.0) -> tuple[np.ndarray, float]:
    """Raise/lower the summit to target_m with a smooth raised-cosine bump of radius_m.

    Global 30 m DSMs under-read sharp summits (the peak falls between posts and radar/optical
    fill smooths it). The correction is local and smooth so the summit ridge keeps its shape.
    Returns (corrected array, applied delta in metres).
    """
    delta = float(target_m - h[row, col])
    r_px = int(math.ceil(radius_m / px))
    r0, r1 = max(row - r_px, 0), min(row + r_px + 1, h.shape[0])
    c0, c1 = max(col - r_px, 0), min(col + r_px + 1, h.shape[1])
    rr, cc = np.mgrid[r0:r1, c0:c1]
    dist = np.hypot(rr - row, cc - col) * px
    w = np.where(dist < radius_m, 0.5 * (1.0 + np.cos(np.pi * dist / radius_m)), 0.0)
    out = h.copy()
    out[r0:r1, c0:c1] += (delta * w).astype(np.float32)
    # the summit must be the local maximum: clip anything in the bump that now pokes above it
    window = out[r0:r1, c0:c1]
    np.minimum(window, target_m, out=window)
    out[row, col] = target_m
    return out, delta
