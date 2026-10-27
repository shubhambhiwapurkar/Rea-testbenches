#pragma once

#include "Sim/Export.h"

#include <cstdint>

#include "Sim/Random.h"

namespace summit::sim {

struct SUMMITSIM_API RopeSpec {
  double diameterMm = 9.8;
  double massPerMeterG = 62.0;
  double ratedImpactForceKN = 8.8;   // EN 892 / UIAA 101: first drop, 80 kg, fall factor ~1.77
  double staticElongation01 = 0.08;
  bool dynamic = true;               // false = static / fixed line (huge impact forces)
  double wear01 = 0.0;               // reduces rated performance
};

// Fall factor = fall distance / rope length paid out between climber and belay.
SUMMITSIM_API double FallFactor(double fallDistanceM, double ropeOutM);

// Rope stiffness k = E·A (N), derived from the rated impact force under the standard UIAA drop
// (80 kg mass, fall factor 1.77) by inverting the equation below. Static ropes use a far higher k.
SUMMITSIM_API double RopeStiffnessN(const RopeSpec& rope);

// Peak force on the climber (Wexler 1950 / standard rope mechanics):
//   F = m g + sqrt((m g)^2 + 2 m g k f)
// belayDynamic01 models a dynamic belay or device slip (0 = locked off, 1 = very soft catch).
SUMMITSIM_API double PeakImpactForceKN(double climberMassKg, double fallFactor, const RopeSpec& rope,
                         double belayDynamic01 = 0.0);

enum class AnchorType : std::uint8_t {
  IceScrew,       // ~10-20 kN in good ice, much less in aerated or warm ice
  SnowPicket,     // vertical ~2-5 kN, T-slot buried ~5-8 kN
  Deadman,        // snow fluke
  Cam,
  Nut,
  Piton,
  Bolt,           // 20-25 kN
  Sling,          // around a horn
  FixedLine,      // expedition fixed-rope anchor of unknown quality
  Count
};

struct SUMMITSIM_API Anchor {
  AnchorType type = AnchorType::IceScrew;
  double ratedStrengthKN = 10.0;
  double placementQuality01 = 0.8;   // player skill and time taken
  double ageDays = 0.0;              // melt-out / UV degradation (fixed lines on Everest!)
};

// Typical rated strength of the hardware of each anchor type (EN/UIAA minimums), a sensible value for
// Anchor::ratedStrengthKN when the gameplay layer has nothing better (e.g. 25 kN for a bolt).
SUMMITSIM_API double NominalAnchorStrengthKN(AnchorType type);

// Strength after medium quality and temperature. Ice is brittle when very cold and soft when warm,
// and snow anchors depend on snow hardness.
SUMMITSIM_API double AnchorEffectiveStrengthKN(const Anchor& anchor, double mediumQuality01, double temperatureC);

struct SUMMITSIM_API FallOutcome {
  double fallDistanceM = 0.0;
  double fallFactor = 0.0;
  double peakForceKN = 0.0;
  bool anchorFailed = false;
  bool ropeCut = false;            // sharp edge / extreme force on worn rope
  double injurySeverity01 = 0.0;   // 0 none, 1 fatal
};

struct SUMMITSIM_API FallScenario {
  double climberMassKg = 85.0;     // body plus pack
  double distanceAboveProtectionM = 2.0;
  double ropeOutM = 10.0;
  double slackM = 0.5;
  double slopeAngleDeg = 70.0;     // <90 means sliding/tumbling contact, raising injury but lowering force
  double belayDynamic01 = 0.3;
  bool sharpEdge = false;
};

SUMMITSIM_API FallOutcome ResolveFall(const FallScenario& scenario, const RopeSpec& rope, const Anchor& anchor,
                        double mediumQuality01, double temperatureC, Pcg32& rng);

// Injury severity (0..1, 1 = fatal) of an unprotected fall or slide of the given length. Falls beyond
// ~20-30 m on steep ground are usually fatal; on gentle slopes much longer slides are survivable.
SUMMITSIM_API double UnprotectedFallInjury01(double fallDistanceM, double slopeAngleDeg);

// Unroped slide on snow/ice: probability a self-arrest with an ice axe succeeds (0..1) given
// slope, surface and reaction time. Slides on hard snow above ~40° rarely stop.
SUMMITSIM_API double SelfArrestSuccessProbability(double slopeAngleDeg, bool iceOrHardSnow, double skill01,
                                    double reactionTimeS, double fatigue01);

}  // namespace summit::sim
