"""Exports terrain built by Tools/terrain into the assets the browser game (Web/) loads.

Inputs (from `summit_terrain.py build`):
  Tools/terrain/output/<peak>/<peak>.r16 + .json + layers/   the playable 20 km area
  Tools/terrain/output/<peak>_far/<peak>/<peak>.r16 + .json  an 80 km low-resolution backdrop

Outputs (Web/assets/):
  <peak>_height.png      2049^2 heights, 16 bits packed into R (high byte) and G (low byte)
  <peak>_far.png         far backdrop heights, same packing
  <peak>_masks.png       1025^2 RGB = rock, snow, glacier weights (the remainder is scree or soil)
  <peak>_aux.png         1025^2 RGB = sky visibility (ambient occlusion), wind exposure, unused
  <peak>_horizon_N.png   horizon elevation angles for 12 azimuths, three per image, for sun shadows
  <peak>_minimap.jpg     shaded relief for the in-game map
  <peak>_world.json      metadata, camps snapped onto the terrain, the climbing route, landmarks

Run from the repository root:
  python3 Tools/web/export_web_assets.py --peak everest --route everest__south_col
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib

import numpy as np
from PIL import Image
from pyproj import Proj
from scipy import ndimage
from skimage.graph import MCP_Geometric

ROOT = pathlib.Path(__file__).resolve().parents[2]
NEAR_SIZE = 2049
COARSE_SIZE = 1025
HORIZON_DIRECTIONS = 12
HORIZON_MAX_ANGLE_DEG = 63.75  # stored as uint8 = angle * 4

# How far to search around each camp's database coordinate for ground that matches its altitude.
CAMP_SEARCH_RADIUS_M = {"ebc": 700.0, "c1": 900.0, "c2": 700.0, "c3": 600.0, "c4": 450.0}


class Dem:
    """A square heightmap in local metres: x east, z south, origin at the peak coordinate."""

    def __init__(self, heights: np.ndarray, extent_m: float):
        self.h = heights.astype(np.float32)
        self.size = heights.shape[0]
        self.extent = extent_m
        self.cell = extent_m / (self.size - 1)

    def index(self, x, z):
        half = self.extent / 2.0
        return (np.asarray(z) + half) / self.cell, (np.asarray(x) + half) / self.cell

    def sample(self, x, z):
        row, col = self.index(np.atleast_1d(x), np.atleast_1d(z))
        out = ndimage.map_coordinates(self.h, [row, col], order=1, mode="nearest")
        return out if np.ndim(x) else float(out[0])

    def inside(self, x, z, margin=0.0):
        half = self.extent / 2.0 - margin
        return (np.abs(x) <= half) & (np.abs(z) <= half)


def load_dem(directory: pathlib.Path, name: str) -> tuple[Dem, dict]:
    meta = json.loads((directory / f"{name}.json").read_text())
    size = meta["grid"]["size_px"]
    raw = np.fromfile(directory / f"{name}.r16", dtype="<u2").reshape(size, size).astype(np.float64)
    lo, hi = meta["elevation"]["min_m"], meta["elevation"]["max_m"]
    return Dem(lo + raw * (hi - lo) / 65535.0, meta["grid"]["extent_m"]), meta


def resample(dem: Dem, size: int, smooth_sigma: float) -> Dem:
    source = ndimage.gaussian_filter(dem.h, smooth_sigma) if smooth_sigma > 0 else dem.h
    return Dem(resample_grid(source, size, order=3), dem.extent)


def resample_grid(image: np.ndarray, size: int, order: int) -> np.ndarray:
    """Corner-aligned resampling: the first and last samples stay exactly on the map edges."""
    axis = np.linspace(0.0, image.shape[0] - 1.0, size)
    rows, cols = np.meshgrid(axis, axis, indexing="ij")
    return ndimage.map_coordinates(image, [rows, cols], order=order, mode="nearest")


def restore_summit(dem: Dem, elevation_m: float) -> tuple[float, float]:
    """Resampling shaves the summit; lift it back to the surveyed height with a small smooth bump."""
    row, col = np.unravel_index(int(dem.h.argmax()), dem.h.shape)
    deficit = elevation_m - float(dem.h[row, col])
    if deficit > 0:
        rows, cols = np.mgrid[0:dem.size, 0:dem.size]
        bump = np.exp(-((rows - row) ** 2 + (cols - col) ** 2) / (2.0 * 2.5**2))
        dem.h += (deficit * bump).astype(np.float32)
    half = dem.extent / 2.0
    return col * dem.cell - half, row * dem.cell - half


def pack16(dem: Dem, path: pathlib.Path) -> tuple[float, float]:
    lo, hi = float(dem.h.min()), float(dem.h.max())
    v = np.round((dem.h - lo) / (hi - lo) * 65535.0).astype(np.uint16)
    rgb = np.zeros((dem.size, dem.size, 3), np.uint8)
    rgb[..., 0] = v >> 8
    rgb[..., 1] = v & 255
    Image.fromarray(rgb, "RGB").save(path, optimize=True)
    return lo, hi


def slope_degrees(dem: Dem) -> np.ndarray:
    gz, gx = np.gradient(dem.h, dem.cell)
    return np.degrees(np.arctan(np.hypot(gx, gz)))


def export_masks(layer_dir: pathlib.Path, name: str, path: pathlib.Path) -> np.ndarray:
    channels = []
    for layer in ("rock", "snow", "glacier"):
        img = np.asarray(Image.open(layer_dir / f"{name}_layer_{layer}.png").convert("L"), np.float32)
        img = ndimage.gaussian_filter(img, 1.2)
        channels.append(np.clip(resample_grid(img, COARSE_SIZE, order=1), 0, 255))
    rgb = np.stack(channels, axis=-1).astype(np.uint8)
    Image.fromarray(rgb, "RGB").save(path, optimize=True)
    return rgb


def horizon_maps(near: Dem, far: Dem, out_prefix: pathlib.Path) -> np.ndarray:
    """Horizon elevation angle per pixel for each azimuth (clockwise from north).

    The game compares the sun's elevation with the horizon in the sun's direction to get soft terrain
    shadows for any time of day in one or two texture fetches (horizon mapping, Max 1988). Ridges
    outside the playable square still cast shadows because distant samples come from the far backdrop.
    """
    half = near.extent / 2.0
    axis = np.linspace(-half, half, near.size, dtype=np.float32)
    x, z = np.meshgrid(axis, axis)
    base = near.h
    # March outwards with growing steps: fine near the pixel, coarse towards 30 km.
    distances = [near.cell * 1.5 * (1.085**k) for k in range(96)]
    distances = [d for d in distances if d < 30000.0]
    result = np.zeros((HORIZON_DIRECTIONS, near.size, near.size), np.uint8)
    for d in range(HORIZON_DIRECTIONS):
        azimuth = math.radians(d * 360.0 / HORIZON_DIRECTIONS)
        dx, dz = math.sin(azimuth), -math.cos(azimuth)  # north is -z
        best = np.zeros_like(base)
        for t in distances:
            sx, sz = x + dx * t, z + dz * t
            inside = near.inside(sx, sz)
            height = np.where(inside, near.sample(sx, sz), far.sample(sx, sz))
            # Past the far backdrop there is no data: treat it as open sky.
            height = np.where(far.inside(sx, sz), height, -1.0e6)
            np.maximum(best, (height - base) / t, out=best)
        angle = np.degrees(np.arctan(best))
        result[d] = np.clip(np.round(angle * 4.0), 0, 255).astype(np.uint8)
        print(f"  horizon azimuth {d * 360 // HORIZON_DIRECTIONS:3d} deg: mean {angle.mean():5.1f} deg")
    for i in range(HORIZON_DIRECTIONS // 3):
        rgb = np.stack([result[i * 3], result[i * 3 + 1], result[i * 3 + 2]], axis=-1)
        Image.fromarray(rgb, "RGB").save(f"{out_prefix}_{i}.png", optimize=True)
    return result


def export_aux(coarse: Dem, horizons: np.ndarray, path: pathlib.Path) -> np.ndarray:
    """R: sky visibility (ambient occlusion). G: wind exposure for the sim and for spindrift."""
    angles = np.radians(horizons.astype(np.float32) / 4.0)
    # Fraction of the sky dome above the horizon, cosine weighted (Lambertian sky light).
    sky = np.mean(np.cos(angles) ** 2, axis=0)
    # Topographic position: ridges stand above their surroundings, gullies sit below them.
    tpi = coarse.h - ndimage.gaussian_filter(coarse.h, 300.0 / coarse.cell)
    ridge = np.clip(0.5 + tpi / 160.0, 0.0, 1.0)
    # Shelter from the prevailing westerlies: a high horizon to the west blocks the jet stream.
    west = angles[HORIZON_DIRECTIONS * 3 // 4]
    open_west = np.clip(1.0 - west / math.radians(35.0), 0.0, 1.0)
    exposure = np.clip(0.15 + 0.55 * ridge + 0.4 * open_west - 0.1, 0.05, 1.0)
    rgb = np.zeros((coarse.size, coarse.size, 3), np.uint8)
    rgb[..., 0] = np.clip(sky * 255.0, 0, 255)
    rgb[..., 1] = np.clip(exposure * 255.0, 0, 255)
    Image.fromarray(rgb, "RGB").save(path, optimize=True)
    return exposure


def export_minimap(coarse: Dem, masks: np.ndarray, path: pathlib.Path) -> None:
    gz, gx = np.gradient(coarse.h, coarse.cell)
    normal = np.stack([-gx, np.ones_like(gx), -gz], axis=-1)
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
    light = np.array([-0.55, 0.62, -0.56])  # from the north-west, the cartographic convention
    shade = np.clip(normal @ (light / np.linalg.norm(light)), 0.0, 1.0)
    rock, snow, glacier = (masks[..., i].astype(np.float32) / 255.0 for i in range(3))
    soil = np.clip(1.0 - rock - snow - glacier, 0.0, 1.0)
    colour = (
        rock[..., None] * np.array([118, 110, 104])
        + snow[..., None] * np.array([244, 247, 252])
        + glacier[..., None] * np.array([176, 208, 228])
        + soil[..., None] * np.array([139, 120, 98])
    )
    image = np.clip(colour * (0.35 + 0.85 * shade[..., None]), 0, 255).astype(np.uint8)
    Image.fromarray(image, "RGB").save(path, quality=86)


def snap_camp(near: Dem, slope: np.ndarray, x: float, z: float, altitude: float, radius: float) -> tuple[float, float]:
    """Moves a camp onto nearby ground whose altitude matches the database and that is flat enough for tents."""
    half = near.extent / 2.0
    axis = np.linspace(-half, half, near.size)
    xs, zs = np.meshgrid(axis, axis)
    distance = np.hypot(xs - x, zs - z)
    score = np.abs(near.h - altitude) / 40.0 + slope / 12.0 + distance / radius
    score[distance > radius] = np.inf
    row, col = np.unravel_index(int(np.argmin(score)), score.shape)
    return float(axis[col]), float(axis[row])


def trace_route(near: Dem, slope: np.ndarray, stops: list[tuple[float, float]]) -> list[np.ndarray]:
    """Least-cost path between consecutive stops. Steep ground costs far more than gentle ground, so
    the line follows valley floors, glaciers and the easiest angle of faces, as real routes do."""
    cost = 1.0 + (slope / 24.0) ** 3
    cost[slope > 62.0] *= 8.0
    mcp_legs = []
    for (x0, z0), (x1, z1) in zip(stops[:-1], stops[1:]):
        r0, c0 = (int(round(float(v))) for v in near.index(x0, z0))
        r1, c1 = (int(round(float(v))) for v in near.index(x1, z1))
        mcp = MCP_Geometric(cost, fully_connected=True)
        mcp.find_costs([(r0, c0)], [(r1, c1)])
        path = np.array(mcp.traceback((r1, c1)), np.float64)
        half = near.extent / 2.0
        pts = np.stack([path[:, 1] * near.cell - half, path[:, 0] * near.cell - half], axis=-1)
        mcp_legs.append(smooth_polyline(pts))
    return mcp_legs


def smooth_polyline(points: np.ndarray, spacing_m: float = 20.0) -> np.ndarray:
    """Removes the 8-direction staircase of a grid path and resamples it at an even spacing."""
    pts = points.copy()
    for _ in range(3):
        inner = (pts[:-2] + 2.0 * pts[1:-1] + pts[2:]) / 4.0
        pts = np.vstack([pts[:1], inner, pts[-1:]])
    seg = np.hypot(*np.diff(pts, axis=0).T)
    along = np.concatenate([[0.0], np.cumsum(seg)])
    n = max(2, int(math.ceil(along[-1] / spacing_m)) + 1)
    target = np.linspace(0.0, along[-1], n)
    return np.stack([np.interp(target, along, pts[:, 0]), np.interp(target, along, pts[:, 1])], axis=-1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--peak", default="everest")
    parser.add_argument("--route", default="everest__south_col")
    parser.add_argument("--first-camp", default="ebc", help="first camp of the route that lies inside the map")
    args = parser.parse_args()

    out = ROOT / "Web/assets"
    out.mkdir(parents=True, exist_ok=True)
    terrain_out = ROOT / "Tools/terrain/output"
    source, meta = load_dem(terrain_out / args.peak, args.peak)
    far, far_meta = load_dem(terrain_out / f"{args.peak}_far" / args.peak, args.peak)
    summit_elevation = meta["peak"]["reference_elevation_m"]

    print("resampling heights")
    near = resample(source, NEAR_SIZE, smooth_sigma=0.7)
    summit_x, summit_z = restore_summit(near, summit_elevation)
    near_lo, near_hi = pack16(near, out / f"{args.peak}_height.png")
    far_lo, far_hi = pack16(far, out / f"{args.peak}_far.png")
    coarse = resample(near, COARSE_SIZE, smooth_sigma=0.6)

    print("masks, horizons, exposure, minimap")
    masks = export_masks(terrain_out / args.peak / "layers", args.peak, out / f"{args.peak}_masks.png")
    horizons = horizon_maps(coarse, far, out / f"{args.peak}_horizon")
    export_aux(coarse, horizons, out / f"{args.peak}_aux.png")
    export_minimap(coarse, masks, out / f"{args.peak}_minimap.jpg")

    print("camps and route")
    projection = Proj(meta["projection"]["proj4"])
    route = json.loads((ROOT / "Data/Mountains/routes" / f"{args.route}.json").read_text())
    slope = slope_degrees(coarse)
    ids = [c["id"] for c in route["camps"]]
    in_map = route["camps"][ids.index(args.first_camp):]
    camps, stops = [], []
    for camp in in_map:
        east, north = projection(camp["lon"], camp["lat"])
        x, z = float(east), float(-north)
        if camp["id"] == "summit":
            x, z = summit_x, summit_z
        elif camp["id"] in CAMP_SEARCH_RADIUS_M:
            x, z = snap_camp(coarse, slope, x, z, camp["altitude_m"], CAMP_SEARCH_RADIUS_M[camp["id"]])
        is_camp = camp["id"] in CAMP_SEARCH_RADIUS_M or camp["id"] == "summit"
        camps.append({"id": camp["id"], "name": camp["name"], "altitude_m": camp["altitude_m"],
                      "tents": camp.get("tent_capacity", 0), "kind": "camp" if is_camp else "waypoint",
                      "x": x, "z": z})
        if is_camp:
            stops.append((x, z))

    legs = trace_route(coarse, slope, stops)
    points, camp_index, leg_start = [], {}, []
    stop_ids = [c["id"] for c in camps if c["kind"] == "camp"]
    for i, leg in enumerate(legs):
        camp_index[stop_ids[i]] = len(points)
        leg_start.append(len(points))
        points.extend(leg[:-1].tolist())
    camp_index[stop_ids[-1]] = len(points)
    points.append(legs[-1][-1].tolist())
    pts = np.array(points)
    along = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(pts, axis=0).T))])
    heights = near.sample(pts[:, 0], pts[:, 1])

    # Waypoints between the top camp and the summit sit on the traced line, spaced by the database's
    # segment lengths, because the 30 m source data blurs the real ridge they are named after.
    segments = {(s["from"], s["to"]): s for s in route["segments"]}
    top_camp = stop_ids[-2]
    chain = [c["id"] for c in camps][[c["id"] for c in camps].index(top_camp):]
    lengths = [segments[(a, b)]["distance_m"] for a, b in zip(chain[:-1], chain[1:])]
    i0, i1 = camp_index[top_camp], camp_index["summit"]
    cumulative = np.cumsum(lengths) / sum(lengths)
    for camp_id, fraction in zip(chain[1:-1], cumulative[:-1]):
        target = along[i0] + fraction * (along[i1] - along[i0])
        index = int(np.argmin(np.abs(along - target)))
        camp_index[camp_id] = index
        for camp in camps:
            if camp["id"] == camp_id:
                camp["x"], camp["z"] = float(pts[index, 0]), float(pts[index, 1])

    for camp in camps:
        camp["terrain_m"] = round(float(near.sample(camp["x"], camp["z"])), 1)
        camp["route_index"] = camp_index[camp["id"]]
        print(f"  {camp['id']:13s} database {camp['altitude_m']:7.1f} m   terrain {camp['terrain_m']:7.1f} m"
              f"   x {camp['x']:7.0f}  z {camp['z']:7.0f}")

    # Per-leg metadata from the database (merging the waypoint segments above the top camp).
    legs_meta = []
    for i, (a, b) in enumerate(zip(stop_ids[:-1], stop_ids[1:])):
        chain_ids = [c["id"] for c in camps]
        hops = chain_ids[chain_ids.index(a):chain_ids.index(b) + 1]
        parts = [segments[(p, q)] for p, q in zip(hops[:-1], hops[1:])]
        legs_meta.append({
            "from": a, "to": b, "i0": camp_index[a], "i1": camp_index[b],
            "terrain": parts[0]["terrain"], "fixed_ropes": all(p["fixed_ropes"] for p in parts),
            "hours_up": sum(p["hours_up"] for p in parts), "hours_down": sum(p["hours_down"] for p in parts),
            "hazards": sorted({h for p in parts for h in p["hazards"]}),
            "length_m": round(float(along[camp_index[b]] - along[camp_index[a]])),
            "ascent_m": round(float(heights[camp_index[b]] - heights[camp_index[a]])),
        })
        print(f"  leg {a:>4s} -> {b:<6s} {legs_meta[-1]['length_m']:5d} m, +{legs_meta[-1]['ascent_m']} m")

    # Fixed lines are rigged where the ground is steep, and through the icefall.
    grade = np.degrees(np.arctan(np.abs(np.gradient(heights, along))))
    grade = ndimage.uniform_filter1d(grade, 5)
    fixed = np.zeros(len(pts), np.uint8)
    for leg in legs_meta:
        if not leg["fixed_ropes"]:
            continue
        section = slice(leg["i0"], leg["i1"] + 1)
        steep = grade[section] > 22.0
        fixed[section] = (steep | ("icefall" in leg["hazards"])).astype(np.uint8)

    peaks = json.loads((ROOT / "Data/Mountains/peaks.json").read_text())["peaks"]
    landmarks = []
    for peak in peaks:
        east, north = projection(peak["lon"], peak["lat"])
        if abs(east) < near.extent / 2 - 300 and abs(north) < near.extent / 2 - 300 and peak["id"] != args.peak:
            landmarks.append({"name": peak["name"], "x": float(east), "z": float(-north),
                              "altitude_m": peak["elevation_m"], "kind": "peak"})
    print("  landmarks:", ", ".join(l["name"] for l in landmarks))

    world = {
        "name": meta["peak"]["name"],
        "peak_id": args.peak,
        "route_id": route["id"],
        "route_name": route["name"],
        "latitude": meta["projection"]["center_lat"],
        "longitude": meta["projection"]["center_lon"],
        "attribution": meta["source"]["attribution"],
        "axes": "x east (m), z south (m), y altitude (m above sea level); origin at the peak coordinate",
        "near": {"file": f"{args.peak}_height.png", "size": NEAR_SIZE, "extent_m": near.extent,
                 "min_m": near_lo, "max_m": near_hi},
        "far": {"file": f"{args.peak}_far.png", "size": far.size, "extent_m": far.extent,
                "min_m": far_lo, "max_m": far_hi},
        "masks": {"file": f"{args.peak}_masks.png", "size": COARSE_SIZE, "channels": ["rock", "snow", "glacier"]},
        "aux": {"file": f"{args.peak}_aux.png", "size": COARSE_SIZE, "channels": ["sky_visibility", "wind_exposure"]},
        "horizon": {"files": [f"{args.peak}_horizon_{i}.png" for i in range(HORIZON_DIRECTIONS // 3)],
                    "size": COARSE_SIZE, "directions": HORIZON_DIRECTIONS, "degrees_per_step": 0.25},
        "minimap": f"{args.peak}_minimap.jpg",
        "summit": {"x": summit_x, "z": summit_z, "altitude_m": summit_elevation},
        "camps": camps,
        "route": {
            "x": [round(float(v), 1) for v in pts[:, 0]],
            "z": [round(float(v), 1) for v in pts[:, 1]],
            "fixed": fixed.tolist(),
            "legs": legs_meta,
            "length_m": round(float(along[-1])),
        },
        "landmarks": landmarks,
    }
    (out / f"{args.peak}_world.json").write_text(json.dumps(world, separators=(",", ":")))
    print(f"route length {along[-1] / 1000:.1f} km; wrote assets to {out}")
    for f in sorted(out.glob(f"{args.peak}_*")):
        print(f"  {f.name:28s} {f.stat().st_size / 1e6:6.2f} MB")


if __name__ == "__main__":
    main()
