"""Peak presets and the build target description."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

PRESETS_PATH = Path(__file__).resolve().parent.parent / "peaks_presets.json"


@dataclass
class Landmark:
    name: str
    lat: float
    lon: float
    elevation_m: float | None = None
    summit: bool = False  # a secondary summit: reported as local max and clamped like the peak


@dataclass
class Peak:
    id: str
    name: str
    lat: float
    lon: float
    elevation_m: float | None = None
    default_extent_km: float = 16.0
    snowline_m: float | None = None
    grassline_m: float | None = None
    landmarks: list[Landmark] = field(default_factory=list)
    elevation_source: str = ""


def load_presets(path: Path = PRESETS_PATH) -> dict[str, Peak]:
    data = json.loads(path.read_text())
    peaks: dict[str, Peak] = {}
    for d in data["peaks"]:
        lms = [Landmark(**lm) for lm in d.get("landmarks", [])]
        peaks[d["id"]] = Peak(
            id=d["id"], name=d["name"], lat=float(d["lat"]), lon=float(d["lon"]),
            elevation_m=d.get("elevation_m"), default_extent_km=d.get("default_extent_km", 16.0),
            snowline_m=d.get("snowline_m"), grassline_m=d.get("grassline_m"), landmarks=lms,
            elevation_source=d.get("elevation_source", ""),
        )
    return peaks


def get_preset(peak_id: str) -> Peak:
    presets = load_presets()
    if peak_id not in presets:
        raise SystemExit(f"unknown peak id {peak_id!r}; known: {', '.join(sorted(presets))}")
    return presets[peak_id]


def slugify(name: str) -> str:
    out = "".join(ch.lower() if ch.isalnum() else "_" for ch in name)
    while "__" in out:
        out = out.replace("__", "_")
    return out.strip("_") or "terrain"
