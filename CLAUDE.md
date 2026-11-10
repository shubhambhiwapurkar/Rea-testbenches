# SUMMIT (working title): agent and contributor guide

SUMMIT is a realistic high-altitude mountaineering game: an expedition RPG with real-world mountains at real scale. Climbers move from base camp through high camps to the summit, with traversal in the spirit of Death Stranding (carrying loads, balance, footing, weather). The game runs on Unreal Engine 5 and C++.

## Repository layout

| Path | What lives there | Builds with |
|---|---|---|
| `Summit.uproject`, `Config/` | UE5 project and engine config | Unreal Editor 5.5+ |
| `Source/SummitSim/` | **Engine-independent simulation core** (pure C++17) plus a thin UE module wrapper | UBT *and* CMake |
| `Source/Summit/` | UE gameplay module: characters, components, subsystems, UI glue | UBT only |
| `Tests/Sim/` | Unit tests for the simulation core | CMake (`ctest`) |
| `Web/` | Playable browser game: WebGL 2 client plus `bridge/`, the WebAssembly build of the sim core | static files; `Web/bridge/build_wasm.sh` |
| `Tools/web/` | Exports terrain, satellite colour and the route for the browser game | Python 3.10+ |
| `Tools/terrain/` | Real-world DEM to UE Landscape heightmap pipeline (Python) | Python 3.10+ |
| `Tools/data/` | Scripts that build or validate the mountain database | Python 3.10+ |
| `Data/Mountains/` | Mountain, route and camp database (JSON, source of truth) | n/a |
| `Docs/` | Game design, technical design, realism references, roadmap | n/a |

## Hard rules for the simulation core (`Source/SummitSim`)

1. **No Unreal headers** anywhere under `Public/Sim` or `Private/Sim`. Only the C++17 standard library. The one exception is `Private/SummitSimModule.cpp`, which is UE-only and excluded from CMake.
2. **No exceptions, no RTTI, no iostream** (UE compiles with exceptions and RTTI off). Report errors through return values.
3. **SI units everywhere** unless the name says otherwise: metres, seconds, kilograms, newtons, watts. Temperatures are in **°C** (suffix `C`) and pressures in **kPa** (suffix `KPa`). Physiology uses mmHg internally, with the suffix `MmHg`. UE wrappers convert centimetres to metres at the boundary.
4. **Deterministic.** All randomness goes through `summit::sim::Pcg32` with explicit seeds. Simulations must be replayable.
5. **Every model cites its source** in a comment (paper, standard or field data) and has unit tests that check it against published values.
6. Namespace `summit::sim`. Types use `PascalCase`, functions use `PascalCase`, members use `camelCase`, and constants use `kPascalCase`.
7. Public headers in `Public/Sim/*.h` are the **API contract** that the UE layer depends on. You may *add* members. Do not rename or remove them without updating `Source/Summit` too.

## Building and testing the simulation core

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/summit_sim_cli everest   # headless summit-push simulation, prints an hourly log
```

## Browser game conventions (`Web`)

- Plain ES modules, no bundler and no runtime dependencies beyond the vendored three.js.
- The game never re-implements a model in JavaScript. Anything about the body, weather, snowpack,
  balance or ropes goes through `Web/bridge/SummitBridge.cpp`. Add a field to `IO_FIELDS` there and
  rebuild; `Web/js/simLayout.js` is generated from that list.
- After changing `Source/SummitSim`, run `ctest` and then `Web/bridge/build_wasm.sh`, and commit the
  rebuilt `Web/assets/summit_sim.wasm`.
- Terrain height must match between `Web/js/terrainData.js` (CPU) and `Web/js/shaders.js` (GPU): the
  climber stands on what the CPU says and the player sees what the GPU draws.
- Test in a real browser: `Web/dev/view.html` renders a fixed camera, and `index.html?autostart=push`
  skips the menu.

## UE module conventions (`Source/Summit`)

- Follow the Unreal coding standard: `U`/`A`/`F`/`E` prefixes, `UPROPERTY`/`UFUNCTION` with categories, and `TObjectPtr` for members.
- Components wrap simulation objects (for example, `USummitPhysiologyComponent` owns a `summit::sim::Climber`). Keep game logic in components and subsystems, not in the Character class.
- Unreal works in centimetres and the sim works in metres. Convert only at the wrapper boundary.

## Data

- `Data/Mountains/peaks.json` is the canonical peak list. Coordinates are WGS84 decimal degrees. Elevations are metres above mean sea level, taken from the most authoritative current survey.
- Routes live in `Data/Mountains/routes/<peak_id>__<route_slug>.json`.
- Large binary outputs (heightmaps, textures) go in `Content/` or `Saved/` through Git LFS. **Never** commit raw DEM downloads.

## Git

- Agents do not commit unless the coordinating session tells them to.
- Binary assets go through Git LFS (see `.gitattributes`).
