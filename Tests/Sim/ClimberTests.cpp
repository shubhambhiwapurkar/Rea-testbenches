// End-to-end climber scenarios against expedition experience.

#include <cmath>
#include <vector>

#include "Sim/Atmosphere.h"
#include "Sim/Climber.h"
#include "TestFramework.h"

using namespace summit::sim;

namespace {

// Simple Khumbu-like weather: ~6.5 °C/km lapse, sunny days, calm-ish.
WeatherSample WeatherAt(double altitudeM, bool day) {
  WeatherSample w;
  w.temperatureC = 18.0 - 6.5 * altitudeM / 1000.0 + (day ? 3.0 : -5.0);
  w.windSpeedMS = 3.0 + altitudeM / 2000.0;
  w.windGustMS = w.windSpeedMS * 1.4;
  w.relativeHumidity01 = 0.5;
  w.solarWm2 = day ? 600.0 : 0.0;
  return w;
}

ClimberGear TrekkingGear() {
  ClimberGear g;
  g.clothing.torsoClo = 2.5;
  g.clothing.handsClo = 1.5;
  g.clothing.feetClo = 1.8;
  g.packMassKg = 8.0;
  return g;
}

ClimberGear SummitGear() {
  ClimberGear g;
  g.clothing.torsoClo = 7.0;
  g.clothing.handsClo = 3.0;
  g.clothing.feetClo = 3.5;
  g.clothing.faceCoverage01 = 0.85;
  g.clothing.windproof01 = 0.9;
  g.crampons = true;
  g.packMassKg = 8.0;
  return g;
}

void Rotations(Climber& c) {
  auto& s = c.Physio().MutableState();
  s.acclimatizedAltitudeM = Physiology::kAcclimatizationCeilingM;
  s.ventilatoryAcclimatization = 0.95;
  s.hematologicAcclimatization = 0.8;
  s.hemoglobinGdl = 17.0;
}

struct DayPlan {
  double sleepM;
  double highM;
};

// Walk 7 h from the previous camp via the day's high point, then evening and night at camp.
// Returns the peak AMS score seen.
double LiveDay(Climber& c, double& altitudeM, const DayPlan& d, const ClimberGear& gear) {
  double peak = 0.0;
  ClimberEnvironment env;
  env.terrain = TerrainType::Trail;
  env.exposure01 = 0.5;
  const double start = altitudeM;
  c.Energy().Eat(1000.0);
  c.Energy().Drink(1.0);
  for (int minute = 0; minute < 7 * 60; ++minute) {
    const double f = (minute + 1) / 420.0;
    const double target = f < 0.7 ? start + (d.highM - start) * f / 0.7 : d.highM + (d.sleepM - d.highM) * (f - 0.7) / 0.3;
    ClimberAction a;
    a.activity = Activity::Walking;
    a.speedMS = 0.7;
    a.gradePercent = (target - altitudeM) / (a.speedMS * 60.0) * 100.0;
    env.altitudeM = altitudeM;
    env.weather = WeatherAt(altitudeM, true);
    c.Step(60.0, env, a, gear);
    altitudeM = target;
    if (minute == 180) {
      c.Energy().Eat(800.0);
      c.Energy().Drink(1.0);
    }
    peak = std::fmax(peak, c.Physio().State().amsScore);
  }
  altitudeM = d.sleepM;
  env.altitudeM = altitudeM;
  env.inTent = true;
  env.inSleepingBag = false;
  ClimberAction rest;
  c.Energy().Eat(1200.0);
  c.Energy().Drink(1.5);
  for (int h = 0; h < 17; ++h) {
    const bool night = h >= 4;
    env.weather = WeatherAt(altitudeM, h < 2);
    env.inSleepingBag = night;
    rest.activity = night ? Activity::Sleeping : Activity::Resting;
    c.Step(3600.0, env, rest, gear);
    peak = std::fmax(peak, c.Physio().State().amsScore);
  }
  return peak;
}

}  // namespace

TEST_CASE("Climber: resting and sleeping metabolic rates (Compendium METs)") {
  Climber c;
  ClimberEnvironment env;
  env.weather = WeatherAt(0.0, true);
  ClimberAction a;
  c.Step(60.0, env, a, ClimberGear{});
  CHECK_NEAR(c.LastMetabolicRateW(), 1.3 * 1.1725 * 75.0, 1.0);
  a.activity = Activity::Sleeping;
  c.Step(60.0, env, a, ClimberGear{});
  CHECK_NEAR(c.LastMetabolicRateW(), 0.95 * 1.1725 * 75.0, 1.0);
  a.activity = Activity::Walking;
  a.speedMS = 1.34;
  ClimberGear noLoad;
  noLoad.packMassKg = 0.0;
  noLoad.clothing.torsoClo = 1.5;  // trekking layers: no hobbling penalty, so this is pure Pandolf
  c.Step(60.0, env, a, noLoad);
  CHECK_NEAR(c.LastMetabolicRateW(), PandolfMetabolicRateW(75.0, 0.0, 1.34, 0.0, 1.1), 1.0);
  CHECK_RANGE(c.LastExertion01(), 0.15, 0.35);
}

TEST_CASE("Climber: large steps are sub-stepped and match small steps") {
  Climber a;
  Climber b;
  ClimberEnvironment env;
  env.altitudeM = 4000.0;
  env.weather = WeatherAt(4000.0, true);
  ClimberAction walk;
  walk.activity = Activity::Walking;
  walk.speedMS = 0.8;
  walk.gradePercent = 10.0;
  const ClimberGear gear = TrekkingGear();
  a.Step(3600.0, env, walk, gear);
  for (int i = 0; i < 60; ++i) b.Step(60.0, env, walk, gear);
  CHECK_NEAR(a.Physio().State().spo2, b.Physio().State().spo2, 1e-9);
  CHECK_NEAR(a.Thermal().State().coreTempC, b.Thermal().State().coreTempC, 1e-9);
  CHECK_NEAR(a.Energy().State().glycogenKcal, b.Energy().State().glycogenKcal, 1e-6);
}

TEST_CASE("Climber: 14-day Khumbu itinerary acclimatizes; flying to 5,300 m causes AMS") {
  // Kathmandu -> Lukla flight, standard WMS-compliant approach, then rest days at base camp.
  const std::vector<DayPlan> plan = {
      {2610, 2860}, {3440, 3440}, {3440, 3880}, {3820, 3870}, {4410, 4410}, {4410, 5080}, {4940, 4940},
      {5164, 5364}, {5164, 5644}, {5364, 5364}, {5364, 5364}, {5364, 5364}, {5364, 5364}, {5364, 5364}};
  Climber trekker;
  const ClimberGear gear = TrekkingGear();
  double altitude = 2860.0;
  double peakAms = 0.0;
  for (const DayPlan& d : plan) peakAms = std::fmax(peakAms, LiveDay(trekker, altitude, d, gear));
  const auto& s = trekker.Physio().State();
  CHECK(!trekker.IsDead());
  CHECK(peakAms < 3.0);
  CHECK(s.amsScore < 1.0);
  CHECK(s.ventilatoryAcclimatization > 0.85);
  CHECK(s.acclimatizedAltitudeM > 5000.0);
  CHECK(s.hapeSeverity01 < 0.05);
  CHECK(s.haceSeverity01 < 0.05);
  CHECK_RANGE(s.spo2, 0.72, 0.88);  // asleep/resting at 5,364 m

  // Same person dropped straight at 5,300 m.
  Climber flown;
  ClimberEnvironment env;
  env.altitudeM = 5300.0;
  env.inTent = true;
  double flownPeak = 0.0;
  for (int h = 0; h < 36; ++h) {
    const bool night = h >= 12 && h < 24;
    env.weather = WeatherAt(5300.0, !night);
    env.inSleepingBag = night;
    ClimberAction a;
    a.activity = night ? Activity::Sleeping : Activity::Resting;
    flown.Step(3600.0, env, a, gear);
    flownPeak = std::fmax(flownPeak, flown.Physio().State().amsScore);
  }
  CHECK(flownPeak >= 3.0);
  CHECK(flown.Physio().State().spo2 < s.spo2);
  bool reportsAms = false;
  for (const auto& r : flown.ActiveConditions()) reportsAms = reportsAms || (r.condition == Condition::AMS && r.severity01 > 0.3);
  CHECK(reportsAms);
}

TEST_CASE("Climber: on the summit 3 L/min O2 raises SpO2 and speed") {
  Climber air;
  Climber o2;
  Rotations(air);
  Rotations(o2);
  ClimberEnvironment env;
  env.altitudeM = 8848.0;
  env.terrain = TerrainType::HardSnow;
  env.weather.temperatureC = -26.0;
  env.weather.windSpeedMS = 8.0;
  env.weather.windGustMS = 10.0;
  ClimberAction a;
  a.activity = Activity::Walking;
  a.gradePercent = 25.0;
  ClimberGear airGear = SummitGear();
  ClimberGear o2Gear = SummitGear();
  o2Gear.oxygen = OxygenSupply{true, 3.0};
  for (int i = 0; i < 30; ++i) {
    a.speedMS = 0.8 * air.MaxSustainableSpeedMS(env, a, airGear);
    air.Step(60.0, env, a, airGear);
    a.speedMS = 0.8 * o2.MaxSustainableSpeedMS(env, a, o2Gear);
    o2.Step(60.0, env, a, o2Gear);
  }
  CHECK(o2.Physio().State().spo2 > air.Physio().State().spo2 + 0.05);
  CHECK(o2.MaxSustainableSpeedMS(env, a, o2Gear) > 1.4 * air.MaxSustainableSpeedMS(env, a, airGear));
  CHECK(o2.Physio().State().equivalentAltitudeM < 8000.0);
}

TEST_CASE("Climber: vertical speeds match expedition experience") {
  const auto verticalMH = [](double speedMS, double grade) {
    return speedMS * std::sin(std::atan(grade / 100.0)) * 3600.0;
  };
  // Acclimatized, no O2, on the summit ridge (Messner/Habeler 1978; typical 50-100 m/h).
  {
    Climber c;
    Rotations(c);
    ClimberEnvironment env;
    env.altitudeM = 8750.0;
    env.terrain = TerrainType::HardSnow;
    ClimberAction a;
    a.activity = Activity::Walking;
    a.gradePercent = 30.0;
    ClimberGear gear = SummitGear();
    a.speedMS = 0.0;
    c.Step(60.0, env, a, gear);
    CHECK_RANGE(verticalMH(c.MaxSustainableSpeedMS(env, a, gear), a.gradePercent), 50.0, 100.0);
    a.activity = Activity::Jumaring;
    CHECK_RANGE(verticalMH(c.MaxSustainableSpeedMS(env, a, gear), a.gradePercent), 40.0, 100.0);
  }
  // A fit trekker on the Namche hill on day 2 (typically 300-400 m/h).
  {
    ClimberProfile p;
    p.physiology.vo2MaxSeaLevelMlKgMin = 45.0;
    Climber c(p);
    ClimberEnvironment env;
    env.altitudeM = 3000.0;
    env.terrain = TerrainType::Trail;
    ClimberAction a;
    a.activity = Activity::Walking;
    a.gradePercent = 15.0;
    const ClimberGear gear = TrekkingGear();
    c.Step(600.0, env, a, gear);
    CHECK_RANGE(verticalMH(c.MaxSustainableSpeedMS(env, a, gear), a.gradePercent), 280.0, 450.0);
    // Deep snow and trail breaking are far slower than the trail.
    const double trail = c.MaxSustainableSpeedMS(env, a, gear);
    env.terrain = TerrainType::DeepSnow;
    a.activity = Activity::BreakingTrail;
    CHECK(c.MaxSustainableSpeedMS(env, a, gear) < 0.6 * trail);
  }
}

TEST_CASE("Climber: sprinting above VO2max drains short-term stamina") {
  Climber c;
  ClimberEnvironment env;
  env.altitudeM = 5000.0;
  env.weather = WeatherAt(5000.0, true);
  ClimberAction a;
  a.activity = Activity::Walking;
  a.speedMS = 2.0;
  a.gradePercent = 20.0;
  c.Step(180.0, env, a, TrekkingGear());
  CHECK_NEAR(c.LastExertion01(), 1.2, 1e-9);
  CHECK(c.Energy().State().shortTermStamina01 < 0.3);
}

TEST_CASE("Climber: snow blindness without goggles in strong sun on snow") {
  Climber bare;
  Climber goggles;
  ClimberEnvironment env;
  env.altitudeM = 5500.0;
  env.terrain = TerrainType::SoftSnow;
  env.weather = WeatherAt(5500.0, true);
  env.weather.solarWm2 = 900.0;
  ClimberAction a;
  ClimberGear noEyewear = TrekkingGear();
  noEyewear.glacierGoggles = false;
  bare.Step(3.0 * 3600.0, env, a, noEyewear);
  goggles.Step(3.0 * 3600.0, env, a, TrekkingGear());
  CHECK(bare.SnowBlindness01() > 0.6);
  CHECK(goggles.SnowBlindness01() < 0.01);
  // Night in a lower tent: the eyes heal.
  env.altitudeM = 4000.0;
  env.weather = WeatherAt(4000.0, false);
  env.inTent = true;
  env.inSleepingBag = true;
  a.activity = Activity::Sleeping;
  bare.Step(36.0 * 3600.0, env, a, noEyewear);
  CHECK(!bare.IsDead());
  CHECK(bare.SnowBlindness01() < 0.3);  // heals in a day or two
}

TEST_CASE("Climber: causes of death and incapacitation") {
  ClimberEnvironment env;
  env.weather = WeatherAt(0.0, true);
  ClimberAction rest;
  {
    Climber c;
    c.Thermal().MutableState().coreTempC = 29.0;
    CHECK(c.IsIncapacitated());
    c.Thermal().MutableState().coreTempC = 23.5;
    c.Step(1.0, env, rest, ClimberGear{});
    CHECK(c.IsDead());
    CHECK(c.Cause() == CauseOfDeath::Hypothermia);
  }
  {
    Climber c;
    c.Physio().MutableState().hapeSeverity01 = 1.0;
    env.altitudeM = 5000.0;
    c.Step(1.0, env, rest, ClimberGear{});
    CHECK(c.Cause() == CauseOfDeath::HAPE);
  }
  {
    Climber c;
    c.Physio().MutableState().haceSeverity01 = 0.75;
    CHECK(c.IsIncapacitated());
    c.Physio().MutableState().haceSeverity01 = 1.0;
    c.Step(1.0, env, rest, ClimberGear{});
    CHECK(c.Cause() == CauseOfDeath::HACE);
  }
  {
    Climber c;
    auto& e = c.Energy().MutableState();
    e.glycogenKcal = 0.0;
    e.fatigue01 = 0.99;
    CHECK(c.IsIncapacitated());
    c.Thermal().MutableState().coreTempC = 32.0;
    c.Step(1.0, env, rest, ClimberGear{});
    CHECK(c.Cause() == CauseOfDeath::Exhaustion);
    const double core = c.Thermal().State().coreTempC;
    c.Step(3600.0, env, rest, ClimberGear{});  // the dead do not change
    CHECK_NEAR(c.Thermal().State().coreTempC, core, 1e-12);
    CHECK(c.MaxSustainableSpeedMS(env, ClimberAction{Activity::Walking, 1.0, 0.0}, ClimberGear{}) == 0.0);
  }
  {
    Climber healthy;
    env.altitudeM = 0.0;
    healthy.Step(600.0, env, rest, ClimberGear{});
    CHECK(!healthy.IsDead());
    CHECK(!healthy.IsIncapacitated());
    bool hypoxic = false;
    for (const auto& r : healthy.ActiveConditions()) hypoxic = hypoxic || r.condition == Condition::Hypoxia;
    CHECK(!hypoxic);
  }
}

TEST_CASE("Climber: an unsheltered night high on the mountain is dangerous") {
  Climber c;
  Rotations(c);
  ClimberEnvironment env;
  env.altitudeM = 8500.0;
  env.terrain = TerrainType::HardSnow;
  env.weather.temperatureC = -32.0;
  env.weather.windSpeedMS = 15.0;
  env.weather.windGustMS = 20.0;
  ClimberAction sit;
  c.Energy().MutableState().glycogenKcal = 300.0;  // after a long summit day
  c.Energy().MutableState().fatigue01 = 0.7;
  c.Step(14.0 * 3600.0, env, sit, SummitGear());
  bool hypothermic = c.IsDead() || c.Thermal().State().coreTempC < 35.0;
  CHECK(hypothermic);
  bool reportsHypothermiaOrDeath = c.IsDead();
  for (const auto& r : c.ActiveConditions()) reportsHypothermiaOrDeath = reportsHypothermiaOrDeath || r.condition == Condition::Hypothermia;
  CHECK(reportsHypothermiaOrDeath);
  CHECK(c.Physio().State().secondsInDeathZone > 0.0);
}

TEST_CASE("Climber: no-O2 summit push from the South Col takes ~8-14 h; skipping food and water fails") {
  // South Col (7,906 m) to summit over ~1.9 km, calm window. Real no-O2 ascents: Messner & Habeler
  // 1978 ~8 h; typical strong climbers 10-14 h.
  const auto push = [](double vo2Max, double massKg, bool fedAndWatered, double& hoursOut) {
    ClimberProfile p;
    p.physiology.vo2MaxSeaLevelMlKgMin = vo2Max;
    p.physiology.bodyMassKg = massKg;
    p.energy.bodyMassKg = massKg;
    Climber c(p);
    Rotations(c);
    ClimberGear gear = SummitGear();
    gear.packMassKg = 6.0;
    ClimberEnvironment env;
    env.altitudeM = 7906.0;
    env.terrain = TerrainType::HardSnow;
    env.weather.temperatureC = -25.0;
    env.weather.windSpeedMS = 8.0;
    env.weather.windGustMS = 11.0;
    ClimberAction a;
    a.activity = Activity::Walking;
    a.gradePercent = (8848.0 - 7906.0) / 1900.0 * 100.0;
    const double verticalFraction = std::sin(std::atan(a.gradePercent / 100.0));
    for (int minute = 0; minute < 15 * 60; ++minute) {
      if (fedAndWatered && minute % 120 == 0) {
        c.Energy().Eat(300.0);
        c.Energy().Drink(0.25);
      }
      a.speedMS = c.MaxSustainableSpeedMS(env, a, gear);
      c.Step(60.0, env, a, gear);
      env.altitudeM += a.speedMS * 60.0 * verticalFraction;
      if (env.altitudeM >= 8848.0) {
        hoursOut = (minute + 1) / 60.0;
        return !c.IsDead();
      }
      if (c.IsDead() || c.IsIncapacitated()) break;
    }
    hoursOut = 15.0;
    return false;
  };
  double eliteHours = 0.0;
  double strongHours = 0.0;
  double hungryHours = 0.0;
  CHECK(push(65.0, 68.0, true, eliteHours));
  CHECK_RANGE(eliteHours, 6.0, 10.0);
  CHECK(push(55.0, 75.0, true, strongHours));
  CHECK_RANGE(strongHours, 9.0, 14.5);
  push(55.0, 75.0, false, hungryHours);
  CHECK(hungryHours > strongHours);
}
