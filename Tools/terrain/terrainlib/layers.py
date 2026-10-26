"""Landscape paint-layer weightmaps derived from slope, altitude, aspect and drainage.

Layers (UE weight-blended, the 8-bit weights of every pixel sum to 255):

rock     slope above ~40-45 deg (cliffs, rock bands); snow cannot hold there.
snow     above the aspect-dependent snowline on slopes that can hold snow.
glacier  low slope (< ~15-25 deg) with large upstream catchment, from the snowline down to
         ``glacier_depth_m`` below it (glacier tongues such as the Khumbu reach ~800-900 m
         below the regional snowline), plus low-slope neve above the snowline.
scree    moderate slopes (~25-42 deg) below the snowline, boosted under cliffs.
grass    low altitude (below ``grassline_m``), gentle slopes.
dirt     everything else: moraine, bare ground, trails (residual layer).

Snowline: equilibrium-line altitude varies with aspect because poleward faces get less sun.
Field data for the Khumbu put the regional snowline near 5,500-5,800 m with north faces a few
hundred metres lower (e.g. Owen & Benn 2005, Quaternary International 138-139; Benn & Owen 1998).
The pipeline lowers the snowline by ``aspect_amp_m`` * cos(angle to the pole) on slopes > ~10 deg.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from scipy import ndimage

from .analysis import slope_aspect, smoothstep

LAYER_NAMES = ("rock", "snow", "glacier", "scree", "grass", "dirt")
LAYER_COLOURS = {  # preview colours (sRGB)
    "rock": (92, 84, 78),
    "snow": (245, 247, 252),
    "glacier": (128, 190, 225),
    "scree": (168, 150, 120),
    "grass": (98, 140, 64),
    "dirt": (140, 108, 76),
}

# Rough regional snowline (m) vs |latitude| when no preset value is given. Interpolated.
_SNOWLINE_BY_LAT = [(0, 4900), (10, 5000), (20, 5300), (30, 5500), (35, 5100), (40, 4000),
                    (46, 3000), (55, 1800), (63, 1900), (70, 900), (78, 200), (90, 0)]


def default_snowline_m(lat: float) -> float:
    xs, ys = zip(*_SNOWLINE_BY_LAT)
    return float(np.interp(abs(lat), xs, ys))


@dataclass
class LayerParams:
    snowline_m: float
    grassline_m: float
    latitude: float
    aspect_amp_m: float = 300.0
    rock_slope_lo_deg: float = 38.0
    rock_slope_hi_deg: float = 48.0
    snow_hold_lo_deg: float = 45.0
    snow_hold_hi_deg: float = 58.0
    glacier_depth_m: float = 900.0
    glacier_min_catchment_m2: float = 3.0e5
    seed: int = 1953


def _breakup_noise(shape: tuple[int, int], px: float, seed: int, wavelength_m: float) -> np.ndarray:
    """Cheap smooth noise in ~[-1, 1] to break up perfectly straight snowlines."""
    rng = np.random.default_rng(seed + 7)
    step = max(int(wavelength_m / px), 1)
    coarse = rng.uniform(-1, 1, size=(shape[0] // step + 3, shape[1] // step + 3))
    up = ndimage.zoom(coarse, step, order=3, grid_mode=False)[: shape[0], : shape[1]]
    return np.clip(up, -1.0, 1.0).astype(np.float32)


def compute_layers(h: np.ndarray, px: float, catchment_m2: np.ndarray,
                   p: LayerParams) -> dict[str, np.ndarray]:
    """Return float32 weights in [0, 1] per layer that sum to 1 per pixel."""
    slope, aspect = slope_aspect(ndimage.gaussian_filter(h, 0.75), px)
    poleward = np.cos(np.radians(aspect)) if p.latitude >= 0 else -np.cos(np.radians(aspect))
    tilt = smoothstep(slope, 5.0, 20.0)
    snowline = (p.snowline_m - p.aspect_amp_m * poleward * tilt
                + 80.0 * _breakup_noise(h.shape, px, p.seed, 300.0))

    rock = smoothstep(slope, p.rock_slope_lo_deg, p.rock_slope_hi_deg)
    above_snow = smoothstep(h - snowline, -60.0, 60.0)
    holds_snow = 1.0 - smoothstep(slope, p.snow_hold_lo_deg, p.snow_hold_hi_deg)

    log_area = np.log10(np.maximum(catchment_m2, 1.0))
    lo = np.log10(p.glacier_min_catchment_m2)
    channel = smoothstep(log_area, lo - 0.5, lo + 0.5)
    flat = 1.0 - smoothstep(slope, 15.0, 25.0)
    glacier_alt = smoothstep(h - (snowline - p.glacier_depth_m), 0.0, 150.0)
    neve = above_snow * (1.0 - smoothstep(slope, 10.0, 20.0)) * 0.5
    glacier = np.clip(flat * glacier_alt * np.maximum(channel, neve), 0.0, 1.0)

    # snow wins over rock where slopes can hold it; rock shows through on the steepest faces
    rock = rock * (1.0 - above_snow * holds_snow * 0.85)
    snow = above_snow * holds_snow * (1.0 - glacier)
    near_cliff = ndimage.gaussian_filter(smoothstep(slope, 40.0, 50.0), 25.0 / px)
    scree_band = smoothstep(slope, 22.0, 30.0) * (1.0 - smoothstep(slope, 40.0, 48.0))
    scree = scree_band * (1.0 - above_snow) * np.clip(0.45 + 1.5 * near_cliff, 0.0, 1.0)
    grass = ((1.0 - smoothstep(h, p.grassline_m - 150.0, p.grassline_m + 150.0))
             * (1.0 - smoothstep(slope, 28.0, 38.0)))

    named = {"rock": rock, "snow": snow, "glacier": glacier, "scree": scree, "grass": grass}
    stack = np.stack([np.clip(named[k], 0.0, 1.0) for k in LAYER_NAMES[:-1]]).astype(np.float32)
    total = stack.sum(axis=0)
    dirt = np.clip(1.0 - total, 0.0, 1.0)
    stack = np.concatenate([stack, dirt[None]], axis=0)
    stack /= np.maximum(stack.sum(axis=0), 1e-6)
    return {name: stack[i] for i, name in enumerate(LAYER_NAMES)}


def to_uint8_weights(layers: dict[str, np.ndarray], chunk_rows: int = 512) -> dict[str, np.ndarray]:
    """Quantise to 8 bits so that the weights of each pixel sum to exactly 255.

    Largest-remainder rounding: floor every weight, then give the missing units to the
    layers with the largest fractional parts.
    """
    names = list(layers)
    rows = next(iter(layers.values())).shape[0]
    out = {n: np.empty(layers[n].shape, dtype=np.uint8) for n in names}
    for r0 in range(0, rows, chunk_rows):
        stack = np.stack([layers[n][r0:r0 + chunk_rows] for n in names]) * 255.0
        q = np.floor(stack)
        remainder = 255.0 - q.sum(axis=0)
        rank = np.argsort(np.argsort(q - stack, axis=0, kind="stable"), axis=0)
        q += rank < remainder[None]
        for i, n in enumerate(names):
            out[n][r0:r0 + chunk_rows] = np.clip(q[i], 0, 255).astype(np.uint8)
    return out
