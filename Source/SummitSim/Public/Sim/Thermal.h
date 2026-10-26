#pragma once

#include <array>
#include <cstdint>

namespace summit::sim {

// North American / UK wind chill index (JAG/TI 2001). Wind measured at 10 m, km/h.
// Valid for T <= 10 °C and V >= 4.8 km/h; returns air temperature outside that range.
double WindChillC(double airTempC, double windKmh);

enum class BodyPart : std::uint8_t { Hands = 0, Feet = 1, Face = 2, Count = 3 };

enum class FrostbiteGrade : std::uint8_t { None, Frostnip, Superficial, Deep };

enum class HypothermiaStage : std::uint8_t {
  None,      // core >= 35 °C
  Mild,      // 32-35 °C: shivering, clumsy
  Moderate,  // 28-32 °C: shivering stops, confusion
  Severe     // < 28 °C: unconscious, arrhythmia risk
};

struct ClothingSystem {
  double torsoClo = 4.0;          // 1 clo = 0.155 m²K/W. Down suit ≈ 6-8, softshell ≈ 1.5
  double handsClo = 2.0;          // liner ≈ 0.5, gloves ≈ 1.5, expedition mitts ≈ 3+
  double feetClo = 2.5;           // single boot ≈ 1.5, double/8000 m boot ≈ 3+
  double faceCoverage01 = 0.5;    // buff, mask, goggles
  double windproof01 = 0.8;       // shell effectiveness against convective stripping
  double wetness01 = 0.0;         // sweat or snowmelt; wet insulation loses most of its value
};

struct ThermalEnvironment {
  double airTempC = 0.0;
  double windMS = 0.0;
  double solarWm2 = 0.0;          // direct + reflected (snow albedo ~0.8 makes this large)
  double relativeHumidity01 = 0.5;
  bool inTent = false;            // cuts wind, adds a few degrees
  bool inSleepingBag = false;
  double sleepingBagComfortC = -20.0;
};

struct ThermalState {
  double coreTempC = 37.0;
  double meanSkinTempC = 33.0;
  std::array<double, 3> extremityTempC{30.0, 30.0, 32.0};   // indexed by BodyPart
  std::array<double, 3> frostbiteDamage01{0.0, 0.0, 0.0};   // permanent tissue damage accumulated
  double shivering01 = 0.0;

  // Additional state / diagnostics.
  std::array<double, 3> perfusion01{1.0, 1.0, 1.0};  // extremity blood flow (0 = max vasoconstriction)
  double heatProductionW = 0.0;                       // metabolic + shivering, last step
  double respiratoryHeatLossW = 0.0;                  // warming and humidifying inspired air
  double shiveringFatigue01 = 0.0;                    // hours of shivering exhaust its capacity
};

struct ThermalInputs {
  ThermalEnvironment env;
  ClothingSystem clothing;
  double metabolicHeatW = 100.0;  // from EnergySystem / Pandolf, includes shivering added internally
  double bodyMassKg = 75.0;
  double spo2 = 0.98;             // hypoxia blunts shivering and increases peripheral vasoconstriction
  double hydrationDeficitL = 0.0; // dehydration reduces peripheral perfusion

  // Additional inputs.
  double minuteVentilationLMin = 8.0;  // respiratory heat loss (BTPS)
  double pressureKPa = 101.325;        // thin air carries less heat (convection, respiration)
  double glycogenFraction01 = 1.0;     // shivering needs carbohydrate
};

// Two-node (core/skin) heat balance after Gagge et al. (1971), with three peripheral nodes for
// frostbite. Tissue freezes around -0.55 °C (Danielsson 1996). Damage accumulates while below.
class ThermalModel {
 public:
  ThermalModel() = default;

  void Step(double dtSeconds, const ThermalInputs& in);

  const ThermalState& State() const { return state_; }
  ThermalState& MutableState() { return state_; }

  HypothermiaStage Hypothermia() const;
  FrostbiteGrade Frostbite(BodyPart part) const;

  // Net heat loss (W) at the last step; positive = losing heat.
  double LastNetHeatLossW() const { return lastNetHeatLossW_; }

  // Thermal "feels like" for UI: wind chill plus clothing context.
  static double EffectiveExposureTempC(const ThermalEnvironment& env);

 private:
  ThermalState state_;
  double lastNetHeatLossW_ = 0.0;
};

}  // namespace summit::sim
