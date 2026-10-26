# SUMMIT: Game Design Document

> Working title. Living document: update it as systems land.

## 1. Pitch

**A mountaineering expedition RPG where the mountain is real.** Real peaks at true scale, real routes and camps, real weather and real physiology. Like Death Stranding, the core loop is about *getting yourself and your load from A to B alive*. You pick a line, balance your pack across a crevasse field, lean into a 60 km/h gust on a corniced ridge, and decide at 8,600 m whether to turn back.

**Pillars**

1. **Authenticity over convenience.** Every number traces back to real data: barometric pressure, SpO₂, wind chill, rope impact forces, avalanche angles. When the game is hard, it is hard for the same reasons the mountain is.
2. **Traversal is the game.** Walking is never automatic. Footing, balance, cargo placement, rope work and pacing are skills the player gets better at.
3. **The expedition is the RPG.** Weeks of acclimatization rotations, team morale, Sherpa relationships, budgets, weather windows and permits. Decisions made at base camp matter at the summit.
4. **Consequences stay.** Frostbite costs fingers, a missed turnaround time costs lives, and losses carry across the career.
5. **Awe.** Lumen-lit sunrise over the Lhotse face, spindrift in the headlamp beam, the curvature of the horizon from the summit.

## 2. Player fantasy and progression

- **Career mode.** Start as a trekker in the Khumbu or the Alps and work up through Island Peak, Mera, Ama Dablam, the Matterhorn and Denali to your first 8,000 m peak, then the Seven Summits and the 14 eight-thousanders, eventually without bottled O₂.
- **Character.** No arbitrary stat points. Skills improve by doing: technical rock, ice, rope work, navigation, medicine, cooking, team leadership. The body changes physically through fitness (VO₂max), acclimatization history, injuries and frostbite amputations.
- **Reputation and sponsors** fund bigger expeditions. Sponsors reward summits *and* safe returns, and penalize deaths on your team.

## 3. Core loop

```
Plan (base camp) → Carry/climb (rotation) → Rest & recover → Read the forecast → Summit push → Descend
      ↑                                                                                         │
      └──────────────────── career, money, reputation, injuries, relationships ─────────────────┘
```

### 3.1 Moment-to-moment traversal (the Death Stranding layer)

| System | Player experience | Sim backing |
|---|---|---|
| **Cargo** | Choose what goes in the pack and where. Heavy items high and centred; a lopsided pack pulls you off line. | `BalanceModel` combined centre of mass |
| **Balance** | Triggers (L2/R2) lean against slope, wind and cargo. Hold both to brace. The stability gauge sits under the character. | Inverted-pendulum `BalanceModel` with player torque |
| **Footing** | Ice versus hard snow versus scree versus verglas; crampons on and off; poles add a third contact. | `SurfaceFriction`, `MaxWalkableSlopeDeg` |
| **Pacing** | Push too hard and you go anaerobic, lose stamina and gasp. Sustainable pace is the "rest step". | `Climber::MaxSustainableSpeedMS`, short-term stamina |
| **Breathing** | The camera, audio and FOV react to SpO₂. Hypoxia blurs the HUD and slows input. | `Physiology` SpO₂, cognitive impairment |

### 3.2 Climbing

- **Scrambling:** hands-down movement on 30-45° rock or snow.
- **Rock climbing:** hold-to-hold with procedural IK limb placement and a grip stamina drain. Moves are chosen by aiming the next hand or foot (as in *Jusant* or *Cairn*, but stricter).
- **Ice climbing:** axe placements and front-pointing. Placement quality depends on ice type and temperature, and a bad stick can shear.
- **Fixed ropes:** clip in, jumar up, and clip past each anchor. Old lines can be frayed or have melted-out anchors (`Anchor.ageDays`).
- **Rappelling:** build or inspect the anchor, thread the rope, descend while managing the brake hand.
- **Protection and falls:** place screws, pickets and cams. A fall resolves through `ResolveFall`: real fall factor, impact force and anchor strength.
- **Self-arrest:** a timing minigame. Success odds come from `SelfArrestSuccessProbability`, which depends on slope, surface, reaction time and skill.

### 3.3 Survival

- **Altitude:** acclimatization (days to weeks), AMS, HAPE, HACE, death-zone deterioration, bottled O₂ flow management and bottle logistics.
- **Cold:** core and skin temperatures, three extremity nodes, frostbite grades, layering and wetness, tents and sleeping bags, stoves (melting snow costs fuel and time; water boils at about 71 °C on the summit).
- **Energy and water:** calories, glycogen against fat, dehydration from breathing dry air, appetite loss above 5,000 m.
- **Hazards:** avalanches (snowpack evolves with the weather), icefall and serac collapse, crevasses, rockfall, cornices, whiteouts, lightning on ridges.

### 3.4 Expedition layer (base-camp RPG)

- **Permits and budget.** Approximate real costs, for example the Everest permit and the Liaison Officer.
- **Team.** Climbing partners, Sherpas and porters, each with skills, morale, relationships and their own physiology (the same `Climber` sim). The player hires, assigns loads, rests and rotates them.
- **Logistics.** Stock camps C1-C4 with tents, fuel, food and O₂ bottles (`OxygenBottleHours`). What the team didn't carry up doesn't exist at C4.
- **Acclimatization plan.** Rotations are checked against WMS guidelines (`EvaluateItinerary`). The game never forbids a bad plan; it lets the body react.
- **Weather.** Daily radio forecasts with error that grows with lead time. The player chooses a summit window.
- **Puja, culture and story.** Interactions in Khumbu villages, monastery visits, the history of each route. Narrative encounters (rescues, a stranded climber, ethical choices) come from a systemic event deck, not scripted levels.

### 3.5 World structure

- **Globe (strategic layer):** a Cesium-based 3D globe with every peak in the database. Choose an objective, fly in, trek.
- **Approach (semi-open):** trek valleys with villages and lodges at real scale, from real elevation data plus authored detail.
- **Mountain (playable hero zone):** 15-25 km square World Partition maps per hero mountain, built from DEM data with procedural detail and hand-authored set pieces (Khumbu Icefall, Hillary Step, Bottleneck, Hörnli hut).

**Hero mountain rollout:** Mera Peak (tutorial) → Island Peak → Ama Dablam → Matterhorn → Everest (South Col) → Denali → K2 → others.

## 4. Presentation

- **Camera:** third person over the shoulder with a dynamic distance (tight on exposed terrain, wide on glaciers). Optional first person for technical climbing.
- **Diegetic UI:** a watch for altitude, time and pressure-trend barometer; an O₂ regulator gauge on the mask; the radio for forecasts and team chatter. A minimal HUD stability arc appears only when it is needed (as in DS).
- **Audio:** wind is the soundtrack (gust layers driven by `WeatherSample`), with crampon crunch per surface, breathing tied to SpO₂ and exertion, and the radio crackle of base camp.
- **Animation:** motion matching for locomotion with load-aware poses; full-body IK for climbing; procedural lean from `BalanceState`; additive layers for shivering, gasping and stumbling.

## 5. Difficulty and accessibility

- **Simulation presets:** *Trek* (forgiving physiology, generous forecasts), *Expedition* (default, real numbers), and *Purist* (permanent death, no HUD, no bottled-O₂ shortcuts).
- Every sim system has a multiplier in the settings. "Realistic" is the reference, never the only option.
- Input assists for the balance triggers, with an auto-brace option.

## 6. Out of scope for v1

- Multiplayer roped teams (planned after the single-player vertical slice, since the deterministic sim already supports lockstep).
- Procedural mountains (real ones only).
- VR (later; the first-person climbing camera keeps that door open).
