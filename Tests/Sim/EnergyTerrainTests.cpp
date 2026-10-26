// Load-carriage energetics, fuel, water, fatigue and terrain.

#include <cmath>
#include <cstring>

#include "Sim/Energy.h"
#include "Sim/Terrain.h"
#include "TestFramework.h"

using namespace summit::sim;

namespace {

void Run(EnergySystem& e, double hours, const EnergyInputs& in) {
  for (double t = 0.0; t < hours * 3600.0; t += 60.0) e.Step(60.0, in);
}

EnergyInputs Activity(double metabolicW, double exertion, double altitudeM, bool sleeping = false) {
  EnergyInputs in;
  in.metabolicRateW = metabolicW;
  in.exertion01 = exertion;
  in.altitudeM = altitudeM;
  in.sleeping = sleeping;
  in.ambientTempC = -10.0;
  in.relativeHumidity01 = 0.4;
  in.minuteVentilationLMin = sleeping ? 9.0 : (exertion > 0.3 ? 50.0 : 14.0);
  return in;
}

}  // namespace

// --- Pandolf -------------------------------------------------------------------------------------

TEST_CASE("Energy: Pandolf 1977 reference value (70 kg, no load, 1.34 m/s, level) = 293.5 W") {
  CHECK_NEAR(PandolfMetabolicRateW(70.0, 0.0, 1.34, 0.0, 1.0), 293.5, 0.5);
  // Standing cost with no movement.
  CHECK_NEAR(PandolfMetabolicRateW(70.0, 0.0, 0.0, 0.0, 1.0), 105.0, 1e-9);
  // Load term 2 (W+L)(L/W)^2: 70 kg carrying 20 kg.
  CHECK_NEAR(PandolfMetabolicRateW(70.0, 20.0, 0.0, 0.0, 1.0), 105.0 + 2.0 * 90.0 * (20.0 / 70.0) * (20.0 / 70.0), 1e-9);
}

TEST_CASE("Energy: uphill costs more; Santee downhill correction") {
  const double level = PandolfMetabolicRateW(70.0, 0.0, 1.34, 0.0, 1.0);
  const double up10 = PandolfMetabolicRateW(70.0, 0.0, 1.34, 10.0, 1.0);
  const double down5 = PandolfMetabolicRateW(70.0, 0.0, 1.34, -5.0, 1.0);
  const double down10 = PandolfMetabolicRateW(70.0, 0.0, 1.34, -10.0, 1.0);
  CHECK(up10 > 1.9 * level);
  // Santee et al. (2003): ~226 W at -10 % (about 0.75 of level walking).
  CHECK_NEAR(down10, 226.0, 5.0);
  CHECK(down5 < level);
  // Steep descents get dearer again but stay bounded, and standing still costs the standing rate.
  const double steep = PandolfMetabolicRateW(75.0, 9.0, 0.4, -50.0, 1.3);
  CHECK_RANGE(steep, 150.0, 450.0);
  CHECK_NEAR(PandolfMetabolicRateW(75.0, 9.0, 0.0, -50.0, 1.3), 1.5 * 75.0 + 2.0 * 84.0 * (9.0 / 75.0) * (9.0 / 75.0), 1e-9);
  // Monotonic in speed on a steep descent (no stall for the speed solver).
  double previous = 0.0;
  for (double v = 0.0; v <= 2.0; v += 0.1) {
    const double m = PandolfMetabolicRateW(75.0, 9.0, v, -40.0, 1.3);
    CHECK(m >= previous - 1e-9);
    previous = m;
  }
}

TEST_CASE("Energy: metabolic rate to VO2 (1 L O2 = 20.1 kJ)") {
  CHECK_NEAR(MetabolicRateToVO2MlKgMin(293.5, 70.0), 12.5, 0.1);
  CHECK_NEAR(MetabolicRateToVO2MlKgMin(1.1725 * 70.0, 70.0), 3.5, 0.01);  // 1 MET
}

// --- Terrain -------------------------------------------------------------------------------------

TEST_CASE("Terrain: Pandolf / Soule & Goldman terrain factors") {
  CHECK_RANGE(PandolfTerrainFactor(TerrainType::Trail), 1.1, 1.2);
  CHECK_RANGE(PandolfTerrainFactor(TerrainType::HardSnow), 1.3, 1.45);
  // Pandolf et al. 1976: eta = 1.30 + 0.082 D^1.6 with D the footprint depth in cm.
  CHECK_NEAR(PandolfTerrainFactor(TerrainType::SoftSnow, 0.10), 1.30 + 0.082 * std::pow(10.0, 1.6), 1e-9);
  CHECK_NEAR(PandolfTerrainFactor(TerrainType::SoftSnow, 0.10), 4.56, 0.02);
  CHECK_RANGE(PandolfTerrainFactor(TerrainType::DeepSnow), 2.5, 4.5);
  CHECK_RANGE(PandolfTerrainFactor(TerrainType::SoftSnow), 1.8, 3.0);
  CHECK(PandolfTerrainFactor(TerrainType::DeepSnow, 0.3) > PandolfTerrainFactor(TerrainType::DeepSnow, 0.15));
  CHECK(PandolfTerrainFactor(TerrainType::DeepSnow, 2.0) <= 20.0);
  CHECK(PandolfTerrainFactor(TerrainType::DeepSnow) > PandolfTerrainFactor(TerrainType::SoftSnow));
  CHECK(PandolfTerrainFactor(TerrainType::SoftSnow) > PandolfTerrainFactor(TerrainType::HardSnow));
  CHECK(PandolfTerrainFactor(TerrainType::Scree) > PandolfTerrainFactor(TerrainType::Trail));
}

TEST_CASE("Terrain: friction coefficients") {
  CHECK_RANGE(SurfaceFriction(TerrainType::Rock, false, 10.0), 0.8, 1.0);
  CHECK_RANGE(SurfaceFriction(TerrainType::HardSnow, false, -10.0), 0.25, 0.35);
  CHECK_RANGE(SurfaceFriction(TerrainType::GlacierIce, false, -10.0), 0.05, 0.12);
  CHECK(SurfaceFriction(TerrainType::GlacierIce, false, -25.0) > SurfaceFriction(TerrainType::GlacierIce, false, -1.0));
  CHECK(SurfaceFriction(TerrainType::GlacierIce, true, -10.0) >= 0.8);
  CHECK(SurfaceFriction(TerrainType::HardSnow, true, -10.0) >= 0.8);
  CHECK(SurfaceFriction(TerrainType::Verglas, true, -10.0) <= 0.35);  // treacherous even with crampons
  CHECK(SurfaceFriction(TerrainType::Verglas, false, -10.0) <= 0.1);
  CHECK(SurfaceFriction(TerrainType::Rock, true, 10.0) < SurfaceFriction(TerrainType::Rock, false, 10.0));
  CHECK(SurfaceFriction(TerrainType::WetRock, false, -5.0) < 0.15);  // refrozen: verglas
}

TEST_CASE("Terrain: walkable slopes and names") {
  CHECK(MaxWalkableSlopeDeg(TerrainType::HardSnow, true) > MaxWalkableSlopeDeg(TerrainType::HardSnow, false));
  CHECK(MaxWalkableSlopeDeg(TerrainType::GlacierIce, false) < 15.0);
  CHECK_RANGE(MaxWalkableSlopeDeg(TerrainType::Scree, false), 30.0, 38.0);
  CHECK(std::strcmp(TerrainTypeName(TerrainType::DeepSnow), "DeepSnow") == 0);
  CHECK(std::strcmp(TerrainTypeName(TerrainType::Count), "Unknown") == 0);
  for (int i = 0; i < static_cast<int>(TerrainType::Count); ++i) {
    const auto t = static_cast<TerrainType>(i);
    CHECK(SurfaceFriction(t, false, -5.0) > 0.0);
    CHECK(PandolfTerrainFactor(t) >= 1.0);
    CHECK(MaxWalkableSlopeDeg(t, true) > 0.0);
  }
}

// --- Water ---------------------------------------------------------------------------------------

TEST_CASE("Energy: respiratory water loss in cold dry air at high ventilation ~0.2-0.4 L/h") {
  const double climbing = EnergySystem::RespiratoryWaterLossLPerHour(100.0, -20.0, 0.3);
  CHECK_RANGE(climbing, 0.18, 0.4);
  CHECK(EnergySystem::RespiratoryWaterLossLPerHour(150.0, -25.0, 0.3) <= 0.4);
  const double rest = EnergySystem::RespiratoryWaterLossLPerHour(8.0, 20.0, 0.5);
  CHECK(rest < 0.03);
  CHECK(EnergySystem::RespiratoryWaterLossLPerHour(60.0, 35.0, 1.0) < 1e-6);
  CHECK_NEAR(EnergySystem::SaturationVapourPressureKPa(20.0), 2.339, 0.01);
  CHECK_NEAR(EnergySystem::SaturationVapourPressureKPa(-20.0), 0.1032, 0.002);
}

TEST_CASE("Energy: dehydration accumulates on a summit day and drinking fixes it") {
  EnergySystem e;
  Run(e, 10.0, Activity(450.0, 0.6, 8000.0));
  CHECK_RANGE(e.State().hydrationDeficitL, 1.0, 4.0);
  const double perf = e.PerformanceMultiplier();
  e.Drink(5.0);
  CHECK_NEAR(e.State().hydrationDeficitL, 0.0, 1e-9);
  CHECK(e.PerformanceMultiplier() > perf);
}

// --- Fuel ----------------------------------------------------------------------------------------

TEST_CASE("Energy: glycogen burns faster at high exertion, fat at low") {
  EnergySystem hard;
  EnergySystem easy;
  // Same total energy: 2 h at 600 W vs 4 h at 300 W.
  Run(hard, 2.0, Activity(600.0, 0.85, 1000.0));
  Run(easy, 4.0, Activity(300.0, 0.35, 1000.0));
  const double hardGly = 2000.0 - hard.State().glycogenKcal;
  const double easyGly = 2000.0 - easy.State().glycogenKcal;
  const double hardFat = 90000.0 - hard.State().fatReserveKcal;
  const double easyFat = 90000.0 - easy.State().fatReserveKcal;
  CHECK(hardGly > 1.5 * easyGly);
  CHECK(easyFat > hardFat);
  // Hard work without food empties glycogen within hours and caps performance ("bonk").
  EnergySystem bonk;
  Run(bonk, 9.0, Activity(650.0, 0.8, 1000.0));
  CHECK(bonk.State().glycogenKcal < 200.0);
  CHECK(bonk.PerformanceMultiplier() < 0.75);
  bonk.Eat(800.0);
  Run(bonk, 3.0, Activity(110.0, 0.1, 1000.0));
  CHECK(bonk.State().glycogenKcal > 250.0);
}

TEST_CASE("Energy: appetite falls with altitude and refuses food beyond it") {
  CHECK_NEAR(EnergySystem::AppetiteFactor01(1000.0), 1.0, 1e-9);
  CHECK(EnergySystem::AppetiteFactor01(6500.0) < 0.75);
  EnergySystem e;
  Run(e, 0.1, Activity(110.0, 0.1, 7000.0));
  e.Eat(5000.0);
  CHECK(e.State().lastMealAcceptedKcal < 2500.0);
  CHECK(e.RemainingAppetiteKcal() < 1.0);
}

TEST_CASE("Energy: fatigue builds with exertion and sleep restores it, less so at altitude") {
  EnergySystem summitDay;
  for (int leg = 0; leg < 4; ++leg) {  // a fed and watered 12 h summit day
    summitDay.Eat(500.0);
    summitDay.Drink(0.75);
    Run(summitDay, 3.0, Activity(400.0, 0.65, 8000.0));
  }
  CHECK_RANGE(summitDay.State().fatigue01, 0.5, 0.95);
  EnergySystem trek;
  trek.Eat(2000.0);
  Run(trek, 8.0, Activity(400.0, 0.5, 3000.0));
  CHECK_RANGE(trek.State().fatigue01, 0.12, 0.4);

  EnergySystem low = summitDay;
  EnergySystem high = summitDay;
  const double before = summitDay.State().fatigue01;
  Run(low, 9.0, Activity(85.0, 0.05, 1000.0, true));
  Run(high, 9.0, Activity(85.0, 0.05, 6500.0, true));
  CHECK(low.State().fatigue01 < 0.4 * before);
  CHECK(high.State().fatigue01 > low.State().fatigue01);
  CHECK(high.State().lastSleepQuality01 < low.State().lastSleepQuality01);
  CHECK(EnergySystem::SleepQuality01(6000.0) < 0.8);
  CHECK_NEAR(EnergySystem::SleepQuality01(500.0), 1.0, 1e-9);
}

TEST_CASE("Energy: short-term stamina drains in minutes above VO2max and recovers at rest") {
  EnergySystem e;
  Run(e, 2.0 / 60.0, Activity(900.0, 1.2, 1000.0));
  CHECK(e.State().shortTermStamina01 < 0.4);
  Run(e, 3.0 / 60.0, Activity(110.0, 0.1, 1000.0));
  CHECK(e.State().shortTermStamina01 > 0.95);
}

TEST_CASE("Energy: two-month expedition costs 5-10 kg (negative balance above ~5,000 m)") {
  EnergySystem e;
  for (int day = 0; day < 60; ++day) {
    // Approach trek for 10 days, then base camp at 5,300 m with load carries and rotations higher.
    const double camp = day < 10 ? 2800.0 + 250.0 * day : 5300.0;
    const bool workDay = day < 10 || day % 3 != 0;
    const double workAlt = day < 10 ? camp : 5300.0 + 300.0 * (day % 6);
    for (int meal = 0; meal < 3; ++meal) {
      e.Eat(1200.0);
      if (meal == 0) {
        if (workDay) {
          Run(e, 6.0, Activity(500.0, 0.55, workAlt));
          Run(e, 2.0, Activity(120.0, 0.1, camp));
        } else {
          Run(e, 8.0, Activity(120.0, 0.1, camp));
        }
      } else if (meal == 1) {
        Run(e, 6.0, Activity(120.0, 0.1, camp));
      } else {
        Run(e, 10.0, Activity(85.0, 0.05, camp, true));
      }
      e.Drink(1.5);
    }
  }
  CHECK_RANGE(e.State().bodyMassLostKg, 5.0, 10.0);
  CHECK(e.State().fatReserveKcal < 90000.0 - 30000.0);
}
