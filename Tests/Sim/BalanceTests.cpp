#include "Sim/Balance.h"
#include "TestFramework.h"

using namespace summit::sim;

namespace balance_tests {

std::vector<CargoItem> BalancedPack(double kg) { return {{"pack", kg, Vec3(0.0, -0.2, 0.0), true}}; }

// A heavy jerry-can style container hung off the right side of the frame.
std::vector<CargoItem> LopsidedLoad() { return {{"container", 45.0, Vec3(0.5, -0.15, 0.0), true}}; }

struct RunResult {
  bool fell = false;
  double minStability = 1.0;
  bool stumbled = false;
  BalanceState last;
};

RunResult Run(const std::vector<CargoItem>& cargo, const BalanceInputs& in, double dt, double seconds,
              std::uint64_t seed = 7) {
  BalanceModel model(BodyGeometry{}, seed);
  model.SetCargo(cargo);
  RunResult r;
  const int frames = static_cast<int>(seconds / dt + 0.5);
  for (int i = 0; i < frames; ++i) {
    model.Step(dt, in);
    r.minStability = std::min(r.minStability, model.State().stability01);
    r.stumbled = r.stumbled || model.State().stumbling;
    if (model.State().fallen) {
      r.fell = true;
      break;
    }
  }
  r.last = model.State();
  return r;
}

BalanceInputs Walking(double speed = 1.0) {
  BalanceInputs in;
  in.speedMS = speed;
  in.terrain = TerrainType::Rock;
  return in;
}

}  // namespace balance_tests

using namespace balance_tests;

TEST_CASE("Balance: combined CoM and cargo bias") {
  BalanceModel model;
  CHECK_NEAR(model.CombinedCoM().z, model.Body().comHeightM, 1e-12);
  CHECK_NEAR(model.CargoLateralBiasRad(), 0.0, 1e-12);

  model.SetCargo(BalancedPack(20.0));
  CHECK_NEAR(model.CargoMassKg(), 20.0, 1e-12);
  CHECK_NEAR(model.CargoLateralBiasRad(), 0.0, 1e-12);
  CHECK(model.CombinedCoM().y < 0.0);                       // pack pulls the CoM backward
  CHECK(model.CombinedCoM().z > model.Body().comHeightM);   // and up

  model.SetCargo(LopsidedLoad());
  CHECK(model.CargoLateralBiasRad() > 0.1);   // load on the right
  std::vector<CargoItem> left = LopsidedLoad();
  left[0].offsetM.x = -0.45;
  model.SetCargo(left);
  CHECK(model.CargoLateralBiasRad() < -0.1);

  // Two equal items on either side cancel.
  model.SetCargo({{"l", 10.0, Vec3(-0.3, -0.2, 0.0), true}, {"r", 10.0, Vec3(0.3, -0.2, 0.0), true}});
  CHECK_NEAR(model.CargoLateralBiasRad(), 0.0, 1e-12);
}

TEST_CASE("Balance: balanced load on flat ground is stable") {
  const RunResult r = Run(BalancedPack(25.0), Walking(1.2), 1.0 / 60.0, 20.0);
  CHECK(!r.fell);
  CHECK(r.last.stability01 > 0.6);
  CHECK(!r.last.stumbling);
}

TEST_CASE("Balance: lopsided load on a cross slope falls without input, holds with counter-lean") {
  BalanceInputs in = Walking(1.0);
  in.slopeAcrossDeg = 20.0;   // ground falls away to the right, same side as the load
  int fallsNoInput = 0;
  int fallsLean = 0;
  for (std::uint64_t seed = 1; seed <= 5; ++seed) {
    fallsNoInput += Run(LopsidedLoad(), in, 1.0 / 60.0, 20.0, seed).fell ? 1 : 0;
    BalanceInputs lean = in;
    lean.leanInput = -0.4;   // lean left, against load and slope
    fallsLean += Run(LopsidedLoad(), lean, 1.0 / 60.0, 20.0, seed).fell ? 1 : 0;
  }
  CHECK(fallsNoInput == 5);
  CHECK(fallsLean == 0);
  // The same lopsided load is far less stable than a balanced one of equal mass.
  const RunResult balanced = Run(BalancedPack(45.0), in, 1.0 / 60.0, 20.0);
  CHECK(!balanced.fell);
}

TEST_CASE("Balance: over-leaning tips you over the other way") {
  BalanceInputs in = Walking(1.0);
  in.leanInput = -0.8;
  CHECK(Run(BalancedPack(20.0), in, 1.0 / 60.0, 10.0).fell);
}

TEST_CASE("Balance: bracing and poles stabilise") {
  BalanceInputs in = Walking(1.0);
  in.slopeAcrossDeg = 20.0;
  BalanceInputs braced = in;
  braced.bracing = true;
  CHECK(!Run(LopsidedLoad(), braced, 1.0 / 60.0, 20.0).fell);
  BalanceInputs poles = in;
  poles.usingPoles = true;
  CHECK(!Run(LopsidedLoad(), poles, 1.0 / 60.0, 20.0).fell);

  BalanceModel model;
  model.SetCargo(LopsidedLoad());
  CHECK(model.SafeSpeedMS(braced) <= 0.2);
}

TEST_CASE("Balance: ice without crampons is slippery, crampons fix it") {
  BalanceInputs in = Walking(0.8);
  in.terrain = TerrainType::GlacierIce;
  in.slopeAlongDeg = 25.0;
  in.temperatureC = -10.0;
  BalanceModel bare;
  bare.Step(0.1, in);
  in.crampons = true;
  BalanceModel spiked;
  spiked.Step(0.1, in);
  CHECK(bare.State().slipRisk01 > 0.8);
  CHECK(spiked.State().slipRisk01 < 0.5);
  CHECK(bare.State().slipRisk01 > spiked.State().slipRisk01 + 0.4);

  // Walking up bare ice at 25° ends in a fall; with crampons it does not.
  in.crampons = false;
  CHECK(Run(BalancedPack(15.0), in, 1.0 / 60.0, 20.0).fell);
  in.crampons = true;
  CHECK(!Run(BalancedPack(15.0), in, 1.0 / 60.0, 20.0).fell);
}

TEST_CASE("Balance: wind pushes the climber") {
  BalanceInputs calm = Walking(0.0);
  BalanceModel model;
  model.SetCargo(BalancedPack(25.0));
  for (int i = 0; i < 120; ++i) model.Step(1.0 / 60.0, calm);
  CHECK_NEAR(model.State().lateralTiltRad, 0.0, 1e-6);
  // A sudden 20 m/s side wind pushes the climber to the right before the controller adapts.
  BalanceInputs windy = calm;
  windy.lateralWindMS = 20.0;
  for (int i = 0; i < 18; ++i) model.Step(1.0 / 60.0, windy);
  CHECK(model.State().lateralVelRadS > 0.0);
  CHECK(model.State().lateralTiltRad > 0.0);
  // Steady wind: the climber ends up leaning into it, with less margin than in calm air.
  BalanceInputs steady = calm;
  steady.lateralWindMS = 12.0;
  const RunResult w = Run(BalancedPack(25.0), steady, 1.0 / 60.0, 10.0);
  const RunResult c = Run(BalancedPack(25.0), calm, 1.0 / 60.0, 10.0);
  CHECK(w.last.lateralTiltRad < -0.005);
  CHECK(w.last.stability01 < c.last.stability01);
  // Storm-force gusts knock you over.
  BalanceInputs storm = Walking(1.0);
  storm.lateralWindMS = 30.0;
  storm.gustiness01 = 0.5;
  CHECK(Run(BalancedPack(25.0), storm, 1.0 / 60.0, 20.0).fell);
}

TEST_CASE("Balance: unstrapped cargo and fatigue reduce stability") {
  BalanceInputs in = Walking(1.0);
  in.roughness01 = 0.5;
  std::vector<CargoItem> loose = BalancedPack(30.0);
  loose[0].strapped = false;
  double strapped = 0.0, unstrapped = 0.0;
  for (std::uint64_t seed = 1; seed <= 5; ++seed) {
    strapped += Run(BalancedPack(30.0), in, 1.0 / 60.0, 15.0, seed).minStability;
    unstrapped += Run(loose, in, 1.0 / 60.0, 15.0, seed).minStability;
  }
  CHECK(unstrapped < strapped);

  BalanceInputs tired = in;
  tired.fatigue01 = 1.0;
  tired.cognitiveImpairment01 = 1.0;
  int freshFalls = 0, tiredFalls = 0;
  for (std::uint64_t seed = 1; seed <= 5; ++seed) {
    freshFalls += Run(BalancedPack(30.0), in, 1.0 / 60.0, 30.0, seed).fell ? 1 : 0;
    tiredFalls += Run(BalancedPack(30.0), tired, 1.0 / 60.0, 30.0, seed).fell ? 1 : 0;
  }
  CHECK(tiredFalls > freshFalls);
}

TEST_CASE("Balance: deterministic with the same seed") {
  BalanceInputs in = Walking(1.2);
  in.roughness01 = 0.7;
  in.lateralWindMS = 10.0;
  in.gustiness01 = 0.8;
  in.slopeAcrossDeg = 10.0;
  const RunResult a = Run(BalancedPack(25.0), in, 1.0 / 60.0, 15.0, 1234);
  const RunResult b = Run(BalancedPack(25.0), in, 1.0 / 60.0, 15.0, 1234);
  const RunResult c = Run(BalancedPack(25.0), in, 1.0 / 60.0, 15.0, 999);
  CHECK(a.last.lateralTiltRad == b.last.lateralTiltRad);
  CHECK(a.last.forwardTiltRad == b.last.forwardTiltRad);
  CHECK(a.last.stability01 == b.last.stability01);
  CHECK(a.last.lateralTiltRad != c.last.lateralTiltRad);
}

TEST_CASE("Balance: frame-rate independent (1/15 to 1/120 s)") {
  BalanceInputs in = Walking(1.0);
  in.roughness01 = 0.5;
  in.lateralWindMS = 12.0;
  in.gustiness01 = 0.5;
  in.slopeAcrossDeg = 15.0;
  const RunResult ref = Run(BalancedPack(30.0), in, 1.0 / 120.0, 10.0);
  for (double dt : {1.0 / 15.0, 1.0 / 30.0, 1.0 / 60.0, 1.0 / 50.0, 1.0 / 144.0}) {
    const RunResult r = Run(BalancedPack(30.0), in, dt, 10.0);
    CHECK(r.fell == ref.fell);
    CHECK_NEAR(r.last.lateralTiltRad, ref.last.lateralTiltRad, 0.01);
    CHECK_NEAR(r.last.forwardTiltRad, ref.last.forwardTiltRad, 0.01);
  }
  // Whole multiples of the internal sub-step are bit-identical.
  const RunResult r30 = Run(BalancedPack(30.0), in, 1.0 / 30.0, 10.0);
  CHECK(r30.last.lateralTiltRad == ref.last.lateralTiltRad);
}

TEST_CASE("Balance: recover resets a fall") {
  BalanceInputs in = Walking(1.0);
  in.leanInput = -1.0;
  BalanceModel model;
  model.SetCargo(BalancedPack(20.0));
  for (int i = 0; i < 600 && !model.State().fallen; ++i) model.Step(1.0 / 60.0, in);
  CHECK(model.State().fallen);
  CHECK_NEAR(model.State().stability01, 0.0, 1e-12);
  CHECK_NEAR(model.SafeSpeedMS(Walking()), 0.0, 1e-12);
  model.Recover();
  CHECK(!model.State().fallen);
  CHECK(!model.State().stumbling);
  in.leanInput = 0.0;
  for (int i = 0; i < 300; ++i) model.Step(1.0 / 60.0, in);
  CHECK(!model.State().fallen);
}

TEST_CASE("Balance: safe speed drops with load, slope, footing, wind and fatigue") {
  BalanceModel model;
  const BalanceInputs flat = Walking(1.2);
  model.SetCargo(BalancedPack(10.0));
  const double light = model.SafeSpeedMS(flat);
  model.SetCargo(BalancedPack(30.0));
  const double medium = model.SafeSpeedMS(flat);
  model.SetCargo(BalancedPack(45.0));
  const double heavy = model.SafeSpeedMS(flat);
  CHECK_RANGE(light, 1.2, 1.6);   // Tobler: ~1.4 m/s on the flat
  CHECK(medium < light);
  CHECK(heavy < medium);

  model.SetCargo(BalancedPack(10.0));
  BalanceInputs steep = flat;
  steep.slopeAlongDeg = 25.0;
  CHECK(model.SafeSpeedMS(steep) < 0.6 * light);
  BalanceInputs ice = flat;
  ice.terrain = TerrainType::GlacierIce;
  ice.slopeAlongDeg = 15.0;
  BalanceInputs iceCrampons = ice;
  iceCrampons.crampons = true;
  CHECK(model.SafeSpeedMS(ice) < model.SafeSpeedMS(iceCrampons));
  BalanceInputs windy = flat;
  windy.lateralWindMS = 20.0;
  CHECK(model.SafeSpeedMS(windy) < 0.8 * light);
  BalanceInputs tired = flat;
  tired.fatigue01 = 0.8;
  CHECK(model.SafeSpeedMS(tired) < light);
}

TEST_CASE("Load penalty follows Knapik 2004 thresholds") {
  CHECK_NEAR(LoadPenalty01(10.0, 75.0), 0.0, 1e-12);    // 13 %
  CHECK_NEAR(LoadPenalty01(15.0, 75.0), 0.0, 1e-12);    // 20 %
  CHECK_RANGE(LoadPenalty01(22.5, 75.0), 0.05, 0.35);   // 30 %
  CHECK(LoadPenalty01(30.0, 75.0) > 2.0 * LoadPenalty01(22.5, 75.0));   // 40 %: steep rise
  CHECK(LoadPenalty01(48.75, 75.0) > 0.95);             // 65 %
  double prev = -1.0;
  for (double kg = 0.0; kg <= 60.0; kg += 2.0) {
    const double p = LoadPenalty01(kg, 75.0);
    CHECK(p >= prev);
    prev = p;
  }
}
