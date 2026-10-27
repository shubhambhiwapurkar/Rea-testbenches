# Summit gameplay module

`Source/Summit` is the Unreal layer around the engine-independent simulation core in
`Source/SummitSim/Public/Sim`. The sim does the physics and physiology in SI units. This module turns
world state into sim inputs, steps the sim, and turns the results into movement, animation targets, UI
and events. Unreal works in centimetres and the sim in metres; every conversion happens in
`Private/Data/SummitSimConversions.h` or at the line where a component builds sim inputs.

## Architecture

```
UGameInstance
 └─ USummitExpeditionSubsystem      peaks.json + routes/*.json → summit::sim::Route, EvaluateItinerary,
                                    campaign state (peak, route, camp, day, itinerary, money, team stub)
UWorld
 ├─ USummitWorldClockSubsystem      sim time (s since 1 Jan, local solar), TimeScale, AdvanceBy(hours)
 └─ USummitWeatherSubsystem         summit::sim::WeatherSystem + SnowpackModel, Sample(location),
                                    forecast, avalanche danger, sun/fog/MPC presentation
ASummitGameMode → ASummitCharacter, ASummitHUD → USummitHUDWidget (abstract, WBP_ derives)

ASummitCharacter (ACharacter, thin coordinator: input routing, ESummitClimbMode authority, speed clamps)
 ├─ USummitCharacterMovementComponent   MOVE_Custom: Climb / Rope / Slide physics
 ├─ USummitClimberComponent             summit::sim::Climber (physiology, thermal, energy) + floor sampling
 ├─ USummitCargoComponent               pack contents (mass, placement, strapping, consumables)
 ├─ USummitBalanceComponent             summit::sim::BalanceModel (lean, brace, stumble, fall)
 ├─ USummitClimbingComponent            walls, holds + limb IK targets, fixed ropes, self-arrest
 ├─ USpringArmComponent + UCameraComponent
ASummitFixedRope (AActor)               spline line + anchors (summit::sim::Anchor / ResolveFall)
USummitPhysicalMaterial                 Terrain (ESummitTerrainType), SnowDepthM, Roughness01, bClimbable
USummitSettings (Project Settings > Game > Summit)
```

Data flow for one frame:

```
Clock ──time──▶ Weather ──sample(alt)──▶ Climber ◀──floor trace (phys material)
                   │                      │  ▲ climb mode, velocity (character / CMC)
                   │                      │  └ cargo mass
                   └──wind──▶ Balance ◀───┘ fatigue, cognition, floor normal/terrain
                               │ safe speed, tilt, OnFallen
Character ◀────────────────────┘  MaxWalkSpeed = min(sim sustainable × burst, balance safe speed)
   │  SetClimbMode ──▶ CMC custom mode, Climber activity, Balance on/off, Climbing bookkeeping
Climbing ──wall/rope/slide state──▶ CMC PhysClimb / PhysRope / PhysSlide
```

### How each piece maps to the sim

| UE class | Sim API it wraps | Notes |
|---|---|---|
| `USummitClimberComponent` | `Climber::Step`, `MaxSustainableSpeedMS`, `ActiveConditions`, `Physio()/Thermal()/Energy()` | `ClimberEnvironment` from altitude (`USummitSettings::WorldZToAltitudeM`), `WeatherSystem::Sample`, floor material. `ClimberAction` from climb mode and velocity (grade = vz / vh × 100). `ClimberGear` from its properties plus cargo mass. Steps in game seconds (clock time scale), and integrates `AdvanceBy` jumps in one call. |
| `USummitBalanceComponent` | `BalanceModel::SetCargo/Step/SafeSpeedMS/Recover` | Slope decomposed into along/across travel; crosswind projected onto the right vector; steps in real time. |
| `USummitCargoComponent` | (`CargoItem` via conversion) | UE offsets are actor-local cm (X fwd, Y right); sim is metres (x right, y fwd). |
| `USummitClimbingComponent` | `MaxWalkableSlopeDeg`, `SelfArrestSuccessProbability`, `Pcg32` | Grip stamina drains `EnergyState::shortTermStamina01`. |
| `ASummitFixedRope` | `AnchorEffectiveStrengthKN`, `ResolveFall` | Static rope by default. |
| `USummitWeatherSubsystem` | `WeatherSystem`, `SnowpackModel`, `RegionalDanger`, `HumanTriggerProbability`, `BarometricPressureKPa`, `WindChillC` | Climate loaded from `Data/Mountains/climate/<ClimateId>.json` over `KhumbuClimate()`. |
| `USummitExpeditionSubsystem` | `Route`, `EvaluateItinerary`, `OxygenBottleHours` | Terrain strings like `hard_snow` map to `ESummitTerrainType`. |

The sim objects are held through `TSimPtr<T>` (`Public/Simulation/SummitSimHandles.h`): a `TUniquePtr`
with an out-of-line deleter, so reflected headers only forward-declare `summit::sim` types and the sim
headers stay in `.cpp` files.

## First run on your machine

1. **Prerequisites.** Unreal Engine 5.8 (compiled and tested with 5.8.4 and MSVC 14.44; other 5.x versions need `EngineAssociation` changed in `Summit.uproject`),
   and Visual Studio 2022 with the "Game development with C++" workload, or Rider. On Linux, use the
   engine's bundled clang toolchain.
2. **Generate project files.** Right-click `Summit.uproject` → *Generate Visual Studio project files*, or
   run `UnrealBuildTool -projectfiles -project="<path>/Summit.uproject" -game -engine`.
3. **Build.** Open `Summit.sln` (or the folder in Rider), select `Development Editor | Win64` and the
   `SummitEditor` target, then build. The `SummitSim` core is compiled by UBT too; you can also unit-test it
   on its own with CMake (see the root `CLAUDE.md`).
4. **Open the editor.** Launch `Summit.uproject`. CesiumForUnreal is optional; if it is not installed,
   accept the prompt to continue without it.
5. **Create the character Blueprint.** In the Content Browser, make `Content/Summit/Characters/BP_SummitCharacter`
   (parent: `SummitCharacter`). Set the Mesh (a UE5 mannequin is fine to start) and the Anim Class. On the
   **Climber** component set the profile (mass, VO2max, skills, starting acclimatization) and gear (clothing
   clo values, crampons, ice axes, oxygen litres). On **Cargo**, add a starting load and press
   *Rebalance* at runtime (or call it from BeginPlay) to pack it sensibly.
6. **Input assets.** Create the Input Actions and one Mapping Context, then assign them on BP_SummitCharacter
   (category *Summit|Input*):

   | Asset | Value type | Suggested keys |
   |---|---|---|
   | `IA_Move` | Axis2D | WASD (Swizzle/Negate modifiers), gamepad left stick |
   | `IA_Look` | Axis2D | Mouse XY (negate Y), right stick |
   | `IA_LeanLeft` / `IA_LeanRight` | Axis1D | Gamepad left/right trigger (analog), Q / E |
   | `IA_Brace` | Digital | Left Shift (both triggers also brace) |
   | `IA_Interact` | Digital | F, gamepad face-left: clip into or off a fixed rope, get up, camp actions |
   | `IA_Climb` | Digital | Space-hold alternative or gamepad face-top: grab or let go of a wall |
   | `IA_UseAxe` | Digital | Right mouse, gamepad right bumper: self-arrest while sliding, plant the axe while walking |
   | `IA_Jump` | Digital | Space, gamepad face-bottom |
   | `IA_ToggleOxygen` | Digital | O |
   | `IA_OpenInventory` | Digital | Tab, gamepad menu |

   `DefaultInput.ini` already makes `EnhancedInputComponent` the default input component. Set
   `BP_SummitGameMode` (parent `SummitGameMode`) to use BP_SummitCharacter and a HUD Blueprint whose
   `HUDWidgetClass` is a `WBP_SummitHUD` derived from `SummitHUDWidget`, and select it in *Maps & Modes*.
7. **Import a real mountain.** Run the terrain pipeline (`Tools/terrain/README.md`), for example
   `python summit_terrain.py build --peak-id everest --extent-km 20 --ue-size 4033 --detail`. In
   *Landscape mode → Manage → New → Import from File*, choose `output/everest/everest.png` (or `.r16`) and
   copy **Location, Scale and the section layout** exactly from `ue_import` in `everest.json` (the build also
   prints them). Then set **Project Settings → Game → Summit → Sea Level World Z Cm** to the JSON's
   `sea_level_world_z_cm`, and the latitude/longitude to the peak. The pipeline puts +X east and +Y south,
   which is what the weather subsystem's default `NorthYawDeg = -90` expects for sun and wind direction.
   Import the paint-layer weightmaps (rock, snow, glacier, scree, grass, dirt) into matching landscape layers.
8. **Physical materials.** Create `USummitPhysicalMaterial` assets (*Physics → Physical Material*, pick
   `SummitPhysicalMaterial`), for example `PM_Rock` (Terrain Rock, bClimbable), `PM_HardSnow`
   (HardSnow, SnowDepthM 0.05), `PM_SoftSnow` (SoftSnow, 0.2), `PM_DeepSnow` (DeepSnow, 0.6),
   `PM_GlacierIce` (GlacierIce, bClimbable), `PM_Scree` (Scree, Roughness 0.5), `PM_Grass`, `PM_Trail`.
   Assign them to the landscape material's layer infos (rock → PM_Rock, snow → PM_HardSnow,
   glacier → PM_GlacierIce, scree → PM_Scree, grass → PM_Grass, dirt → PM_Trail) and to cliff and serac
   meshes. Plain physical materials still work through the SurfaceType1-11 mapping in `DefaultEngine.ini`.
9. **Scene actors.** Make the level's Directional Light *Movable* and give it the actor tag `Sun`. Add an
   Exponential Height Fog (optionally tagged `SummitFog`). For weather visuals, create `MPC_Weather` with
   scalars `WindSpeed, WindGust, WindDirection, SnowIntensity, CloudCover, StormIntensity, Temperature,
   Visibility, SunElevation` and the vector `WindVector`, and set it in the Summit settings. Snow, cloud and
   Niagara materials read it.
10. **Fixed ropes.** Drop `SummitFixedRope` actors, shape the spline from the bottom anchor to the top one,
    add anchors (`DistanceAlongRopeCm` along the spline), and set `RopeMesh` to a 1 cm-diameter cylinder
    mesh along +X.
11. **Play.** Press Play. The clock starts at `StartDayOfYear`/`StartHour`. Watch `Vitals` on the HUD widget.
    Run `stat game` and `showdebug` as usual; the log category is `LogSummit`.

## Animation Blueprint: Motion Matching and climbing IK

- **Locomotion (Walking, Scrambling).** Use Motion Matching (PoseSearch plugin) with a Pose Search Database
  of loaded-walk, uphill, downhill and stumble clips, chosen by a Chooser Table keyed on `GetClimbMode()`
  and the pack mass from `Cargo->GetTotalMassKg()`. Add `Balance->GetLateralTiltDeg()` /
  `GetForwardTiltDeg()` as an additive lean (spine and pelvis rotation, or Control Rig) so the body visibly
  fights the load and wind. Use Orientation and Stride Warping (AnimationWarping plugin) to match the
  speed clamp.
- **Climbing (RockClimbing, IceClimbing, Jumaring, Rappelling).** Blend to a climbing state machine. Each
  frame read `Climbing->LeftHand / RightHand / LeftFoot / RightFoot` (`FSummitLimbTarget`: world Location,
  Rotation with X into the surface, `Alpha`) when `bLimbTargetsValid` is true. Feed them to a Control
  Rig (full-body IK) or to four Two Bone IK nodes (hand_l/r, foot_l/r, effector in world space,
  joint targets from the wall normal `Climbing->WallNormal`). Use `Alpha` as the IK weight, so limbs
  in flight blend toward their next hold.
- **Mantle.** Bind `Climbing->OnMantleStarted(TargetLocation)`, add a Motion Warping target there
  (MotionWarping component on the BP), and play a mantle montage. The component moves the capsule
  kinematically over `MantleDurationS`.
- **Falls and slides.** Implement `OnClimbModeChanged`, `OnFallenDown(bHardImpact)` (stumble/fall
  montage, or a partial ragdoll via physical animation), `OnRecoveredFromFall` and `OnClimberDied`.
  For self-arrest, bind `Climbing->OnSelfArrestAttempt`.

## Things that need the editor (TODO)

- None of this has been compiled yet: the first UBT build may surface include-order or deprecation fixes.
- Content assets: input actions and mapping context, BP_SummitCharacter, AnimBP, WBP_SummitHUD,
  MPC_Weather, physical materials, rope mesh, test map `L_Sandbox` (referenced as the default map in
  `DefaultEngine.ini`).
- Packaging: `Data/Mountains` sits outside `Content/`. Add it to *Additional Non-Asset Directories to Copy*
  (or stage it as non-UFS) before shipping, otherwise the expedition database will not load in a packaged
  build. The weather subsystem falls back to the built-in Khumbu climate.
- Holds are procedural (sphere traces on any climbable surface). Authored hold markup (sockets or a hold
  component) would replace `UpdateWallLimbs` tracing.
- Fall damage from the roped-fall model (`ASummitFixedRope::SimulateFall`) is exposed but not yet triggered
  automatically. Gameplay should call it when a climber slips while clipped to a line.
- Multiplayer replication is not implemented (single-player first).
