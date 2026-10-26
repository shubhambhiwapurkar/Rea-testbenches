"""Deterministic sub-30 m detail synthesis for upsampled DEMs.

Cubic upsampling of a 30 m DEM to 2-5 m only produces smooth surfaces. This module adds
plausible high-frequency relief while keeping the real low-frequency signal intact:

1. Multi-octave noise at wavelengths below what the 30 m DEM resolves (default 64 m down to
   ~2.5 pixels). Rocky terrain (steep) gets *ridged* noise with amplitude ~ 5 % of the
   wavelength (fractal, self-similar roughness) that is first smeared along the local fall
   line (line-integral convolution), so it forms ribs, gullies and couloirs running downhill
   as rockfall, avalanches and meltwater carve them, instead of isotropic "brain" patterns.
   Gentle terrain (snow, glacier, meadows) gets low-amplitude isotropic smooth noise whose
   high octaves are damped (snow smoothing). Every octave uses a randomly rotated lattice so
   no axis-aligned artefacts appear.
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
    fall_line_stretch: float = 2.5       # rock streak length along the fall line, x wavelength
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


def fall_line_field(h: np.ndarray, px: float, smooth_m: float = 40.0) -> tuple[np.ndarray, np.ndarray]:
    """Unit downslope direction (d_row, d_col) of the smoothed surface, in pixel units."""
    hs = ndimage.gaussian_filter(h, smooth_m / px)
    g_row, g_col = np.gradient(hs)
    norm = np.maximum(np.hypot(g_row, g_col), 1e-6)
    return (-g_row / norm).astype(np.float32), (-g_col / norm).astype(np.float32)


def _streak(n: np.ndarray, d_row: np.ndarray, d_col: np.ndarray, length_px: float,
            taps: int = 4, chunk_rows: int = 512) -> np.ndarray:
    """Line-integral convolution of n along the direction field over +-length_px/2."""
    rows, cols = n.shape
    out = np.zeros_like(n)
    step = length_px / (2 * taps)
    col_idx = np.arange(cols, dtype=np.float32)[None, :]
    for r0 in range(0, rows, chunk_rows):
        r1 = min(r0 + chunk_rows, rows)
        row_idx = np.arange(r0, r1, dtype=np.float32)[:, None]
        dr, dc = d_row[r0:r1], d_col[r0:r1]
        acc = np.zeros((r1 - r0, cols), dtype=np.float32)
        for k in range(-taps, taps + 1):
            w = 1.0 - abs(k) / (taps + 1)  # triangle kernel
            acc += w * ndimage.map_coordinates(n, [row_idx + k * step * dr, col_idx + k * step * dc],
                                               order=1, mode="reflect")
        out[r0:r1] = acc
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
    d_row, d_col = fall_line_field(h, px)
    detail = np.zeros(h.shape, dtype=np.float32)
    for lam in octave_wavelengths(px, p):
        t0 = time.time()
        n = _rotated_value_noise(h.shape, lam / px, rng)
        streaks = _streak(n, d_row, d_col, p.fall_line_stretch * lam / px)
        rock_amp = p.rock_amp_per_wavelength * lam
        soft_amp = p.soft_amp_per_wavelength * lam * math.sqrt(lam / p.max_wavelength_m)
        detail += p.strength * (rock * rock_amp * _ridged(streaks) + (1.0 - rock) * soft_amp * n)
        log(f"    noise octave {lam:5.1f} m: rock std {rock_amp:.2f} m, soft std {soft_amp:.2f} m"
            f" ({time.time() - t0:.1f}s)")
    detail -= ndimage.gaussian_filter(detail, p.lowpass_sigma_m / px)
    return detail


def thermal_erosion(h: np.ndarray, talus_tan: np.ndarray, px: float, iterations: int,
                    rate: float) -> np.ndarray:
    """4-neighbour thermal erosion; material above the per-pixel talus slope slides downhill.

    Each iteration, every pixel computes its height excess over the stable difference
    (talus * px) towards each lower neighbour, then moves rate * half of the largest excess,
    split between the neighbours in proportion to their excess (mass conserving).
    All buffers are preallocated: allocating ~10 full-size temporaries per iteration made
    the kernel spend most of its time in page faults.
    """
    h = h.astype(np.float32, copy=True)
    t = (talus_tan * px).astype(np.float32)  # max stable height difference to a neighbour
    t_l, t_r, t_u, t_d = t[:, :-1], t[:, 1:], t[:-1, :], t[1:, :]
    e_r, e_l, e_d, e_u = (np.zeros_like(h) for _ in range(4))  # excess towards each side
    dx = np.empty((h.shape[0], h.shape[1] - 1), dtype=np.float32)
    dy = np.empty((h.shape[0] - 1, h.shape[1]), dtype=np.float32)
    total, biggest, tmp = np.empty_like(h), np.empty_like(h), np.empty_like(h)
    half_rate = np.float32(rate * 0.5)
    for _ in range(iterations):
        np.subtract(h[:, :-1], h[:, 1:], out=dx)          # >0: west pixel higher
        np.subtract(h[:-1, :], h[1:, :], out=dy)          # >0: north pixel higher
        np.subtract(dx, t_l, out=e_r[:, :-1]); np.maximum(e_r, 0.0, out=e_r)
        np.negative(dx, out=dx)
        np.subtract(dx, t_r, out=e_l[:, 1:]); np.maximum(e_l, 0.0, out=e_l)
        np.subtract(dy, t_u, out=e_d[:-1, :]); np.maximum(e_d, 0.0, out=e_d)
        np.negative(dy, out=dy)
        np.subtract(dy, t_d, out=e_u[1:, :]); np.maximum(e_u, 0.0, out=e_u)
        np.add(e_r, e_l, out=total); total += e_d; total += e_u
        np.maximum(e_r, e_l, out=biggest); np.maximum(biggest, e_d, out=biggest)
        np.maximum(biggest, e_u, out=biggest)
        # k = rate/2 * biggest / total (0 where nothing moves); outflow_side = excess_side * k
        np.maximum(total, 1e-12, out=total)
        np.divide(biggest, total, out=biggest)
        biggest *= half_rate
        for e in (e_r, e_l, e_d, e_u):
            e *= biggest
        np.add(e_r, e_l, out=tmp); tmp += e_d; tmp += e_u
        h -= tmp
        h[:, 1:] += e_r[:, :-1]
        h[:, :-1] += e_l[:, 1:]
        h[1:, :] += e_d[:-1, :]
        h[:-1, :] += e_u[1:, :]
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


def peak_shift_stats(raw: np.ndarray, out: np.ndarray, px: float, window_m: float = 300.0,
                     search_m: float = 25.0) -> dict:
    """How much did detail move real peak heights?

    Peaks = raw-DEM pixels that are the maximum of a window_m square (upper half of the
    elevation range, away from the border). For each, compare the highest output pixel within
    search_m with the raw peak height.
    """
    size = int(window_m / px) | 1
    mf = ndimage.maximum_filter(raw, size=size)
    border = size
    mask = (raw == mf) & (raw > np.percentile(raw[::8, ::8], 50))
    mask[:border] = mask[-border:] = False
    mask[:, :border] = mask[:, -border:] = False
    k = max(1, int(round(search_m / px)))
    diffs = []
    for r, c in np.argwhere(mask):
        diffs.append(float(out[r - k:r + k + 1, c - k:c + k + 1].max() - raw[r, c]))
    if not diffs:
        return {"peaks": 0}
    d = np.abs(np.asarray(diffs))
    return {"peaks": len(diffs), "mean_abs_m": round(float(d.mean()), 2),
            "p95_abs_m": round(float(np.percentile(d, 95)), 2), "max_abs_m": round(float(d.max()), 2),
            "mean_signed_m": round(float(np.mean(diffs)), 2)}
