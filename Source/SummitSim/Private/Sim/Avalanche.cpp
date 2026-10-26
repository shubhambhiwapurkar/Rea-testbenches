// Avalanche danger, release probabilities and a lumped snowpack-stability model.
//
// Sources:
//  - Schweizer J & Jamieson JB (2001) "Snow cover properties for skier triggering of avalanches",
//    Cold Reg Sci Technol 33:207-221: skier-triggered slab release angles cluster at 35-45 deg with a
//    median of ~38-39 deg; very few below 30 deg.
//  - McClung D & Schaerer P (2006) "The Avalanche Handbook", 3rd ed.: slab avalanches rare below
//    ~25 deg; above ~50-55 deg snow sluffs continuously and slabs seldom build up; most direct-action
//    avalanches occur during or within 24-48 h of storms; new snow stabilises faster when warm.
//  - EAWS (European Avalanche Warning Services) danger scale and "critical new snow amount"
//    guidance: 10-20 cm unfavourable, 20-30 cm moderately favourable, 30-50 cm favourable conditions;
//    wind slabs, wet snow and persistent weak layers raise danger independently.
//  - Schweizer J, Jamieson JB & Schneebeli M (2003) "Snow avalanche formation", Rev Geophys 41:1016:
//    settlement and sintering of new snow; slab stability recovers over days.
//  - Munter W (1997) "3x3 Lawinen" (reduction method): risk potential roughly doubles per danger
//    level; spacing and group size reduce triggering by ~2x. Techel F et al. (2015) "Avalanche
//    fatalities in the European Alps: long-term trends and statistics", Geogr Helv 70:147-159:
//    relative risk per unit of backcountry use rises ~2-3x per danger level.
//  - Li L & Pomeroy JW (1997) "Estimates of threshold wind speeds for snow transport using
//    meteorological data", J Appl Meteorol 36:205-213: dry-snow transport threshold ~7-8 m/s mean;
//    transport rate scales with a high power of the excess wind speed (Pomeroy & Gray 1990).
//  - Jamieson JB & Schweizer J (2000) "Texture and strength changes of buried surface-hoar layers
//    with implications for dry snow-slab avalanche release", J Glaciol 46:151-160: buried surface
//    hoar persists for weeks to months. Lang RM, Leo BR & Brown RL (1984) / Hachikubo A & Akitaya E
//    (1997): surface hoar forms on clear, calm, humid nights.
//  - Degree-day melt factor for snow ~3-5 mm w.e. per deg C per day (Hock 2003, J Hydrol 282:104-115).

#include "Sim/Avalanche.h"

#include <algorithm>
#include <cmath>

#include "Sim/Constants.h"
#include "Sim/Math.h"
#include "Sim/Weather.h"

namespace summit::sim {

namespace {

// Piecewise-linear interpolation through (x, y) knots, clamped at the ends.
double PiecewiseLinear(const double* xs, const double* ys, int n, double x) {
  if (x <= xs[0]) return ys[0];
  for (int i = 1; i < n; ++i) {
    if (x <= xs[i]) return Lerp(ys[i - 1], ys[i], (x - xs[i - 1]) / (xs[i] - xs[i - 1]));
  }
  return ys[n - 1];
}

// Effective unstable new-snow load (cm). Uses the settling state when the model is running, and the
// raw 24 h / 72 h sums when a caller sets them directly (weighting the last 24 h more heavily).
double EffectiveNewSnowCm(const SnowpackState& s) {
  const double fromSums = 0.6 * std::max(0.0, s.newSnow72hCm) + 0.4 * std::max(0.0, s.newSnow24hCm);
  return std::max(std::max(0.0, s.unsettledNewSnowCm), fromSums);
}

// Danger index (1..5) -> per-hour natural release rate on a ~38 deg slope.
// Calibration: Low ~1e-5/h ("natural avalanches very unlikely"), roughly x10 per level, so High is
// ~1e-2/h before slope modifiers and a loaded lee slope reaches a few %/h ("numerous natural
// avalanches likely"). Capped at 0.2/h at Very High.
double NaturalBaseRatePerHour(double dangerIndex) {
  return std::min(0.2, 1e-5 * std::pow(10.0, dangerIndex - 1.0));
}

// Per-person trigger probability for one crossing of a ~38 deg slope. ~0.4% at Considerable for
// a single climber, x3 per danger level (Munter 1997; Techel et al. 2015).
double PerPersonTriggerProbability(double dangerIndex) {
  return std::min(0.3, 0.00387 * std::pow(3.0, dangerIndex - 3.0));
}

// How much a slope's aspect faces the sun (northern hemisphere): 1 = south, 0 = north.
double SunExposure(double aspectDeg) { return 0.5 * (1.0 - std::cos(DegToRad(aspectDeg))); }

}  // namespace

double SlopeAngleFactor(double angleDeg) {
  // Rises from ~0 at 25 deg to a peak at 39-41 deg (Schweizer & Jamieson 2001 median ~38-39 deg), then
  // falls: above ~50 deg snow sluffs off before slabs can form, ~0 by 60 deg (McClung & Schaerer).
  const double rise = SmoothStep(25.0, 39.0, angleDeg);
  const double fall = 1.0 - SmoothStep(41.0, 60.0, angleDeg);
  return Clamp01(rise * fall);
}

double DangerIndex(const SnowpackState& snow) {
  const double newSnow = EffectiveNewSnowCm(snow);
  // EAWS critical new-snow amounts: 10-20 cm -> Moderate, 30 cm -> Considerable, 50 cm -> High,
  // ~1 m -> Very High.
  static const double kSnowX[] = {0.0, 10.0, 20.0, 30.0, 50.0, 100.0};
  static const double kSnowY[] = {0.0, 0.6, 1.2, 2.0, 3.0, 4.0};
  const double newSnowTerm = PiecewiseLinear(kSnowX, kSnowY, 6, newSnow);

  const double wind = Clamp01(snow.windLoading01);
  const double warm = Clamp01(snow.warming01);
  const double pwl = Clamp01(snow.persistentWeakLayer01);

  // Wind slabs alone: up to ~Considerable.
  const double windTerm = 1.4 * wind;
  // Wet-snow instability (rapid warming, strong sun), worse with fresh snow on top.
  const double warmTerm = 1.8 * warm * (0.75 + 0.25 * SmoothStep(0.0, 20.0, newSnow));
  // Persistent weak layer: reactive mostly once loaded by new or wind-drifted snow.
  const double pwlTerm = 1.3 * pwl * (0.6 + 0.4 * SmoothStep(0.0, 30.0, newSnow + 20.0 * wind));
  // Instability peaks during and just after loading (direct-action avalanches).
  const double loadingTerm =
      0.3 * (1.0 - SmoothStep(0.0, 12.0, snow.hoursSinceLoading)) * SmoothStep(0.0, 10.0, newSnow + 15.0 * wind);

  return Clamp(1.0 + newSnowTerm + windTerm + warmTerm + pwlTerm + loadingTerm, 1.0, 5.0);
}

DangerLevel RegionalDanger(const SnowpackState& snow) {
  const int level = static_cast<int>(std::floor(DangerIndex(snow) + 0.5));
  return static_cast<DangerLevel>(std::clamp(level, 1, 5));
}

double NaturalReleaseProbabilityPerHour(const SnowpackState& snow, const SlopeInfo& slope) {
  const double d = DangerIndex(snow);
  double rate = NaturalBaseRatePerHour(d) * SlopeAngleFactor(slope.angleDeg);
  if (slope.leeward) rate *= 1.0 + 2.0 * Clamp01(snow.windLoading01);  // drifted slabs on lee slopes
  if (slope.convex) rate *= 1.5;                                        // tensile stress at rolls
  // Sun-facing slopes release wet snow when warming is strong.
  rate *= 1.0 + 2.0 * Clamp01(snow.warming01) * SunExposure(slope.aspectDeg);
  return Clamp01(1.0 - std::exp(-rate));
}

double HumanTriggerProbability(const SnowpackState& snow, const SlopeInfo& slope, int partySize, bool spacedOut) {
  const double angle = SlopeAngleFactor(slope.angleDeg);
  if (angle <= 0.0 || partySize <= 0) return 0.0;
  const double d = DangerIndex(snow);
  double perPerson = PerPersonTriggerProbability(d) * angle;
  if (slope.leeward) perPerson *= 1.5 + Clamp01(snow.windLoading01);
  if (slope.convex) perPerson *= 1.5;
  // Persistent weak layers propagate fractures and can be triggered remotely by a light load.
  perPerson *= 1.0 + 0.5 * Clamp01(snow.persistentWeakLayer01);
  perPerson *= 1.0 + 0.5 * Clamp01(snow.warming01) * SunExposure(slope.aspectDeg);
  // Bunched climbers stress the weak layer with their combined load; crossing one at a time keeps
  // the load to one person (Munter: spacing reduces risk by ~2x).
  const int n = partySize;
  const double loadMultiplier = spacedOut ? 1.0 : std::min(1.0 + 0.25 * (n - 1), 2.5);
  const double pEach = Clamp(perPerson * loadMultiplier, 0.0, 0.95);
  return Clamp01(1.0 - std::pow(1.0 - pEach, static_cast<double>(n)));
}

void SnowpackModel::Step(double dtSeconds, const WeatherSample& weather, double windExposure01) {
  if (!(dtSeconds > 0.0)) return;
  // Integrate in chunks that never cross an hour boundary, so the ring buffers stay exact and large
  // steps (fast-forward) behave like many small ones.
  double remaining = dtSeconds;
  while (remaining > 0.0) {
    const double chunk = std::min(remaining, kSecondsPerHour - hourAccumulatorS_);
    StepChunk(chunk, weather, windExposure01);
    hourAccumulatorS_ += chunk;
    remaining -= chunk;
    if (hourAccumulatorS_ >= kSecondsPerHour - 1e-6) {
      hourAccumulatorS_ = 0.0;
      hourIndex_ = (hourIndex_ + 1) % 72;
      snow72Accum_[hourIndex_] = 0.0;      // oldest hour drops out of the 72 h window
      snow24Accum_[hourIndex_ % 24] = 0.0;  // and out of the 24 h window
      currentHourSnowCm_ = 0.0;
    }
  }
  double sum24 = 0.0;
  double sum72 = 0.0;
  for (double v : snow24Accum_) sum24 += v;
  for (double v : snow72Accum_) sum72 += v;
  state_.newSnow24hCm = sum24;
  state_.newSnow72hCm = sum72;
}

void SnowpackModel::StepChunk(double dtSeconds, const WeatherSample& weather, double windExposure01) {
  SnowpackState& s = state_;
  const double h = dtSeconds / kSecondsPerHour;
  const double tempC = weather.temperatureC;
  const double wind = std::max(0.0, weather.windSpeedMS);
  const double exposure = Clamp01(windExposure01);
  const double warmth = SmoothStep(-15.0, 0.0, tempC);  // 0 cold .. 1 near melting

  // --- New snow into the ring buffers (the current hour's slot holds the running total) ---------
  const double snowCm = std::max(0.0, weather.snowfallCmH) * h;
  snow24Accum_[hourIndex_ % 24] += snowCm;
  snow72Accum_[hourIndex_] += snowCm;
  currentHourSnowCm_ += snowCm;

  // --- Settlement of new snow: tau ~72 h when very cold, ~20 h near 0 deg C ------------------------
  const double settleTauH = Lerp(72.0, 20.0, warmth);
  const double unsettledBefore = s.unsettledNewSnowCm;
  // Exact solution for a constant snowfall rate over the chunk, so step size does not matter.
  const double settleDecay = std::exp(-h / settleTauH);
  s.unsettledNewSnowCm = unsettledBefore * settleDecay + (h > 0.0 ? snowCm / h * settleTauH * (1.0 - settleDecay) : 0.0);
  // Base depth: new snow adds, settling compacts it by ~30%, and melt removes it above 0 deg C
  // (~1 cm snow per deg C per day, from a degree-day factor of ~4 mm w.e. / deg C / day; Hock 2003).
  const double settled = std::max(0.0, unsettledBefore + snowCm - s.unsettledNewSnowCm);
  const double melt = std::max(0.0, tempC) * h / 24.0;
  s.baseDepthCm = std::max(0.0, s.baseDepthCm + snowCm - 0.3 * settled - melt);

  // --- Wind loading: transport above ~7 m/s (Li & Pomeroy 1997) with loose, dry snow available ----
  const double looseSupply = Clamp01(s.unsettledNewSnowCm / 20.0 + 0.1) * (1.0 - SmoothStep(-3.0, 0.0, tempC));
  const double excess = std::max(0.0, wind - 7.0) / 10.0;
  const double loadRatePerH = Clamp(0.12 * excess * excess * looseSupply * exposure, 0.0, 0.5);
  s.windLoading01 += (1.0 - s.windLoading01) * (1.0 - std::exp(-loadRatePerH * h));
  // Wind slabs bond over days: tau ~5 days when cold, ~1.5 days near 0 deg C.
  s.windLoading01 = Clamp01(s.windLoading01 * std::exp(-h / Lerp(120.0, 36.0, warmth)));

  // --- Time since loading -------------------------------------------------------------------------
  const bool loading = weather.snowfallCmH > 0.2 || loadRatePerH > 0.02;
  s.hoursSinceLoading = loading ? 0.0 : std::min(s.hoursSinceLoading + h, 1.0e6);

  // --- Warming (wet-snow instability): air above ~-2 deg C, strong sun, or a rapid temperature rise ---
  if (!tempEmaValid_) {
    tempEmaC_ = tempC;
    tempEmaValid_ = true;
  }
  tempEmaC_ = ApproachExp(tempEmaC_, tempC, dtSeconds, 24.0 * kSecondsPerHour);
  const double fromTemp = SmoothStep(-2.0, 3.0, tempC);
  const double fromSun = 0.8 * SmoothStep(500.0, 1000.0, weather.solarWm2) * SmoothStep(-10.0, -2.0, tempC);
  const double fromRapidRise = 0.7 * SmoothStep(3.0, 8.0, tempC - tempEmaC_) * SmoothStep(-12.0, -3.0, tempC);
  const double warmTarget = std::max({fromTemp, fromSun, fromRapidRise});
  // Warms up within a couple of hours; refreezing (e.g. overnight) takes longer.
  const double warmTauS = (warmTarget > s.warming01 ? 2.0 : 8.0) * kSecondsPerHour;
  s.warming01 = Clamp01(ApproachExp(s.warming01, warmTarget, dtSeconds, warmTauS));

  // --- Surface hoar: grows on clear, calm, humid nights; destroyed by wind and sun; buried -> PWL ----
  const double night = weather.solarWm2 < 20.0 ? 1.0 : 0.0;
  const double hoarGrowth = night * (1.0 - SmoothStep(0.3, 0.6, weather.cloudCover01)) * (1.0 - SmoothStep(2.0, 5.0, wind)) *
                            SmoothStep(0.4, 0.8, weather.relativeHumidity01) * (tempC < -2.0 ? 1.0 : 0.0);
  s.surfaceHoar01 += (1.0 - s.surfaceHoar01) * (1.0 - std::exp(-0.08 * hoarGrowth * h));
  s.surfaceHoar01 *= std::exp(-h * (0.5 * SmoothStep(5.0, 10.0, wind) + 0.3 * s.warming01));
  const double buried = s.surfaceHoar01 * (1.0 - std::exp(-snowCm / 3.0));
  s.surfaceHoar01 = Clamp01(s.surfaceHoar01 - buried);
  s.persistentWeakLayer01 += 0.8 * buried * (1.0 - s.persistentWeakLayer01);

  // Faceting in a thin, very cold snowpack (strong temperature gradient): ~0.02 per day.
  const double facetRatePerH =
      (0.02 / 24.0) * (1.0 - SmoothStep(-25.0, -10.0, tempC)) * (1.0 - SmoothStep(80.0, 150.0, s.baseDepthCm));
  s.persistentWeakLayer01 += facetRatePerH * h * (1.0 - s.persistentWeakLayer01);

  // Persistent weak layers heal very slowly: tau ~45 days cold, ~20 days near 0 deg C
  // (Jamieson & Schweizer 2000).
  const double healTauH = Lerp(45.0, 20.0, warmth) * 24.0;
  s.persistentWeakLayer01 = Clamp01(s.persistentWeakLayer01 * std::exp(-h / healTauH));
}

}  // namespace summit::sim
