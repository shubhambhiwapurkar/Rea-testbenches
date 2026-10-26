# Art, World and Animation Pipeline

How we reach AA/AAA visual quality with a small team: lean on real data, scanned assets and engine features, and spend handmade effort only where the player looks.

## 1. Terrain: from satellite to playable mountain

```
Copernicus GLO-30 / HMA 8 m / swissALTI3D 0.5 m      (real elevation)
        │  Tools/terrain/summit_terrain.py build --detail
        ▼
16-bit heightmap + slope/altitude/aspect layer masks  (true metres, local projection)
        │  optional: Gaea / Houdini erosion pass for hero areas
        ▼
UE5 Landscape (World Partition, Nanite landscape)     (import settings from the JSON)
        │  auto-material from masks, RVT, Megascans layers
        ▼
Hand-placed hero features: seracs, crevasse fields, cornices, the Hillary Step, huts, camps
```

- **Scale is real.** 1 UE unit = 1 cm, and Large World Coordinates make an 8.8 km tall, 25 km wide map precise. Never scale a mountain down; compress *time* instead (fast travel, sleep).
- **Where to spend art time:** route corridors (±200 m of the climbing line) get Nanite rock meshes, ice and authored set pieces. Everything else is landscape plus material.
- **Glaciers and icefalls:** Houdini or Blender procedural serac kits (Voronoi-fractured ice blocks plus displacement), placed with PCG along the glacier mask. Crevasses come from PCG splines across tension zones (convex rolls in the slope mask).
- **Snow:** Runtime Virtual Texture deformation for footprints, a Niagara spindrift on ridges driven by `WindSpeed` in the weather material parameter collection, and cornice meshes on lee ridges.

## 2. Materials and textures

| Need | Source |
|---|---|
| Rock, scree, gravel, moraine | Quixel Megascans on Fab (free with UE; scanned real surfaces) |
| Snow, firn, ice, blue ice | Megascans snow/ice plus custom Substance Designer graphs for blue glacier ice with subsurface scattering |
| Landscape auto-material | Height-blended layers: rock where slope > 40°, snow above the aspect-aware snowline, glacier ice, scree, grass. Masks come from the terrain pipeline. |
| Gear (down suits, ropes, crampons) | Marvelous Designer for suits; Substance Painter; reference from real brands, but **no logos** without licences |
| Characters | MetaHuman (climbers, Sherpas), with altitude effects as material params: sunburn, wind-chapped lips, frost on the beard and eyelashes, frostbite stages on fingers and nose |

Optional **Substrate** (UE 5.5+) for physically layered materials such as ice over rock, wet rock and frosted fabric. It's off by default for performance, and we enable it once the vertical slice is profiled.

## 3. Lighting and atmosphere

- Sky Atmosphere with real sun position (the weather subsystem computes the solar elevation and azimuth from latitude, day and hour).
- Volumetric Clouds driven by `CloudCover`/`StormIntensity`. Cloud seas below the camps are a signature shot.
- Lumen GI and reflections; Virtual Shadow Maps.
- Exponential Height Fog density mapped from `WeatherSample.visibilityM` (a whiteout at < 50 m).
- High-altitude look: the sky darkens with altitude (thinner atmosphere), harsh shadows, and strong snow glare. Tune Sky Atmosphere Rayleigh and Mie per altitude.

## 4. Animation

The goal is that every hand and foot lands on the actual geometry.

| Layer | Tech |
|---|---|
| Locomotion (walk, trek, rest-step, carry heavy) | **Motion Matching** (PoseSearch plugin) with databases per load class (light, heavy, porter) and per slope |
| Balance and lean | Procedural spine and pelvis offsets from `BalanceState` (tilt, stumble) via Control Rig; DS-style arm flailing on low stability |
| Foot placement | Foot IK on uneven terrain; crampon front-pointing pose on steep snow |
| Rock and ice climbing | `USummitClimbingComponent` exposes four limb targets. The AnimBP uses Full-Body IK (Control Rig) to reach them, plus Motion Warping for reach and mantle moves. |
| Fixed ropes | Jumar cycle animation driven by speed along the rope spline; carabiner clip-pass set piece at anchors |
| Physiology | Additive layers: heavy breathing (scaled by exertion and SpO₂), shivering (`ThermalState.shivering01`), coughing (HAPE), stumbling (HACE ataxia) |
| Falls | Physical animation and ragdoll blending; rope tension handled by a constraint at the last anchor |

**Mocap plan:** start with UE's free retargetable animation sets plus marketplace climbing packs. For the vertical slice, record real climbing moves with a markerless system (Move.ai, Rokoko Vision or a Rokoko suit) on a climbing wall, with a weighted pack for load locomotion. Retarget to the MetaHuman skeleton with IK Retargeter.

## 5. Audio

- Wind: 4-6 looping layers crossfaded by speed and gust, plus an occlusion change inside tents.
- Footsteps by `TerrainType` and crampons; ice-axe placements by ice quality.
- Breathing driven by minute ventilation and SpO₂ from the sim; mask regulator sounds with O₂ on.
- Radio with comms degradation in storms.
- MetaSounds for procedural wind and breath.

## 6. Performance budget (target: 60 fps on an RTX 3070 / PS5 class)

- Nanite for all rock and ice meshes; World Partition streaming with HLODs for distant ranges.
- Distant mountains beyond the playable tile: low-res landscape proxies or Cesium tiles.
- Niagara GPU particles, capped spindrift emitters near the camera.
- Weather and physiology cost: the sim is CPU-light (microseconds per tick); AI teammates tick at reduced rates.
