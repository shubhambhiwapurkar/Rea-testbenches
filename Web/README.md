# SUMMIT: Everest (browser game)

A playable version of the game that runs in any modern browser. It uses the same simulation core as
the Unreal project (`Source/SummitSim`, compiled to WebAssembly) on the real Everest terrain from
`Tools/terrain`, with a real Sentinel-2 satellite image draped over it.

## Play

```bash
python3 -m http.server 8080 -d Web
# then open http://localhost:8080
```

Any static web server works; the game is plain files with no build step. It needs WebGL 2 and
WebAssembly (current Chrome, Edge, Firefox or Safari). Add `?q=low` for slower graphics cards.

Three modes:

- **Summit push** starts at Camp 2 (6,400 m), acclimatized, as a storm clears. Read the forecast in
  the tent, climb to Camp 3 and Camp 4, leave the South Col at night, summit, and get back down.
- **Full expedition** starts at Base Camp on 10 April. Acclimatization is yours to manage.
- **Free roam** cannot kill you. Jump between camps and change the time of day from any tent.

## Controls

| Key | Action |
|---|---|
| `W A S D`, mouse, wheel | walk, look, zoom |
| `Shift` / `Ctrl` | push harder / careful steps |
| `Q` `E`, `Space` | lean left or right; brace, or self-arrest while sliding |
| `T` / `B` | march up / down the wanded route in time-lapse |
| `1` `2` `3` | time speed ×1, ×4, ×16 |
| `F` | enter a tent, or clip the fixed rope |
| `R` `G` `H` | rest, eat a gel, drink |
| `O`, `[` `]` | oxygen mask, flow rate |
| `C` `V` `P` `L` | crampons, goggles, poles, headlamp |
| `M`, `Tab` | map, help |

## What is simulated

Everything on the HUD comes from the C++ simulation core, the same code the 130 unit tests cover:

- **Daylight**: sun and moon positions for the date and latitude; terrain shadows from precomputed
  horizon maps, so dawn reaches the summit long before the Western Cwm.
- **Weather**: seeded Khumbu climatology with jet-stream wind that rises with altitude, storms,
  snowfall, visibility and a forecast that gets less reliable further out.
- **Elevation**: barometric pressure, blood oxygen, acclimatization, altitude illness, bottled oxygen.
- **Cold**: wind chill, core and extremity temperatures, frostbite, wet clothing.
- **Effort**: load carriage cost (Pandolf), sustainable pace, glycogen, hydration, fatigue.
- **Balance**: an inverted pendulum pushed by wind, slope and an off-centre load; you lean against it.
- **Hazards**: falls and self-arrest odds, fixed-line anchor forces, crevasses off the wanded route,
  slab avalanches on 27-55° snow driven by a snowpack that follows the weather.

## How it is built

| Path | What |
|---|---|
| `index.html`, `css/`, `js/` | the game: ES modules, no bundler |
| `js/sim.js`, `js/simLayout.js` | loads the WebAssembly core; the field layout is generated |
| `js/terrainData.js`, `js/terrainView.js`, `js/shaders.js` | terrain queries on the CPU; geometry-clipmap rendering on the GPU |
| `js/game.js` | game rules: movement limits, falls, hazards, camps, marching |
| `bridge/` | C bridge to `Source/SummitSim` and the WebAssembly build script |
| `assets/` | exported terrain, satellite image and `summit_sim.wasm` |
| `dev/view.html` | developer view: render the world from a fixed camera |
| `vendor/` | three.js (MIT) |

### Rebuilding the simulation

Needs clang with the wasm32 target, `wasm-ld`, and the [wasi-sdk](https://github.com/WebAssembly/wasi-sdk/releases) sysroot:

```bash
WASI_SDK=/path/to/wasi-sdk Web/bridge/build_wasm.sh
```

### Rebuilding the terrain assets

```bash
cd Tools/terrain
python3 summit_terrain.py build --peak-id everest --extent-km 20 --ue-size 4033 --detail --fit-landmarks
python3 summit_terrain.py build --peak-id everest --extent-km 80 --ue-size 1009 --no-layers --output-dir output/everest_far
cd ../..
python3 Tools/web/fetch_satellite.py --peak everest      # optional: real colour
python3 Tools/web/export_web_assets.py --peak everest --route everest__south_col
```

The exporter traces the climbing route over the terrain by least slope cost between the database's
camps, and snaps each camp onto nearby ground that matches its real altitude.

## Known limits

- The 30 m source elevation data smooths sharp ridges, so the summit ridge is gentler than the real one.
- The climber is a simple jointed figure with procedural animation, not a motion-captured character.
- One mountain and one route so far. `Tools/web/export_web_assets.py --peak <id> --route <id>` works
  for any peak that `Tools/terrain` can build and that has a route in `Data/Mountains/routes`.

## Attribution

- Elevation: produced using Copernicus WorldDEM-30 © DLR e.V. 2010-2014 and © Airbus Defence and Space
  GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA; all rights reserved.
- Satellite colour: contains modified Copernicus Sentinel data 2023.
- three.js © three.js authors, MIT licence (`vendor/three.LICENSE`).
