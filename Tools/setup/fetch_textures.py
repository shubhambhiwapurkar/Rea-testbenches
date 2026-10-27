"""Download free terrain textures for a Tools/terrain map.

1. Poly Haven (CC0) scanned surface sets for close-up detail: snow, cliff rock, moraine.
2. A satellite colour map from a cloud-free Copernicus Sentinel-2 L2A scene (10 m true colour, AWS open data
   via the Earth Search STAC API), reprojected onto the exact landscape grid of the terrain metadata, so real
   snow, glaciers and rock show where they really are. Scenes from several MGRS tiles of one date are mosaicked.

Usage (from the terrain venv, which has numpy/pyproj/pillow/rasterio):
    python Tools/setup/fetch_textures.py --terrain Tools/terrain/output/everest --out Tools/setup/textures
Attribution required for the satellite map: "Contains modified Copernicus Sentinel data <year>".
"""
from __future__ import annotations

import argparse
import io
import json
import os
import pathlib
import urllib.request
from collections import defaultdict

import numpy as np
import rasterio
from PIL import Image
from pyproj import Transformer
from rasterio.transform import from_origin
from rasterio.vrt import WarpedVRT
from rasterio.enums import Resampling

USER_AGENT = "SUMMIT-terrain-setup/1.0 (game dev; contact via repo)"
POLYHAVEN_SETS = {"snow": "snow_02", "rock": "cliff_side", "moraine": "aerial_rocks_02"}
POLYHAVEN_MAPS = {"diff": "Diffuse", "nor": "nor_dx", "rough": "Rough"}  # UE uses DirectX normals
STAC_SEARCH = "https://earth-search.aws.element84.com/v1/search"


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.read()


def fetch_polyhaven(out: pathlib.Path, resolution: str) -> None:
    for role, asset in POLYHAVEN_SETS.items():
        files = json.loads(fetch(f"https://api.polyhaven.com/files/{asset}"))
        for short, key in POLYHAVEN_MAPS.items():
            dest = out / f"T_{role}_{short}.png"
            if dest.exists():
                continue
            url = files[key][resolution]["jpg"]["url"]
            Image.open(io.BytesIO(fetch(url))).save(dest)
            print(f"  {dest.name} <- {asset} {key} {resolution}")


def scene_dates(bbox: list[float], months: tuple[str, ...]) -> list[tuple[str, list[dict]]]:
    """Clear dates in the given months, least fresh snow first; each with one STAC item per MGRS tile."""
    body = json.dumps({"collections": ["sentinel-2-l2a"], "bbox": bbox, "limit": 200,
                       "datetime": "2019-01-01T00:00:00Z/2030-01-01T00:00:00Z",
                       "query": {"eo:cloud_cover": {"lt": 5}}}).encode()
    req = urllib.request.Request(STAC_SEARCH, data=body, headers={"User-Agent": USER_AGENT, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=90) as resp:
        features = json.loads(resp.read())["features"]
    # One item per (date, MGRS tile); reprocessed duplicates of the same tile collapse to one.
    by_date: dict[str, dict[str, dict]] = defaultdict(dict)
    for f in features:
        props = f["properties"]
        by_date[props["datetime"][:10]][props.get("s2:mgrs_tile") or f["id"].split("_")[1]] = f
    # Whether a date's tiles actually cover the map is checked after reading (edge tiles may barely touch it).
    candidates = []
    for date, tiles in by_date.items():
        cloud = max(t["properties"]["eo:cloud_cover"] for t in tiles.values())
        snow = sum(t["properties"].get("s2:snow_ice_percentage", 50.0) for t in tiles.values()) / len(tiles)
        if date[5:7] in months and cloud < 1.0:
            candidates.append((snow, cloud, date))
    return [(date, list(by_date[date].values())) for _, _, date in sorted(candidates)]


def band(item: dict, name: str) -> tuple[str, float, float]:
    """(href, scale, offset) turning stored DN into surface reflectance (processing baseline 04.00+ adds an offset)."""
    asset = item["assets"][name]
    raster = (asset.get("raster:bands") or [{}])[0]
    return asset["href"], raster.get("scale", 0.0001), raster.get("offset", 0.0)


def fetch_satellite(meta: dict, out: pathlib.Path, size: int, months: tuple[str, ...]) -> None:
    dest = out / "T_satellite.png"
    if dest.exists():
        return
    bounds = meta["grid"]["bounds_m"]
    crs = meta["projection"]["proj4"]
    to_wgs84 = Transformer.from_crs(crs, "EPSG:4326", always_xy=True)
    lons, lats = to_wgs84.transform([bounds["xmin"], bounds["xmax"], bounds["xmin"], bounds["xmax"]],
                                    [bounds["ymin"], bounds["ymin"], bounds["ymax"], bounds["ymax"]])
    for date, items in scene_dates([min(lons), min(lats), max(lons), max(lats)], months)[:6]:
        print(f"  Sentinel-2 L2A {date}: {len(items)} tile(s)")
        if read_scene(meta, items, size, date, dest):
            return
    raise SystemExit("no clear Sentinel-2 date covers this map; try other --months")


def read_scene(meta: dict, items: list[dict], size: int, date: str, dest: pathlib.Path) -> bool:
    """Mosaic one date's tiles onto the landscape grid; writes dest and returns True if they cover the map."""
    bounds = meta["grid"]["bounds_m"]
    crs = meta["projection"]["proj4"]
    # Row 0 is north, matching the heightmap (vertex 0,0 = NW corner).
    pixel = (bounds["xmax"] - bounds["xmin"]) / size
    transform = from_origin(bounds["xmin"], bounds["ymax"], pixel, pixel)
    reflectance = np.zeros((3, size, size), dtype=np.float32)
    covered = np.zeros((size, size), dtype=bool)
    os.environ.setdefault("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR")
    for item in items:
        channels = []
        for name in ("red", "green", "blue"):
            href, scale, offset = band(item, name)
            with rasterio.open(href) as src, WarpedVRT(src, crs=crs, transform=transform, width=size, height=size,
                                                       resampling=Resampling.bilinear, nodata=0) as vrt:
                dn = vrt.read(1).astype(np.float32)
            channels.append(np.where(dn > 0, dn * scale + offset, np.nan))
        data = np.stack(channels)
        fill = ~covered & np.all(np.isfinite(data), axis=0)
        reflectance[:, fill] = data[:, fill]
        covered |= fill
    coverage = covered.mean()
    if coverage < 0.98:
        print(f"    covers only {coverage:.1%}; trying the next date")
        return False
    # Surface reflectance is the physical albedo, so store it sRGB-encoded as-is: snow stays detailed instead
    # of clipping like the 8-bit "true colour" product, and rock keeps its real darkness.
    srgb = np.where(reflectance <= 0.0031308, reflectance * 12.92, 1.055 * np.power(np.clip(reflectance, 0, 1), 1 / 2.4) - 0.055)
    Image.fromarray((np.clip(srgb, 0, 1) * 255 + 0.5).astype(np.uint8).transpose(1, 2, 0)).save(dest)
    dest.with_suffix(".txt").write_text(f"Contains modified Copernicus Sentinel data {date[:4]} (L2A {date})\n")
    print(f"  {dest.name}: {size}x{size}, {pixel:.1f} m/px, coverage {coverage:.1%}")
    return True


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--terrain", required=True, help="Tools/terrain output dir with the map's .json")
    ap.add_argument("--out", required=True)
    ap.add_argument("--detail-resolution", default="2k")
    ap.add_argument("--satellite-size", type=int, default=2048)
    ap.add_argument("--months", default="10,11,12,1", help="acquisition months to prefer; post-monsoon is clearest in the Himalaya")
    args = ap.parse_args()

    terrain = pathlib.Path(args.terrain)
    metas = list(terrain.glob("*.json"))
    if len(metas) != 1:
        raise SystemExit(f"expected one metadata .json in {terrain}, found {len(metas)}")
    meta = json.loads(metas[0].read_text())
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    print("Poly Haven detail textures (CC0)")
    fetch_polyhaven(out, args.detail_resolution)
    print("Copernicus Sentinel-2 satellite colour (free and open, attribution required)")
    fetch_satellite(meta, out, args.satellite_size, tuple(m.zfill(2) for m in args.months.split(",")))

    # Self-check: every expected file exists and the satellite map is the requested size.
    expected = [f"T_{r}_{m}.png" for r in POLYHAVEN_SETS for m in POLYHAVEN_MAPS] + ["T_satellite.png"]
    missing = [name for name in expected if not (out / name).exists()]
    assert not missing, f"missing textures: {missing}"
    assert Image.open(out / "T_satellite.png").size == (args.satellite_size, args.satellite_size)
    print("textures ready:", out)


if __name__ == "__main__":
    main()
