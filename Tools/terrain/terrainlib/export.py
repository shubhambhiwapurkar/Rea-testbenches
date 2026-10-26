"""Heightmap / weightmap file writers and readers (formats UE5 Landscape import accepts)."""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image


def quantize_u16(h: np.ndarray, min_m: float, max_m: float) -> np.ndarray:
    """h16 = round((alt - min) / (max - min) * 65535), clamped to [0, 65535]."""
    scaled = (h.astype(np.float64) - min_m) * (65535.0 / (max_m - min_m))
    return np.clip(np.rint(scaled), 0, 65535).astype(np.uint16)


def dequantize_u16(h16: np.ndarray, min_m: float, max_m: float) -> np.ndarray:
    return (min_m + h16.astype(np.float64) * ((max_m - min_m) / 65535.0)).astype(np.float32)


def write_png16(path: Path, h16: np.ndarray) -> None:
    """16-bit single-channel grayscale PNG (UE: Landscape > Import > Heightmap File)."""
    Image.fromarray(np.ascontiguousarray(h16, dtype=np.uint16)).save(path, compress_level=6)


def write_r16(path: Path, h16: np.ndarray) -> None:
    """Headerless little-endian uint16, row-major, north row first (UE .r16 / .raw)."""
    np.ascontiguousarray(h16, dtype="<u2").tofile(path)


def read_heightmap(path: Path, size: int | None = None) -> np.ndarray:
    """Read a 16-bit PNG or .r16 written by this tool back into uint16."""
    if path.suffix.lower() in (".r16", ".raw"):
        data = np.fromfile(path, dtype="<u2")
        n = size or int(round(np.sqrt(data.size)))
        return data.reshape(n, n)
    with Image.open(path) as img:
        arr = np.asarray(img)
    if arr.dtype != np.uint16:
        arr = arr.astype(np.uint16)
    return arr


def write_mask(path: Path, w8: np.ndarray) -> None:
    """8-bit grayscale weightmap (UE: Landscape > Paint > Target Layers > Import from file)."""
    Image.fromarray(np.ascontiguousarray(w8, dtype=np.uint8)).save(path, optimize=True)


def read_mask(path: Path) -> np.ndarray:
    with Image.open(path) as img:
        return np.asarray(img.convert("L"))
