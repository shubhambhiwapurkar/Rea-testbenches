"""Copernicus GLO-30 DEM access: tile naming, cached download, mosaicking and reprojection.

Data source
-----------
Copernicus DEM GLO-30 Public, distributed as 1x1 degree Cloud Optimized GeoTIFFs on the
AWS Open Data bucket ``copernicus-dem-30m`` (Sinergise). Heights are float32 metres
above the EGM2008 geoid (i.e. "above mean sea level"), horizontal datum WGS84.
Tile ``Copernicus_DSM_COG_10_N27_00_E086_00_DEM`` covers 27..28 N, 86..87 E: the name is
the floor() of the south-west corner. Ocean tiles do not exist (HTTP 404) and are treated as
height 0. A few land tiles are withheld from the 30 m public release; for those the 90 m
GLO-90 tile is used when available.

Licence (modified data): "produced using Copernicus WorldDEM-30 (c) DLR e.V. 2010-2014 and
(c) Airbus Defence and Space GmbH 2014-2018 provided under COPERNICUS by the European Union
and ESA; all rights reserved".
"""

from __future__ import annotations

import math
import shutil
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Sequence

import numpy as np
import rasterio
from rasterio.crs import CRS
from rasterio.enums import Resampling
from rasterio.merge import merge
from rasterio.transform import Affine
from rasterio.warp import reproject, transform as warp_transform, transform_bounds
from scipy import ndimage

COP30_URL = "https://copernicus-dem-30m.s3.amazonaws.com"
COP90_URL = "https://copernicus-dem-90m.s3.amazonaws.com"
COPERNICUS_ATTRIBUTION = (
    "produced using Copernicus WorldDEM-30 © DLR e.V. 2010-2014 and © Airbus Defence "
    "and Space GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA; "
    "all rights reserved"
)
WGS84 = CRS.from_epsg(4326)
GDAL_REMOTE_ENV = {
    "GDAL_DISABLE_READDIR_ON_OPEN": "EMPTY_DIR",
    "CPL_VSIL_CURL_ALLOWED_EXTENSIONS": ".tif",
    "GDAL_HTTP_MAX_RETRY": "3",
    "GDAL_HTTP_RETRY_DELAY": "1",
}

Logger = Callable[[str], None]


def _print(msg: str) -> None:
    print(msg, flush=True)


# --------------------------------------------------------------------------------------
# Tile naming
# --------------------------------------------------------------------------------------
@dataclass(frozen=True, order=True)
class TileId:
    """A 1x1 degree Copernicus tile identified by the floor of its south-west corner."""

    lat: int
    lon: int

    def name(self, arcsec: int = 1) -> str:
        """Copernicus product name; arcsec=1 is GLO-30 ("_10_"), arcsec=3 is GLO-90 ("_30_")."""
        ns = "N" if self.lat >= 0 else "S"
        ew = "E" if self.lon >= 0 else "W"
        code = "10" if arcsec == 1 else "30"
        return f"Copernicus_DSM_COG_{code}_{ns}{abs(self.lat):02d}_00_{ew}{abs(self.lon):03d}_00_DEM"

    def short(self) -> str:
        return self.name(1)[22:-7].replace("_00", "")

    def url(self, arcsec: int = 1) -> str:
        base = COP30_URL if arcsec == 1 else COP90_URL
        n = self.name(arcsec)
        return f"{base}/{n}/{n}.tif"


def tiles_for_bounds(west: float, south: float, east: float, north: float) -> list[TileId]:
    """All 1-degree tiles intersecting a lon/lat box (no antimeridian support)."""
    if east < west:
        raise ValueError("Area crosses the antimeridian; this pipeline does not support that.")
    lats = range(math.floor(south), math.floor(north - 1e-12) + 1)
    lons = range(math.floor(west), math.floor(east - 1e-12) + 1)
    return [TileId(la, lo) for la in lats for lo in lons]


# --------------------------------------------------------------------------------------
# Local metric projection around the peak
# --------------------------------------------------------------------------------------
def local_crs(lat: float, lon: float, kind: str = "aeqd") -> CRS:
    """Local projection centred on the peak so that x/y are true metres near the peak.

    aeqd: Azimuthal Equidistant; distances *from the centre* are exact in every direction,
          scale error elsewhere < 0.0002 % within 10 km.
    tmerc: Transverse Mercator with central meridian through the peak, k0 = 1.
    """
    if kind == "aeqd":
        proj = f"+proj=aeqd +lat_0={lat:.8f} +lon_0={lon:.8f} +x_0=0 +y_0=0 +datum=WGS84 +units=m +no_defs"
    elif kind == "tmerc":
        proj = (
            f"+proj=tmerc +lat_0={lat:.8f} +lon_0={lon:.8f} +k=1 +x_0=0 +y_0=0 "
            "+datum=WGS84 +units=m +no_defs"
        )
    else:
        raise ValueError(f"unknown projection kind {kind!r}")
    return CRS.from_proj4(proj)


def square_geo_bounds(crs: CRS, x0: float, y0: float, x1: float, y1: float,
                      margin_m: float = 500.0) -> tuple[float, float, float, float]:
    """Lon/lat bounding box (w, s, e, n) of a projected rectangle plus a safety margin."""
    return transform_bounds(crs, WGS84, x0 - margin_m, y0 - margin_m, x1 + margin_m,
                            y1 + margin_m, densify_pts=64)


def project_point(crs: CRS, lon: float, lat: float) -> tuple[float, float]:
    xs, ys = warp_transform(WGS84, crs, [lon], [lat])
    return float(xs[0]), float(ys[0])


def unproject_point(crs: CRS, x: float, y: float) -> tuple[float, float]:
    lons, lats = warp_transform(crs, WGS84, [x], [y])
    return float(lons[0]), float(lats[0])


# --------------------------------------------------------------------------------------
# Download cache
# --------------------------------------------------------------------------------------
@dataclass
class FetchResult:
    tile: TileId
    path: Path | None
    status: str  # cached | downloaded | glo90 | ocean


def _download(url: str, dest: Path, timeout: float = 120.0) -> bool:
    """Stream url to dest atomically. Returns False on HTTP 404/403 (tile does not exist)."""
    tmp = dest.with_suffix(dest.suffix + ".part")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp, open(tmp, "wb") as fh:
            shutil.copyfileobj(resp, fh, length=1 << 20)
    except urllib.error.HTTPError as exc:
        tmp.unlink(missing_ok=True)
        if exc.code in (403, 404):  # S3 answers 403/404 for keys that do not exist
            return False
        raise
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise
    tmp.replace(dest)
    return True


def fetch_tiles(tiles: Iterable[TileId], cache_dir: Path, refresh: bool = False,
                allow_glo90: bool = True, log: Logger = _print) -> list[FetchResult]:
    """Download Copernicus tiles into cache_dir, skipping files already present.

    A ``<name>.missing`` marker remembers tiles that do not exist (ocean) so they are not
    re-requested on every build; use refresh=True to retry them.
    """
    cache_dir.mkdir(parents=True, exist_ok=True)
    results: list[FetchResult] = []
    for tile in tiles:
        p30 = cache_dir / f"{tile.name(1)}.tif"
        p90 = cache_dir / f"{tile.name(3)}.tif"
        marker = cache_dir / f"{tile.name(1)}.missing"
        if p30.exists() and not refresh:
            results.append(FetchResult(tile, p30, "cached"))
            continue
        if p90.exists() and marker.exists() and not refresh:
            results.append(FetchResult(tile, p90, "glo90"))
            continue
        if marker.exists() and not refresh:
            results.append(FetchResult(tile, None, "ocean"))
            continue
        log(f"  downloading {tile.name(1)} ...")
        if _download(tile.url(1), p30):
            marker.unlink(missing_ok=True)
            log(f"    -> {p30.name} ({p30.stat().st_size / 1e6:.1f} MB)")
            results.append(FetchResult(tile, p30, "downloaded"))
            continue
        marker.write_text("404 on GLO-30 public bucket\n")
        if allow_glo90 and _download(tile.url(3), p90):
            log(f"    GLO-30 tile not public; using GLO-90 {p90.name}")
            results.append(FetchResult(tile, p90, "glo90"))
            continue
        log("    no tile (ocean): filled with 0 m")
        results.append(FetchResult(tile, None, "ocean"))
    return results


# --------------------------------------------------------------------------------------
# Mosaic and reprojection
# --------------------------------------------------------------------------------------
@dataclass
class Mosaic:
    """A source-CRS raster covering the area of interest. NaN marks 'no data'."""

    data: np.ndarray
    transform: Affine
    crs: CRS
    sources: list[str] = field(default_factory=list)


def build_mosaic(paths: Sequence[str | Path], geo_bounds: tuple[float, float, float, float],
                 fill_uncovered: float | None = 0.0) -> Mosaic:
    """Merge one or more GeoTIFFs (same CRS) over geo_bounds (lon/lat w, s, e, n).

    Pixels not covered by any source get ``fill_uncovered`` (0 = sea level, which is right for
    Copernicus because only ocean tiles are absent) or stay NaN when it is None.
    """
    if not paths:
        raise ValueError("no DEM sources to mosaic (area is entirely ocean?)")
    with rasterio.Env(**GDAL_REMOTE_ENV):
        datasets = [rasterio.open(str(p)) for p in paths]
        try:
            crs = datasets[0].crs
            for ds in datasets[1:]:
                if ds.crs != crs:
                    raise ValueError(f"all sources must share a CRS ({ds.name} differs)")
            # Finest source first, and bounds snapped to its pixel grid: merge() then copies
            # pixels 1:1 (nearest == exact). Unsnapped bounds + interpolation leave a NaN
            # row/column along every tile seam.
            datasets.sort(key=lambda ds: ds.res[0] * ds.res[1])
            ref = datasets[0].transform
            res = (abs(ref.a), abs(ref.e))
            w, s, e, n = transform_bounds(WGS84, crs, *geo_bounds, densify_pts=64)
            w = ref.c + math.floor((w - ref.c) / res[0]) * res[0]
            e = ref.c + math.ceil((e - ref.c) / res[0]) * res[0]
            n = ref.f - math.floor((ref.f - n) / res[1]) * res[1]
            s = ref.f - math.ceil((ref.f - s) / res[1]) * res[1]
            src_nodata = [ds.nodata for ds in datasets]
            arr, transform = merge(datasets, bounds=(w, s, e, n), res=res, nodata=np.nan,
                                   dtype="float32", resampling=Resampling.nearest)
        finally:
            for ds in datasets:
                ds.close()
    data = arr[0].astype(np.float32, copy=False)
    for nd in {v for v in src_nodata if v is not None and not np.isnan(v)}:
        data[data == nd] = np.nan
    if fill_uncovered is not None:
        data[np.isnan(data)] = fill_uncovered
    return Mosaic(data, transform, crs, [str(p) for p in paths])


def warp_mosaic(mosaic: Mosaic, dst_crs: CRS, dst_transform: Affine, shape: tuple[int, int],
                resampling: Resampling = Resampling.cubic, threads: int = 4) -> np.ndarray:
    """Reproject + resample the mosaic onto the destination grid in one interpolation step."""
    dst = np.full(shape, np.nan, dtype=np.float32)
    reproject(source=mosaic.data, destination=dst, src_transform=mosaic.transform,
              src_crs=mosaic.crs, src_nodata=np.nan, dst_transform=dst_transform,
              dst_crs=dst_crs, dst_nodata=np.nan, resampling=resampling, num_threads=threads)
    return dst


def fill_voids_nearest(arr: np.ndarray) -> tuple[np.ndarray, float]:
    """Replace NaNs by the nearest valid value. Returns (array, void fraction)."""
    mask = np.isnan(arr)
    frac = float(mask.mean())
    if frac == 0.0:
        return arr, 0.0
    if frac == 1.0:
        raise ValueError("the DEM source does not cover the requested area at all")
    idx = ndimage.distance_transform_edt(mask, return_distances=False, return_indices=True)
    return arr[tuple(idx)], frac


# --------------------------------------------------------------------------------------
# Sampling helpers (used for summit / landmark checks)
# --------------------------------------------------------------------------------------
def sample_bilinear(arr: np.ndarray, transform: Affine, x: float, y: float) -> float:
    """Bilinear sample of a north-up grid at map coordinate (x, y); pixel-centre convention."""
    col, row = ~transform * (x, y)
    val = ndimage.map_coordinates(arr, [[row - 0.5], [col - 0.5]], order=1, mode="nearest")
    return float(val[0])


def local_max(arr: np.ndarray, transform: Affine, x: float, y: float, radius_m: float,
              metres_per_unit: tuple[float, float] = (1.0, 1.0)) -> tuple[float, int, int, float]:
    """Highest pixel within radius_m of (x, y). Returns (value, row, col, distance_m).

    metres_per_unit converts map units to metres (x, y) for geographic grids.
    """
    col0, row0 = ~transform * (x, y)
    px_m = (abs(transform.a) * metres_per_unit[0], abs(transform.e) * metres_per_unit[1])
    rc, rr = int(math.ceil(radius_m / px_m[0])) + 1, int(math.ceil(radius_m / px_m[1])) + 1
    r_lo, r_hi = max(int(row0) - rr, 0), min(int(row0) + rr + 1, arr.shape[0])
    c_lo, c_hi = max(int(col0) - rc, 0), min(int(col0) + rc + 1, arr.shape[1])
    if r_lo >= r_hi or c_lo >= c_hi:
        raise ValueError("point is outside the grid")
    rows, cols = np.mgrid[r_lo:r_hi, c_lo:c_hi]
    dist = np.hypot((cols + 0.5 - col0) * px_m[0], (rows + 0.5 - row0) * px_m[1])
    window = np.where(dist <= radius_m, arr[r_lo:r_hi, c_lo:c_hi], -np.inf)
    k = int(np.argmax(window))
    r, c = int(rows.flat[k]), int(cols.flat[k])
    return float(arr[r, c]), r, c, float(dist.flat[k])


def metres_per_degree(lat: float) -> tuple[float, float]:
    """WGS84 metres per degree of longitude and latitude at a latitude."""
    phi = math.radians(lat)
    m_lat = 111132.954 - 559.822 * math.cos(2 * phi) + 1.175 * math.cos(4 * phi)
    m_lon = 111412.84 * math.cos(phi) - 93.5 * math.cos(3 * phi) + 0.118 * math.cos(5 * phi)
    return m_lon, m_lat


def remote_tile_paths(tiles: Iterable[TileId], cache_dir: Path) -> list[str]:
    """Cached file if present, otherwise a /vsicurl/ URL for windowed HTTP range reads."""
    out = []
    for t in tiles:
        local = cache_dir / f"{t.name(1)}.tif"
        out.append(str(local) if local.exists() else f"/vsicurl/{t.url(1)}")
    return out


def existing_tiles(tiles: Iterable[TileId], cache_dir: Path) -> list[TileId]:
    """Filter tiles to those present in the bucket using the cached tileList.txt."""
    listing = cache_dir / "tileList.txt"
    if not listing.exists():
        cache_dir.mkdir(parents=True, exist_ok=True)
        _download(f"{COP30_URL}/tileList.txt", listing)
    names = set(listing.read_text().split()) if listing.exists() else None
    tiles = list(tiles)
    return tiles if names is None else [t for t in tiles if t.name(1) in names]
