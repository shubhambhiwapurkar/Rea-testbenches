#include <cmath>

#include "Sim/Avalanche.h"
#include "Sim/Weather.h"
#include "TestFramework.h"

using namespace summit::sim;

namespace {

constexpr double kHour = 3600.0;

SnowpackState WithNewSnow(double cm) {
  SnowpackState s;
  s.newSnow24hCm = cm;
  s.newSnow72hCm = cm;
  s.hoursSinceLoading = 2.0;
  return s;
}

SlopeInfo Slope(double angleDeg, bool leeward = false) {
  SlopeInfo s;
  s.angleDeg = angleDeg;
  s.leeward = leeward;
  return s;
}

WeatherSample ColdCalm(double tempC = -10.0) {
  WeatherSample w;
  w.temperatureC = tempC;
  w.windSpeedMS = 2.0;
  w.cloudCover01 = 0.8;  // no surface hoar, no strong sun
  w.relativeHumidity01 = 0.6;
  w.solarWm2 = 0.0;
  return w;
}

}  // namespace

TEST_CASE("Avalanche: slope angle factor shape (Schweizer & Jamieson 2001)") {
  CHECK_NEAR(SlopeAngleFactor(15.0), 0.0, 1e-12);
  CHECK_NEAR(SlopeAngleFactor(25.0), 0.0, 1e-12);
  CHECK(SlopeAngleFactor(28.0) < 0.15);
  CHECK(SlopeAngleFactor(38.0) > 0.95);
  CHECK(SlopeAngleFactor(40.0) > 0.95);
  CHECK_RANGE(SlopeAngleFactor(50.0), 0.3, 0.75);
  CHECK(SlopeAngleFactor(56.0) < 0.15);
  CHECK_NEAR(SlopeAngleFactor(60.0), 0.0, 1e-12);
  CHECK_NEAR(SlopeAngleFactor(75.0), 0.0, 1e-12);
  // Peak between 36 and 42 degrees; monotone up then down.
  double best = 0.0, bestAngle = 0.0;
  double prev = 0.0;
  for (double a = 20.0; a <= 65.0; a += 0.5) {
    const double f = SlopeAngleFactor(a);
    if (a <= 39.0) CHECK(f >= prev - 1e-12);
    if (a >= 41.0) CHECK(f <= prev + 1e-12);
    if (f > best) {
      best = f;
      bestAngle = a;
    }
    prev = f;
  }
  CHECK_RANGE(bestAngle, 36.0, 42.0);
}

TEST_CASE("Avalanche: regional danger rises with new snow (EAWS critical amounts)") {
  CHECK(RegionalDanger(SnowpackState{}) == DangerLevel::Low);
  CHECK(RegionalDanger(WithNewSnow(15.0)) == DangerLevel::Moderate);
  CHECK(RegionalDanger(WithNewSnow(30.0)) >= DangerLevel::Considerable);
  CHECK(RegionalDanger(WithNewSnow(50.0)) >= DangerLevel::High);
  CHECK(RegionalDanger(WithNewSnow(120.0)) == DangerLevel::VeryHigh);
  double prev = 0.0;
  for (double cm = 0.0; cm <= 150.0; cm += 5.0) {
    const double d = DangerIndex(WithNewSnow(cm));
    CHECK(d >= prev - 1e-12);
    CHECK_RANGE(d, 1.0, 5.0);
    prev = d;
  }
}

TEST_CASE("Avalanche: wind loading, warming and weak layers raise danger") {
  SnowpackState base = WithNewSnow(10.0);
  const double d0 = DangerIndex(base);
  SnowpackState windy = base;
  windy.windLoading01 = 0.8;
  SnowpackState warm = base;
  warm.warming01 = 0.8;
  SnowpackState pwl = base;
  pwl.persistentWeakLayer01 = 0.8;
  CHECK(DangerIndex(windy) > d0 + 0.5);
  CHECK(DangerIndex(warm) > d0 + 0.5);
  CHECK(DangerIndex(pwl) > d0 + 0.3);
  SnowpackState windOnly;
  windOnly.windLoading01 = 1.0;
  CHECK(RegionalDanger(windOnly) >= DangerLevel::Moderate);
}

TEST_CASE("Avalanche: natural release magnitudes") {
  // Low danger, 38 deg: ~1e-5 per hour.
  CHECK_RANGE(NaturalReleaseProbabilityPerHour(SnowpackState{}, Slope(38.0)), 3e-6, 3e-5);
  // High danger on a loaded 38 deg lee slope: several % per hour.
  SnowpackState high = WithNewSnow(25.0);
  high.windLoading01 = 0.8;
  CHECK(RegionalDanger(high) == DangerLevel::High);
  CHECK_RANGE(NaturalReleaseProbabilityPerHour(high, Slope(38.0, true)), 0.015, 0.08);
  // Gentle slopes do not release; leeward > windward; convex > planar.
  CHECK_NEAR(NaturalReleaseProbabilityPerHour(high, Slope(22.0, true)), 0.0, 1e-12);
  CHECK(NaturalReleaseProbabilityPerHour(high, Slope(38.0, true)) > NaturalReleaseProbabilityPerHour(high, Slope(38.0)));
  SlopeInfo convex = Slope(38.0);
  convex.convex = true;
  CHECK(NaturalReleaseProbabilityPerHour(high, convex) > NaturalReleaseProbabilityPerHour(high, Slope(38.0)));
  // Strong warming affects sunny (south) aspects more than shady (north) ones.
  SnowpackState wet = WithNewSnow(10.0);
  wet.warming01 = 1.0;
  SlopeInfo south = Slope(38.0);
  south.aspectDeg = 180.0;
  SlopeInfo north = Slope(38.0);
  north.aspectDeg = 0.0;
  CHECK(NaturalReleaseProbabilityPerHour(wet, south) > 2.0 * NaturalReleaseProbabilityPerHour(wet, north));
}

TEST_CASE("Avalanche: human trigger probability per crossing") {
  const SnowpackState considerable = WithNewSnow(30.0);
  CHECK(RegionalDanger(considerable) == DangerLevel::Considerable);
  const SlopeInfo lee38 = Slope(38.0, true);
  const double grouped = HumanTriggerProbability(considerable, lee38, 4, false);
  const double spaced = HumanTriggerProbability(considerable, lee38, 4, true);
  CHECK_RANGE(grouped, 0.01, 0.06);
  CHECK_RANGE(spaced, 0.005, 0.05);
  CHECK(spaced < grouped);
  CHECK(spaced > 0.3 * grouped);
  // Larger parties trigger more; near zero below 25 deg; rises with danger.
  CHECK(HumanTriggerProbability(considerable, lee38, 1, false) < HumanTriggerProbability(considerable, lee38, 3, false));
  CHECK(HumanTriggerProbability(considerable, Slope(24.0, true), 6, false) < 1e-9);
  CHECK(HumanTriggerProbability(SnowpackState{}, lee38, 4, false) < 0.25 * grouped);
  CHECK(HumanTriggerProbability(WithNewSnow(60.0), lee38, 4, false) > 2.0 * grouped);
  CHECK_NEAR(HumanTriggerProbability(considerable, lee38, 0, false), 0.0, 1e-12);
}

TEST_CASE("Avalanche: snowpack ring buffers accumulate 24 h and 72 h new snow") {
  SnowpackModel model;
  WeatherSample snowing = ColdCalm();
  snowing.snowfallCmH = 2.0;
  for (int i = 0; i < 12 * 60; ++i) model.Step(60.0, snowing, 0.5);  // 12 h of 1-min steps
  CHECK_NEAR(model.State().newSnow24hCm, 24.0, 0.1);
  CHECK_NEAR(model.State().newSnow72hCm, 24.0, 0.1);
  CHECK(model.State().hoursSinceLoading < 0.1);
  model.Step(30.0 * kHour, ColdCalm(), 0.5);  // one big step: 30 h without snow
  CHECK_NEAR(model.State().newSnow24hCm, 0.0, 0.1);
  CHECK_NEAR(model.State().newSnow72hCm, 24.0, 0.1);
  CHECK_NEAR(model.State().hoursSinceLoading, 30.0, 0.1);
  model.Step(61.0 * kHour, ColdCalm(), 0.5);  // 103 h after the snow started
  CHECK_NEAR(model.State().newSnow72hCm, 0.0, 0.1);
  CHECK(model.State().baseDepthCm > 150.0);
}

TEST_CASE("Avalanche: storm then settling over days lowers danger") {
  SnowpackModel model;
  WeatherSample storm = ColdCalm(-10.0);
  storm.snowfallCmH = 2.0;  // 48 cm in 24 h
  model.Step(24.0 * kHour, storm, 0.5);
  const DangerLevel peak = RegionalDanger(model.State());
  CHECK(peak >= DangerLevel::High);
  const double dPeak = DangerIndex(model.State());
  double prev = dPeak;
  for (int day = 1; day <= 5; ++day) {
    model.Step(24.0 * kHour, ColdCalm(-10.0), 0.5);
    const double d = DangerIndex(model.State());
    CHECK(d < prev);
    prev = d;
  }
  CHECK(RegionalDanger(model.State()) <= DangerLevel::Moderate);
}

TEST_CASE("Avalanche: new snow settles faster when warm") {
  WeatherSample snow = ColdCalm(-15.0);
  snow.snowfallCmH = 3.0;
  SnowpackModel cold, warm;
  cold.Step(10.0 * kHour, snow, 0.5);
  WeatherSample warmSnow = snow;
  warmSnow.temperatureC = -2.0;
  warm.Step(10.0 * kHour, warmSnow, 0.5);
  cold.Step(48.0 * kHour, ColdCalm(-15.0), 0.5);
  warm.Step(48.0 * kHour, ColdCalm(-2.0), 0.5);
  CHECK(warm.State().unsettledNewSnowCm < 0.6 * cold.State().unsettledNewSnowCm);
  // Cold: ~2-3 day time scale, so after 2 days roughly half remains.
  CHECK_RANGE(cold.State().unsettledNewSnowCm / 30.0, 0.35, 0.7);
}

TEST_CASE("Avalanche: wind loading needs wind above ~7 m/s and loose snow") {
  WeatherSample fresh = ColdCalm(-12.0);
  fresh.snowfallCmH = 3.0;
  SnowpackModel calm, windy, noSnow;
  calm.Step(8.0 * kHour, fresh, 1.0);
  windy.Step(8.0 * kHour, fresh, 1.0);
  WeatherSample gale = ColdCalm(-12.0);
  gale.windSpeedMS = 18.0;
  WeatherSample breeze = ColdCalm(-12.0);
  breeze.windSpeedMS = 5.0;
  calm.Step(8.0 * kHour, breeze, 1.0);
  windy.Step(8.0 * kHour, gale, 1.0);
  noSnow.Step(8.0 * kHour, gale, 1.0);
  CHECK(calm.State().windLoading01 < 0.02);
  CHECK(windy.State().windLoading01 > 0.4);
  CHECK(noSnow.State().windLoading01 < 0.5 * windy.State().windLoading01);
  CHECK(DangerIndex(windy.State()) > DangerIndex(calm.State()));
  // Sheltered terrain loads less.
  SnowpackModel sheltered;
  sheltered.Step(8.0 * kHour, fresh, 0.2);
  sheltered.Step(8.0 * kHour, gale, 0.2);
  CHECK(sheltered.State().windLoading01 < windy.State().windLoading01);
  // Wind slabs stabilise over days.
  const double loaded = windy.State().windLoading01;
  windy.Step(5.0 * 24.0 * kHour, ColdCalm(-12.0), 1.0);
  CHECK(windy.State().windLoading01 < 0.5 * loaded);
}

TEST_CASE("Avalanche: warming tracks temperature above -2 C and strong sun") {
  SnowpackModel model;
  model.Step(6.0 * kHour, ColdCalm(-12.0), 0.5);
  CHECK(model.State().warming01 < 0.05);
  WeatherSample thaw = ColdCalm(2.0);
  model.Step(6.0 * kHour, thaw, 0.5);
  CHECK(model.State().warming01 > 0.6);
  model.Step(48.0 * kHour, ColdCalm(-12.0), 0.5);
  CHECK(model.State().warming01 < 0.05);
  WeatherSample sunny = ColdCalm(-5.0);
  sunny.cloudCover01 = 0.0;
  sunny.solarWm2 = 1100.0;
  model.Step(4.0 * kHour, sunny, 0.5);
  CHECK(model.State().warming01 > 0.3);
}

TEST_CASE("Avalanche: surface hoar buried by snow becomes a slow-healing weak layer") {
  SnowpackModel model;
  WeatherSample clearNight = ColdCalm(-15.0);
  clearNight.cloudCover01 = 0.05;
  clearNight.windSpeedMS = 1.0;
  clearNight.relativeHumidity01 = 0.85;
  model.Step(10.0 * kHour, clearNight, 0.5);
  CHECK(model.State().surfaceHoar01 > 0.3);
  WeatherSample snow = ColdCalm(-10.0);
  snow.snowfallCmH = 2.0;
  model.Step(6.0 * kHour, snow, 0.5);
  const double pwl = model.State().persistentWeakLayer01;
  CHECK(pwl > 0.2);
  CHECK(model.State().surfaceHoar01 < 0.05);
  model.Step(10.0 * 24.0 * kHour, ColdCalm(-10.0), 0.5);  // ten days later
  CHECK(model.State().persistentWeakLayer01 > 0.6 * pwl);
  CHECK(model.State().persistentWeakLayer01 < pwl);
}

TEST_CASE("Avalanche: snowpack step is insensitive to step size") {
  WeatherSample w = ColdCalm(-8.0);
  w.snowfallCmH = 1.5;
  w.windSpeedMS = 12.0;
  SnowpackModel fine, coarse;
  for (int i = 0; i < 18 * 60; ++i) fine.Step(60.0, w, 0.7);
  coarse.Step(18.0 * kHour, w, 0.7);
  CHECK_NEAR(fine.State().newSnow24hCm, coarse.State().newSnow24hCm, 1e-6);
  CHECK_NEAR(fine.State().unsettledNewSnowCm, coarse.State().unsettledNewSnowCm, 0.05);
  CHECK_NEAR(fine.State().windLoading01, coarse.State().windLoading01, 0.02);
  CHECK_NEAR(fine.State().warming01, coarse.State().warming01, 0.02);
}

TEST_CASE("Avalanche: coupled to the weather system through a season") {
  WeatherSystem weather(KhumbuClimate(), 9);
  SnowpackModel snow;
  double maxDanger = 1.0, minDanger = 5.0;
  for (double t = 150.0 * 86400.0; t < 250.0 * 86400.0; t += 1800.0) {  // monsoon
    weather.AdvanceTo(t);
    snow.Step(1800.0, weather.Sample(6000.0, 0.6), 0.6);
    const double d = DangerIndex(snow.State());
    maxDanger = d > maxDanger ? d : maxDanger;
    minDanger = d < minDanger ? d : minDanger;
  }
  CHECK(maxDanger >= 3.0);    // monsoon snow loading produces Considerable or worse at times
  CHECK(maxDanger <= 5.0);
  CHECK(minDanger >= 1.0);
}
