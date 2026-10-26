#pragma once

#include "Sim/Terrain.h"

namespace summit::sim {

// Pandolf et al. (1977) load-carriage metabolic rate (W):
//   M = 1.5 W + 2.0 (W+L)(L/W)^2 + eta (W+L)(1.5 V^2 + 0.35 V G)
// W body mass kg, L load kg, V speed m/s, G grade %, eta terrain factor.
// Negative grades use the Santee et al. (2003) downhill correction.
double PandolfMetabolicRateW(double bodyMassKg, double loadKg, double speedMS, double gradePercent,
                             double terrainFactor);

// Approximate oxygen cost of a metabolic rate: 1 L O2 ≈ 20.1 kJ (≈4.8 kcal).
double MetabolicRateToVO2MlKgMin(double metabolicW, double bodyMassKg);

struct EnergyParams {
  double bodyMassKg = 75.0;
  double glycogenCapacityKcal = 2000.0;   // muscle + liver
  double fatReserveKcal = 90000.0;        // ~12 kg fat-store equivalent; usable over expedition
  double totalBodyWaterL = 42.0;
};

struct EnergyState {
  double glycogenKcal = 2000.0;
  double fatReserveKcal = 90000.0;
  double hydrationDeficitL = 0.0;        // >0 means dehydrated; 4% body mass ≈ severe performance loss
  double shortTermStamina01 = 1.0;       // anaerobic reserve: sprint, hard moves, recovers in minutes
  double fatigue01 = 0.0;                // accumulated muscular/central fatigue; sleep restores
  double sleepDebtHours = 0.0;
  double bodyMassLostKg = 0.0;           // expeditions commonly cost 5-10 kg

  // Additional state.
  double appetite01 = 1.0;               // share of normal appetite; falls above ~3,500 m
  double stomachKcal = 0.0;              // eaten but not yet absorbed
  double recentIntakeKcal = 0.0;         // rolling ~24 h intake; Eat() refuses beyond appetite
  double lastMealAcceptedKcal = 0.0;     // what the last Eat() call actually got down
  double lastSleepQuality01 = 1.0;       // periodic breathing at altitude fragments sleep
};

struct EnergyInputs {
  double metabolicRateW = 100.0;
  double exertion01 = 0.0;               // fraction of current VO2max
  double ambientTempC = 15.0;
  double relativeHumidity01 = 0.3;
  double minuteVentilationLMin = 7.0;    // drives respiratory water loss
  bool sleeping = false;
  double altitudeM = 0.0;                // appetite suppression / malabsorption above ~5000 m
};

class EnergySystem {
 public:
  explicit EnergySystem(const EnergyParams& params = {});

  void Step(double dtSeconds, const EnergyInputs& in);
  void Eat(double kcal);
  void Drink(double liters);

  const EnergyState& State() const { return state_; }
  EnergyState& MutableState() { return state_; }

  // Multiplier (0..1] on maximal output from glycogen depletion, dehydration and fatigue.
  double PerformanceMultiplier() const;

  // Respiratory water loss (L/h) from humidifying cold, dry air.
  static double RespiratoryWaterLossLPerHour(double minuteVentilationLMin, double ambientTempC,
                                             double relativeHumidity01);

  // Saturation vapour pressure (kPa) over water (T >= 0) or ice (T < 0), Buck (1996).
  static double SaturationVapourPressureKPa(double temperatureC);

  // Appetite (0..1 of sea-level appetite) and sleep quality (0..1) at a physiological altitude.
  static double AppetiteFactor01(double altitudeM);
  static double SleepQuality01(double altitudeM);

  // Kcal the climber could still eat right now before appetite refuses.
  double RemainingAppetiteKcal() const;

 private:
  EnergyParams params_;
  EnergyState state_;
  double lastAltitudeM_ = 0.0;
};

}  // namespace summit::sim
