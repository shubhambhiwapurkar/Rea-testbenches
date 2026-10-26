# Roadmap

Status key: ✅ done · 🟡 in progress · ⬜ not started

## Phase 0: Foundations (this repository today)

- ✅ Repository layout, agent conventions (`CLAUDE.md`), Git LFS rules, ruflo MCP config
- ✅ Simulation API contract (`Source/SummitSim/Public/Sim`)
- 🟡 Simulation core implementations plus unit tests (atmosphere, physiology, energy, thermal, weather, avalanche, rope, balance, expedition)
- 🟡 Headless CLI scenarios (`summit_sim_cli everest|trek|forecast`)
- 🟡 UE5 project and gameplay module scaffolding (character, components, subsystems)
- 🟡 Real-world terrain pipeline (Copernicus DEM → UE Landscape heightmaps plus layer masks)
- 🟡 Mountain database: 100+ peaks, detailed routes, regional climatologies

## Phase 1: First run on a workstation (2-4 weeks)

Needs a Windows or Linux machine with UE 5.6+, an RTX 3070-class GPU or better, 32-64 GB of RAM and 300 GB+ of free NVMe space.

1. ⬜ Generate project files and compile `Summit` and `SummitSim` in the editor (fix any UBT warnings).
2. ⬜ Import the Everest heightmap and masks (`Tools/terrain`), and set the `SummitSettings.SeaLevelWorldZCm` value from the metadata.
3. ⬜ Landscape auto-material: Megascans rock, snow, ice and scree layers driven by the masks, plus Runtime Virtual Texturing.
4. ⬜ Lighting: Sky Atmosphere, Volumetric Clouds, a sun driven by `SummitWeatherSubsystem`, exponential height fog, Lumen.
5. ⬜ `BP_SummitCharacter` with a MetaHuman or placeholder mannequin, input assets, and an AnimBP with motion-matching locomotion.
6. ⬜ Graybox the EBC → C1 Icefall section and walk it with the full sim running.

## Phase 2: Vertical slice: Mera Peak or Island Peak (2-4 months)

- ⬜ The first complete hero mountain from trailhead to summit and back
- ⬜ Cargo and balance feel tuning (the Death Stranding layer): the single most important feel milestone
- ⬜ Fixed ropes, jumaring, rappelling, crampons and ice-axe self-arrest
- ⬜ Weather visuals (spindrift Niagara systems, blowing snow, whiteout fog, cloud sea)
- ⬜ Tent, stove and sleep loop; camp inventory
- ⬜ Diegetic UI (watch, O₂ gauge, radio forecast)
- ⬜ Audio (wind layers, breathing, crampons by surface)
- ⬜ Save and load (the deterministic sim serializes small state)

## Phase 3: The expedition RPG (3-6 months)

- ⬜ Team members (AI climbers using the same `Climber` sim), Sherpas, porters and loads
- ⬜ Rotations and camp logistics, oxygen bottle economy
- ⬜ Permits, budget, sponsors, reputation
- ⬜ Event deck: rescues, crowds at the Hillary Step, avalanches, team conflicts
- ⬜ Everest South Col as the second hero mountain (Icefall, Western Cwm, Lhotse Face, South Col, summit ridge)

## Phase 4: The world (ongoing)

- ⬜ Cesium globe strategic layer with every peak in the database
- ⬜ Additional hero mountains: Ama Dablam, Matterhorn, Denali, K2, Aconcagua, Kilimanjaro, Elbrus, Vinson
- ⬜ Higher-resolution DEMs where available (swissALTI3D 0.5 m, USGS 3DEP, HMA 8 m, REMA)
- ⬜ Career mode across peaks; permanent injuries

## Phase 5: Polish and platforms

- ⬜ Performance passes (Nanite landscape, World Partition HLODs, a 60 fps target on PS5/Series X-class hardware)
- ⬜ Accessibility, difficulty presets
- ⬜ Co-op roped teams (deterministic lockstep on top of the sim)

## Team and agent workflow

- **Simulation agents** own `Source/SummitSim` and `Tests/Sim`. CI rule: `ctest` must be green.
- **Gameplay agents** own `Source/Summit` and must compile in UE before merging.
- **Data agents** own `Data/` and `Tools/data`. `validate_data.py` must pass.
- **Terrain agents** own `Tools/terrain`.
- **A human (you)** supervises: playtests feel, art direction, and approves anything that changes the API contract.
