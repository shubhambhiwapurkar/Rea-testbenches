"""Terrain analysis on a north-up metric grid: slope, aspect, resampling, flow accumulation.

Grid convention used throughout the pipeline: row 0 is the NORTH edge, column 0 the WEST edge,
square pixels of ``px`` metres.
"""

from __future__ import annotations

import heapq
import math

import numpy as np
from scipy import ndimage


def slope_aspect(h: np.ndarray, px: float) -> tuple[np.ndarray, np.ndarray]:
    """Slope (degrees) and aspect (degrees clockwise from north, direction the slope faces)."""
    d_row, d_col = np.gradient(h.astype(np.float32, copy=False), px)
    dz_east = d_col
    dz_north = -d_row
    slope = np.degrees(np.arctan(np.hypot(dz_east, dz_north))).astype(np.float32)
    aspect = (np.degrees(np.arctan2(-dz_east, -dz_north)) % 360.0).astype(np.float32)
    return slope, aspect


def slope_deg(h: np.ndarray, px: float) -> np.ndarray:
    d_row, d_col = np.gradient(h.astype(np.float32, copy=False), px)
    return np.degrees(np.arctan(np.hypot(d_row, d_col))).astype(np.float32)


def smoothstep(x: np.ndarray | float, lo: float, hi: float) -> np.ndarray:
    t = np.clip((np.asarray(x, dtype=np.float32) - lo) / (hi - lo), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def resize(arr: np.ndarray, shape: tuple[int, int], order: int = 1) -> np.ndarray:
    """Resample a grid to an exact shape with pixel-area alignment."""
    zoom = (shape[0] / arr.shape[0], shape[1] / arr.shape[1])
    out = ndimage.zoom(arr, zoom, order=order, mode="nearest", grid_mode=True)
    if out.shape != shape:  # guard against rounding
        out = out[: shape[0], : shape[1]]
        out = np.pad(out, ((0, shape[0] - out.shape[0]), (0, shape[1] - out.shape[1])), mode="edge")
    return out.astype(np.float32, copy=False)


def block_mean(arr: np.ndarray, f: int) -> np.ndarray:
    """Downsample by an integer factor (crops the ragged edge)."""
    if f <= 1:
        return arr.astype(np.float32, copy=True)
    n0, n1 = (arr.shape[0] // f) * f, (arr.shape[1] // f) * f
    return arr[:n0, :n1].reshape(n0 // f, f, n1 // f, f).mean(axis=(1, 3)).astype(np.float32)


_D8 = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]


def priority_flood(h: np.ndarray, eps: float = 1e-3) -> np.ndarray:
    """Fill depressions so every cell drains to the border (Barnes et al. 2014, +epsilon)."""
    rows, cols = h.shape
    flat = h.ravel().astype(np.float64).tolist()
    filled = list(flat)
    seen = bytearray(rows * cols)
    heap: list[tuple[float, int]] = []
    for r in range(rows):
        for c in (0, cols - 1):
            i = r * cols + c
            if not seen[i]:
                seen[i] = 1
                heap.append((flat[i], i))
    for c in range(cols):
        for r in (0, rows - 1):
            i = r * cols + c
            if not seen[i]:
                seen[i] = 1
                heap.append((flat[i], i))
    heapq.heapify(heap)
    pop, push = heapq.heappop, heapq.heappush
    while heap:
        z, i = pop(heap)
        r, c = divmod(i, cols)
        for dr, dc in _D8:
            rr, cc = r + dr, c + dc
            if 0 <= rr < rows and 0 <= cc < cols:
                j = rr * cols + cc
                if not seen[j]:
                    seen[j] = 1
                    zj = flat[j]
                    if zj <= z:
                        zj = z + eps
                    filled[j] = zj
                    push(heap, (zj, j))
    return np.asarray(filled, dtype=np.float64).reshape(rows, cols)


def d8_accumulation(h: np.ndarray, cell_m: float) -> np.ndarray:
    """D8 upstream contributing area (m^2) on a depression-filled grid."""
    rows, cols = h.shape
    pad = np.pad(h, 1, mode="edge")
    best_drop = np.zeros_like(h)
    receiver = np.full(h.shape, -1, dtype=np.int64)
    rr, cc = np.mgrid[0:rows, 0:cols]
    for dr, dc in _D8:
        nb = pad[1 + dr: 1 + dr + rows, 1 + dc: 1 + dc + cols]
        drop = (h - nb) / math.hypot(dr, dc)
        tr, tc = rr + dr, cc + dc
        valid = (tr >= 0) & (tr < rows) & (tc >= 0) & (tc < cols)
        better = valid & (drop > best_drop)
        best_drop[better] = drop[better]
        receiver[better] = (tr * cols + tc)[better]
    order = np.argsort(-h, axis=None, kind="stable").tolist()
    rec = receiver.ravel().tolist()
    acc = [cell_m * cell_m] * (rows * cols)
    for i in order:
        j = rec[i]
        if j >= 0:
            acc[j] += acc[i]
    return np.asarray(acc, dtype=np.float64).reshape(rows, cols)


def flow_accumulation(h: np.ndarray, px: float, max_cells: int = 768) -> np.ndarray:
    """Upstream area (m^2) at full resolution, computed on a <= max_cells^2 coarse grid."""
    f = max(1, math.ceil(max(h.shape) / max_cells))
    pr, pc = (-h.shape[0]) % f, (-h.shape[1]) % f
    padded_shape = (h.shape[0] + pr, h.shape[1] + pc)
    coarse = block_mean(np.pad(h, ((0, pr), (0, pc)), mode="edge"), f)
    filled = priority_flood(coarse)
    acc = d8_accumulation(filled, px * f)
    log_acc = ndimage.gaussian_filter(np.log10(acc).astype(np.float32), 0.7)
    full = resize(log_acc, padded_shape, order=1)[: h.shape[0], : h.shape[1]]
    return np.power(10.0, full).astype(np.float32)
