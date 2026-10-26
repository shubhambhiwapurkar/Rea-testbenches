# Technical Design

## 1. Engine and toolchain

| Concern | Choice | Why |
|---|---|---|
| Engine | **Unreal Engine 5.6+** | Nanite, Lumen, World Partition, Large World Coordinates, MetaHuman, Motion Matching, Niagara |
| Language | C++ for systems, Blueprints for content and tuning | Simulation needs speed and testability |
| Simulation core | `Source/SummitSim`: **engine-independent C++17** | Unit-testable in seconds on any CI box, deterministic, reusable for tools, servers and co-op lockstep |
| Real terrain | Copernicus GLO-30 DEM + `Tools/terrain` pipeline; Cesium for Unreal for the globe | Free global data, true scale |
| Data | JSON in `Data/Mountains` (validated by `Tools/data/validate_data.py`) | Diffable, agent-friendly, loaded at runtime by `USummitExpeditionSubsystem` |
| Version control | Git + Git LFS (Perforce optional when the team grows) | Binary assets in LFS |
| Agents | Claude Code subagents; ruflo MCP (`.mcp.json`) for swarm orchestration on local machines | Parallel development with clear file ownership |

## 2. Architecture

```
┌─────────────────────────── Unreal Engine (Source/Summit) ───────────────────────────┐
│ ASummitCharacter ──owns──► USummitClimberComponent ──wraps──► sim::Climber          │
│        │                   USummitBalanceComponent ──wraps──► sim::BalanceModel     │
│        │                   USummitCargoComponent   ──feeds──► cargo list            │
│        │                   USummitClimbingComponent (holds, IK targets, ropes,       │
│        │                        self-arrest ──uses──► sim::Rope functions)          │
│        └─ USummitCharacterMovementComponent (custom climbing movement modes)        │
│                                                                                      │
│ USummitWorldClockSubsystem ─► USummitWeatherSubsystem ──wraps──► sim::WeatherSystem │
│                                              └──────────wraps──► sim::SnowpackModel │
│                                              └─ drives sun, fog, clouds, MPC         │
│ USummitExpeditionSubsystem ──loads──► Data/Mountains/*.json ─► sim::Route/Itinerary │
│ USummitHUDWidget (UMG) ◄── FSummitVitals snapshot                                    │
└──────────────────────────────────────────────────────────────────────────────────────┘
┌──────────────────── Simulation core (Source/SummitSim, pure C++) ───────────────────┐
│ Atmosphere · Physiology · Energy · Thermal · Terrain · Weather · Avalanche · Rope   │
│ Balance · Expedition · Climber (aggregate)                                          │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

### Boundaries

- The UE layer converts **cm → m** and **cm/s → m/s** at the component boundary. No sim code knows about UE.
- World altitude: `altitudeM = (WorldZ - SeaLevelWorldZCm) / 100`. The terrain pipeline outputs the right `SeaLevelWorldZCm` per map.
- Physical materials → `TerrainType` through `USummitPhysicalMaterial`.

### Ticking

| System | Rate | Notes |
|---|---|---|
| Balance | every frame, fixed internal sub-step ≤ 1/120 s | feel-critical |
| Climber (physiology, thermal, energy) | every frame, internally sub-stepped ≤ 60 s | large dt allowed for sleeping and fast travel |
| Weather | every frame (`AdvanceTo`) | storm schedule precomputed; cheap |
| Snowpack | every in-game hour | |
| AI teammates | 1-4 Hz | same `Climber` class |

### Determinism

All randomness goes through `Pcg32` with explicit seeds, so a run is reproducible from (seed, inputs). This enables replays, bug repro, save files that store only seeds and state, and future co-op lockstep.

## 3. Realism references (calibration targets enforced by unit tests)

| Model | Source | Target |
|---|---|---|
| Barometric pressure | West 1996 (J Appl Physiol 81:1850) | Everest summit ≈ 33.7 kPa (253 Torr) in May |
| O₂ saturation | Severinghaus 1979 | PO₂ 40 mmHg → ~75% |
| Gas exchange on Everest | Grocott et al. 2009 (NEJM, Caudwell Xtreme Everest) | 8,400 m: PaO₂ 24.6 mmHg, PaCO₂ 13.3, SaO₂ 54% |
| VO₂max at altitude | West et al. 1983 (AMREE), Operation Everest II | ~15 mL/kg/min on the summit |
| Load carriage | Pandolf et al. 1977; Santee et al. 2003 (downhill) | 70 kg, 1.34 m/s, level → 293.5 W |
| Wind chill | JAG/TI 2001 | −20 °C, 30 km/h → −32.6 °C |
| Thermal model | Gagge et al. 1971 two-node | |
| Freezing of tissue | Danielsson 1996 | −0.55 °C |
| Acclimatization guidelines | Luks et al. 2019 (WMS) | ≤ 500 m/day sleeping gain above 3,000 m |
| Rope impact | UIAA 101 / EN 892; Wexler 1950 | rated force reproduced at FF 1.77, 80 kg |
| Avalanche slope angle | Schweizer & Jamieson 2001; McClung & Schaerer | peak at 38-40° |
| Danger scale | EAWS | levels 1-5 |
| Load and injury | Knapik et al. 2004 | > 30-35% bodyweight sharply worse |
| Everest weather | Matthews et al. 2020; Perry et al. 2020 (Balcony AWS) | summit May mean ≈ −20 to −26 °C |

## 4. Testing

- `ctest` runs unit tests for every sim model (fast, no engine).
- `summit_sim_cli` scenarios are integration checks against expedition reports (a summit push with and without O₂, a trek with and without rest days).
- In-engine: Automation Tests (`Source/Summit/Private/Tests`) for component wiring, plus Gauntlet/functional tests for map-load smoke tests (Phase 1).
- Data: `python3 Tools/data/validate_data.py`.

## 5. Save data

Small and deterministic: expedition state (day, money, camps' inventories, team), per-climber sim state structs (PhysiologyState, ThermalState, EnergyState), the weather seed and the clock. Serialized via `USaveGame` with plain USTRUCT mirrors.
