"""Fetches a real satellite colour map for the browser game's terrain.

Reuses the mosaicking in Tools/setup/fetch_textures.py (Sentinel-2 L2A surface reflectance warped
onto the terrain grid), but finds scenes by listing the public `sentinel-cogs` bucket directly
instead of using the Earth Search API, so it also works where only the data bucket is reachable.

Output: Web/assets/<peak>_satellite.jpg (sRGB surface reflectance, row 0 = north).
Attribution required: "Contains modified Copernicus Sentinel data <year>".

    python3 Tools/web/fetch_satellite.py --peak everest
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import urllib.request

import mgrs
from PIL import Image
from pyproj import Transformer

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools/setup"))
import fetch_textures  # noqa: E402  (shared mosaicking)

BUCKET = "https://sentinel-cogs.s3.us-west-2.amazonaws.com"
PREFIX = "sentinel-s2-l2a-cogs"


def get(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": fetch_textures.USER_AGENT})
    with urllib.request.urlopen(request, timeout=90) as response:
        return response.read()


def map_tiles(meta: dict) -> list[str]:
    """MGRS 100 km squares that the map touches, e.g. ['45RVL', '45RVM']."""
    bounds = meta["grid"]["bounds_m"]
    to_wgs84 = Transformer.from_crs(meta["projection"]["proj4"], "EPSG:4326", always_xy=True)
    converter = mgrs.MGRS()
    tiles = set()
    steps = 5
    for i in range(steps):
        for j in range(steps):
            x = bounds["xmin"] + (bounds["xmax"] - bounds["xmin"]) * i / (steps - 1)
            y = bounds["ymin"] + (bounds["ymax"] - bounds["ymin"]) * j / (steps - 1)
            lon, lat = to_wgs84.transform(x, y)
            tiles.add(converter.toMGRS(lat, lon, MGRSPrecision=0))
    return sorted(tiles)


def list_scenes(tile: str, year: int, month: int) -> dict[str, str]:
    """date -> scene id for one tile and month."""
    zone, band, square = tile[:-3], tile[-3], tile[-2:]
    prefix = f"{PREFIX}/{int(zone)}/{band}/{square}/{year}/{month}/"
    xml = get(f"{BUCKET}/?list-type=2&prefix={prefix}&delimiter=/").decode()
    scenes = {}
    for scene in re.findall(r"<Prefix>" + re.escape(prefix) + r"([^/<]+)/</Prefix>", xml):
        date = re.search(r"_(\d{8})_", scene)
        if date:
            d = date.group(1)
            scenes.setdefault(f"{d[:4]}-{d[4:6]}-{d[6:]}", scene)
    return scenes


def load_item(tile: str, date: str, scene: str) -> dict:
    zone, band, square = tile[:-3], tile[-3], tile[-2:]
    year, month = int(date[:4]), int(date[5:7])
    item = json.loads(get(f"{BUCKET}/{PREFIX}/{int(zone)}/{band}/{square}/{year}/{month}/{scene}/{scene}.json"))
    # Older items name the bands B04/B03/B02; the mosaicking code expects red/green/blue.
    for new, old in (("red", "B04"), ("green", "B03"), ("blue", "B02")):
        if new not in item["assets"] and old in item["assets"]:
            item["assets"][new] = item["assets"][old]
    return item


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--peak", default="everest")
    parser.add_argument("--size", type=int, default=2048)
    parser.add_argument("--months", default="11,12,10", help="months to search, clearest first")
    parser.add_argument("--years", default="2023,2022,2021,2020")
    parser.add_argument("--max-cloud", type=float, default=3.0)
    args = parser.parse_args()

    meta = json.loads((ROOT / "Tools/terrain/output" / args.peak / f"{args.peak}.json").read_text())
    tiles = map_tiles(meta)
    print("MGRS tiles:", ", ".join(tiles))

    candidates = []
    for year in (int(y) for y in args.years.split(",")):
        for month in (int(m) for m in args.months.split(",")):
            per_tile = [list_scenes(tile, year, month) for tile in tiles]
            for date in sorted(set.intersection(*(set(s) for s in per_tile))):
                items = [load_item(tile, date, scenes[date]) for tile, scenes in zip(tiles, per_tile)]
                cloud = max(i["properties"].get("eo:cloud_cover", 100.0) for i in items)
                snow = sum(i["properties"].get("s2:snow_ice_percentage", 50.0) for i in items) / len(items)
                print(f"  {date}: cloud {cloud:5.1f}%  snow {snow:5.1f}%")
                if cloud <= args.max_cloud:
                    candidates.append((snow, cloud, date, items))
        if len(candidates) >= 4:
            break
    if not candidates:
        raise SystemExit("no clear scene found; widen --months, --years or --max-cloud")

    out = ROOT / "Web/assets"
    scratch = out / f"{args.peak}_satellite.png"
    for snow, cloud, date, items in sorted(candidates, key=lambda c: (c[0], c[1])):
        print(f"using {date} (cloud {cloud:.1f}%, snow {snow:.1f}%)")
        if fetch_textures.read_scene(meta, items, args.size, date, scratch):
            Image.open(scratch).convert("RGB").save(out / f"{args.peak}_satellite.jpg", quality=90)
            scratch.unlink()   # read_scene also wrote <peak>_satellite.txt with the attribution line
            print("wrote", out / f"{args.peak}_satellite.jpg")
            return
    raise SystemExit("no candidate scene covered the whole map")


if __name__ == "__main__":
    main()
