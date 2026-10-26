# SUMMIT

**A realistic mountaineering expedition game built in Unreal Engine 5.** It uses real mountains at real scale. Climbers move from base camp through the high camps to the summit, carrying loads and keeping their balance the way you do in Death Stranding, while the game simulates altitude physiology, cold, weather, avalanches and rope physics from published science.

> Status: **Phase 0: foundations.** The simulation core, data and terrain pipeline are developed and tested here. The UE5 project opens on a workstation (see the [Roadmap](Docs/Roadmap.md)).

## What's here

| | |
|---|---|
| [`Docs/GameDesign.md`](Docs/GameDesign.md) | What the game is: pillars, core loop, systems, world structure |
| [`Docs/TechnicalDesign.md`](Docs/TechnicalDesign.md) | Architecture, ticking, determinism, realism calibration table |
| [`Docs/ArtAndAnimationPipeline.md`](Docs/ArtAndAnimationPipeline.md) | How we reach AA/AAA visuals: terrain, Megascans, Lumen, motion matching, IK |
| [`Docs/Roadmap.md`](Docs/Roadmap.md) | Phases from foundations to the full world |
| `Source/SummitSim/` | Engine-independent simulation core (C++17), unit-tested with CMake |
| `Source/Summit/` | UE5 gameplay module: character, climbing, balance, weather, expedition |
| `Tools/terrain/` | Real-world DEM → UE Landscape heightmaps and paint-layer masks |
| `Tools/data/` | Mountain database validation and OpenStreetMap peak import |
| `Data/Mountains/` | Peaks, routes with camps, regional climatologies |
| `Tools/cli/` | Headless scenarios: Everest summit push, Khumbu trek, weather forecast |

## Quick start (no Unreal needed)

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
./build/summit_sim_cli everest            # South Col summit push on bottled O2
./build/summit_sim_cli everest --no-o2    # the hard way
./build/summit_sim_cli trek --fast        # skip acclimatization days and see what happens
./build/summit_sim_cli forecast --day 138 # 7-day summit forecast

python3 Tools/data/validate_data.py
pip install -r Tools/terrain/requirements.txt
python3 Tools/terrain/summit_terrain.py build --peak-id everest --extent-km 20 --ue-size 4033 --detail
```

## Opening in Unreal Engine 5

See [`Source/Summit/README.md`](Source/Summit/README.md) for first-run steps: generating project files, building, importing the terrain and setting up the character.

## Working with agents

`CLAUDE.md` holds the conventions every agent follows, including file ownership and the rule that the simulation core stays engine-independent. `.mcp.json` registers the **ruflo** MCP server for swarm orchestration when you run Claude Code locally.

## Data licences

- Elevation data: Copernicus DEM GLO-30, © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018, provided under COPERNICUS by the European Union and ESA. See `Tools/terrain/README.md`.
- OpenStreetMap-derived peaks (when imported): © OpenStreetMap contributors, ODbL.
