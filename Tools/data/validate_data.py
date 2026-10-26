#!/usr/bin/env python3
"""Validate the SUMMIT mountain database (Data/Mountains).

Checks
  1. Every JSON file against its JSON Schema (draft 2020-12). Uses the `jsonschema` package when it is
     installed, otherwise a built-in validator that covers the subset of keywords our schemas use.
  2. Cross-file consistency:
       peaks   unique ids, climate_id exists, listed routes exist, eight-thousander / Seven Summits tags agree
               with the data, prominence <= elevation, near-duplicate coordinates.
       routes  id matches file name and peak, peak exists, summit camp is last and matches the peak's
               elevation (within 5 m) and position, camps ascend, camps lie within 25 km of the summit
               (80 km for camps flagged "approach"), segments chain camp to camp, ascent/descent agree with
               camp altitudes, terrain values match summit::sim::TerrainType in Terrain.h, itinerary camps
               exist and altitudes are plausible.
       climate id matches file name, plausible seasonality for the hemisphere, plausible sea-level
               temperature, referenced by a peak.
       osm     Data/Mountains/peaks_osm.json (optional) against its schema, no id clash with curated peaks.

Prints a report and exits 1 on any error (warnings do not fail unless --strict).

Usage
  python3 Tools/data/validate_data.py [--root Data/Mountains] [--light] [--strict] [--quiet]
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEFAULT_ROOT = os.path.join(REPO, "Data", "Mountains")
TERRAIN_HEADER = os.path.join(REPO, "Source", "SummitSim", "Public", "Sim", "Terrain.h")

SUMMIT_ELEVATION_TOLERANCE_M = 5.0
SUMMIT_POSITION_TOLERANCE_KM = 1.0
CAMP_RADIUS_KM = 25.0
APPROACH_RADIUS_KM = 80.0
ALTITUDE_BALANCE_TOLERANCE_M = 5.0
# Camps that may sit lower than the camp before them (e.g. Lukla above Phakding, a waypoint on a pass).
NON_MONOTONIC_CAMP_TYPES = {"trailhead", "waypoint"}


# --------------------------------------------------------------------------------------------
# Report
# --------------------------------------------------------------------------------------------
class Report:
    def __init__(self, quiet: bool = False):
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.info: list[str] = []
        self.quiet = quiet

    def error(self, where: str, msg: str) -> None:
        self.errors.append(f"{where}: {msg}")

    def warn(self, where: str, msg: str) -> None:
        self.warnings.append(f"{where}: {msg}")

    def note(self, msg: str) -> None:
        self.info.append(msg)


# --------------------------------------------------------------------------------------------
# Minimal JSON Schema validator (fallback when `jsonschema` is not installed)
# --------------------------------------------------------------------------------------------
_TYPES = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool) or (isinstance(v, float) and v.is_integer()),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    "boolean": lambda v: isinstance(v, bool),
    "null": lambda v: v is None,
}


class LightValidator:
    """Supports: $ref (local #/...), type, enum, const, required, properties, additionalProperties,
    items, minItems, maxItems, uniqueItems, minimum, maximum, exclusiveMinimum, exclusiveMaximum,
    minLength, maxLength, pattern, anyOf, oneOf, allOf."""

    def __init__(self, schema: dict):
        self.root = schema

    def _resolve(self, ref: str) -> dict:
        if not ref.startswith("#/"):
            raise ValueError(f"only local $ref supported, got {ref}")
        node = self.root
        for part in ref[2:].split("/"):
            node = node[part.replace("~1", "/").replace("~0", "~")]
        return node

    def iter_errors(self, instance, schema=None, path="$"):
        schema = self.root if schema is None else schema
        if schema is True or schema == {}:
            return
        if schema is False:
            yield f"{path}: not allowed"
            return
        if "$ref" in schema:
            yield from self.iter_errors(instance, self._resolve(schema["$ref"]), path)
        t = schema.get("type")
        if t is not None:
            types = t if isinstance(t, list) else [t]
            if not any(_TYPES[x](instance) for x in types):
                yield f"{path}: expected {t}, got {type(instance).__name__}"
                return
        if "enum" in schema and instance not in schema["enum"]:
            yield f"{path}: {instance!r} not in enum {schema['enum']}"
        if "const" in schema and instance != schema["const"]:
            yield f"{path}: expected const {schema['const']!r}"
        if isinstance(instance, (int, float)) and not isinstance(instance, bool):
            if "minimum" in schema and instance < schema["minimum"]:
                yield f"{path}: {instance} < minimum {schema['minimum']}"
            if "maximum" in schema and instance > schema["maximum"]:
                yield f"{path}: {instance} > maximum {schema['maximum']}"
            if "exclusiveMinimum" in schema and instance <= schema["exclusiveMinimum"]:
                yield f"{path}: {instance} <= exclusiveMinimum {schema['exclusiveMinimum']}"
            if "exclusiveMaximum" in schema and instance >= schema["exclusiveMaximum"]:
                yield f"{path}: {instance} >= exclusiveMaximum {schema['exclusiveMaximum']}"
        if isinstance(instance, str):
            if "minLength" in schema and len(instance) < schema["minLength"]:
                yield f"{path}: shorter than {schema['minLength']}"
            if "maxLength" in schema and len(instance) > schema["maxLength"]:
                yield f"{path}: longer than {schema['maxLength']}"
            if "pattern" in schema and not re.search(schema["pattern"], instance):
                yield f"{path}: {instance!r} does not match {schema['pattern']}"
        if isinstance(instance, list):
            if "minItems" in schema and len(instance) < schema["minItems"]:
                yield f"{path}: fewer than {schema['minItems']} items"
            if "maxItems" in schema and len(instance) > schema["maxItems"]:
                yield f"{path}: more than {schema['maxItems']} items"
            if schema.get("uniqueItems"):
                seen = [json.dumps(x, sort_keys=True) for x in instance]
                if len(seen) != len(set(seen)):
                    yield f"{path}: items are not unique"
            if "items" in schema:
                for i, item in enumerate(instance):
                    yield from self.iter_errors(item, schema["items"], f"{path}[{i}]")
        if isinstance(instance, dict):
            for key in schema.get("required", []):
                if key not in instance:
                    yield f"{path}: missing required property '{key}'"
            props = schema.get("properties", {})
            for key, value in instance.items():
                if key in props:
                    yield from self.iter_errors(value, props[key], f"{path}.{key}")
                else:
                    extra = schema.get("additionalProperties", True)
                    if extra is False:
                        yield f"{path}: unexpected property '{key}'"
                    elif isinstance(extra, dict):
                        yield from self.iter_errors(value, extra, f"{path}.{key}")
        if "allOf" in schema:
            for sub in schema["allOf"]:
                yield from self.iter_errors(instance, sub, path)
        if "anyOf" in schema:
            if not any(not list(self.iter_errors(instance, sub, path)) for sub in schema["anyOf"]):
                yield f"{path}: does not match any of anyOf"
        if "oneOf" in schema:
            n = sum(1 for sub in schema["oneOf"] if not list(self.iter_errors(instance, sub, path)))
            if n != 1:
                yield f"{path}: matches {n} of oneOf (expected exactly 1)"


def make_validator(schema: dict, light: bool):
    """Returns (name, fn(instance) -> list[str])."""
    if not light:
        try:
            import jsonschema  # type: ignore

            cls = jsonschema.Draft202012Validator
            cls.check_schema(schema)
            v = cls(schema)

            def run(instance):
                out = []
                for e in sorted(v.iter_errors(instance), key=lambda e: list(e.absolute_path)):
                    p = "$" + "".join(f"[{x}]" if isinstance(x, int) else f".{x}" for x in e.absolute_path)
                    out.append(f"{p}: {e.message}")
                return out

            return "jsonschema", run
        except ImportError:
            pass
    lv = LightValidator(schema)
    return "built-in", lambda instance: list(lv.iter_errors(instance))


# --------------------------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------------------------
def load_json(path: str, rep: Report):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        rep.error(rel(path), f"cannot parse JSON: {exc}")
        return None


DISPLAY_BASE = REPO  # set in main() to the directory two levels above --root


def rel(path: str) -> str:
    try:
        return os.path.relpath(path, DISPLAY_BASE)
    except ValueError:
        return path


def haversine_km(lat1, lon1, lat2, lon2) -> float:
    r = 6371.0088
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(min(1.0, a)))


def terrain_names_from_header(path: str) -> list[str] | None:
    """Parse `enum class TerrainType` from Terrain.h and convert to snake_case."""
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return None
    m = re.search(r"enum\s+class\s+TerrainType[^{]*\{(.*?)\};", text, re.S)
    if not m:
        return None
    body = re.sub(r"//[^\n]*", "", m.group(1))
    names = []
    for tok in body.split(","):
        tok = tok.strip().split("=")[0].strip()
        if not tok or tok == "Count":
            continue
        names.append(re.sub(r"(?<!^)(?=[A-Z])", "_", tok).lower())
    return names


# --------------------------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------------------------
def check_peaks(doc, rep: Report, climate_ids: set[str], route_docs: dict[str, dict]) -> dict[str, dict]:
    where = "peaks.json"
    peaks: dict[str, dict] = {}
    if not doc:
        return peaks
    for p in doc.get("peaks", []):
        pid = p.get("id")
        if pid in peaks:
            rep.error(where, f"duplicate peak id '{pid}'")
        peaks[pid] = p
    for pid, p in peaks.items():
        w = f"{where}#{pid}"
        cid = p.get("climate_id")
        if cid is None:
            rep.warn(w, "no climate_id")
        elif cid not in climate_ids:
            rep.error(w, f"climate_id '{cid}' has no file in climate/")
        for rid in p.get("routes", []):
            if rid not in route_docs:
                rep.error(w, f"route '{rid}' listed but routes/{rid}.json does not exist")
            elif route_docs[rid].get("peak_id") != pid:
                rep.error(w, f"route '{rid}' belongs to peak '{route_docs[rid].get('peak_id')}'")
        elev, prom = p.get("elevation_m"), p.get("prominence_m")
        if isinstance(elev, (int, float)) and isinstance(prom, (int, float)) and prom > elev + 0.01:
            rep.error(w, f"prominence {prom} exceeds elevation {elev}")
        tags = set(p.get("tags", []))
        if isinstance(elev, (int, float)):
            if ("eight_thousander" in tags) != (elev >= 8000):
                rep.error(w, f"eight_thousander tag inconsistent with elevation {elev}")
            if "alps_4000er" in tags and elev < 4000:
                rep.error(w, f"alps_4000er tag but elevation {elev} < 4000")
        if p.get("difficulty") == "expedition_8000" and (not isinstance(elev, (int, float)) or elev < 8000):
            rep.error(w, "difficulty expedition_8000 below 8000 m")
        if p.get("difficulty") == "expedition_7000" and isinstance(elev, (int, float)) and not 7000 <= elev < 8000:
            rep.error(w, f"difficulty expedition_7000 but elevation {elev}")
        fa = p.get("first_ascent")
        if fa and fa.get("year") is None and fa.get("climbers"):
            rep.warn(w, "first_ascent has climbers but no year")
        if p.get("confidence") == "low":
            rep.note(f"low-confidence peak: {pid}")

    # Peak-list totals
    e8 = [pid for pid, p in peaks.items() if "eight_thousander" in p.get("tags", [])]
    if len(e8) != 14:
        rep.error(where, f"expected 14 eight-thousanders, found {len(e8)}")
    seven = [pid for pid, p in peaks.items() if "seven_summits" in p.get("tags", [])]
    if len(seven) != 8:  # 7 continents, Oceania listed twice (Bass + Messner)
        rep.error(where, f"expected 8 seven_summits-tagged peaks (Bass and Messner lists), found {len(seven)}")

    # Near-duplicates
    items = list(peaks.items())
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            a, b = items[i][1], items[j][1]
            try:
                d = haversine_km(a["lat"], a["lon"], b["lat"], b["lon"])
            except (KeyError, TypeError):
                continue
            if d < 0.3:
                rep.warn(where, f"peaks '{items[i][0]}' and '{items[j][0]}' are only {d * 1000:.0f} m apart")

    # Every route file is referenced by its peak
    for rid, r in route_docs.items():
        pid = r.get("peak_id")
        if pid in peaks and rid not in peaks[pid].get("routes", []):
            rep.error(f"routes/{rid}.json", f"not listed in peaks.json#{pid}.routes")
    return peaks


def check_route(path: str, r: dict, peaks: dict[str, dict], terrain_ok: set[str] | None, rep: Report) -> None:
    w = rel(path)
    stem = os.path.splitext(os.path.basename(path))[0]
    rid = r.get("id")
    if rid != stem:
        rep.error(w, f"id '{rid}' does not match file name '{stem}'")
    pid = r.get("peak_id")
    if rid and pid and not rid.startswith(pid + "__"):
        rep.error(w, f"id '{rid}' should start with '{pid}__'")
    peak = peaks.get(pid)
    if peak is None:
        rep.error(w, f"peak_id '{pid}' not in peaks.json")

    camps = r.get("camps", [])
    descent = r.get("descent_camps", [])
    by_id: dict[str, dict] = {}
    for c in camps + descent:
        if c["id"] in by_id:
            rep.error(w, f"duplicate camp id '{c['id']}'")
        by_id[c["id"]] = c
    if not camps:
        return

    summit = camps[-1]
    if summit.get("id") != "summit":
        rep.error(w, f"last camp must have id 'summit', got '{summit.get('id')}'")
    if summit.get("type", "summit") != "summit":
        rep.error(w, "last camp must have type 'summit'")
    for c in camps[:-1]:
        if c.get("type") == "summit" or c.get("id") == "summit":
            rep.error(w, f"camp '{c['id']}' is a summit but not the last camp")
    top = summit["altitude_m"]
    if peak is not None:
        if abs(top - peak["elevation_m"]) > SUMMIT_ELEVATION_TOLERANCE_M:
            rep.error(w, f"summit altitude {top} differs from peak elevation {peak['elevation_m']} "
                         f"by more than {SUMMIT_ELEVATION_TOLERANCE_M} m")
        d = haversine_km(summit["lat"], summit["lon"], peak["lat"], peak["lon"])
        if d > SUMMIT_POSITION_TOLERANCE_KM:
            rep.error(w, f"summit camp is {d:.2f} km from the peak coordinates")

    # Camp order and altitude
    last_alt = None
    for c in camps:
        if c["altitude_m"] > top + 0.01:
            rep.error(w, f"camp '{c['id']}' ({c['altitude_m']} m) is above the summit")
        if c.get("type") in NON_MONOTONIC_CAMP_TYPES:
            continue
        if last_alt is not None and c["altitude_m"] < last_alt:
            rep.error(w, f"camp '{c['id']}' ({c['altitude_m']} m) is lower than the previous camp ({last_alt} m); "
                         "camps must be ordered bottom to summit")
        last_alt = c["altitude_m"]

    # Distance from the summit
    for c in camps + descent:
        d = haversine_km(c["lat"], c["lon"], summit["lat"], summit["lon"])
        limit = APPROACH_RADIUS_KM if c.get("approach") else CAMP_RADIUS_KM
        if d > limit:
            rep.error(w, f"camp '{c['id']}' is {d:.1f} km from the summit (limit {limit:.0f} km"
                         f"{'' if c.get('approach') else '; set approach=true for trek/approach camps'})")
        if c.get("confidence") == "low":
            pass

    # Segments
    segs = r.get("segments", [])
    order = [c["id"] for c in camps]
    if segs:
        if segs[0]["from"] != order[0]:
            rep.error(w, f"first segment starts at '{segs[0]['from']}', expected first camp '{order[0]}'")
        if segs[-1]["to"] != "summit":
            rep.error(w, f"last segment ends at '{segs[-1]['to']}', expected 'summit'")
    expected_pairs = list(zip(order[:-1], order[1:]))
    actual_pairs = [(s["from"], s["to"]) for s in segs]
    if actual_pairs != expected_pairs:
        missing = [p for p in expected_pairs if p not in actual_pairs]
        extra = [p for p in actual_pairs if p not in expected_pairs]
        rep.error(w, f"segments must join consecutive camps; missing {missing} unexpected {extra}")
    for i, s in enumerate(segs):
        sw = f"{w} segment[{i}] {s['from']}->{s['to']}"
        a, b = by_id.get(s["from"]), by_id.get(s["to"])
        if a is None:
            rep.error(sw, f"'from' camp '{s['from']}' does not exist")
        if b is None:
            rep.error(sw, f"'to' camp '{s['to']}' does not exist")
        if terrain_ok is not None and s.get("terrain") not in terrain_ok:
            rep.error(sw, f"terrain '{s.get('terrain')}' is not a summit::sim::TerrainType ({sorted(terrain_ok)})")
        if a is None or b is None:
            continue
        net = s["ascent_m"] - s["descent_m"]
        diff = b["altitude_m"] - a["altitude_m"]
        if abs(net - diff) > ALTITUDE_BALANCE_TOLERANCE_M:
            rep.error(sw, f"ascent-descent = {net:.0f} m but camp altitudes differ by {diff:.0f} m")
        straight = haversine_km(a["lat"], a["lon"], b["lat"], b["lon"]) * 1000
        slant = math.hypot(straight, diff)
        if s["distance_m"] < 0.7 * slant:
            rep.warn(sw, f"distance {s['distance_m']:.0f} m is shorter than the straight-line slant distance {slant:.0f} m")
        if s["distance_m"] > 8 * max(slant, 300) and s["distance_m"] > 5000:
            rep.warn(sw, f"distance {s['distance_m']:.0f} m is >8x the straight-line distance {slant:.0f} m")
        if s["hours_up"] > 24 or s["hours_down"] > 24:
            rep.warn(sw, "segment time over 24 h")

    # Itinerary
    days = r.get("typical_itinerary", [])
    reached_summit = False
    prev_sleep = None
    big_jumps = 0
    for i, d in enumerate(days):
        dw = f"{w} itinerary day {d.get('day')}"
        if d.get("day") != i + 1:
            rep.error(dw, f"days must be numbered consecutively from 1 (expected {i + 1})")
        c = by_id.get(d["sleep_camp"])
        if c is None:
            rep.error(dw, f"sleep_camp '{d['sleep_camp']}' is not a camp on this route")
            continue
        if d["max_altitude_m"] + 1 < c["altitude_m"]:
            rep.error(dw, f"max_altitude_m {d['max_altitude_m']} is below the sleeping camp ({c['altitude_m']} m)")
        if d["max_altitude_m"] > top + 1:
            rep.error(dw, f"max_altitude_m {d['max_altitude_m']} is above the summit")
        if abs(d["max_altitude_m"] - top) <= 1:
            reached_summit = True
        if prev_sleep is not None and c["altitude_m"] > 3000 and c["altitude_m"] - prev_sleep > 500:
            big_jumps += 1
        prev_sleep = c["altitude_m"]
    if days and not reached_summit:
        rep.error(w, "typical_itinerary never reaches the summit altitude")
    if big_jumps:
        rep.note(f"{rid}: {big_jumps} itinerary night(s) raise sleeping altitude >500 m above 3000 m "
                 "(WMS guideline exceeded; realistic for many expeditions)")
    if r.get("confidence") == "low":
        rep.note(f"low-confidence route: {rid}")
    lows = [c["id"] for c in camps + descent if c.get("confidence") == "low"]
    if lows:
        rep.note(f"{rid}: low-confidence camp positions: {', '.join(lows)}")


def check_climate(path: str, c: dict, used: set[str], rep: Report) -> None:
    w = rel(path)
    stem = os.path.splitext(os.path.basename(path))[0]
    if c.get("id") != stem:
        rep.error(w, f"id '{c.get('id')}' does not match file name '{stem}'")
    temps = c.get("mean_temp_c", [])
    lat = c.get("latitude_deg", 0.0)
    if len(temps) == 12:
        warmest = max(range(12), key=lambda i: temps[i]) + 1
        coldest = min(range(12), key=lambda i: temps[i]) + 1
        if abs(lat) >= 15:
            if lat > 0 and warmest not in (5, 6, 7, 8, 9):
                rep.error(w, f"northern-hemisphere profile but warmest month is {warmest}")
            if lat > 0 and coldest not in (11, 12, 1, 2, 3):
                rep.error(w, f"northern-hemisphere profile but coldest month is {coldest}")
            if lat < 0 and warmest not in (11, 12, 1, 2, 3):
                rep.error(w, f"southern-hemisphere profile but warmest month is {warmest}")
            if lat < 0 and coldest not in (5, 6, 7, 8, 9):
                rep.error(w, f"southern-hemisphere profile but coldest month is {coldest}")
        amp = max(temps) - min(temps)
        if abs(lat) < 10 and amp > 6:
            rep.warn(w, f"tropical profile with a {amp:.1f} °C seasonal range")
        if abs(lat) > 30 and amp < 4:
            rep.warn(w, f"mid/high-latitude profile with only a {amp:.1f} °C seasonal range")
        annual = sum(temps) / 12
        sea_level = annual + c.get("lapse_rate_c_per_km", 6.5) * c.get("reference_elevation_m", 0) / 1000
        if not -30 <= sea_level <= 32:
            rep.error(w, f"implied sea-level annual mean {sea_level:.1f} °C is implausible")
    pr = c.get("precipitation_mm_per_day", [])
    st = c.get("storms_per_week", [])
    if len(pr) == 12 and len(st) == 12:
        # Wettest month should not be among the least stormy months.
        wet = max(range(12), key=lambda i: pr[i])
        if st[wet] < sorted(st)[3]:
            rep.warn(w, f"wettest month ({wet + 1}) is one of the least stormy months")
    jet = c.get("jet_stream_wind_ms", [])
    if len(jet) == 12 and abs(lat) >= 25:
        # Mid-latitude jet should be stronger in winter than in summer (Patagonia is nearly flat).
        winter = [jet[i] for i in ((0, 1, 11) if lat > 0 else (5, 6, 7))]
        summer = [jet[i] for i in ((5, 6, 7) if lat > 0 else (0, 1, 11))]
        if sum(winter) + 3 < sum(summer):
            rep.warn(w, "upper-level wind is stronger in summer than in winter")
    if c.get("id") not in used:
        rep.warn(w, "climate profile is not referenced by any peak")
    if c.get("confidence") == "low":
        rep.note(f"low-confidence climate: {c.get('id')}")


def check_peak_climate_latitudes(peaks: dict[str, dict], climates: dict[str, dict], rep: Report) -> None:
    for pid, p in peaks.items():
        c = climates.get(p.get("climate_id"))
        if c and abs(p["lat"] - c["latitude_deg"]) > 15:
            rep.warn(f"peaks.json#{pid}", f"latitude {p['lat']} is {abs(p['lat'] - c['latitude_deg']):.0f}° from "
                                         f"climate '{c['id']}' ({c['latitude_deg']})")


def check_osm(root: str, peaks: dict[str, dict], schemas: dict, light: bool, rep: Report) -> None:
    path = os.path.join(root, "peaks_osm.json")
    if not os.path.exists(path):
        rep.note("peaks_osm.json not present (run Tools/data/fetch_osm_peaks.py to generate it)")
        return
    doc = load_json(path, rep)
    if doc is None:
        return
    schema = schemas.get("peaks_osm")
    if schema:
        _, run = make_validator(schema, light)
        for e in run(doc)[:50]:
            rep.error(rel(path), e)
    ids = set()
    for p in doc.get("peaks", []):
        if p.get("id") in peaks:
            rep.error(rel(path), f"id '{p.get('id')}' collides with a curated peak")
        if p.get("id") in ids:
            rep.error(rel(path), f"duplicate id '{p.get('id')}'")
        ids.add(p.get("id"))
    rep.note(f"peaks_osm.json: {len(ids)} generated peaks")


# --------------------------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------------------------
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=DEFAULT_ROOT, help="Data/Mountains directory")
    ap.add_argument("--light", action="store_true", help="use the built-in validator even if jsonschema is installed")
    ap.add_argument("--strict", action="store_true", help="treat warnings as errors")
    ap.add_argument("--quiet", action="store_true", help="only print errors, warnings and the summary")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    global DISPLAY_BASE
    DISPLAY_BASE = os.path.dirname(os.path.dirname(root))
    rep = Report(args.quiet)

    schema_dir = os.path.join(root, "schema")
    schemas = {}
    for name in ("peaks", "route", "climate", "peaks_osm"):
        p = os.path.join(schema_dir, f"{name}.schema.json")
        if os.path.exists(p):
            schemas[name] = load_json(p, rep)
        elif name != "peaks_osm":
            rep.error(rel(p), "schema file missing")
    if rep.errors:
        return finish(rep, args, "n/a")

    engine = "built-in"
    validators = {}
    for name, s in schemas.items():
        if s is None:
            continue
        try:
            engine, validators[name] = make_validator(s, args.light)
        except Exception as exc:  # jsonschema.SchemaError and friends
            rep.error(f"schema/{name}.schema.json", f"invalid schema: {exc}")
    if rep.errors:
        return finish(rep, args, engine)

    terrain = terrain_names_from_header(TERRAIN_HEADER)
    if terrain is None:
        rep.warn(rel(TERRAIN_HEADER), "could not parse TerrainType; using the schema enum only")
        terrain_ok = None
    else:
        terrain_ok = set(terrain)
        schema_terrain = set(schemas["route"]["$defs"]["terrain"]["enum"])
        if schema_terrain != terrain_ok:
            rep.error("schema/route.schema.json", f"terrain enum {sorted(schema_terrain)} differs from Terrain.h "
                                                  f"{sorted(terrain_ok)}")

    # Load and schema-validate
    peaks_path = os.path.join(root, "peaks.json")
    peaks_doc = load_json(peaks_path, rep) if os.path.exists(peaks_path) else None
    if peaks_doc is None and not os.path.exists(peaks_path):
        rep.error(rel(peaks_path), "missing")
    if peaks_doc is not None:
        for e in validators["peaks"](peaks_doc):
            rep.error(rel(peaks_path), e)

    # Files that fail the schema still take part in the cross-checks (guarded), so that one bad
    # field does not cascade into "missing file" errors elsewhere.
    schema_failed: set[str] = set()
    route_docs: dict[str, dict] = {}
    route_paths: dict[str, str] = {}
    for p in sorted(glob.glob(os.path.join(root, "routes", "*.json"))):
        d = load_json(p, rep)
        if d is None:
            continue
        errs = validators["route"](d)
        for e in errs:
            rep.error(rel(p), e)
        stem = os.path.splitext(os.path.basename(p))[0]
        route_docs[stem] = d
        route_paths[stem] = p
        if errs:
            schema_failed.add(p)

    climates: dict[str, dict] = {}
    climate_paths: dict[str, str] = {}
    for p in sorted(glob.glob(os.path.join(root, "climate", "*.json"))):
        d = load_json(p, rep)
        if d is None:
            continue
        errs = validators["climate"](d)
        for e in errs:
            rep.error(rel(p), e)
        stem = os.path.splitext(os.path.basename(p))[0]
        climates[stem] = d
        climate_paths[stem] = p
        if errs:
            schema_failed.add(p)

    # Cross checks
    def guarded(path, fn, *a):
        try:
            fn(*a)
        except (KeyError, TypeError, ValueError, AttributeError, IndexError) as exc:
            if path in schema_failed:
                rep.warn(rel(path), f"some cross-checks skipped because of schema errors ({type(exc).__name__}: {exc})")
            else:
                rep.error(rel(path), f"cross-check failed: {type(exc).__name__}: {exc}")

    peaks: dict[str, dict] = {}
    try:
        peaks = check_peaks(peaks_doc, rep, set(climates), route_docs)
    except (KeyError, TypeError, ValueError, AttributeError) as exc:
        rep.error(rel(peaks_path), f"peak cross-checks failed: {type(exc).__name__}: {exc}")
    for rid, d in route_docs.items():
        guarded(route_paths[rid], check_route, route_paths[rid], d, peaks, terrain_ok, rep)
    used = {p.get("climate_id") for p in peaks.values()}
    for cid, d in climates.items():
        guarded(climate_paths[cid], check_climate, climate_paths[cid], d, used, rep)
    try:
        check_peak_climate_latitudes(peaks, climates, rep)
    except (KeyError, TypeError) as exc:
        rep.warn(rel(peaks_path), f"latitude cross-check skipped: {exc}")
    check_osm(root, peaks, schemas, args.light, rep)

    rep.note(f"counts: {len(peaks)} peaks, {len(route_docs)} routes, {len(climates)} climate profiles")
    return finish(rep, args, engine)


def finish(rep: Report, args, engine: str) -> int:
    print(f"SUMMIT data validation (schema engine: {engine})")
    if not args.quiet:
        for i in rep.info:
            print(f"  info: {i}")
    for w in rep.warnings:
        print(f"  WARNING: {w}")
    for e in rep.errors:
        print(f"  ERROR: {e}")
    failed = bool(rep.errors) or (args.strict and bool(rep.warnings))
    print(f"{'FAILED' if failed else 'OK'}: {len(rep.errors)} error(s), {len(rep.warnings)} warning(s)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
