#pragma once

#include <string>
#include <vector>

#include "Sim/Math.h"
#include "Sim/Random.h"
#include "Sim/Terrain.h"

namespace summit::sim {

// Load carriage and balance, the Death Stranding-style core of traversal.
// A climber is modelled as an inverted pendulum with a base of support set by the stance width and
// footing. Cargo shifts the combined centre of mass (CoM). Slope, wind, terrain roughness and fatigue
// disturb it, and player lean input corrects it. When the CoM leaves the support polygon for longer
// than the recovery window, the climber stumbles, and then falls.

struct CargoItem {
  std::string id;
  double massKg = 1.0;
  Vec3 offsetM;            // from the pack attachment point (x right, y forward, z up); packs sit at y < 0
  bool strapped = true;    // unstrapped cargo sways and amplifies disturbances
};

struct BalanceInputs {
  double slopeAlongDeg = 0.0;   // + uphill along travel direction
  double slopeAcrossDeg = 0.0;  // + ground falls away to the right
  double speedMS = 0.0;
  TerrainType terrain = TerrainType::Trail;
  bool crampons = false;
  bool usingPoles = false;      // trekking poles / axe as third point of contact
  double temperatureC = 0.0;
  double lateralWindMS = 0.0;   // + pushes right
  double gustiness01 = 0.0;
  double roughness01 = 0.0;     // from terrain sampling (boulders, sastrugi)
  double fatigue01 = 0.0;
  double cognitiveImpairment01 = 0.0;   // hypoxia slows corrections
  double leanInput = 0.0;       // player: -1 left .. +1 right (L2/R2 in Death Stranding)
  bool bracing = false;         // player holds both: slow, very stable
};

struct BalanceState {
  double lateralTiltRad = 0.0;     // + leaning right
  double forwardTiltRad = 0.0;     // + leaning forward
  double lateralVelRadS = 0.0;
  double forwardVelRadS = 0.0;
  double stability01 = 1.0;        // 1 = rock solid, 0 = falling; drives UI
  double slipRisk01 = 0.0;         // footing friction vs. demand
  bool stumbling = false;
  bool fallen = false;
  double secondsOutsideSupport = 0.0;
};

struct BodyGeometry {
  double bodyMassKg = 75.0;
  double heightM = 1.78;
  double comHeightM = 1.0;          // body CoM height while standing
  double stanceHalfWidthM = 0.15;
  double packAttachHeightM = 1.25;  // shoulders/upper back
};

class BalanceModel {
 public:
  explicit BalanceModel(const BodyGeometry& body = {}, std::uint64_t seed = 1);

  void SetCargo(const std::vector<CargoItem>& cargo);
  const std::vector<CargoItem>& Cargo() const { return cargo_; }

  double CargoMassKg() const;
  // Combined body plus cargo CoM, relative to the point between the feet, in metres.
  Vec3 CombinedCoM() const;
  // Static lateral imbalance from cargo alone (radians of tilt needed to compensate).
  double CargoLateralBiasRad() const;

  void Step(double dtSeconds, const BalanceInputs& in);

  // Recover from a fall (player presses "get up"). Takes time in gameplay. Here it resets state.
  void Recover();

  const BalanceState& State() const { return state_; }
  const BodyGeometry& Body() const { return body_; }

  // Recommended maximum speed (m/s) for current conditions; gameplay uses it for auto-slowdown.
  double SafeSpeedMS(const BalanceInputs& in) const;

 private:
  BodyGeometry body_;
  std::vector<CargoItem> cargo_;
  BalanceState state_;
  Pcg32 rng_;
  double swayPhase_ = 0.0;
};

// Load-to-bodyweight guidance: >30-35% of body mass sharply raises injury and stumble rates
// (Knapik et al. 2004, "Soldier load carriage: historical, physiological, biomechanical and
// medical aspects"). Himalayan porters routinely carry far more on gentle trails.
double LoadPenalty01(double loadKg, double bodyMassKg);

}  // namespace summit::sim
