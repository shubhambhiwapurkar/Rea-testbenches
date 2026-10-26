#!/usr/bin/env python3
"""Expand the SUMMIT mountain database toward "every mountain in the world".

Sources
  * OpenStreetMap via the Overpass API: nodes tagged natural=peak (and optionally natural=volcano)
    that carry both `name` and `ele`, selected by bounding box, by country (ISO 3166-1 alpha-2) or by
    tiling the whole world. Elevation is filtered on the server when possible and always again here.
  * GeoNames dumps (https://download.geonames.org/export/dump/): allCountries.zip or a per-country file
    such as NP.zip. Rows with feature class T and code MT (mountain), PK (peak) or VLC (volcano).
    Note: cities500.zip contains populated places only, not mountains.

Output
  Data/Mountains/peaks_osm.json (schema: Data/Mountains/schema/peaks_osm.schema.json). The curated
  Data/Mountains/peaks.json is never modified. A candidate is dropped when it lies within
  --curated-radius-km of a curated peak (or within 5 km with the same normalised name), so curated
  entries always win. Existing entries in peaks_osm.json are kept and updated by source id.

Licences (read before shipping anything built from the output)
  * OpenStreetMap data is (c) OpenStreetMap contributors and licensed under the Open Database License
    (ODbL 1.0). peaks_osm.json is a Derivative Database: it must stay under ODbL, and anything that
    shows it publicly (including in-game maps or credits) must attribute "(c) OpenStreetMap
    contributors" with a link to https://www.openstreetmap.org/copyright. Keep it a separate file from
    the curated peaks.json so the curated data is not pulled under share-alike.
  * GeoNames data is licensed under Creative Commons Attribution 4.0 (credit "GeoNames,
    https://www.geonames.org").
  * Respect the Overpass usage policy: one request at a time, small areas, and a pause between requests
    (--sleep). Large or world-wide pulls should use a planet/regional extract (e.g. Geofabrik + osmium)
    instead:  osmium tags-filter region.osm.pbf n/natural=peak -o peaks.osm.pbf
              osmium export peaks.osm.pbf -f geojson --attributes id -o peaks.geojson
              python3 Tools/data/fetch_osm_peaks.py --overpass-json peaks.geojson --min-ele 3000

Examples
  # Show the Overpass query that would be sent, without any network access
  python3 Tools/data/fetch_osm_peaks.py --country NP --min-ele 6000 --dry-run

  # All named peaks >= 4000 m in the Alps bounding box
  python3 Tools/data/fetch_osm_peaks.py --bbox 43.5 5.0 48.5 16.5 --min-ele 4000

  # Whole world, peaks >= 5000 m, in 10° tiles, 20 s between requests
  python3 Tools/data/fetch_osm_peaks.py --world --tile-deg 10 --min-ele 5000 --sleep 20

  # Offline: merge a saved Overpass JSON response and a GeoNames dump
  python3 Tools/data/fetch_osm_peaks.py --overpass-json export.json --geonames allCountries.zip --min-ele 3000
"""
from __future__ import annotations

import argparse
import datetime as dt
import io
import json
import math
import os
import re
import sys
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import zipfile

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DATA = os.path.join(REPO, "Data", "Mountains")
DEFAULT_OUT = os.path.join(DATA, "peaks_osm.json")
CURATED = os.path.join(DATA, "peaks.json")
DEFAULT_ENDPOINT = "https://overpass-api.de/api/interpreter"
USER_AGENT = "SUMMIT-mountain-db/1.0 (Tools/data/fetch_osm_peaks.py; offline game data build)"
GEONAMES_CODES = {"MT", "PK", "VLC"}

OSM_LICENSE = {
    "name": "Open Database License (ODbL) 1.0",
    "url": "https://opendatacommons.org/licenses/odbl/1-0/",
    "attribution": "© OpenStreetMap contributors (https://www.openstreetmap.org/copyright)",
}
GEONAMES_LICENSE = {
    "name": "Creative Commons Attribution 4.0",
    "url": "https://creativecommons.org/licenses/by/4.0/",
    "attribution": "GeoNames (https://www.geonames.org)",
}


# --------------------------------------------------------------------------------------------
# Parsing helpers
# --------------------------------------------------------------------------------------------
_NUM = re.compile(r"[-+]?\d+(?:[.,]\d+)?")


def parse_length_m(raw) -> float | None:
    """Parse OSM-style `ele`/`prominence` values: "8848", "8848.86", "8 848 m", "29,029 ft", "4478m"."""
    if raw is None:
        return None
    if isinstance(raw, (int, float)):
        return float(raw)
    s = str(raw).strip().lower().replace(" ", " ")
    s = s.split(";")[0]  # multiple values: take the first
    feet = bool(re.search(r"(ft|feet|')\s*$", s))
    s = re.sub(r"(?<=\d)[ ](?=\d{3}\b)", "", s)  # "8 848" -> "8848"
    if re.fullmatch(r"\s*\d{1,2},\d{3}(\.\d+)?\s*(m|ft|feet|')?\s*", s):
        s = s.replace(",", "")  # thousands separator
    m = _NUM.search(s)
    if not m:
        return None
    try:
        v = float(m.group(0).replace(",", "."))
    except ValueError:
        return None
    if feet:
        v *= 0.3048
    if not -500 <= v <= 8900:
        return None
    return round(v, 2)


def norm_name(name: str) -> str:
    s = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode().lower()
    s = re.sub(r"\b(mount|mt|monte|mont|pic|pico|piz|cerro|nevado|peak|pik|pizzo|punta|la|le|el|de|del|di|the)\b", " ", s)
    return re.sub(r"[^a-z0-9]+", "", s)


def slug(s: str) -> str:
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode().lower()
    return re.sub(r"[^a-z0-9]+", "_", s).strip("_") or "x"


def haversine_km(lat1, lon1, lat2, lon2) -> float:
    r = 6371.0088
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(min(1.0, a)))


class GridIndex:
    """Tiny spatial hash so proximity checks stay fast for hundreds of thousands of points."""

    def __init__(self, cell_deg: float = 0.1):
        self.cell = cell_deg
        self.cells: dict[tuple[int, int], list[dict]] = {}

    def _key(self, lat, lon):
        return int(math.floor(lat / self.cell)), int(math.floor(lon / self.cell))

    def add(self, p: dict) -> None:
        self.cells.setdefault(self._key(p["lat"], p["lon"]), []).append(p)

    def near(self, lat, lon, radius_km):
        ky, kx = self._key(lat, lon)
        span = int(math.ceil(radius_km / (111.0 * self.cell))) + 1
        for dy in range(-span, span + 1):
            for dx in range(-span, span + 1):
                for p in self.cells.get((ky + dy, kx + dx), []):
                    d = haversine_km(lat, lon, p["lat"], p["lon"])
                    if d <= radius_km:
                        yield p, d


# --------------------------------------------------------------------------------------------
# Overpass
# --------------------------------------------------------------------------------------------
def build_query(args, bbox=None) -> str:
    kinds = ["peak"] + (["volcano"] if args.volcanoes else [])
    if args.server_filter and args.min_ele > 0:
        cond = f'(if: is_number(t["ele"]) && number(t["ele"]) >= {args.min_ele:g})'
    else:
        cond = ""
    if bbox is not None:
        s, w, n, e = bbox
        scope, prelude = f"({s:g},{w:g},{n:g},{e:g})", ""
    elif args.country:
        prelude = f'area["ISO3166-1"="{args.country.upper()}"][admin_level=2]->.searchArea;\n'
        scope = "(area.searchArea)"
    else:
        raise ValueError("need --bbox, --country or --world")
    body = "\n".join(f'  node["natural"="{k}"]["name"]["ele"]{scope}{cond};' for k in kinds)
    return f"[out:json][timeout:{args.timeout}];\n{prelude}(\n{body}\n);\nout body qt;\n"


def world_tiles(tile_deg: float, south=-85.0, north=84.0):
    lat = south
    while lat < north:
        lon = -180.0
        while lon < 180.0:
            yield (lat, lon, min(lat + tile_deg, north), min(lon + tile_deg, 180.0))
            lon += tile_deg
        lat += tile_deg


def overpass_request(endpoint: str, query: str, timeout: int, retries: int = 4) -> dict:
    data = urllib.parse.urlencode({"data": query}).encode()
    delay = 30.0
    for attempt in range(retries + 1):
        req = urllib.request.Request(endpoint, data=data, headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=timeout + 60) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            if exc.code in (429, 502, 503, 504) and attempt < retries:
                print(f"  Overpass HTTP {exc.code}; retrying in {delay:.0f} s", file=sys.stderr)
                time.sleep(delay)
                delay *= 2
                continue
            raise
        except urllib.error.URLError as exc:
            if attempt < retries:
                print(f"  network error {exc.reason}; retrying in {delay:.0f} s", file=sys.stderr)
                time.sleep(delay)
                delay *= 2
                continue
            raise
    raise RuntimeError("unreachable")


def geojson_to_elements(doc: dict) -> list[dict]:
    """Convert `osmium export -f geojson --attributes id` output (Point features) to Overpass-like elements."""
    els = []
    for f in doc.get("features", []):
        g = f.get("geometry") or {}
        if g.get("type") != "Point":
            continue
        props = dict(f.get("properties") or {})
        raw = str(props.pop("@id", f.get("id", "")))
        m = re.search(r"(\d+)$", raw)
        if not m or raw.startswith(("w", "r", "way", "relation")):
            continue
        props.pop("@type", None)
        els.append({"type": "node", "id": int(m.group(1)), "lon": g["coordinates"][0], "lat": g["coordinates"][1],
                    "tags": {k: v for k, v in props.items() if not k.startswith("@")}})
    return els


def peaks_from_overpass(doc: dict, country: str | None) -> list[dict]:
    out = []
    if "features" in doc and "elements" not in doc:
        doc = {"elements": geojson_to_elements(doc)}
    for el in doc.get("elements", []):
        if el.get("type") != "node" or "lat" not in el:
            continue
        tags = el.get("tags", {})
        name = tags.get("name:en") or tags.get("name")
        ele = parse_length_m(tags.get("ele"))
        if not name or ele is None:
            continue
        p = {
            "id": f"osm_n{el['id']}",
            "name": name,
            "alt_names": sorted({v for k, v in tags.items()
                                 if (k == "name" or k.startswith(("name:", "alt_name", "official_name", "int_name")))
                                 and v != name})[:12],
            "elevation_m": ele,
            "prominence_m": parse_length_m(tags.get("prominence")),
            "lat": round(el["lat"], 5),
            "lon": round(el["lon"], 5),
            "country": [country.upper()] if country else [],
            "tags": ["volcano"] if tags.get("natural") == "volcano" else [],
            "source": {"dataset": "osm", "id": f"node/{el['id']}",
                       "url": f"https://www.openstreetmap.org/node/{el['id']}"},
            "confidence": "low",
        }
        if tags.get("wikidata"):
            p["wikidata"] = tags["wikidata"]
        out.append(p)
    return out


# --------------------------------------------------------------------------------------------
# GeoNames
# --------------------------------------------------------------------------------------------
def iter_geonames_lines(path: str):
    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as zf:
            names = [n for n in zf.namelist() if n.endswith(".txt") and not n.lower().startswith("readme")]
            for n in names:
                with zf.open(n) as fh:
                    yield from io.TextIOWrapper(fh, encoding="utf-8")
    else:
        with open(path, encoding="utf-8") as fh:
            yield from fh


def peaks_from_geonames(path: str, min_ele: float, bbox=None, country: str | None = None) -> list[dict]:
    """GeoNames 'geoname' table: geonameid, name, asciiname, alternatenames, latitude, longitude, feature class,
    feature code, country code, cc2, admin1..4, population, elevation, dem, timezone, modification date."""
    out = []
    for line in iter_geonames_lines(path):
        f = line.rstrip("\n").split("\t")
        if len(f) < 17 or f[6] != "T" or f[7] not in GEONAMES_CODES:
            continue
        if country and f[8].upper() != country.upper():
            continue
        try:
            lat, lon = float(f[4]), float(f[5])
        except ValueError:
            continue
        if bbox and not (bbox[0] <= lat <= bbox[2] and bbox[1] <= lon <= bbox[3]):
            continue
        ele = parse_length_m(f[15]) if f[15] else None
        if ele is None and f[16] and f[16] not in ("-9999", ""):
            ele = parse_length_m(f[16])  # SRTM-derived 'dem' (lower quality)
            dem_only = True
        else:
            dem_only = False
        if ele is None or ele < min_ele:
            continue
        alts = [a for a in f[3].split(",") if a and a != f[1] and not a.startswith("http")][:12]
        p = {
            "id": f"gn_{f[0]}",
            "name": f[1],
            "alt_names": alts,
            "elevation_m": ele,
            "prominence_m": None,
            "lat": round(lat, 5),
            "lon": round(lon, 5),
            "country": [c for c in [f[8].upper()] + [c.strip().upper() for c in f[9].split(",")] if len(c) == 2],
            "tags": ["volcano"] if f[7] == "VLC" else [],
            "source": {"dataset": "geonames", "id": f[0], "url": f"https://www.geonames.org/{f[0]}"},
            "confidence": "low",
        }
        if dem_only:
            p["elevation_source"] = "geonames_dem"
        p["country"] = sorted(set(p["country"]))
        out.append(p)
    return out


# --------------------------------------------------------------------------------------------
# Merge
# --------------------------------------------------------------------------------------------
def load_curated(path: str) -> list[dict]:
    with open(path, encoding="utf-8") as f:
        return json.load(f)["peaks"]


def merge(existing: list[dict], candidates: list[dict], curated: list[dict], args, stats: dict) -> list[dict]:
    cur_idx = GridIndex()
    for c in curated:
        cur_idx.add({**c, "_norm": norm_name(c["name"])})
    curated_ids = {c["id"] for c in curated}

    by_source: dict[str, dict] = {}
    for p in existing:
        by_source[f"{p['source']['dataset']}:{p['source']['id']}"] = p
    idx = GridIndex()
    for p in by_source.values():
        idx.add(p)

    for c in candidates:
        if c["elevation_m"] < args.min_ele:
            stats["below_min_ele"] += 1
            continue
        if args.min_prominence > 0:
            if c.get("prominence_m") is None:
                if args.require_prominence:
                    stats["no_prominence"] += 1
                    continue
            elif c["prominence_m"] < args.min_prominence:
                stats["below_min_prominence"] += 1
                continue
        # Curated entries always win.
        nm = norm_name(c["name"])
        shadow = None
        for q, d in cur_idx.near(c["lat"], c["lon"], max(args.curated_radius_km, 5.0)):
            if d <= args.curated_radius_km or (nm and nm == q["_norm"]):
                shadow = q
                break
        if shadow is not None:
            stats["shadowed_by_curated"] += 1
            continue
        key = f"{c['source']['dataset']}:{c['source']['id']}"
        if key in by_source:
            old = by_source[key]
            for k in ("name", "alt_names", "elevation_m", "prominence_m", "lat", "lon", "tags", "wikidata"):
                if k in c and c[k] not in (None, [], ""):
                    old[k] = c[k]
            if c.get("country"):
                old["country"] = sorted(set(old.get("country", [])) | set(c["country"]))
            stats["updated"] += 1
            continue
        # Cross-dataset duplicate (OSM vs GeoNames): same summit within 0.5 km and 60 m of elevation.
        dup = None
        for q, d in idx.near(c["lat"], c["lon"], 0.5):
            if abs(q["elevation_m"] - c["elevation_m"]) <= 60:
                dup = q
                break
        if dup is not None:
            also = dup.setdefault("also_in", [])
            ref = {"dataset": c["source"]["dataset"], "id": c["source"]["id"]}
            if ref not in also:
                also.append(ref)
            if c.get("country") and not dup.get("country"):
                dup["country"] = c["country"]
            stats["merged_duplicates"] += 1
            continue
        if c["id"] in curated_ids:  # paranoia: never reuse a curated id
            c["id"] = f"{c['id']}_{slug(c['name'])}"
        by_source[key] = c
        idx.add(c)
        stats["added"] += 1
    peaks = list(by_source.values())
    peaks.sort(key=lambda p: (-p["elevation_m"], p["name"]))
    return peaks


def write_output(path: str, peaks: list[dict], queries: list[str], used: set[str]) -> None:
    licenses = []
    if any(p["source"]["dataset"] == "osm" for p in peaks) or "osm" in used:
        licenses.append({"dataset": "osm", **OSM_LICENSE})
    if any(p["source"]["dataset"] == "geonames" for p in peaks) or "geonames" in used:
        licenses.append({"dataset": "geonames", **GEONAMES_LICENSE})
    doc = {
        "$schema": "./schema/peaks_osm.schema.json",
        "version": 1,
        "generated_at": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat(),
        "generator": "Tools/data/fetch_osm_peaks.py",
        "licenses": licenses,
        "notes": "Machine-imported, unreviewed. Curated peaks.json takes precedence. Promote a peak by adding a "
                 "reviewed entry to peaks.json; it will then be dropped from this file on the next run.",
        "queries": queries[-50:],
        "peaks": peaks,
    }
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
        f.write("\n")
    os.replace(tmp, path)


# --------------------------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------------------------
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Import peaks from OpenStreetMap (Overpass) and/or GeoNames into peaks_osm.json",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__.split("Examples")[1])
    where = ap.add_mutually_exclusive_group()
    where.add_argument("--bbox", nargs=4, type=float, metavar=("SOUTH", "WEST", "NORTH", "EAST"))
    where.add_argument("--country", help="ISO 3166-1 alpha-2 code, e.g. NP, CH, PK")
    where.add_argument("--world", action="store_true", help="tile the whole world (use a high --min-ele)")
    ap.add_argument("--tile-deg", type=float, default=10.0, help="tile size for --world (degrees)")
    ap.add_argument("--min-ele", type=float, default=3000.0, help="minimum elevation in metres (default 3000)")
    ap.add_argument("--min-prominence", type=float, default=0.0,
                    help="minimum prominence in metres; peaks without a prominence value are kept unless --require-prominence")
    ap.add_argument("--require-prominence", action="store_true")
    ap.add_argument("--volcanoes", action="store_true", help="also import natural=volcano nodes")
    ap.add_argument("--no-server-filter", dest="server_filter", action="store_false",
                    help="do not filter ele on the Overpass server (keeps values like '4478 m')")
    ap.add_argument("--geonames", metavar="FILE", help="GeoNames dump: allCountries.zip/.txt or a country file (e.g. NP.zip)")
    ap.add_argument("--overpass-json", metavar="FILE", action="append", default=[],
                    help="use a saved Overpass JSON response, or GeoJSON from `osmium export --attributes id`, "
                         "instead of querying (repeatable)")
    ap.add_argument("--endpoint", default=DEFAULT_ENDPOINT)
    ap.add_argument("--timeout", type=int, default=180, help="Overpass [timeout:] in seconds")
    ap.add_argument("--sleep", type=float, default=10.0, help="pause between Overpass requests (seconds)")
    ap.add_argument("--curated-radius-km", type=float, default=1.5,
                    help="drop candidates this close to a curated peak (default 1.5 km)")
    ap.add_argument("--curated", default=CURATED)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--dry-run", action="store_true", help="print the Overpass query/queries and exit without network or writes")
    args = ap.parse_args(argv)

    if not (args.bbox or args.country or args.world or args.geonames or args.overpass_json):
        ap.error("give --bbox, --country, --world, --geonames or --overpass-json")

    bbox = None
    if args.bbox:
        s, w, n, e = args.bbox
        if not (-90 <= s < n <= 90 and -180 <= w < e <= 180):
            ap.error("--bbox must be SOUTH WEST NORTH EAST with SOUTH<NORTH and WEST<EAST")
        bbox = (s, w, n, e)

    queries: list[str] = []
    if not args.overpass_json and (args.bbox or args.country or args.world):
        if args.world:
            queries = [build_query(args, t) for t in world_tiles(args.tile_deg)]
        else:
            queries = [build_query(args, bbox)]

    if args.dry_run:
        print(f"# endpoint: {args.endpoint}")
        print(f"# {len(queries)} Overpass request(s); min ele {args.min_ele:g} m; sleep {args.sleep:g} s between requests")
        for i, q in enumerate(queries[:3]):
            print(f"# --- query {i + 1} ---")
            print(q)
        if len(queries) > 3:
            print(f"# ... {len(queries) - 3} more tile queries of the same form")
        if args.geonames:
            print(f"# would also read GeoNames dump {args.geonames} (feature class T, codes {sorted(GEONAMES_CODES)})")
        print(f"# would merge into {os.path.relpath(args.out, REPO)} (curated {os.path.relpath(args.curated, REPO)} is read-only)")
        print(f"# licence: {OSM_LICENSE['attribution']} - {OSM_LICENSE['name']}")
        return 0

    candidates: list[dict] = []
    used: set[str] = set()
    for path in args.overpass_json:
        with open(path, encoding="utf-8") as f:
            candidates += peaks_from_overpass(json.load(f), args.country)
        used.add("osm")
    for i, q in enumerate(queries):
        if i:
            time.sleep(args.sleep)
        print(f"Overpass request {i + 1}/{len(queries)} ...", file=sys.stderr)
        try:
            doc = overpass_request(args.endpoint, q, args.timeout)
        except Exception as exc:  # keep going with the other tiles
            print(f"  failed: {exc}", file=sys.stderr)
            continue
        got = peaks_from_overpass(doc, args.country)
        print(f"  {len(got)} named peaks with elevation", file=sys.stderr)
        candidates += got
        used.add("osm")
    if args.geonames:
        got = peaks_from_geonames(args.geonames, args.min_ele, bbox, args.country)
        print(f"GeoNames: {len(got)} mountains/peaks >= {args.min_ele:g} m", file=sys.stderr)
        candidates += got
        used.add("geonames")

    # OSM first so that GeoNames rows become also_in references of OSM nodes rather than the reverse.
    candidates.sort(key=lambda p: (p["source"]["dataset"] != "osm", -p["elevation_m"]))
    existing = []
    if os.path.exists(args.out):
        with open(args.out, encoding="utf-8") as f:
            existing = json.load(f).get("peaks", [])
    curated = load_curated(args.curated)
    stats = {k: 0 for k in ("added", "updated", "merged_duplicates", "shadowed_by_curated", "below_min_ele",
                            "below_min_prominence", "no_prominence")}
    peaks = merge(existing, candidates, curated, args, stats)
    write_output(args.out, peaks, queries, used)
    print(f"wrote {os.path.relpath(args.out, REPO)}: {len(peaks)} peaks "
          + ", ".join(f"{k}={v}" for k, v in stats.items()))
    print(f"Attribution required: {OSM_LICENSE['attribution']}" if "osm" in used else "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
