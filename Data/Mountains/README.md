# Mountain database

This folder is the source of truth for SUMMIT's real-world mountains: peaks, climbing routes with camps, and regional climatologies. Everything is plain JSON. `USummitExpeditionSubsystem` loads it at runtime into `summit::sim::Route`, `Camp`, `RouteSegment`, `ItineraryDay` (see `Source/SummitSim/Public/Sim/Expedition.h`) and `ClimateProfile` (see `Weather.h`).

```
Data/Mountains/
├── peaks.json                 curated peak list (124 peaks)
├── peaks_osm.json             optional, machine-imported peaks (generated, ODbL; not committed by default)
├── routes/<peak_id>__<slug>.json   one file per route (17 routes)
├── climate/<id>.json          regional climatologies (21 profiles)
└── schema/                    JSON Schema (draft 2020-12) for each file type
    ├── peaks.schema.json
    ├── route.schema.json
    ├── climate.schema.json
    └── peaks_osm.schema.json
```

Validate after every change:

```bash
python3 Tools/data/validate_data.py          # exit code 1 on any error
python3 Tools/data/validate_data.py --light  # force the built-in validator (no jsonschema package)
```

## Conventions

| Item | Convention |
|---|---|
| Elevation | Metres above mean sea level, from the most authoritative current survey (Everest 8848.86 m from the 2020 China-Nepal survey; Mont Blanc 4805.59 m from the 2023 survey; Denali 6190 m from the 2015 survey). |
| Coordinates | WGS84 decimal degrees, 4-5 decimals (about 1-10 m). Camp positions are often less precise. Check the `confidence` field. |
| Countries | ISO 3166-1 alpha-2 (`AQ` for Antarctica). |
| Ids | `snake_case`. A route id is `<peak_id>__<route_slug>` and matches its file name. |
| Months | 1 = January ... 12 = December. Climate arrays run January to December. |
| Units | SI, matching the sim core. Temperatures are in °C and winds in m/s. Times are in hours, which is the one deliberate exception to SI and is used because route guides quote hours. |
| Confidence | `high` means checked against an authoritative source. `medium` means consistent across several secondary sources. `low` means best estimate, so verify before relying on it. `uncertain_fields` lists individual fields that are weaker than the record's overall confidence. |

## Data model

### peaks.json

`{"version": 1, "peaks": [Peak, ...]}`. A Peak has these fields:

- `id`, `name`, `alt_names[]`
- `elevation_m`, `prominence_m` (null when unknown)
- `lat`, `lon`
- `range`, `subrange`, `country[]`
- `first_ascent`: `{year, climbers[]}`, or null when the peak is unclimbed or has no record
- `difficulty`: the grade of the easiest normal route. One of `trekking`, `scramble`, `trekking_peak`, `alpine_F` through `alpine_ED` (IFAS/French alpine grades), `big_wall`, `expedition` (remote, high or polar, but not technical), `expedition_7000`, `expedition_8000` or `station` (reached by lift).
- `tags[]`, from a fixed list: `eight_thousander`, `seven_summits`, `seven_summits_bass`, `seven_summits_messner`, `seven_summits_alt`, `volcanic_seven_summits`, `alps_4000er`, `volcano`, `nma_trekking_peak`, `unclimbed`, `closed`, …
- `climate_id`: a file name in `climate/`
- `routes[]`: the route ids defined in `routes/`
- `confidence`, `uncertain_fields[]`, `notes`

Coverage:

- all 14 eight-thousanders
- both Seven Summits lists: Kosciuszko (Bass list) and Puncak Jaya (Messner list)
- Mont Blanc, as the alternative European summit
- notable Himalaya, Karakoram, Pamir and Tian Shan peaks
- the main Alpine 4000ers
- major Andean, North American, African, Oceanian, Caucasus and Asian peaks

### routes/*.json

The fields mirror `summit::sim::Route`, plus planning data:

- `id`, `peak_id`, `name`
- `style`: `expedition`, `alpine` or `trek`
- `grade` (free text) and `technical`
- `season_best[]`
- `permit_cost_usd`, `permit_year`, `permit_notes`: an approximate per-climber fee for the stated year, with the source and caveats in the notes. It is null when the permit is only sold as part of a package.
- `camps[]`, ordered from the bottom of the route to the summit. The **last camp is the summit and has `id: "summit"`**. Each camp has:
  - `id`, `name`, `altitude_m`, `lat`, `lon`, `tent_capacity`
  - `type`: one of `trailhead`, `village`, `hut`, `base_camp`, `camp`, `bivouac`, `waypoint` or `summit`
  - `approach`: true for trek camps more than 25 km from the summit, such as Lukla
  - `confidence`, `notes`
- `descent_camps[]`: optional camps that are only on a different descent route (for example Kilimanjaro's Mweka camps). Itinerary nights may use them.
- `segments[]`, one for each consecutive pair of camps:
  - `from`, `to`, `distance_m` (path length)
  - `ascent_m`, `descent_m`. Their difference equals the altitude difference between the two camps; the extra ascent and descent capture undulations.
  - `terrain`: the snake_case name of `summit::sim::TerrainType`: `trail`, `grass`, `scree`, `boulder`, `rock`, `wet_rock`, `verglas`, `hard_snow`, `soft_snow`, `deep_snow` or `glacier_ice`
  - `hours_up`: the time from `from` to `to` for an acclimatized, fit climber
  - `hours_down`: the time for the reverse direction
  - `fixed_ropes`, `hazards[]` (from a fixed list), `max_slope_deg`, `notes`
- `typical_itinerary[]`: `{day, sleep_camp, max_altitude_m, note}` for every night of a realistic expedition, including rotations ("climb high, sleep low"), rest days, weather waits and the summit push. `EvaluateItinerary` checks these days against the Wilderness Medical Society guidelines. The validator reports, but does not fail on, nights that raise the sleeping altitude by more than 500 m, because real expeditions often do.
- `sources[]`, `confidence`, `notes`

Waypoints (for example the Balcony, the Hillary Step, the Second Step and Lava Tower) and trailheads do not need to be higher than the camp before them. All other camps must ascend in order.

### climate/*.json

These files mirror `summit::sim::ClimateProfile`, with one value per month at `reference_elevation_m`:

- `mean_temp_c[12]`
- `diurnal_range_c[12]`
- `jet_stream_wind_ms[12]`: mean wind near 300 hPa
- `storms_per_week[12]`
- `precipitation_mm_per_day[12]`: water equivalent
- `lapse_rate_c_per_km`
- `prevailing_wind_from_deg`
- `latitude_deg`
- `summit_windows[]`: a gameplay hint, not part of `ClimateProfile`
- `sources[]`, `confidence`, `notes`

The UE loader maps snake_case keys to the camelCase struct members.

The profiles are: `khumbu`, `karakoram`, `western_himalaya`, `pamir_tianshan`, `alps_western`, `caucasus`, `anatolia_iran`, `alaska_range`, `saint_elias`, `cascades`, `rocky_mountains`, `trans_mexican`, `tropical_andes`, `central_andes`, `patagonia`, `east_africa`, `equatorial_islands`, `japan`, `southern_alps_nz`, `australian_alps` and `antarctica_ellsworth`. Several of them are shared across a wider region than the name suggests; each profile's `region` and `notes` say where it is an approximation.

## What the validator checks

`Tools/data/validate_data.py` checks every file against its schema. It uses the `jsonschema` package when that is installed and otherwise falls back to a built-in subset validator. It then runs these cross-checks:

- **Peaks**
  - ids are unique and every `climate_id` exists
  - every route listed in `routes` exists, and every route file is listed by its peak
  - exactly 14 peaks carry the `eight_thousander` tag, and the tag matches the elevation
  - 8 peaks carry the Seven Summits tags (Bass and Messner lists)
  - prominence is never greater than elevation
  - flags near-duplicate coordinates
- **Routes**
  - the route id matches its file name and `peak_id`
  - the summit camp is last, within 5 m of the peak's elevation and within 1 km of the peak's position
  - camps ascend in order
  - camps lie within 25 km of the summit, or 80 km when `approach` is true
  - segments join consecutive camps
  - ascent minus descent equals the altitude difference between the camps
  - terrain values are parsed directly from `enum class TerrainType` in `Terrain.h`, so the schema and the C++ cannot drift apart
  - path lengths are at least plausible relative to the straight-line distance
  - itinerary days are consecutive, nights are spent at existing camps, `max_altitude_m` is never below the sleeping camp or above the summit, and the summit is reached
- **Climate**
  - the id matches the file name
  - the warmest and coldest months fit the hemisphere
  - the implied sea-level temperature is plausible
  - the upper-air wind is stronger in winter
  - the wettest month is not among the least stormy
  - every profile is used by at least one peak
- **OSM import**: when `peaks_osm.json` is present, it is checked against its schema and none of its ids may collide with a curated id.

## Sources

The main references are the following. Each route and climate file lists its own sources.

- **Elevations:**
  - the joint Nepal-China announcement of 8 Dec 2020 (Everest 8848.86 m)
  - the Haute-Savoie Mont Blanc survey (2023)
  - the USGS/NGS Denali survey (2015)
  - swisstopo
  - IGN (France)
  - the Instituto Geográfico Nacional (Argentina)
  - published summit surveys cited in each peak's `notes`
- **Himalayan expeditions:** *The Himalayan Database* (Hawley and Salisbury), the Nepal Department of Tourism and NMA fee schedules, the Gilgit-Baltistan royalty schedule, and the CTMA.
- **Routes:**
  - US National Park Service (Denali, Mount Rainier)
  - Aconcagua Provincial Park
  - Kilimanjaro National Park (KINAPA)
  - Antarctic Logistics & Expeditions
  - Swiss Alpine Club (SAC) guidebooks
  - FFCAM and PGHM Chamonix
  - classic accounts: Houston and Bates, *K2: The Savage Mountain*; Harrer, *The White Spider*; Hemmleb et al., *Ghosts of Everest*
- **Climate:**
  - the ERA5 reanalysis (Hersbach et al. 2020)
  - National Geographic and Tribhuvan University Everest weather stations (Matthews et al. 2020, BAMS; Perry et al. 2020, One Earth)
  - MeteoSwiss Jungfraujoch normals
  - JMA normals for the Mt. Fuji station
  - NWAC stations on Mount Rainier
  - the BoM Thredbo Top station
  - further published regional studies, cited in each file

Permit fees change often. Each route records the fee year and a confidence level in its notes, so check the current fee before using it for anything beyond gameplay balance.

## Licences

- **Curated data** (`peaks.json`, `routes/`, `climate/`) was compiled by the SUMMIT team from the public sources above. It contains facts and our own summaries; no third-party database was copied wholesale.
- **`peaks_osm.json`** is generated from **OpenStreetMap** data, which is © OpenStreetMap contributors and licensed under the [Open Database License 1.0](https://opendatacommons.org/licenses/odbl/1-0/).
  - The file is a Derivative Database, so it must stay under ODbL (share-alike).
  - Anything that publicly uses it, including in-game maps, must show "© OpenStreetMap contributors" with a link to <https://www.openstreetmap.org/copyright>, for example in the credits.
  - Keep it in its own file. Do not copy OSM-derived rows into `peaks.json` without first reviewing and re-sourcing them.
- **GeoNames** rows (when imported) are licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Credit "GeoNames (geonames.org)".
- **DEM data** used by `Tools/terrain` (SRTM, Copernicus GLO-30, ALOS) has its own terms. See that tool's documentation. Never commit raw DEM downloads.

## How to add a mountain

1. **Research.** Get the elevation from the most authoritative current survey (a national mapping agency, or a summit survey that is newer than the one Wikipedia cites). Get the coordinates of the highest point, the prominence, the first ascent and the normal route. Cross-check at least two sources.
2. **Add the peak** to `peaks.json`, in the section for its region:
   - Use a new `snake_case` `id`.
   - Set `confidence` honestly and list weak fields in `uncertain_fields`.
   - Use null for unknown values rather than guessing.
   - Choose a `climate_id` from `climate/`. If no profile fits, add a new climate file (see step 4).
3. **Optionally, add a route** as `routes/<peak_id>__<slug>.json`:
   - Order camps from the bottom to the summit and end with `{"id": "summit", "type": "summit", ...}`, whose altitude must equal the peak's elevation (±5 m).
   - Add one segment per consecutive pair of camps, using a terrain value from `TerrainType`.
   - Give each camp its own `confidence`.
   - Write a realistic `typical_itinerary`.
   - Add the route id to the peak's `routes` list.
4. **Optionally, add a climate profile** as `climate/<id>.json`, with twelve monthly values for each array, a reference elevation and site, and sources.
5. Run `python3 Tools/data/validate_data.py` and fix everything it reports.

### Bulk import (unreviewed)

```bash
# preview the Overpass query without any network access
python3 Tools/data/fetch_osm_peaks.py --country NP --min-ele 6000 --dry-run
# named OSM peaks >= 4000 m in the Alps
python3 Tools/data/fetch_osm_peaks.py --bbox 43.5 5.0 48.5 16.5 --min-ele 4000
# add GeoNames mountains (download allCountries.zip from download.geonames.org/export/dump/)
python3 Tools/data/fetch_osm_peaks.py --geonames allCountries.zip --min-ele 3000
```

`fetch_osm_peaks.py` never modifies `peaks.json`. It skips any candidate within 1.5 km of a curated peak, or within 5 km if the normalised name matches. It merges OSM and GeoNames duplicates (within 0.5 km and 60 m), keeps existing rows and updates them by source id. Respect the Overpass usage policy. For world-wide coverage, prefer a planet or regional extract (Geofabrik) filtered with osmium. `--overpass-json` accepts osmium's GeoJSON directly:

```bash
osmium tags-filter asia-latest.osm.pbf n/natural=peak -o peaks.osm.pbf
osmium export peaks.osm.pbf -f geojson --attributes id -o peaks.geojson
python3 Tools/data/fetch_osm_peaks.py --overpass-json peaks.geojson --min-ele 3000
```
