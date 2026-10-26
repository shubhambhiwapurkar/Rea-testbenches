// Wind chill, core/skin heat balance, extremities and frostbite.

#include <cmath>

#include "Sim/Atmosphere.h"
#include "Sim/Thermal.h"
#include "TestFramework.h"

using namespace summit::sim;

namespace {

void Run(ThermalModel& m, double minutes, const ThermalInputs& in, double dt = 60.0) {
  for (double t = 0.0; t < minutes * 60.0; t += dt) m.Step(dt, in);
}

// Down suit (~7 clo), expedition mitts, 8000 m boots, at -30 °C and 15 m/s wind high on a peak.
ThermalInputs SummitNight(double metabolicHeatW) {
  ThermalInputs in;
  in.env.airTempC = -30.0;
  in.env.windMS = 15.0;
  in.env.relativeHumidity01 = 0.3;
  in.clothing.torsoClo = 7.0;
  in.clothing.handsClo = 3.0;
  in.clothing.feetClo = 3.5;
  in.clothing.faceCoverage01 = 0.85;
  in.clothing.windproof01 = 0.9;
  in.pressureKPa = BarometricPressureKPa(8000.0);
  in.spo2 = 0.6;
  in.bodyMassKg = 75.0;
  in.metabolicHeatW = metabolicHeatW;
  in.minuteVentilationLMin = metabolicHeatW > 200.0 ? 70.0 : 20.0;
  return in;
}

}  // namespace

TEST_CASE("Thermal: JAG/TI wind chill (-20 C at 30 km/h = -32.6 C)") {
  CHECK_NEAR(WindChillC(-20.0, 30.0), -32.6, 0.1);
  CHECK_NEAR(WindChillC(-30.0, 54.0), -49.6, 0.3);
  CHECK_NEAR(WindChillC(15.0, 30.0), 15.0, 1e-12);  // above 10 °C: not defined
  CHECK_NEAR(WindChillC(-10.0, 3.0), -10.0, 1e-12); // calm
  ThermalEnvironment env;
  env.airTempC = -20.0;
  env.windMS = 30.0 / 3.6;
  CHECK_NEAR(ThermalModel::EffectiveExposureTempC(env), -32.6, 0.1);
  env.inTent = true;
  CHECK(ThermalModel::EffectiveExposureTempC(env) > -20.0);
}

TEST_CASE("Thermal: hypothermia stages and frostbite grades") {
  ThermalModel m;
  CHECK(m.Hypothermia() == HypothermiaStage::None);
  m.MutableState().coreTempC = 34.0;
  CHECK(m.Hypothermia() == HypothermiaStage::Mild);
  m.MutableState().coreTempC = 30.0;
  CHECK(m.Hypothermia() == HypothermiaStage::Moderate);
  m.MutableState().coreTempC = 26.0;
  CHECK(m.Hypothermia() == HypothermiaStage::Severe);
  CHECK(m.Frostbite(BodyPart::Hands) == FrostbiteGrade::None);
  m.MutableState().frostbiteDamage01[0] = 0.05;
  CHECK(m.Frostbite(BodyPart::Hands) == FrostbiteGrade::Frostnip);
  m.MutableState().frostbiteDamage01[0] = 0.3;
  CHECK(m.Frostbite(BodyPart::Hands) == FrostbiteGrade::Superficial);
  m.MutableState().frostbiteDamage01[0] = 0.7;
  CHECK(m.Frostbite(BodyPart::Hands) == FrostbiteGrade::Deep);
}

TEST_CASE("Thermal: well-dressed climber moving at -30 C / 15 m/s stays normothermic") {
  ThermalModel m;
  const ThermalInputs in = SummitNight(0.8 * 400.0);
  Run(m, 6.0 * 60.0, in);
  CHECK_RANGE(m.State().coreTempC, 36.0, 37.8);
  CHECK(m.Hypothermia() == HypothermiaStage::None);
  CHECK(m.Frostbite(BodyPart::Hands) == FrostbiteGrade::None);
  CHECK(m.Frostbite(BodyPart::Feet) == FrostbiteGrade::None);
  CHECK(m.State().respiratoryHeatLossW > 50.0);  // breathing thin, dry, cold air is a big heat sink
}

TEST_CASE("Thermal: sitting still unsheltered for many hours drifts into hypothermia") {
  ThermalModel m;
  const ThermalInputs in = SummitNight(110.0);
  Run(m, 2.0 * 60.0, in);
  CHECK(m.State().coreTempC > 36.0);  // a slow drift, not a crash
  CHECK(m.State().shivering01 > 0.05);
  Run(m, 14.0 * 60.0, in);
  CHECK(m.State().coreTempC < 35.0);
  CHECK(m.State().coreTempC > 28.0);
  CHECK(m.Hypothermia() != HypothermiaStage::None);
}

TEST_CASE("Thermal: thin gloves at -30 C and 15 m/s give frostbite within ~30-60 min") {
  ThermalInputs in = SummitNight(0.8 * 350.0);
  in.clothing.torsoClo = 4.0;
  in.clothing.windproof01 = 0.6;
  in.clothing.handsClo = 0.6;
  in.pressureKPa = BarometricPressureKPa(6000.0);
  in.spo2 = 0.75;
  in.minuteVentilationLMin = 60.0;
  ThermalModel m;
  Run(m, 15.0, in);
  CHECK(m.Frostbite(BodyPart::Hands) == FrostbiteGrade::None);
  double onsetMin = -1.0;
  for (int minute = 16; minute <= 120; ++minute) {
    m.Step(60.0, in);
    if (onsetMin < 0.0 && m.Frostbite(BodyPart::Hands) >= FrostbiteGrade::Superficial) onsetMin = minute;
  }
  CHECK_RANGE(onsetMin, 25.0, 75.0);
  CHECK(m.Frostbite(BodyPart::Feet) == FrostbiteGrade::None);  // boots are fine
}

TEST_CASE("Thermal: exposed face freezes within minutes") {
  ThermalInputs in = SummitNight(0.8 * 350.0);
  in.clothing.faceCoverage01 = 0.0;
  ThermalModel m;
  Run(m, 10.0, in);
  CHECK(m.Frostbite(BodyPart::Face) >= FrostbiteGrade::Frostnip);
  ThermalInputs covered = SummitNight(0.8 * 350.0);
  ThermalModel c;
  Run(c, 60.0, covered);
  CHECK(c.Frostbite(BodyPart::Face) == FrostbiteGrade::None);
}

TEST_CASE("Thermal: tent and sleeping bag rated at the air temperature keep a sleeper stable") {
  ThermalInputs in;
  in.env.airTempC = -25.0;
  in.env.windMS = 12.0;
  in.env.inTent = true;
  in.env.inSleepingBag = true;
  in.env.sleepingBagComfortC = -25.0;
  in.clothing.torsoClo = 2.0;
  in.metabolicHeatW = 83.0;  // 0.95 MET
  in.minuteVentilationLMin = 10.0;
  in.pressureKPa = BarometricPressureKPa(6400.0);
  in.spo2 = 0.75;
  ThermalModel m;
  Run(m, 10.0 * 60.0, in);
  CHECK_RANGE(m.State().coreTempC, 36.4, 37.5);
  CHECK(m.State().shivering01 < 0.1);
  CHECK(m.Frostbite(BodyPart::Feet) == FrostbiteGrade::None);

  // A summer bag in the same tent is cold.
  ThermalInputs summer = in;
  summer.env.sleepingBagComfortC = 5.0;
  ThermalModel s;
  Run(s, 10.0 * 60.0, summer);
  CHECK(s.State().meanSkinTempC < m.State().meanSkinTempC - 2.0);
  CHECK(s.State().shivering01 > m.State().shivering01);
}

TEST_CASE("Thermal: wet insulation and hypoxia make the cold worse") {
  ThermalInputs dry = SummitNight(110.0);
  ThermalInputs wet = dry;
  wet.clothing.wetness01 = 0.8;
  ThermalModel a;
  ThermalModel b;
  Run(a, 4.0 * 60.0, dry);
  Run(b, 4.0 * 60.0, wet);
  CHECK(b.State().coreTempC < a.State().coreTempC - 0.3);

  // Hypoxia blunts shivering.
  ThermalInputs cold = SummitNight(110.0);
  cold.spo2 = 0.97;
  ThermalInputs hypoxic = cold;
  hypoxic.spo2 = 0.55;
  ThermalModel c;
  ThermalModel d;
  Run(c, 60.0, cold);
  Run(d, 60.0, hypoxic);
  CHECK(d.State().shivering01 < c.State().shivering01);
  // Glycogen depletion blunts it too.
  ThermalInputs empty = cold;
  empty.glycogenFraction01 = 0.0;
  ThermalModel e;
  Run(e, 60.0, empty);
  CHECK(e.State().shivering01 < c.State().shivering01);
}

TEST_CASE("Thermal: sun on snow heats a lightly dressed climber (Western Cwm)") {
  ThermalInputs in;
  in.env.airTempC = -5.0;
  in.env.windMS = 1.0;
  in.clothing.torsoClo = 1.0;
  in.metabolicHeatW = 0.8 * 450.0;
  in.minuteVentilationLMin = 60.0;
  in.pressureKPa = BarometricPressureKPa(6400.0);
  in.spo2 = 0.8;
  ThermalInputs sunny = in;
  sunny.env.solarWm2 = 1100.0;
  ThermalModel shade;
  ThermalModel sun;
  Run(shade, 120.0, in);
  Run(sun, 120.0, sunny);
  CHECK(sun.State().coreTempC > shade.State().coreTempC);
  CHECK(sun.State().meanSkinTempC > shade.State().meanSkinTempC + 0.5);
}

TEST_CASE("Thermal: numerically stable with 60 s steps") {
  ThermalInputs in = SummitNight(0.8 * 300.0);
  ThermalModel fine;
  ThermalModel coarse;
  Run(fine, 120.0, in, 5.0);
  Run(coarse, 120.0, in, 60.0);
  CHECK_NEAR(coarse.State().coreTempC, fine.State().coreTempC, 0.05);
  CHECK_NEAR(coarse.State().extremityTempC[0], fine.State().extremityTempC[0], 0.5);

  // Naked in a -40 °C, 30 m/s storm with a single huge step: no blow-up, just cold.
  ThermalInputs brutal;
  brutal.env.airTempC = -40.0;
  brutal.env.windMS = 30.0;
  brutal.clothing.torsoClo = 0.0;
  brutal.clothing.handsClo = 0.0;
  brutal.clothing.feetClo = 0.0;
  brutal.clothing.faceCoverage01 = 0.0;
  brutal.metabolicHeatW = 100.0;
  ThermalModel b;
  b.Step(3600.0, brutal);
  CHECK(std::isfinite(b.State().coreTempC));
  CHECK(b.State().coreTempC < 37.0);
  CHECK(b.State().coreTempC > 20.0);
  CHECK(b.State().extremityTempC[0] > -41.0);
  CHECK(b.Frostbite(BodyPart::Hands) == FrostbiteGrade::Deep);
  CHECK(b.LastNetHeatLossW() > 0.0);
}
