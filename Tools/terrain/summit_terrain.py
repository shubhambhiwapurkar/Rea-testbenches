#!/usr/bin/env python3
"""SUMMIT terrain pipeline: real-world DEM -> Unreal Engine 5 Landscape heightmaps.

Subcommands
-----------
presets        list the peak presets
check-presets  compare every preset coordinate/elevation with the Copernicus DEM (remote read)
fetch          download the Copernicus GLO-30 tiles an area needs into Tools/terrain/cache/
build          mosaic, reproject (local metric projection), crop, resample, optional detail,
               write 16-bit PNG + .r16 heightmaps, 8-bit layer weightmaps and <name>.json
               with the exact UE import settings
preview        hillshade + contour (or layer composite) preview from a build's JSON

Examples
--------
  python summit_terrain.py fetch --peak-id everest --extent-km 20
  python summit_terrain.py build --peak-id everest --extent-km 20 --ue-size 4033 --detail
  python summit_terrain.py build --lat 46.5775 --lon 8.0053 --name eiger --extent-km 10 \
      --resolution-m 2 --ue-size auto --source swissalti3d_mosaic.tif
  python summit_terrain.py preview --metadata output/everest/everest.json --width 1400 \
      --out ../../Docs/images/everest_hillshade.png
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import shlex
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rasterio.crs import CRS  # noqa: E402
from rasterio.transform import Affine  # noqa: E402

from terrainlib import __version__  # noqa: E402
from terrainlib import analysis, dem, detail, export, layers, render, ue  # noqa: E402
from terrainlib.presets import Landmark, Peak, get_preset, load_presets, slugify  # noqa: E402

TOOL_DIR = Path(__file__).resolve().parent
CACHE_DIR = TOOL_DIR / "cache"
OUTPUT_DIR = TOOL_DIR / "output"
SUMMIT_SEARCH_RADIUS_M = 200.0  # Copernicus summits sit up to ~130 m from surveyed coordinates


def log(msg: str) -> None:
    print(msg, flush=True)


# ======================================================================================
# Target and grid planning
# ======================================================================================
def resolve_peak(args: argparse.Namespace) -> Peak:
    """Preset by --peak-id, or an ad-hoc peak from --lat/--lon/--name (+ --elevation-m)."""
    if getattr(args, "peak_id", None):
        peak = get_preset(args.peak_id)
        if getattr(args, "elevation_m", None) is not None:
            peak.elevation_m = args.elevation_m
        return peak
    if args.lat is None or args.lon is None:
        raise SystemExit("give --peak-id or both --lat and --lon")
    name = getattr(args, "name", None) or f"terrain_{args.lat:.4f}_{args.lon:.4f}"
    return Peak(id=slugify(name), name=name, lat=args.lat, lon=args.lon,
                elevation_m=getattr(args, "elevation_m", None))


@dataclass
class GridPlan:
    extent_m: float
    tile_size: int        # vertices per side of one UE landscape (tile)
    tiles: int            # tiles per side
    n: int                # total vertices per side
    pixel_m: float
    crs: CRS
    transform: Affine     # pixel-is-area transform whose pixel centres are the vertices

    @property
    def half_m(self) -> float:
        return self.extent_m / 2.0

    def xy_to_colrow(self, x: float, y: float) -> tuple[float, float]:
        return (x + self.half_m) / self.pixel_m, (self.half_m - y) / self.pixel_m


def landmarks_extent_m(peak: Peak, crs: CRS, margin_m: float = 750.0) -> float:
    """Smallest square extent (centred on the peak) that contains every preset landmark."""
    need = 0.0
    for lm in peak.landmarks:
        x, y = dem.project_point(crs, lm.lon, lm.lat)
        need = max(need, abs(x) + margin_m, abs(y) + margin_m)
    return 2.0 * need


def plan_grid(args: argparse.Namespace, peak: Peak) -> GridPlan:
    crs = dem.local_crs(peak.lat, peak.lon, args.projection)
    extent_m = (args.extent_km or peak.default_extent_km) * 1000.0
    lm_extent = landmarks_extent_m(peak, crs) if peak.landmarks else 0.0
    if lm_extent > extent_m:
        if args.fit_landmarks:
            new = math.ceil(lm_extent / 500.0) * 500.0
            log(f"  widening extent {extent_m / 1000:.1f} km -> {new / 1000:.1f} km to include landmarks")
            extent_m = new
        else:
            log(f"  WARNING: landmarks need {lm_extent / 1000:.2f} km; pass --fit-landmarks to widen")
    tiles = max(1, args.tiles)
    if args.ue_size == "auto":
        size = ue.choose_size(extent_m, args.resolution_m, tiles)
    else:
        size = int(args.ue_size)
        if not ue.is_recommended(size):
            log(f"  WARNING: {size} is not a UE recommended size {sorted(ue.RECOMMENDED_LAYOUTS)}")
    n = ue.ue_size_for_tiles(size, tiles)
    px = extent_m / (n - 1)
    half = extent_m / 2.0
    transform = Affine(px, 0.0, -half - px / 2.0, 0.0, -px, half + px / 2.0)
    return GridPlan(extent_m, size, tiles, n, px, crs, transform)


# ======================================================================================
# Source acquisition
# ======================================================================================
@dataclass
class SourceInfo:
    kind: str                 # copernicus | local
    paths: list[str]
    tiles: list[dict]
    fill_uncovered: float | None


def needed_tiles(crs: CRS, x0: float, y0: float, x1: float, y1: float,
                 margin_m: float = 2000.0) -> tuple[list[dem.TileId], tuple[float, float, float, float]]:
    bounds = dem.square_geo_bounds(crs, x0, y0, x1, y1, margin_m)
    return dem.tiles_for_bounds(*bounds), bounds


def acquire_sources(args: argparse.Namespace, plan: GridPlan) -> tuple[SourceInfo, tuple]:
    h = plan.half_m
    tiles, geo_bounds = needed_tiles(plan.crs, -h, -h, h, h)
    if args.source:
        paths = [str(Path(p).resolve()) for p in args.source]
        for p in paths:
            if not Path(p).exists():
                raise SystemExit(f"--source file not found: {p}")
        log(f"  using local source(s): {', '.join(Path(p).name for p in paths)}")
        return SourceInfo("local", paths, [], None), geo_bounds
    log(f"  Copernicus GLO-30 tiles needed: {', '.join(t.short() for t in tiles)}")
    results = dem.fetch_tiles(tiles, Path(args.cache_dir), refresh=args.refresh, log=log)
    paths = [str(r.path) for r in results if r.path is not None]
    info = [{"tile": r.tile.name(1), "status": r.status,
             "file": Path(r.path).name if r.path else None} for r in results]
    return SourceInfo("copernicus", paths, info, 0.0), geo_bounds


# ======================================================================================
# Measurements (summit / landmark readings)
# ======================================================================================
def geo_readings(mosaic: dem.Mosaic, lon: float, lat: float, radius_m: float) -> dict:
    """Raw source-DEM reading at a lon/lat (bilinear) and the local max around it."""
    if mosaic.crs.is_geographic:
        x, y, mpu = lon, lat, dem.metres_per_degree(lat)
    else:
        x, y = dem.project_point(mosaic.crs, lon, lat)
        mpu = (1.0, 1.0)
    at = dem.sample_bilinear(mosaic.data, mosaic.transform, x, y)
    mx, r, c, dist = dem.local_max(mosaic.data, mosaic.transform, x, y, radius_m, mpu)
    return {"at_point_m": round(at, 2), "local_max_m": round(mx, 2),
            "local_max_offset_m": round(dist, 1)}


def grid_readings(grid: np.ndarray, plan: GridPlan, x: float, y: float, radius_m: float) -> dict:
    at = dem.sample_bilinear(grid, plan.transform, x, y)
    mx, r, c, dist = dem.local_max(grid, plan.transform, x, y, radius_m)
    return {"at_point_m": round(at, 2), "local_max_m": round(mx, 2), "local_max_row": r,
            "local_max_col": c, "local_max_offset_m": round(dist, 1)}


# ======================================================================================
# Build
# ======================================================================================
def load_raw_grid(args, plan: GridPlan, src: SourceInfo, geo_bounds) -> tuple[np.ndarray, dem.Mosaic]:
    t0 = time.time()
    mosaic = dem.build_mosaic(src.paths, geo_bounds, fill_uncovered=src.fill_uncovered)
    log(f"  mosaic {mosaic.data.shape[1]}x{mosaic.data.shape[0]} px in {mosaic.crs.to_string()[:40]}"
        f" ({time.time() - t0:.1f}s)")
    t0 = time.time()
    grid = dem.warp_mosaic(mosaic, plan.crs, plan.transform, (plan.n, plan.n))
    grid, void = dem.fill_voids_nearest(grid)
    if void > 0:
        log(f"  WARNING: {void * 100:.2f}% of the grid had no data; filled with nearest values")
    log(f"  reprojected + cubic-resampled to {plan.n}x{plan.n} @ {plan.pixel_m:.3f} m "
        f"({time.time() - t0:.1f}s)")
    return grid, mosaic


def peak_anchor(raw: np.ndarray, plan: GridPlan) -> tuple[int, int]:
    """Summit pixel: highest raw-DEM pixel within SUMMIT_SEARCH_RADIUS_M of the peak."""
    _, r, c, _ = dem.local_max(raw, plan.transform, 0.0, 0.0, SUMMIT_SEARCH_RADIUS_M)
    return r, c


def clamp_summit(h: np.ndarray, plan: GridPlan, anchor: tuple[int, int], target_m: float,
                 base_radius_m: float) -> tuple[np.ndarray, dict]:
    """Correct a summit pixel to its surveyed height with a smooth raised-cosine bump.

    The bump radius grows with the correction (3x delta) so the added slope stays < ~30 deg.
    """
    radius = max(base_radius_m, 3.0 * abs(target_m - float(h[anchor])))
    h, delta = detail.summit_correction(h, plan.pixel_m, anchor[0], anchor[1], target_m, radius)
    return h, {"delta_m": round(delta, 2), "radius_m": round(radius, 1), "shape": "raised cosine"}


def clamp_landmark_summits(h: np.ndarray, raw: np.ndarray, plan: GridPlan, peak: Peak,
                           base_radius_m: float) -> tuple[np.ndarray, list[dict]]:
    """Apply the summit correction to landmarks flagged "summit": true (e.g. Lhotse)."""
    out = []
    for lm in peak.landmarks:
        if not lm.summit or lm.elevation_m is None:
            continue
        x, y = dem.project_point(plan.crs, lm.lon, lm.lat)
        if max(abs(x), abs(y)) > plan.half_m - SUMMIT_SEARCH_RADIUS_M:
            continue
        raw_max, r, c, dist = dem.local_max(raw, plan.transform, x, y, SUMMIT_SEARCH_RADIUS_M)
        h, info = clamp_summit(h, plan, (r, c), lm.elevation_m, base_radius_m)
        info.update({"name": lm.name, "reference_m": lm.elevation_m, "raw_local_max_m": round(raw_max, 2),
                     "offset_m": round(dist, 1), "row": r, "col": c})
        log(f"  secondary summit {lm.name} clamped: {info}")
        out.append(info)
    return h, out


def make_zmap(args, h: np.ndarray) -> ue.ZMapping:
    lo = float(h.min()) if args.z_min is None else args.z_min
    hi = float(h.max()) if args.z_max is None else args.z_max
    if args.z_min is None:
        lo = math.floor(lo * 10.0) / 10.0  # small headroom so no value clips
    if args.z_max is None:
        hi = math.ceil(hi * 10.0) / 10.0
    if hi - lo < 1.0:  # flat area (e.g. all sea): keep a 1 m range so Scale.Z is non-zero
        hi = lo + 1.0
    return ue.z_mapping(lo, hi, args.z_origin)


def tile_slices(plan: GridPlan):
    """Yield (i, j, row_slice, col_slice) for tiles sharing one edge row/column."""
    step = plan.tile_size - 1
    for j in range(plan.tiles):
        for i in range(plan.tiles):
            yield i, j, slice(j * step, j * step + plan.tile_size), slice(i * step, i * step + plan.tile_size)


def write_heightmaps(out_dir: Path, name: str, h16: np.ndarray, plan: GridPlan,
                     zmap: ue.ZMapping) -> list[dict]:
    files = []
    layout = ue.RECOMMENDED_LAYOUTS.get(plan.tile_size)
    if plan.tiles == 1:
        png, r16 = out_dir / f"{name}.png", out_dir / f"{name}.r16"
        export.write_png16(png, h16)
        export.write_r16(r16, h16)
        files.append({"tile": [0, 0], "png": png.name, "r16": r16.name,
                      "ue_import": ue.import_settings(plan.n, plan.pixel_m, zmap, layout)})
        return files
    # full mosaic too (World Partition can import it in one go)
    export.write_png16(out_dir / f"{name}_full.png", h16)
    for i, j, rs, cs in tile_slices(plan):
        stem = f"{name}_x{i}_y{j}"
        tile = h16[rs, cs]
        export.write_png16(out_dir / f"{stem}.png", tile)
        export.write_r16(out_dir / f"{stem}.r16", tile)
        off = (cs.start, rs.start)
        files.append({"tile": [i, j], "png": f"{stem}.png", "r16": f"{stem}.r16",
                      "ue_import": ue.import_settings(plan.tile_size, plan.pixel_m, zmap, layout,
                                                      tile_offset_px=off, total_n=plan.n)})
    return files


def write_layer_masks(out_dir: Path, name: str, w8: dict[str, np.ndarray], plan: GridPlan) -> dict:
    mask_dir = out_dir / "layers"
    mask_dir.mkdir(exist_ok=True)
    written: dict[str, list[str]] = {k: [] for k in w8}
    for lname, arr in w8.items():
        if plan.tiles == 1:
            p = mask_dir / f"{name}_layer_{lname}.png"
            export.write_mask(p, arr)
            written[lname].append(f"layers/{p.name}")
        else:
            for i, j, rs, cs in tile_slices(plan):
                p = mask_dir / f"{name}_x{i}_y{j}_layer_{lname}.png"
                export.write_mask(p, arr[rs, cs])
                written[lname].append(f"layers/{p.name}")
    return written


def collect_markers(peak: Peak, plan: GridPlan, anchor: tuple[int, int]) -> list[render.Marker]:
    label = peak.name.split(" (")[0]
    if peak.elevation_m:
        label += f" {peak.elevation_m:g} m"
    marks = [render.Marker(label, anchor[1], anchor[0], "summit")]
    for lm in peak.landmarks:
        x, y = dem.project_point(plan.crs, lm.lon, lm.lat)
        col, row = plan.xy_to_colrow(x, y)
        if 0 <= col < plan.n and 0 <= row < plan.n:
            marks.append(render.Marker(lm.name, col, row, "dot"))
    return marks


def landmark_report(peak: Peak, plan: GridPlan, raw: np.ndarray, final: np.ndarray,
                    mosaic: dem.Mosaic, zmap: ue.ZMapping) -> list[dict]:
    rows = []
    for lm in peak.landmarks:
        x, y = dem.project_point(plan.crs, lm.lon, lm.lat)
        inside = max(abs(x), abs(y)) <= plan.half_m
        entry = {"name": lm.name, "lat": lm.lat, "lon": lm.lon, "reference_m": lm.elevation_m,
                 "x_m": round(x, 1), "y_m": round(y, 1), "inside": inside}
        if inside:
            entry["source_dem_m"] = round(geo_readings(mosaic, lm.lon, lm.lat, 1.0)["at_point_m"], 2)
            entry["pipeline_raw_m"] = round(dem.sample_bilinear(raw, plan.transform, x, y), 2)
            entry["pipeline_final_m"] = round(dem.sample_bilinear(final, plan.transform, x, y), 2)
            compare = entry["pipeline_final_m"]
            if lm.summit:  # summits: compare the local maximum, not the (offset) coordinate
                entry["source_dem_local_max_m"] = geo_readings(
                    mosaic, lm.lon, lm.lat, SUMMIT_SEARCH_RADIUS_M)["local_max_m"]
                compare = entry["pipeline_final_local_max_m"] = round(
                    dem.local_max(final, plan.transform, x, y, SUMMIT_SEARCH_RADIUS_M)[0], 2)
            entry["ue_world_z_cm"] = round(zmap.h_to_world_z_cm(zmap.altitude_to_h(compare)), 1)
            if lm.elevation_m is not None:
                entry["delta_final_minus_reference_m"] = round(compare - lm.elevation_m, 2)
        rows.append(entry)
    return rows


def run_layers(args, peak: Peak, plan: GridPlan, h: np.ndarray) -> tuple[dict, dict]:
    t0 = time.time()
    snowline = args.snowline_m if args.snowline_m is not None else (
        peak.snowline_m if peak.snowline_m is not None else layers.default_snowline_m(peak.lat))
    grassline = args.grassline_m if args.grassline_m is not None else (
        peak.grassline_m if peak.grassline_m is not None else snowline - 900.0)
    params = layers.LayerParams(snowline_m=snowline, grassline_m=grassline, latitude=peak.lat,
                                aspect_amp_m=args.snow_aspect_amp_m, seed=args.seed)
    catchment = analysis.flow_accumulation(h, plan.pixel_m)
    weights = layers.to_uint8_weights(layers.compute_layers(h, plan.pixel_m, catchment, params))
    cover = {k: round(float(v.mean()) / 255.0 * 100.0, 1) for k, v in weights.items()}
    log(f"  layers (snowline {snowline:.0f} m, grassline {grassline:.0f} m) coverage %: {cover} "
        f"({time.time() - t0:.1f}s)")
    return weights, {"params": asdict(params), "coverage_percent": cover}


def cmd_build(args: argparse.Namespace) -> int:
    t_start = time.time()
    peak = resolve_peak(args)
    name = slugify(args.name) if getattr(args, "name", None) else peak.id
    out_dir = Path(args.output_dir) / name
    out_dir.mkdir(parents=True, exist_ok=True)
    log(f"[build] {peak.name} ({peak.lat:.5f}, {peak.lon:.5f}) -> {out_dir}")

    plan = plan_grid(args, peak)
    log(f"  grid: {plan.tiles}x{plan.tiles} tile(s) of {plan.tile_size} -> {plan.n}x{plan.n} px, "
        f"extent {plan.extent_m / 1000:.3f} km, {plan.pixel_m:.4f} m/px, {args.projection}")
    src, geo_bounds = acquire_sources(args, plan)
    raw, mosaic = load_raw_grid(args, plan, src, geo_bounds)

    summit = {"reference_m": peak.elevation_m,
              "source_dem": geo_readings(mosaic, peak.lon, peak.lat, SUMMIT_SEARCH_RADIUS_M),
              "pipeline_raw": grid_readings(raw, plan, 0.0, 0.0, SUMMIT_SEARCH_RADIUS_M)}
    anchor = peak_anchor(raw, plan)
    log(f"  summit: source DEM {summit['source_dem']}, grid {summit['pipeline_raw']}")

    h = raw
    detail_meta = None
    if args.detail:
        p = detail.DetailParams(seed=args.seed, strength=args.detail_strength,
                                erosion_iterations=args.erosion_iters)
        h = detail.add_detail(raw, plan.pixel_m, p, log)
        detail_meta = asdict(p)
        detail_meta["octaves_m"] = detail.octave_wavelengths(plan.pixel_m, p)
        detail_meta["shift_vs_raw_m"] = {
            "mean_abs": round(float(np.abs(h - raw).mean()), 3),
            "p99_abs": round(float(np.percentile(np.abs(h - raw)[::7, ::7], 99)), 3),
            "max_abs": round(float(np.abs(h - raw).max()), 3),
            "at_summit_pixel": round(float(h[anchor] - raw[anchor]), 3),
            "peaks": detail.peak_shift_stats(raw, h, plan.pixel_m)}
        log(f"  detail shift: {detail_meta['shift_vs_raw_m']}")
    summit["summit_pixel"] = {"row": anchor[0], "col": anchor[1],
                              "raw_m": round(float(raw[anchor]), 2),
                              "after_detail_m": round(float(h[anchor]), 2)}
    if peak.elevation_m is not None and not args.no_summit_clamp:
        h, summit["summit_correction"] = clamp_summit(h, plan, anchor, peak.elevation_m,
                                                      args.summit_radius_m)
        log(f"  summit clamped to {peak.elevation_m} m ({summit['summit_correction']})")
    if not args.no_summit_clamp:
        h, summit["secondary_summits"] = clamp_landmark_summits(h, raw, plan, peak, args.summit_radius_m)

    zmap = make_zmap(args, h)
    h16 = export.quantize_u16(h, zmap.min_m, zmap.max_m)
    summit["final_m"] = round(float(export.dequantize_u16(h16[anchor], zmap.min_m, zmap.max_m)), 3)
    if peak.elevation_m is not None:
        summit["final_minus_reference_m"] = round(summit["final_m"] - peak.elevation_m, 3)
        summit["source_dem_minus_reference_m"] = round(
            summit["source_dem"]["local_max_m"] - peak.elevation_m, 2)
    log(f"  elevation range {zmap.min_m:.1f} .. {zmap.max_m:.1f} m; Scale.Z {zmap.scale_z}, "
        f"Location.Z {zmap.location_z_cm} cm, SeaLevelWorldZCm {zmap.sea_level_world_z_cm}")
    files = write_heightmaps(out_dir, name, h16, plan, zmap)

    landmark_rows = landmark_report(peak, plan, raw, h, mosaic, zmap)
    for row in landmark_rows:
        log(f"  landmark {row['name']}: {json.dumps({k: v for k, v in row.items() if k != 'name'})}")

    layer_meta, weights = None, None
    if not args.no_layers:
        weights, layer_meta = run_layers(args, peak, plan, h)
        layer_meta["files"] = write_layer_masks(out_dir, name, weights, plan)

    markers = collect_markers(peak, plan, anchor)
    info = render.PreviewInfo(
        title=f"{peak.name}: {plan.extent_m / 1000:.1f} km, {plan.n}px @ {plan.pixel_m:.2f} m"
              + (", detail" if args.detail else ""),
        pixel_m=plan.pixel_m, markers=markers,
        footer=("Copernicus GLO-30 DEM (c) DLR e.V. 2010-2014, (c) Airbus DS 2014-2018, "
                "provided under COPERNICUS by the EU and ESA" if src.kind == "copernicus"
                else f"Source: {', '.join(Path(p).name for p in src.paths)}"))
    previews = write_previews(out_dir, name, h, weights, info, args.preview_width)

    meta = build_metadata(args, peak, name, plan, src, zmap, files, summit, landmark_rows,
                          detail_meta, layer_meta, previews, markers)
    meta_path = out_dir / f"{name}.json"
    meta_path.write_text(json.dumps(meta, indent=2))
    log(f"[build] wrote {meta_path} in {time.time() - t_start:.1f}s")
    print_import_summary(meta)
    return 0


def write_previews(out_dir: Path, name: str, h: np.ndarray, weights, info: render.PreviewInfo,
                   width: int) -> dict:
    out = {}
    img = render.render_hillshade(h, info, width)
    p = out_dir / f"{name}_hillshade.png"
    img.save(p, optimize=True)
    out["hillshade"] = p.name
    if weights is not None:
        img = render.render_layers(h, weights, info, width)
        p = out_dir / f"{name}_layers.png"
        img.save(p, optimize=True)
        out["layers"] = p.name
    return out


def build_metadata(args, peak: Peak, name: str, plan: GridPlan, src: SourceInfo, zmap: ue.ZMapping,
                   files: list[dict], summit: dict, landmarks: list[dict], detail_meta,
                   layer_meta, previews: dict, markers: list[render.Marker]) -> dict:
    layout = ue.RECOMMENDED_LAYOUTS.get(plan.tile_size)
    lon_c, lat_c = dem.unproject_point(plan.crs, 0.0, 0.0)
    corners = {k: dem.unproject_point(plan.crs, x, y) for k, (x, y) in {
        "nw": (-plan.half_m, plan.half_m), "ne": (plan.half_m, plan.half_m),
        "sw": (-plan.half_m, -plan.half_m), "se": (plan.half_m, -plan.half_m)}.items()}
    max_abs_cm = max(abs(zmap.h_to_world_z_cm(0)), abs(zmap.h_to_world_z_cm(65535)), plan.half_m * 100)
    return {
        "name": name,
        "generator": {"tool": "Tools/terrain/summit_terrain.py", "version": __version__,
                      "command": " ".join(shlex.quote(a) for a in sys.argv),
                      "created_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds")},
        "peak": {"id": peak.id, "name": peak.name, "lat": peak.lat, "lon": peak.lon,
                 "reference_elevation_m": peak.elevation_m,
                 "reference_elevation_source": peak.elevation_source},
        "source": {
            "kind": src.kind,
            "dataset": "Copernicus DEM GLO-30 Public (DSM, 1 arc-second)" if src.kind == "copernicus"
            else "local GeoTIFF(s)",
            "files": [Path(p).name for p in src.paths], "tiles": src.tiles,
            "vertical_datum": "EGM2008 geoid (m above MSL)" if src.kind == "copernicus" else "as source",
            "attribution": dem.COPERNICUS_ATTRIBUTION if src.kind == "copernicus" else None,
        },
        "projection": {
            "kind": args.projection, "proj4": plan.crs.to_proj4(), "wkt": plan.crs.to_wkt(),
            "center_lat": lat_c, "center_lon": lon_c,
            "corners_lonlat": {k: [round(v[0], 6), round(v[1], 6)] for k, v in corners.items()},
            "map_axes": "x = east (m), y = north (m), origin at the peak",
            "ue_axes": "UE +X = east, UE +Y = south (image row 0 = north edge = most negative Y); "
                       "peak at world X=0, Y=0",
        },
        "grid": {"size_px": plan.n, "tiles_per_side": plan.tiles, "tile_size_px": plan.tile_size,
                 "extent_m": plan.extent_m, "pixel_size_m": plan.pixel_m,
                 "bounds_m": {"xmin": -plan.half_m, "xmax": plan.half_m,
                              "ymin": -plan.half_m, "ymax": plan.half_m},
                 "registration": "pixel centres are landscape vertices; vertex (0,0) = NW corner",
                 "resampling": "cubic (single reprojection step)"},
        "elevation": {
            "min_m": zmap.min_m, "max_m": zmap.max_m, "range_m": round(zmap.max_m - zmap.min_m, 3),
            "encoding": "h16 = round((altitude_m - min_m) / (max_m - min_m) * 65535)",
            "decoding": "altitude_m = min_m + h16 * (max_m - min_m) / 65535",
            "precision_m_per_step": round(zmap.metres_per_step, 6),
        },
        "ue_import": {
            **ue.import_settings(plan.n, plan.pixel_m, zmap, layout if plan.tiles == 1 else None),
            "z_origin": args.z_origin,
            "derivation": {
                "landscape_local_z": "LocalZ = (h16 - 32768) / 128 (LANDSCAPE_ZSCALE = 1/128)",
                "world_z_cm": "WorldZ_cm = Location.Z + (h16 - 32768) / 128 * Scale.Z",
                "scale_100_means": "Scale.Z = 100 -> 65536/128*100 cm = 512 m total (-256..+256 m)",
                "scale_z": "Scale.Z = range_m * 100 * 128 / 65535  (= range_m/512*100 * 65536/65535)",
                "location_z": "Location.Z = 100*min_m + SeaLevelWorldZCm + 256*Scale.Z",
                "summit_settings": "set SummitSettings.SeaLevelWorldZCm to sea_level_world_z_cm",
            },
            "float32_ulp_at_extremes_cm": ue.float32_ulp_cm(max_abs_cm),
            "tiles": files,
        },
        "ue_landscape_sizes": [lay.as_dict() for lay in ue.RECOMMENDED_LAYOUTS.values()],
        "summit": summit,
        "landmarks": landmarks,
        "markers": [asdict(m) for m in markers],
        "detail": detail_meta,
        "layers": layer_meta,
        "previews": previews,
    }


def print_import_summary(meta: dict) -> None:
    imp = meta["ue_import"]
    log("\n=== UE5 Landscape import (Landscape mode > Manage > New > Import from File) ===")
    log(f"  Heightmap file : {imp['tiles'][0]['png']}  ({imp['heightmap_resolution']})")
    log(f"  Location (cm)  : X {imp['location_cm']['x']}  Y {imp['location_cm']['y']}  "
        f"Z {imp['location_cm']['z']}")
    log(f"  Scale          : X {imp['scale']['x']}  Y {imp['scale']['y']}  Z {imp['scale']['z']}")
    if "landscape_layout" in imp:
        lay = imp["landscape_layout"]
        log(f"  Section size {lay['section_size']}, {lay['sections_per_component']}, "
            f"components {lay['number_of_components']}")
    log(f"  SummitSettings.SeaLevelWorldZCm = {imp['sea_level_world_z_cm']}  "
        f"(altitude_m = (WorldZ_cm - SeaLevelWorldZCm)/100)")
    log(f"  height precision {imp['height_precision_m_per_step'] * 100:.2f} cm per 16-bit step")
    s = meta["summit"]
    log(f"  summit: source DEM local max {s['source_dem']['local_max_m']} m, final {s['final_m']} m, "
        f"reference {s['reference_m']} m")


# ======================================================================================
# fetch / presets / check-presets / preview
# ======================================================================================
def cmd_fetch(args: argparse.Namespace) -> int:
    peak = resolve_peak(args)
    extent_m = (args.extent_km or peak.default_extent_km) * 1000.0
    crs = dem.local_crs(peak.lat, peak.lon, "aeqd")
    h = extent_m / 2.0
    tiles, _ = needed_tiles(crs, -h, -h, h, h)
    log(f"[fetch] {peak.name}: {len(tiles)} tile(s) for {extent_m / 1000:.1f} km")
    results = dem.fetch_tiles(tiles, Path(args.cache_dir), refresh=args.refresh, log=log)
    for r in results:
        log(f"  {r.tile.name(1)}: {r.status}" + (f" ({r.path})" if r.path else ""))
    return 0


def cmd_presets(args: argparse.Namespace) -> int:
    log(f"{'id':14s} {'elev m':>8s} {'lat':>10s} {'lon':>11s} {'km':>4s}  name")
    for p in load_presets().values():
        log(f"{p.id:14s} {p.elevation_m:8.2f} {p.lat:10.5f} {p.lon:11.5f} {p.default_extent_km:4.0f}  {p.name}")
    return 0


def cmd_check_presets(args: argparse.Namespace) -> int:
    """Read a small window of each preset's tile (HTTP range reads) and compare with the DEM."""
    presets = load_presets()
    ids = args.ids or list(presets)
    radius = args.radius_m
    log(f"{'id':14s} {'ref m':>8s} {'DEM@pt':>8s} {'DEM max':>8s} {'max-ref':>8s} {'offset m':>8s} "
        f"{'bearing':>7s}  max location")
    worst = 0.0
    for pid in ids:
        p = presets[pid]
        m_lon, m_lat = dem.metres_per_degree(p.lat)
        dlon, dlat = (radius + 300) / m_lon, (radius + 300) / m_lat
        bounds = (p.lon - dlon, p.lat - dlat, p.lon + dlon, p.lat + dlat)
        tiles = dem.existing_tiles(dem.tiles_for_bounds(*bounds), Path(args.cache_dir))
        if not tiles:
            log(f"{pid:14s} no Copernicus tile")
            continue
        mosaic = dem.build_mosaic(dem.remote_tile_paths(tiles, Path(args.cache_dir)), bounds, 0.0)
        at = dem.sample_bilinear(mosaic.data, mosaic.transform, p.lon, p.lat)
        mx, r, c, dist = dem.local_max(mosaic.data, mosaic.transform, p.lon, p.lat, radius, (m_lon, m_lat))
        lon_m, lat_m = mosaic.transform * (c + 0.5, r + 0.5)
        bearing = math.degrees(math.atan2((lon_m - p.lon) * m_lon, (lat_m - p.lat) * m_lat)) % 360
        delta = mx - (p.elevation_m or 0.0)
        worst = max(worst, dist)
        log(f"{pid:14s} {p.elevation_m:8.1f} {at:8.1f} {mx:8.1f} {delta:+8.1f} {dist:8.0f} "
            f"{bearing:7.0f}  {lat_m:.5f}, {lon_m:.5f}")
    log(f"largest offset between preset coordinate and DEM local max: {worst:.0f} m")
    return 0


def cmd_preview(args: argparse.Namespace) -> int:
    meta_path = Path(args.metadata)
    meta = json.loads(meta_path.read_text())
    base = meta_path.parent
    imp = meta["ue_import"]
    if meta["grid"]["tiles_per_side"] == 1:
        h16 = export.read_heightmap(base / imp["tiles"][0]["png"])
    else:
        h16 = export.read_heightmap(base / f"{meta['name']}_full.png")
    h = export.dequantize_u16(h16, meta["elevation"]["min_m"], meta["elevation"]["max_m"])
    markers = [render.Marker(**m) for m in meta.get("markers", [])]
    g = meta["grid"]
    title = (f"{meta['peak']['name']}: {g['extent_m'] / 1000:.1f} km, {g['size_px']}px "
             f"@ {g['pixel_size_m']:.2f} m" + (", detail" if meta.get("detail") else ""))
    footer = ("Copernicus GLO-30 DEM (c) DLR e.V. 2010-2014, (c) Airbus DS 2014-2018, provided "
              "under COPERNICUS by the EU and ESA" if meta["source"]["kind"] == "copernicus" else "")
    info = render.PreviewInfo(title=title, pixel_m=g["pixel_size_m"], markers=markers, footer=footer)
    if args.layers:
        if not meta.get("layers") or g["tiles_per_side"] != 1:
            raise SystemExit("layer preview needs a single-tile build with layers")
        weights = {k: export.read_mask(base / v[0]) for k, v in meta["layers"]["files"].items()}
        img = render.render_layers(h, weights, info, args.width)
        default_out = base / f"{meta['name']}_layers_preview.png"
    else:
        img = render.render_hillshade(h, info, args.width, args.contour_interval)
        default_out = base / f"{meta['name']}_hillshade_preview.png"
    out = Path(args.out) if args.out else default_out
    out.parent.mkdir(parents=True, exist_ok=True)
    size = render.save_compact_png(img, out, max_bytes=int(args.max_kb * 1024))
    log(f"[preview] wrote {out} ({img.width}x{img.height}, {size / 1024:.0f} KB)")
    return 0


# ======================================================================================
# Argument parsing
# ======================================================================================
def _add_target_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--peak-id", help="preset id from peaks_presets.json (e.g. everest)")
    p.add_argument("--lat", type=float, help="peak latitude (WGS84, decimal degrees)")
    p.add_argument("--lon", type=float, help="peak longitude (WGS84, decimal degrees)")
    p.add_argument("--name", help="output name for ad-hoc peaks")
    p.add_argument("--elevation-m", type=float, help="true summit elevation (enables summit clamp)")
    p.add_argument("--extent-km", type=float, help="square side length (default: preset or 16)")
    p.add_argument("--cache-dir", default=str(CACHE_DIR))
    p.add_argument("--refresh", action="store_true", help="re-download tiles / retry missing")


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("presets", help="list peak presets")
    p.set_defaults(func=cmd_presets)

    p = sub.add_parser("check-presets", help="verify presets against the Copernicus DEM")
    p.add_argument("ids", nargs="*")
    p.add_argument("--radius-m", type=float, default=1000.0)
    p.add_argument("--cache-dir", default=str(CACHE_DIR))
    p.set_defaults(func=cmd_check_presets)

    p = sub.add_parser("fetch", help="download Copernicus GLO-30 tiles for an area")
    _add_target_args(p)
    p.set_defaults(func=cmd_fetch)

    p = sub.add_parser("build", help="build UE Landscape heightmaps + layers + metadata")
    _add_target_args(p)
    p.add_argument("--resolution-m", type=float, default=2.0, help="target m/px for --ue-size auto")
    p.add_argument("--ue-size", default="auto", choices=["auto", "1009", "2017", "4033", "8129"],
                   help="landscape vertices per side (per tile with --tiles)")
    p.add_argument("--tiles", type=int, default=1, help="split into NxN landscape tiles")
    p.add_argument("--projection", choices=["aeqd", "tmerc"], default="aeqd")
    p.add_argument("--source", action="append", help="local GeoTIFF(s) instead of Copernicus")
    p.add_argument("--fit-landmarks", action="store_true", help="widen extent to include landmarks")
    p.add_argument("--detail", action="store_true", help="synthesise sub-30 m detail + erosion")
    p.add_argument("--detail-strength", type=float, default=1.0)
    p.add_argument("--erosion-iters", type=int, default=200)
    p.add_argument("--seed", type=int, default=1953)
    p.add_argument("--no-summit-clamp", action="store_true",
                   help="do not correct the summit to the reference elevation")
    p.add_argument("--summit-radius-m", type=float, default=250.0)
    p.add_argument("--z-origin", choices=["mid", "min", "sealevel"], default="mid",
                   help="where world Z=0 lies (see ue.py); default mid = best precision")
    p.add_argument("--z-min", type=float, help="force min altitude of the 16-bit range")
    p.add_argument("--z-max", type=float, help="force max altitude of the 16-bit range")
    p.add_argument("--no-layers", action="store_true")
    p.add_argument("--snowline-m", type=float)
    p.add_argument("--grassline-m", type=float)
    p.add_argument("--snow-aspect-amp-m", type=float, default=300.0)
    p.add_argument("--preview-width", type=int, default=1600)
    p.add_argument("--output-dir", default=str(OUTPUT_DIR))
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("preview", help="render hillshade/contours or layer composite from a build")
    p.add_argument("--metadata", required=True, help="<name>.json written by build")
    p.add_argument("--layers", action="store_true", help="render the paint-layer composite")
    p.add_argument("--width", type=int, default=1600)
    p.add_argument("--contour-interval", type=float, default=500.0)
    p.add_argument("--max-kb", type=float, default=1400.0)
    p.add_argument("--out")
    p.set_defaults(func=cmd_preview)
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
