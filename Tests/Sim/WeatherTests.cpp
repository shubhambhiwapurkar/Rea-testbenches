#include <algorithm>
#include <cmath>
#include <vector>

#include "Sim/Weather.h"
#include "TestFramework.h"

using namespace summit::sim;

namespace {

constexpr double kDay = 86400.0;
constexpr double kHour = 3600.0;
constexpr double kEbcM = 5364.0;
constexpr double kSummitM = 8848.0;
constexpr int kMonthStart[13] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};

bool SameSample(const WeatherSample& a, const WeatherSample& b) {
  return a.temperatureC == b.temperatureC && a.windSpeedMS == b.windSpeedMS && a.windGustMS == b.windGustMS &&
         a.windDirectionDeg == b.windDirectionDeg && a.visibilityM == b.visibilityM &&
         a.precipitationMmH == b.precipitationMmH && a.snowfallCmH == b.snowfallCmH &&
         a.cloudCover01 == b.cloudCover01 && a.relativeHumidity01 == b.relativeHumidity01 &&
         a.solarWm2 == b.solarWm2 && a.stormIntensity01 == b.stormIntensity01;
}

struct MonthMeans {
  double tempC = 0.0;
  double windMS = 0.0;
};

// Hourly mean over a calendar month, averaged over several seeds.
MonthMeans MeanOverMonth(int month, double altitudeM, int seeds) {
  MonthMeans m;
  int n = 0;
  for (int seed = 1; seed <= seeds; ++seed) {
    WeatherSystem w(KhumbuClimate(), static_cast<std::uint64_t>(seed) * 7919u);
    for (double t = kMonthStart[month] * kDay; t < kMonthStart[month + 1] * kDay; t += kHour) {
      w.AdvanceTo(t);
      const WeatherSample s = w.Sample(altitudeM, 1.0);
      m.tempC += s.temperatureC;
      m.windMS += s.windSpeedMS;
      ++n;
    }
  }
  m.tempC /= n;
  m.windMS /= n;
  return m;
}

}  // namespace

TEST_CASE("Weather: calendar helpers (365-day year, solar time)") {
  CHECK(DayOfYear(0.0) == 0);
  CHECK(DayOfYear(364.5 * kDay) == 364);
  CHECK(DayOfYear(365.0 * kDay) == 0);
  CHECK(DayOfYear(-0.5 * kDay) == 364);
  CHECK_NEAR(HourOfDay(13.5 * kHour), 13.5, 1e-9);
  CHECK_NEAR(HourOfDay(3.0 * kDay + 6.25 * kHour), 6.25, 1e-9);
  CHECK(MonthOfYear(0.0) == 0);
  CHECK(MonthOfYear(30.9 * kDay) == 0);
  CHECK(MonthOfYear(31.0 * kDay) == 1);
  CHECK(MonthOfYear(59.0 * kDay) == 2);   // 1 March, non-leap year
  CHECK(MonthOfYear(140.0 * kDay) == 4);  // 21 May
  CHECK(MonthOfYear(334.0 * kDay) == 11);
  CHECK(MonthOfYear(364.9 * kDay) == 11);
}

TEST_CASE("Weather: Khumbu climatology matches EBC observations") {
  const ClimateProfile c = KhumbuClimate();
  CHECK_NEAR(c.referenceElevationM, 5364.0, 1.0);
  CHECK_NEAR(c.latitudeDeg, 27.98, 0.05);
  CHECK_RANGE(c.meanTempC[0], -18.0, -15.0);  // January
  CHECK_RANGE(c.meanTempC[3], -6.0, -2.0);    // April
  CHECK_RANGE(c.meanTempC[4], -5.0, -2.0);    // May
  for (int m = 5; m <= 8; ++m) CHECK_RANGE(c.meanTempC[m], -1.0, 2.0);  // monsoon
  // Jet: very strong in winter, collapsing in May, minimum in monsoon, rising again from October.
  CHECK(c.jetStreamWindMS[0] >= 40.0);
  CHECK(c.jetStreamWindMS[4] < 0.6 * c.jetStreamWindMS[1]);
  CHECK(c.jetStreamWindMS[6] < c.jetStreamWindMS[4]);
  CHECK(c.jetStreamWindMS[9] < c.jetStreamWindMS[11]);
  // Monsoon delivers most of the precipitation.
  double total = 0.0, monsoon = 0.0;
  for (int m = 0; m < 12; ++m) {
    const double mm = c.precipitationMmPerDay[m] * (kMonthStart[m + 1] - kMonthStart[m]);
    total += mm;
    if (m >= 5 && m <= 8) monsoon += mm;
  }
  CHECK_RANGE(total, 400.0, 700.0);
  CHECK(monsoon / total > 0.7);
}

TEST_CASE("Weather: deterministic for a seed, independent of step size") {
  const ClimateProfile c = KhumbuClimate();
  WeatherSystem fine(c, 1234);
  WeatherSystem coarse(c, 1234);
  WeatherSystem other(c, 4321);
  const double start = 130.0 * kDay;
  bool allSame = true;
  bool anyDifferent = false;
  // Fine: one-minute steps. Coarse: jumps straight to each check time (6 h apart, after a big jump).
  double t = 0.0;
  for (int k = 0; k < 12; ++k) {
    const double check = start + k * 6.0 * kHour;
    while (t < check) {
      t = std::min(check, t + (t < start - kDay ? 6.0 * kHour : 60.0));
      fine.AdvanceTo(t);
    }
    coarse.AdvanceTo(check);
    other.AdvanceTo(check);
    for (double alt : {kEbcM, 7000.0, kSummitM}) {
      const WeatherSample a = fine.Sample(alt, 0.7);
      const WeatherSample b = coarse.Sample(alt, 0.7);
      if (!SameSample(a, b)) allSame = false;
      if (!SameSample(a, other.Sample(alt, 0.7))) anyDifferent = true;
    }
  }
  CHECK(allSame);
  CHECK(anyDifferent);
  CHECK_NEAR(fine.CurrentTime(), coarse.CurrentTime(), 1e-9);
}

TEST_CASE("Weather: lapse rate between EBC and summit") {
  const ClimateProfile c = KhumbuClimate();
  WeatherSystem w(c, 99);
  // Daily-mean difference: the diurnal cycle averages out, so the gap is the May lapse rate x 3.484 km.
  double diff = 0.0;
  int n = 0;
  for (double t = 135.0 * kDay; t < 136.0 * kDay; t += kHour) {
    w.AdvanceTo(t);
    diff += w.Sample(kEbcM).temperatureC - w.Sample(kSummitM).temperatureC;
    ++n;
  }
  diff /= n;
  CHECK_NEAR(diff, c.lapseRateCPerKmByMonth[4] * (kSummitM - kEbcM) / 1000.0, 0.5);
  CHECK_RANGE(diff / 3.484, 5.5, 7.0);  // deg C per km, typical free-air lapse rate
}

TEST_CASE("Weather: seasonal temperatures at EBC, Balcony and summit") {
  // Targets: EBC May -5..-2, Jan ~ -15..-18, monsoon 0..+2 (Bollasina 2002); Balcony (8430 m) May
  // -20..-25 (Perry et al. 2020); summit May ~ -20..-26, winter -35 or colder (Moore & Semple 2006).
  CHECK_RANGE(MeanOverMonth(4, kEbcM, 3).tempC, -6.0, -1.5);
  CHECK_RANGE(MeanOverMonth(0, kEbcM, 3).tempC, -19.0, -14.0);
  CHECK_RANGE(MeanOverMonth(6, kEbcM, 3).tempC, -1.5, 2.5);
  CHECK_RANGE(MeanOverMonth(4, 8430.0, 3).tempC, -25.0, -20.0);
  CHECK_RANGE(MeanOverMonth(4, kSummitM, 3).tempC, -26.5, -20.0);
  CHECK(MeanOverMonth(0, kSummitM, 3).tempC <= -34.0);
}

TEST_CASE("Weather: diurnal cycle with minimum near dawn and maximum in the early afternoon") {
  WeatherSystem w(KhumbuClimate(), 5);
  double byHour[24] = {};
  for (int day = 0; day < 30; ++day) {
    for (int hr = 0; hr < 24; ++hr) {
      const double t = (310.0 + day) * kDay + hr * kHour;  // November: dry and mostly clear
      w.AdvanceTo(t);
      byHour[hr] += w.Sample(kEbcM).temperatureC / 30.0;
    }
  }
  const int hottest = static_cast<int>(std::max_element(byHour, byHour + 24) - byHour);
  const int coldest = static_cast<int>(std::min_element(byHour, byHour + 24) - byHour);
  CHECK_RANGE(hottest, 12, 15);
  CHECK_RANGE(coldest, 4, 7);
  CHECK_RANGE(byHour[hottest] - byHour[coldest], 8.0, 18.0);
}

TEST_CASE("Weather: wind rises with altitude toward jet speed; gusts 1.3-1.6x; exposure matters") {
  WeatherSystem w(KhumbuClimate(), 77);
  double sum5 = 0.0, sum7 = 0.0, sum9 = 0.0, sumSheltered = 0.0;
  int n = 0;
  for (double t = 10.0 * kDay; t < 40.0 * kDay; t += 3.0 * kHour) {
    w.AdvanceTo(t);
    const WeatherSample s5 = w.Sample(5000.0);
    const WeatherSample s9 = w.Sample(kSummitM);
    sum5 += s5.windSpeedMS;
    sum7 += w.Sample(7000.0).windSpeedMS;
    sum9 += s9.windSpeedMS;
    sumSheltered += w.Sample(kSummitM, 0.0).windSpeedMS;
    CHECK_RANGE(s9.windGustMS / std::max(s9.windSpeedMS, 1e-6), 1.3 - 1e-9, 1.6 + 1e-9);
    ++n;
  }
  CHECK(sum5 < sum7);
  CHECK(sum7 < sum9);
  CHECK(sumSheltered < 0.5 * sum9);
  CHECK_RANGE(sum9 / n, 35.0, 60.0);  // January summit winds (Moore & Semple 2006)
}

TEST_CASE("Weather: May summit winds are lower than January (seasonal ordering)") {
  const double jan = MeanOverMonth(0, kSummitM, 4).windMS;
  const double may = MeanOverMonth(4, kSummitM, 4).windMS;
  const double jul = MeanOverMonth(6, kSummitM, 4).windMS;
  CHECK(may < 0.7 * jan);
  CHECK(jul < may);
  CHECK_RANGE(may, 12.0, 32.0);
}

TEST_CASE("Weather: jet oscillation opens multi-day summit windows in late May") {
  // Between 10 May and 5 June, most seasons should offer a >= 48 h spell with summit sustained wind
  // below 15 m/s (the classic pre-monsoon summit window).
  int seasonsWithWindow = 0;
  for (int seed = 1; seed <= 20; ++seed) {
    WeatherSystem w(KhumbuClimate(), static_cast<std::uint64_t>(seed));
    int run = 0, best = 0;
    for (double t = 129.0 * kDay; t < 156.0 * kDay; t += kHour) {
      w.AdvanceTo(t);
      if (w.Sample(kSummitM).windSpeedMS < 15.0) {
        best = std::max(best, ++run);
      } else {
        run = 0;
      }
    }
    if (best >= 48) ++seasonsWithWindow;
  }
  CHECK(seasonsWithWindow >= 12);
}

TEST_CASE("Weather: storms bring snowfall, cloud and wind and cut visibility") {
  WeatherSystem w(KhumbuClimate(), 2024);
  double snowStorm = 0.0, snowCalm = 0.0, visStorm = 0.0, visCalm = 0.0, cloudStorm = 0.0, cloudCalm = 0.0;
  int nStorm = 0, nCalm = 0;
  double minStrongStormVis = 1e9;
  for (double t = 0.0; t < 365.0 * kDay; t += kHour) {
    w.AdvanceTo(t);
    const WeatherSample s = w.Sample(7000.0);
    if (s.stormIntensity01 > 0.5) {
      snowStorm += s.snowfallCmH;
      visStorm += std::log(s.visibilityM);
      cloudStorm += s.cloudCover01;
      ++nStorm;
    } else if (s.stormIntensity01 < 0.05) {
      snowCalm += s.snowfallCmH;
      visCalm += std::log(s.visibilityM);
      cloudCalm += s.cloudCover01;
      ++nCalm;
    }
    if (s.stormIntensity01 > 0.8) minStrongStormVis = std::min(minStrongStormVis, w.Sample(8000.0).visibilityM);
  }
  CHECK(nStorm > 50);
  CHECK(nCalm > 1000);
  CHECK(snowStorm / nStorm > 3.0 * (snowCalm / nCalm));
  CHECK(visStorm / nStorm < visCalm / nCalm - 1.0);  // geometric mean visibility lower by > e
  CHECK(cloudStorm / nStorm > 0.85);
  CHECK(cloudStorm / nStorm > cloudCalm / nCalm + 0.3);
  CHECK(minStrongStormVis < 50.0);  // whiteout in a strong storm with blowing snow
}

TEST_CASE("Weather: precipitation phase and snow ratio") {
  WeatherSystem w(KhumbuClimate(), 11);
  int coldWet = 0, warmWet = 0;
  for (double t = 150.0 * kDay; t < 260.0 * kDay; t += kHour) {
    w.AdvanceTo(t);
    for (double alt : {3500.0, kEbcM, 7000.0}) {
      const WeatherSample s = w.Sample(alt);
      if (s.precipitationMmH <= 1e-3) continue;
      if (s.temperatureC < -5.0) {
        ++coldWet;
        CHECK(s.snowfallCmH >= s.precipitationMmH * 0.999);  // >= 10:1 when cold
      }
      if (s.temperatureC > 3.0) {
        ++warmWet;
        CHECK(s.snowfallCmH < 1e-9);  // rain
      }
    }
  }
  CHECK(coldWet > 10);
  CHECK(warmWet > 10);
}

TEST_CASE("Weather: blowing snow reduces visibility without precipitation") {
  WeatherSystem w(KhumbuClimate(), 8);
  double windyLog = 0.0, calmLog = 0.0;
  int nWindy = 0, nCalm = 0;
  for (double t = 0.0; t < 365.0 * kDay; t += kHour) {
    w.AdvanceTo(t);
    const WeatherSample s = w.Sample(7500.0);
    if (s.precipitationMmH > 1e-3 || s.cloudCover01 > 0.6) continue;
    if (s.windSpeedMS > 25.0) {
      windyLog += std::log(s.visibilityM);
      ++nWindy;
    } else if (s.windSpeedMS < 9.0) {
      calmLog += std::log(s.visibilityM);
      ++nCalm;
    }
  }
  CHECK(nWindy > 20);
  CHECK(nCalm > 5);
  CHECK(std::exp(windyLog / nWindy) < 0.5 * std::exp(calmLog / nCalm));
  CHECK(std::exp(calmLog / nCalm) > 10000.0);
}

TEST_CASE("Weather: solar geometry and clear-sky irradiance") {
  const double lat = 27.98;
  // Equinox noon elevation ~ 90 - latitude; June solstice ~ 90 - (lat - 23.45).
  CHECK_NEAR(SolarElevationDeg(lat, 79, 12.0), 90.0 - lat, 1.5);
  CHECK_NEAR(SolarElevationDeg(lat, 171, 12.0), 90.0 - (lat - 23.45), 0.6);
  CHECK(SolarElevationDeg(lat, 140, 0.0) < 0.0);
  CHECK_NEAR(ClearSkyGlobalIrradianceWm2(lat, 140, 0.0, kSummitM), 0.0, 1e-12);
  CHECK_NEAR(ClearSkyGlobalIrradianceWm2(lat, 140, 23.0, kEbcM), 0.0, 1e-12);
  // Peak at solar noon.
  double best = -1.0, bestHour = 0.0;
  for (double hr = 4.0; hr <= 20.0; hr += 0.25) {
    const double g = ClearSkyGlobalIrradianceWm2(lat, 140, hr, kEbcM);
    if (g > best) {
      best = g;
      bestHour = hr;
    }
  }
  CHECK_NEAR(bestHour, 12.0, 0.26);
  // Thin air: high-altitude clear-sky noon in May reaches ~1000-1200 W/m^2 and exceeds sea level.
  const double seaLevel = ClearSkyGlobalIrradianceWm2(lat, 140, 12.0, 0.0);
  const double ebc = ClearSkyGlobalIrradianceWm2(lat, 140, 12.0, kEbcM);
  const double summit = ClearSkyGlobalIrradianceWm2(lat, 140, 12.0, kSummitM);
  CHECK_RANGE(seaLevel, 900.0, 1080.0);
  CHECK_RANGE(ebc, 1000.0, 1250.0);
  CHECK(summit > ebc);
  CHECK(ebc > seaLevel);
  CHECK(summit < 1361.0 * 1.035);
}

TEST_CASE("Weather: sampled solar is zero at night and reduced by cloud") {
  WeatherSystem w(KhumbuClimate(), 3);
  for (int day = 100; day < 110; ++day) {
    w.AdvanceTo(day * kDay + 1.0 * kHour);
    CHECK_NEAR(w.Sample(kEbcM).solarWm2, 0.0, 1e-12);
    w.AdvanceTo(day * kDay + 22.0 * kHour);
    CHECK_NEAR(w.Sample(kEbcM).solarWm2, 0.0, 1e-12);
  }
  WeatherSystem w2(KhumbuClimate(), 4);
  for (double t = 120.0 * kDay + 12.0 * kHour; t < 180.0 * kDay; t += kDay) {
    w2.AdvanceTo(t);
    const WeatherSample s = w2.Sample(kEbcM);
    const double clear = ClearSkyGlobalIrradianceWm2(KhumbuClimate().latitudeDeg, DayOfYear(t), 12.0, kEbcM);
    CHECK(s.solarWm2 <= clear + 1e-9);
    CHECK(s.solarWm2 >= 0.25 * clear - 1e-9);
  }
}

TEST_CASE("Weather: forecast confidence decays with lead time") {
  WeatherSystem w(KhumbuClimate(), 55);
  w.AdvanceTo(130.0 * kDay);
  const std::vector<ForecastEntry> f = w.Forecast(168.0, 6.0, kSummitM);
  CHECK(f.size() == 28u);
  if (f.size() != 28u) return;
  CHECK_NEAR(f[3].time - w.CurrentTime(), 24.0 * kHour, 1e-6);
  CHECK_NEAR(f[3].confidence01, 0.9, 0.03);    // 24 h
  CHECK_NEAR(f[11].confidence01, 0.6, 0.05);   // 72 h
  CHECK_NEAR(f[27].confidence01, 0.3, 0.05);   // 168 h
  for (std::size_t i = 1; i < f.size(); ++i) CHECK(f[i].confidence01 < f[i - 1].confidence01);
}

TEST_CASE("Weather: forecast is reproducible and leaves the live state untouched") {
  WeatherSystem w(KhumbuClimate(), 314);
  w.AdvanceTo(140.0 * kDay + 5.0 * kHour);
  const WeatherSample before = w.Sample(kSummitM);
  const double timeBefore = w.CurrentTime();
  const std::vector<ForecastEntry> a = w.Forecast(120.0, 3.0, kSummitM);
  const std::vector<ForecastEntry> b = w.Forecast(120.0, 3.0, kSummitM);
  CHECK(a.size() == b.size());
  bool identical = a.size() == b.size();
  for (std::size_t i = 0; identical && i < a.size(); ++i) {
    identical = SameSample(a[i].expected, b[i].expected) && a[i].confidence01 == b[i].confidence01 &&
                a[i].time == b[i].time;
  }
  CHECK(identical);
  CHECK_NEAR(w.CurrentTime(), timeBefore, 0.0);
  CHECK(SameSample(before, w.Sample(kSummitM)));

  // A different issue time gives a different (re-seeded) forecast error.
  WeatherSystem w2 = w;
  w2.AdvanceTo(w.CurrentTime() + 3.0 * kHour);
  const std::vector<ForecastEntry> c = w2.Forecast(117.0, 3.0, kSummitM);
  CHECK(!c.empty() && c.back().time == a.back().time);
  if (!c.empty()) CHECK(c.back().expected.temperatureC != a.back().expected.temperatureC);

  // Short leads track the truth closely; the live system later experiences that weather.
  const WeatherSample predicted = a[0].expected;  // +3 h
  w.AdvanceTo(a[0].time);
  const WeatherSample actual = w.Sample(kSummitM);
  CHECK_NEAR(predicted.temperatureC, actual.temperatureC, 0.5);
  CHECK_NEAR(predicted.windSpeedMS, actual.windSpeedMS, 0.15 * actual.windSpeedMS + 1.0);
}

TEST_CASE("Weather: forecast error grows with lead time") {
  // Mean absolute temperature error at 24 h vs 7 days, over many issue times.
  double err24 = 0.0, err168 = 0.0;
  int n = 0;
  for (int seed = 1; seed <= 4; ++seed) {
    WeatherSystem w(KhumbuClimate(), static_cast<std::uint64_t>(seed));
    for (int issue = 0; issue < 10; ++issue) {
      w.AdvanceTo((60.0 + issue * 9.0) * kDay);
      const std::vector<ForecastEntry> f = w.Forecast(168.0, 24.0, kSummitM);
      WeatherSystem truth = w;
      truth.AdvanceTo(f[0].time);
      err24 += std::fabs(f[0].expected.temperatureC - truth.Sample(kSummitM).temperatureC);
      truth.AdvanceTo(f[6].time);
      err168 += std::fabs(f[6].expected.temperatureC - truth.Sample(kSummitM).temperatureC);
      ++n;
    }
  }
  CHECK(err168 / n > 2.0 * (err24 / n));
  CHECK(err24 / n < 1.5);
}

TEST_CASE("Weather: large jumps and per-frame stepping stay cheap and finite") {
  WeatherSystem w(KhumbuClimate(), 1);
  w.AdvanceTo(200.0 * kDay);  // jump ~200 days
  CHECK_NEAR(w.CurrentTime(), 200.0 * kDay, 1e-9);
  for (int frame = 0; frame < 60 * 60; ++frame) w.AdvanceTo(w.CurrentTime() + 1.0 / 60.0 * 60.0);  // 1 h of frames
  const WeatherSample s = w.Sample(6500.0, 0.5);
  CHECK(std::isfinite(s.temperatureC) && std::isfinite(s.windSpeedMS) && std::isfinite(s.visibilityM));
  CHECK_RANGE(s.visibilityM, 5.0, 20000.0);
  CHECK_RANGE(s.cloudCover01, 0.0, 1.0);
  CHECK_RANGE(s.relativeHumidity01, 0.0, 1.0);
  CHECK_RANGE(s.windDirectionDeg, 0.0, 360.0);
  w.AdvanceTo(10.0);  // going backwards is ignored
  CHECK(w.CurrentTime() > 200.0 * kDay);
}
