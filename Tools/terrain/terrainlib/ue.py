"""Unreal Engine 5 Landscape sizing and import-setting maths.

Landscape height encoding (UE source: Runtime/Landscape, LandscapeDataAccess.h)
---------------------------------------------------------------------------
Heights are stored as uint16 ``h``. The landscape-local height is::

    LocalZ = (h - 32768) * LANDSCAPE_ZSCALE,   LANDSCAPE_ZSCALE = 1/128

and world Z (cm) = Location.Z + LocalZ * Scale.Z. With Scale.Z = 100 the 65536 steps span
65536/128 * 100 cm = 51 200 cm = 512 m, i.e. -256 m .. +256 m around Location.Z, which is
the documented "Z scale 100 = 512 m range". Hence, for a heightmap normalised so that
h = 0 is ``min_m`` and h = 65535 is ``max_m``::

    cm per step  = Scale.Z / 128           must equal  (max_m - min_m) * 100 / 65535
    => Scale.Z   = (max_m - min_m) * 12800 / 65535     (= range_m * 100/512 * 65536/65535)
    world Z(h)   = Location.Z + (h - 32768) * Scale.Z / 128
    altitude(h)  = min_m + h * (max_m - min_m) / 65535
    => Location.Z = 100 * min_m + SeaLevelWorldZCm + 256 * Scale.Z

where ``SeaLevelWorldZCm`` is the world Z (cm) of 0 m altitude, so that in game
``altitude_m = (WorldZ_cm - SeaLevelWorldZCm) / 100``.

Horizontal: Scale.X = Scale.Y = pixel size in cm; a heightmap of N vertices spans
(N - 1) * Scale.X cm. Location X/Y = -(N - 1)/2 * Scale so the peak sits at the world origin.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass

import numpy as np

UE_HEIGHT_STEPS = 65535  # max uint16 value
LANDSCAPE_ZSCALE = 1.0 / 128.0
LANDSCAPE_MID = 32768


@dataclass(frozen=True)
class LandscapeLayout:
    """One row of the UE 'recommended landscape sizes' table."""

    size: int                 # vertices per side == heightmap pixels per side
    quads_per_section: int
    sections_per_component: int  # per side (1 => 1x1, 2 => 2x2)
    components_per_side: int

    @property
    def quads_per_component(self) -> int:
        return self.quads_per_section * self.sections_per_component

    @property
    def total_components(self) -> int:
        return self.components_per_side ** 2

    def as_dict(self) -> dict:
        d = asdict(self)
        d["section_size_label"] = f"{self.quads_per_section}x{self.quads_per_section} quads"
        d["sections_per_component_label"] = (
            f"{self.sections_per_component}x{self.sections_per_component} sections")
        d["components_label"] = f"{self.components_per_side}x{self.components_per_side}"
        d["total_components"] = self.total_components
        return d


# Epic's recommended sizes (Landscape Technical Guide). Every size satisfies
# size - 1 == quads_per_section * sections_per_component * components_per_side, so the
# heightmap maps 1:1 to vertices with no stretching or padding on import.
RECOMMENDED_LAYOUTS: dict[int, LandscapeLayout] = {
    1009: LandscapeLayout(1009, 63, 2, 8),     # 64 components
    2017: LandscapeLayout(2017, 63, 2, 16),    # 256 components
    4033: LandscapeLayout(4033, 63, 2, 32),    # 1024 components
    8129: LandscapeLayout(8129, 127, 2, 32),   # 1024 components, bigger sections
}
for _lay in RECOMMENDED_LAYOUTS.values():
    assert _lay.size - 1 == _lay.quads_per_component * _lay.components_per_side


def choose_size(extent_m: float, resolution_m: float, tiles: int = 1) -> int:
    """Smallest recommended size whose pixel size is <= resolution_m (largest if none fits)."""
    for size in sorted(RECOMMENDED_LAYOUTS):
        if extent_m / (tiles * (size - 1)) <= resolution_m + 1e-9:
            return size
    return max(RECOMMENDED_LAYOUTS)


@dataclass(frozen=True)
class ZMapping:
    """How the 16-bit range maps to real altitude and to UE world Z."""

    min_m: float
    max_m: float
    scale_z: float              # Landscape Scale.Z
    location_z_cm: float        # Landscape Location.Z
    sea_level_world_z_cm: float  # world Z of altitude 0 m

    @property
    def metres_per_step(self) -> float:
        return self.scale_z / 128.0 / 100.0

    def altitude_to_h(self, alt_m: float) -> float:
        return (alt_m - self.min_m) / (self.max_m - self.min_m) * UE_HEIGHT_STEPS

    def h_to_world_z_cm(self, h: float) -> float:
        return self.location_z_cm + (h - LANDSCAPE_MID) * self.scale_z * LANDSCAPE_ZSCALE

    def world_z_cm_to_altitude(self, z_cm: float) -> float:
        return (z_cm - self.sea_level_world_z_cm) / 100.0


def z_mapping(min_m: float, max_m: float, z_origin: str = "mid") -> ZMapping:
    """Derive Scale.Z / Location.Z for a [min_m, max_m] normalised heightmap.

    z_origin:
      sealevel  world Z 0 == 0 m altitude (SeaLevelWorldZCm = 0); the landscape sits at
                its real altitude (fine with UE5 Large World Coordinates).
      min       the lowest point of the landscape sits at world Z 0.
      mid       the landscape's mid-height (h = 32768) sits at world Z 0, i.e.
                Location.Z = 0: smallest |Z| everywhere, best float precision.
    """
    if max_m <= min_m:
        raise ValueError("max must be greater than min")
    scale_z = round((max_m - min_m) * 128.0 * 100.0 / UE_HEIGHT_STEPS, 6)
    half_range_cm = LANDSCAPE_MID * scale_z * LANDSCAPE_ZSCALE  # 256 * scale_z
    if z_origin == "sealevel":
        sea = 0.0
        loc_z = 100.0 * min_m + half_range_cm
    elif z_origin == "min":
        loc_z = half_range_cm
    elif z_origin == "mid":
        loc_z = 0.0
    else:
        raise ValueError(f"unknown z_origin {z_origin!r}")
    loc_z = round(loc_z, 3)
    sea = loc_z - half_range_cm - 100.0 * min_m  # consistent with the rounded values
    return ZMapping(min_m, max_m, scale_z, loc_z, round(sea, 3))


def import_settings(n: int, pixel_m: float, zmap: ZMapping, layout: LandscapeLayout | None,
                    tile_offset_px: tuple[int, int] = (0, 0), total_n: int | None = None) -> dict:
    """The values to type into UE's Landscape 'Import from File' panel for one heightmap."""
    total_n = total_n or n
    scale_xy = round(pixel_m * 100.0, 6)
    half = (total_n - 1) / 2.0 * scale_xy
    loc_x = -half + tile_offset_px[0] * scale_xy
    loc_y = -half + tile_offset_px[1] * scale_xy
    out = {
        "location_cm": {"x": round(loc_x, 3), "y": round(loc_y, 3), "z": zmap.location_z_cm},
        "rotation_deg": {"pitch": 0.0, "yaw": 0.0, "roll": 0.0},
        "scale": {"x": scale_xy, "y": scale_xy, "z": zmap.scale_z},
        "heightmap_resolution": f"{n}x{n}",
        "sea_level_world_z_cm": zmap.sea_level_world_z_cm,
        "altitude_formula": "altitude_m = (WorldZ_cm - SeaLevelWorldZCm) / 100",
        "sea_level_offset_m": round(-zmap.sea_level_world_z_cm / 100.0, 5),
        "sea_level_offset_formula": "altitude_m = WorldZ_cm / 100 + sea_level_offset_m",
        "height_precision_m_per_step": round(zmap.metres_per_step, 6),
        "z_range_covered_m": round(65536 / 128 * zmap.scale_z / 100, 3),
    }
    if layout is not None:
        out["landscape_layout"] = {
            "section_size": f"{layout.quads_per_section}x{layout.quads_per_section} Quads",
            "sections_per_component": (
                f"{layout.sections_per_component}x{layout.sections_per_component} Sections"),
            "number_of_components": f"{layout.components_per_side}x{layout.components_per_side}",
            "overall_resolution": f"{layout.size}x{layout.size}",
        }
    return out


def landscape_table_markdown() -> str:
    rows = ["| Heightmap | Section size | Sections/component | Components | Total |",
            "|---|---|---|---|---|"]
    for lay in RECOMMENDED_LAYOUTS.values():
        rows.append(f"| {lay.size}x{lay.size} | {lay.quads_per_section}x{lay.quads_per_section} "
                    f"| {lay.sections_per_component}x{lay.sections_per_component} "
                    f"| {lay.components_per_side}x{lay.components_per_side} "
                    f"| {lay.total_components} |")
    return "\n".join(rows)


def ue_size_for_tiles(size: int, tiles: int) -> int:
    """Total heightmap vertices per side for tiles x tiles tiles sharing edge vertices."""
    return tiles * (size - 1) + 1


def is_recommended(size: int) -> bool:
    return size in RECOMMENDED_LAYOUTS


def float32_ulp_cm(max_abs_cm: float) -> float:
    """float32 spacing (cm) at the largest |world coordinate| (informational)."""
    return float(np.spacing(np.float32(abs(max_abs_cm))))
