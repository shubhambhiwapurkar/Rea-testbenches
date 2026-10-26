// Seeded, deterministic mountain weather for the Khumbu / high Himalaya.
//
// Sources for the models and calibration targets used below:
//  - Moore GWK & Semple JL (2006) "Weather and death on Mount Everest: an analysis of the into thin
//    air storm", BAMS 87:465-480; Moore & Semple (2009) "The impact of global warming on Mount
//    Everest", Geophys Res Lett 36: summit winds track the ~300 hPa jet; winter jet 40-80 m/s,
//    weakening in May (northward jump of the subtropical jet before monsoon onset) and in October.
//  - Matthews T et al. (2020) "Going to extremes: installing the world's highest weather stations
//    on Mount Everest", One Earth 3:529-532, and Perry LB et al. (2020) "An extreme-altitude weather
//    station network in the Everest region", BAMS: Balcony (8430 m) mean May temperature ~ -20 to
//    -25 deg C, summer ~ -10 to -15 deg C; free-air lapse rates of ~5-6.5 deg C/km.
//  - Bollasina M, Bertolani L, Tartari G (2002) "Meteorological observations at high altitude in
//    the Khumbu Valley, Nepal Himalayas, 1994-1999", Bull Glaciol Res 19:1-11 (Pyramid, 5050 m):
//    seasonal temperature cycle, diurnal ranges, afternoon up-valley wind and convective cloud.
//  - Salerno F et al. (2015) "Weak precipitation, warm winters and springs impact glaciers of south
//    slopes of Mt. Everest", The Cryosphere 9:1229-1247: ~450-550 mm/yr near EBC, ~80% in monsoon.
//  - Seko K (1987) "Seasonal variation of altitudinal dependence of precipitation in Langtang
//    Valley, Nepal Himalayas", Bull Glacier Res 5:41-47: precipitation decreases above ~5 km.
//  - Cooper PI (1969) "The absorption of radiation in solar stills", Solar Energy 12:333-346
//    (declination); Meinel AB & Meinel MP (1976) "Applied Solar Energy" (clear-sky direct beam,
//    I = S0 * 0.7^(AM^0.678)) with a pressure-corrected air mass (Kasten & Young 1989);
//    Kasten F & Czeplak G (1980) "Solar and terrestrial radiation dependent on the amount and type
//    of cloud", Solar Energy 24:177-189 (G = Gclear * (1 - 0.75 C^3.4)).
//  - Roebber PJ et al. (2003) "Improving snowfall forecasting by diagnosing snow density",
//    Weather and Forecasting 18:264-287 (snow-to-liquid ratio ~10:1 on average, lower when warm).
//  - Rasmussen RM et al. (1999) "The estimation of snowfall rate using visibility", J Appl Meteorol
//    38:1542-1563 (visibility in falling snow); Koschmieder (1924): V = 3.912 / extinction, so
//    extinction coefficients of independent obscurants add.
//  - Li L & Pomeroy JW (1997) "Estimates of threshold wind speeds for snow transport using
//    meteorological data", J Appl Meteorol 36:205-213 (blowing snow starts at ~7-10 m/s);
//    Mellor M (1966) "Light scattering and particle aggregation in snow storms" (visibility of tens
//    of metres in dense blowing snow).
//  - Forecast skill: ECMWF medium-range verification (anomaly correlation ~0.9 at day 1, ~0.6-0.7
//    at day 3-4, falling toward ~0.3 by day 7 in the extratropics/mountains) informs the confidence
//    curve; error blends toward climatology with lead time.

#include "Sim/Weather.h"

#include <algorithm>
#include <cmath>

#include "Sim/Constants.h"
#include "Sim/Math.h"

namespace summit::sim {

namespace {

constexpr int kMonthLengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
constexpr int kMonthStarts[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
constexpr double kDaysPerYear = 365.0;

// Noise channels: each stochastic field draws from its own seeded stream.
enum NoiseChannel : std::uint64_t {
  kChanJetSlow = 1,
  kChanJetFast,
  kChanTempSlow,
  kChanTempFast,
  kChanCloud,
  kChanPrecip,
  kChanWind,
  kChanGust,
  kChanDirSlow,
  kChanDirMonsoon,
  kChanStormRng,
  kChanForecast,
};

// SplitMix64 finaliser (Steele et al. 2014), used only to derive Pcg32 seeds from (seed, channel, index).
std::uint64_t Mix64(std::uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

// Deterministic uniform value in [-1, 1) for an integer lattice point.
double LatticeValue(std::uint64_t seed, std::uint64_t channel, std::int64_t index) {
  Pcg32 rng(Mix64(seed ^ Mix64(channel * 0x632BE59BD9B4E019ULL + static_cast<std::uint64_t>(index))), channel);
  return 2.0 * rng.NextDouble() - 1.0;
}

// Smooth 1D value noise: lattice values joined by Hermite interpolation. Stateless, so any time can
// be evaluated directly (cheap per frame, exact for large jumps and for forecasts).
double ValueNoise(std::uint64_t seed, std::uint64_t channel, double x) {
  const double fl = std::floor(x);
  const auto i = static_cast<std::int64_t>(fl);
  const double f = x - fl;
  const double a = LatticeValue(seed, channel, i);
  const double b = LatticeValue(seed, channel, i + 1);
  const double s = f * f * (3.0 - 2.0 * f);
  return a + (b - a) * s;
}

double FractionalDayOfYear(SimTimeSeconds t) {
  double d = std::fmod(t / kSecondsPerDay, kDaysPerYear);
  if (d < 0.0) d += kDaysPerYear;
  return d;
}

// Linear interpolation of a monthly-mean table between mid-month points (cyclic), so the climate
// changes smoothly through the year instead of stepping at month boundaries.
double MonthlyInterp(const std::array<double, 12>& table, double doyF) {
  double mid[12];
  for (int m = 0; m < 12; ++m) mid[m] = kMonthStarts[m] + 0.5 * kMonthLengths[m];
  int lo = 11;
  for (int m = 0; m < 12; ++m) {
    if (doyF >= mid[m]) lo = m;
  }
  const int hi = (lo + 1) % 12;
  double span = mid[hi] - mid[lo];
  double x = doyF - mid[lo];
  if (span <= 0.0) span += kDaysPerYear;
  if (x < 0.0) x += kDaysPerYear;
  return Lerp(table[lo], table[hi], Clamp01(x / span));
}

// Diurnal temperature shape in [-1, 1]: minimum near dawn (06:00 solar), maximum early afternoon
// (14:00), cosine rise over 8 h and cosine fall over 16 h. Zero mean over a day.
double DiurnalShape(double hour) {
  if (hour >= 6.0 && hour <= 14.0) return -std::cos(kPi * (hour - 6.0) / 8.0);
  const double hh = hour < 6.0 ? hour + 24.0 : hour;
  return std::cos(kPi * (hh - 14.0) / 16.0);
}

// Fraction of the ~300 hPa (jet-level) wind felt at a given altitude. Near-surface winds in the
// valleys, rising steeply through the free troposphere so that the summit (8848 m) sees ~0.99 of the
// jet speed (Moore & Semple 2006 show summit winds closely track 300 hPa winds).
double JetProfileFraction(double altitudeM) { return 0.03 + 0.97 * SmoothStep(3500.0, 9200.0, altitudeM); }

// Snow-to-liquid ratio (cm snow per cm water). ~10:1 average (Roebber et al. 2003); ~7:1 near
// 0 deg C (wet, dense), up to ~18:1 in very cold, dry conditions.
double SnowToLiquidRatio(double tempC) {
  if (tempC >= -5.0) return Clamp(10.0 - 0.6 * (tempC + 5.0), 6.0, 10.0);
  return Clamp(10.0 + 0.5 * (-5.0 - tempC), 10.0, 18.0);
}

// Storm-cell shape: smooth build, plateau, smooth decay (0..1).
double StormShape(const WeatherSystem::StormCell& c, SimTimeSeconds t) {
  const double x = (t - c.start) / kSecondsPerHour;
  if (x <= 0.0) return 0.0;
  if (x < c.buildHours) return SmoothStep(0.0, c.buildHours, x);
  const double afterPeak = x - c.buildHours - c.peakHours;
  if (afterPeak <= 0.0) return 1.0;
  if (afterPeak < c.decayHours) return 1.0 - SmoothStep(0.0, c.decayHours, afterPeak);
  return 0.0;
}

// Storm-cell duration parameters (hours) and their means; used to calibrate precipitation totals.
constexpr double kBuildMinH = 6.0, kBuildMaxH = 24.0;
constexpr double kPeakMinH = 6.0, kPeakMaxH = 30.0;
constexpr double kDecayMinH = 8.0, kDecayMaxH = 30.0;
constexpr double kIntensityMin = 0.2;
// Mean over storms of the integral of (intensity*shape)^1.3 dt in hours for the duration and
// intensity distributions above (Monte Carlo over 2e4 cells: 17.6 h). Used so that storm cells plus
// background precipitation reproduce the monthly climatology.
constexpr double kStormPrecipIntegralH = 17.6;
// Mean of the precipitation-occurrence weight produced by ValueNoise through the SmoothStep below
// (measured numerically over 2e6 samples: 0.252).
constexpr double kMeanPrecipOccurrence = 0.252;

double PrecipOccurrence(double noise) { return SmoothStep(0.1, 0.7, noise); }

double StormsPerDay(const ClimateProfile& c, double doyF) {
  return std::max(0.0, MonthlyInterp(c.stormsPerWeek, doyF)) / 7.0;
}

// Expected storm intensity for a climatological (no-skill) forecast: rate x mean duration x mean
// intensity, saturating.
double ClimatologicalStorm(const ClimateProfile& c, double doyF) {
  const double perDay = StormsPerDay(c, doyF);
  return Clamp(perDay * (35.0 / 24.0) * 0.6, 0.0, 0.6);
}

double LapseRate(const ClimateProfile& c, double doyF) {
  bool useMonthly = true;
  for (double v : c.lapseRateCPerKmByMonth) {
    if (v <= 0.0) useMonthly = false;
  }
  return useMonthly ? MonthlyInterp(c.lapseRateCPerKmByMonth, doyF) : c.lapseRateCPerKm;
}

double Extinction(double visibilityM) { return 3.912 / std::max(visibilityM, 1.0); }

constexpr double kMaxVisibilityM = 20000.0;

}  // namespace

int DayOfYear(SimTimeSeconds t) {
  const auto day = static_cast<long long>(std::floor(t / kSecondsPerDay));
  const long long d = ((day % 365) + 365) % 365;
  return static_cast<int>(d);
}

double HourOfDay(SimTimeSeconds t) {
  const double dayStart = std::floor(t / kSecondsPerDay) * kSecondsPerDay;
  return Clamp((t - dayStart) / kSecondsPerHour, 0.0, 24.0);
}

int MonthOfYear(SimTimeSeconds t) {
  const int doy = DayOfYear(t);
  int month = 0;
  for (int m = 0; m < 12; ++m) {
    if (doy >= kMonthStarts[m]) month = m;
  }
  return month;
}

ClimateProfile KhumbuClimate() {
  ClimateProfile c;
  c.latitudeDeg = 27.98;            // Everest Base Camp (south side)
  c.referenceElevationM = 5364.0;
  // Daily mean at EBC. Winter -15..-18, pre-monsoon (Apr-May) -5..-2, monsoon 0..+2 deg C
  // (Bollasina et al. 2002 scaled from Pyramid 5050 m; Matthews et al. 2020 Base Camp AWS).
  c.meanTempC = {-16.5, -15.0, -11.0, -5.5, -2.5, 0.5, 1.5, 1.2, -0.5, -6.0, -11.5, -15.0};
  // Max-min at EBC: clear dry winter ~13 deg C, cloudy monsoon ~5 deg C (Bollasina et al. 2002).
  c.diurnalRangeC = {13.0, 13.0, 12.0, 11.0, 10.0, 7.0, 5.0, 5.0, 6.0, 10.0, 12.0, 13.0};
  // Mean ~300 hPa wind over Everest (Moore & Semple 2006, ERA reanalysis): strong winter westerly
  // jet, collapsing in late May as the subtropical jet jumps north of the Tibetan Plateau, minimum
  // in monsoon, returning through October-November.
  c.jetStreamWindMS = {42.0, 45.0, 40.0, 33.0, 22.0, 12.0, 9.0, 9.0, 12.0, 20.0, 32.0, 38.0};
  // Synoptic disturbances: western disturbances in winter/spring, monsoon depressions in summer.
  c.stormsPerWeek = {0.5, 0.6, 0.8, 0.8, 0.9, 1.8, 2.2, 2.2, 1.6, 0.6, 0.35, 0.4};
  // Water-equivalent precipitation at EBC (Salerno et al. 2015: ~500 mm/yr, ~80% Jun-Sep).
  c.precipitationMmPerDay = {0.3, 0.5, 0.6, 0.5, 0.8, 2.8, 4.0, 3.8, 2.5, 0.7, 0.1, 0.2};
  c.lapseRateCPerKm = 6.0;
  // Steeper (drier) lapse rates pre-monsoon, near-moist-adiabatic in the monsoon. Calibrated so the
  // summit (8848 m) mean is ~ -36 deg C in January, ~ -24 deg C in May and ~ -16 deg C in July
  // (Moore & Semple 2006; Perry et al. 2020 Balcony/South Col AWS).
  c.lapseRateCPerKmByMonth = {5.8, 5.9, 6.1, 6.3, 6.3, 5.6, 5.0, 5.0, 5.5, 6.0, 5.9, 5.8};
  c.prevailingWindFromDeg = 280.0;  // west-northwesterly jet over the summit
  return c;
}

double SolarElevationDeg(double latitudeDeg, int dayOfYear, double hourOfDay) {
  // Cooper (1969): declination = 23.45 sin(360 (284 + n) / 365), n = 1..365.
  const double n = static_cast<double>(dayOfYear) + 1.0;
  const double decl = DegToRad(23.45) * std::sin(2.0 * kPi * (284.0 + n) / 365.0);
  const double hourAngle = DegToRad(15.0 * (hourOfDay - 12.0));
  const double lat = DegToRad(latitudeDeg);
  const double sinElev = std::sin(lat) * std::sin(decl) + std::cos(lat) * std::cos(decl) * std::cos(hourAngle);
  return RadToDeg(std::asin(Clamp(sinElev, -1.0, 1.0)));
}

double ClearSkyGlobalIrradianceWm2(double latitudeDeg, int dayOfYear, double hourOfDay, double altitudeM) {
  const double elevDeg = SolarElevationDeg(latitudeDeg, dayOfYear, hourOfDay);
  if (elevDeg <= 0.0) return 0.0;
  const double elev = DegToRad(elevDeg);
  // Extraterrestrial irradiance with orbital eccentricity (solar constant 1361 W/m^2).
  const double s0 = 1361.0 * (1.0 + 0.033 * std::cos(2.0 * kPi * (dayOfYear + 1.0) / 365.0));
  // Relative optical air mass (Kasten & Young 1989), corrected for station pressure using a
  // ~8.4 km scale height: the thin atmosphere at altitude passes much more of the beam.
  const double zenithDeg = 90.0 - elevDeg;
  const double airMass = 1.0 / (std::cos(DegToRad(zenithDeg)) + 0.50572 * std::pow(96.07995 - zenithDeg, -1.6364));
  const double pressureRatio = std::exp(-std::max(altitudeM, 0.0) / 8434.0);
  const double beamNormal = s0 * std::pow(0.7, std::pow(airMass * pressureRatio, 0.678));  // Meinel & Meinel
  // Diffuse sky adds ~10% at sea level and less in the thin, clean air at altitude.
  const double diffuseFactor = Lerp(1.10, 1.03, Clamp01(altitudeM / 9000.0));
  return std::max(0.0, diffuseFactor * beamNormal * std::sin(elev));
}

WeatherSystem::WeatherSystem(const ClimateProfile& climate, std::uint64_t seed)
    : climate_(climate), seed_(seed), rng_(Mix64(seed ^ 0xA5A5A5A5DEADBEEFULL), kChanStormRng) {
  // Random offset into the jet-stream noise so different seeds have different window timing.
  jetPhase_ = rng_.Uniform(0.0, 10000.0);
  // Start the storm schedule three days in the past so t = 0 is already in a statistically steady
  // state (a storm may be in progress).
  scheduledUntil_ = -3.0 * kSecondsPerDay;
  ScheduleStormsUntil(0.0);
}

void WeatherSystem::ScheduleStormsUntil(SimTimeSeconds t) {
  // Storms are generated one calendar day at a time from the sequential RNG, so the schedule depends
  // only on the seed and never on how the caller chunks AdvanceTo (frame steps vs. day jumps).
  while (scheduledUntil_ <= t) {
    const SimTimeSeconds dayStart = scheduledUntil_;
    const double doyF = FractionalDayOfYear(dayStart + 0.5 * kSecondsPerDay);
    const double lambda = StormsPerDay(climate_, doyF);
    // Poisson count for this day (Knuth's multiplication method).
    const double limit = std::exp(-lambda);
    int count = 0;
    double p = rng_.NextDouble();
    while (p > limit && count < 8) {
      ++count;
      p *= rng_.NextDouble();
    }
    for (int i = 0; i < count; ++i) {
      StormCell cell;
      cell.start = dayStart + rng_.NextDouble() * kSecondsPerDay;
      cell.buildHours = rng_.Uniform(kBuildMinH, kBuildMaxH);
      cell.peakHours = rng_.Uniform(kPeakMinH, kPeakMaxH);
      cell.decayHours = rng_.Uniform(kDecayMinH, kDecayMaxH);
      cell.intensity01 = kIntensityMin + (1.0 - kIntensityMin) * rng_.NextDouble();
      storms_.push_back(cell);
    }
    scheduledUntil_ = dayStart + kSecondsPerDay;
  }
}

void WeatherSystem::AdvanceTo(SimTimeSeconds t) {
  if (t <= time_) return;  // non-decreasing; ignore stale calls
  time_ = t;
  ScheduleStormsUntil(t);
  // Drop storms that ended more than two days ago (recent storms still feed blowing-snow supply).
  std::size_t keep = 0;
  for (std::size_t i = 0; i < storms_.size(); ++i) {
    const StormCell& c = storms_[i];
    const double endS = c.start + (c.buildHours + c.peakHours + c.decayHours) * kSecondsPerHour;
    if (endS > time_ - 2.0 * kSecondsPerDay) storms_[keep++] = c;
  }
  storms_.resize(keep);
}

double WeatherSystem::StormIntensityAt(SimTimeSeconds t) const {
  // Overlapping cells combine like independent probabilities: 1 - prod(1 - s_i), saturating at 1.
  double clear = 1.0;
  for (const StormCell& c : storms_) clear *= 1.0 - Clamp01(c.intensity01 * StormShape(c, t));
  return 1.0 - clear;
}

double WeatherSystem::JetStreamFactorAt(SimTimeSeconds t) const {
  // Multi-day jet oscillation (Rossby-wave passages / jet meanders): a slow component with ~3-day
  // lattice spacing (troughs and ridges every ~6 days, lulls of 2-5 days) plus a faster ~20 h one.
  const double h = t / kSecondsPerHour + jetPhase_;
  const double slow = ValueNoise(seed_, kChanJetSlow, h / 72.0);
  const double fast = ValueNoise(seed_, kChanJetFast, h / 20.0);
  return Clamp(1.0 + 0.6 * slow + 0.15 * fast, 0.2, 1.9);
}

WeatherSample WeatherSystem::Sample(double altitudeM, double exposure01) const {
  return Evaluate(time_, altitudeM, exposure01, false);
}

WeatherSample WeatherSystem::Evaluate(SimTimeSeconds t, double altitudeM, double exposure01,
                                      bool climatological) const {
  const ClimateProfile& c = climate_;
  const double tH = t / kSecondsPerHour;
  const double doyF = FractionalDayOfYear(t);
  const double hour = HourOfDay(t);
  const double noiseScale = climatological ? 0.0 : 1.0;
  const double exposure = Clamp01(exposure01);
  const double dz = altitudeM - c.referenceElevationM;

  const double storm = climatological ? ClimatologicalStorm(c, doyF) : StormIntensityAt(t);
  const double jetFactor = climatological ? 1.0 : JetStreamFactorAt(t);
  // Recent storms leave loose snow for the wind to pick up.
  const double recentStorm =
      climatological ? storm
                     : std::max({storm, 0.8 * StormIntensityAt(t - 12.0 * kSecondsPerHour),
                                 0.6 * StormIntensityAt(t - 24.0 * kSecondsPerHour),
                                 0.4 * StormIntensityAt(t - 36.0 * kSecondsPerHour)});

  WeatherSample s;
  s.stormIntensity01 = storm;

  // ---- Cloud ---------------------------------------------------------------------------------
  const double climPrecip = std::max(0.0, MonthlyInterp(c.precipitationMmPerDay, doyF));
  const double climTempRef = MonthlyInterp(c.meanTempC, doyF);
  const double baseCloud = Clamp(0.10 + 0.11 * climPrecip, 0.10, 0.75);
  // Afternoon convective cloud (Bollasina et al. 2002): clear mornings, cloud building by noon in the
  // pre-monsoon and monsoon; weak in the cold, dry winter.
  const double convectiveAmp = 0.35 * SmoothStep(-12.0, -3.0, climTempRef);
  const double convectiveBump = std::exp(-((hour - 14.0) / 3.0) * ((hour - 14.0) / 3.0));
  const double precipNoise = ValueNoise(seed_, kChanPrecip, tH / 10.0);
  const double occurrence = climatological ? kMeanPrecipOccurrence : PrecipOccurrence(precipNoise);
  const double wetSeason = SmoothStep(0.3, 2.0, climPrecip);
  const double cloudBg = Clamp01(baseCloud + convectiveAmp * convectiveBump +
                                 0.15 * noiseScale * ValueNoise(seed_, kChanCloud, tH / 18.0) +
                                 0.4 * occurrence * wetSeason);
  s.cloudCover01 = Clamp01(cloudBg + (1.0 - cloudBg) * SmoothStep(0.05, 0.6, storm));

  // ---- Temperature ---------------------------------------------------------------------------
  const double lapse = LapseRate(c, doyF);
  const double meanAtAlt = climTempRef - lapse * dz / 1000.0;
  // The diurnal range shrinks with height above the valley boundary layer toward the free
  // atmosphere (~3-4 deg C at the summit), and with cloud: clear nights are colder, clear days warmer.
  const double altDamp = Clamp(1.0 - 0.17 * dz / 1000.0, 0.35, 1.5);
  const double cloudAmp = Lerp(1.25, 0.4, s.cloudCover01);
  const double diurnal = 0.5 * MonthlyInterp(c.diurnalRangeC, doyF) * altDamp * cloudAmp * DiurnalShape(hour);
  const double anomaly = noiseScale * (3.0 * ValueNoise(seed_, kChanTempSlow, tH / 60.0) +
                                       0.8 * ValueNoise(seed_, kChanTempFast, tH / 10.0));
  const double stormCooling = -3.0 * storm;
  s.temperatureC = meanAtAlt + diurnal + anomaly + stormCooling;

  // ---- Wind ----------------------------------------------------------------------------------
  const double jet = std::max(0.0, MonthlyInterp(c.jetStreamWindMS, doyF)) * jetFactor;
  const double frac = JetProfileFraction(altitudeM);
  // Valley/glacier winds near the surface with an afternoon up-valley maximum (Bollasina 2002).
  const double surfaceWind = 2.5 + 3.0 * convectiveBump;
  const double exposureFactor = 0.3 + 0.7 * exposure;  // sheltered couloir still feels ~30%
  const double stormWind = storm * (6.0 + 14.0 * frac);
  const double windNoise = 1.0 + 0.1 * noiseScale * ValueNoise(seed_, kChanWind, tH / 3.0);
  s.windSpeedMS = std::max(0.0, (surfaceWind * (1.0 - frac) + jet * frac + stormWind) * exposureFactor * windNoise);
  const double gustFactor =
      Clamp(1.4 + 0.1 * storm + 0.1 * noiseScale * ValueNoise(seed_, kChanGust, tH * 2.0), 1.3, 1.6);
  s.windGustMS = s.windSpeedMS * gustFactor;
  const double monsoonVariability = SmoothStep(1.0, 3.0, climPrecip);
  double dir = c.prevailingWindFromDeg + noiseScale * (30.0 * ValueNoise(seed_, kChanDirSlow, tH / 36.0) +
                                                       100.0 * monsoonVariability *
                                                           ValueNoise(seed_, kChanDirMonsoon, tH / 24.0));
  dir = std::fmod(dir, 360.0);
  if (dir < 0.0) dir += 360.0;
  s.windDirectionDeg = dir;

  // ---- Precipitation -------------------------------------------------------------------------
  // Storm cells deliver most of the dry-season precipitation; the remainder of the monthly
  // climatology falls as intermittent background (monsoon showers, afternoon snow showers).
  const double stormsPerDay = StormsPerDay(c, doyF);
  const double stormShareTarget = 0.65;
  double stormPeakMmH = 0.2;
  if (stormsPerDay > 1e-6) {
    stormPeakMmH = std::max(0.2, stormShareTarget * climPrecip / (stormsPerDay * kStormPrecipIntegralH));
  }
  const double expectedStormMmDay = stormsPerDay * kStormPrecipIntegralH * stormPeakMmH;
  const double backgroundMmDay = std::max(0.1 * climPrecip, climPrecip - expectedStormMmDay);
  const double backgroundMmH = backgroundMmDay / 24.0 * occurrence / kMeanPrecipOccurrence;
  const double stormMmH = stormPeakMmH * std::pow(storm, 1.3);
  // Orographic profile: precipitation peaks below ~5 km on the south slopes and drops off above
  // (Seko 1987); a little more below the reference elevation.
  const double altPrecip = dz >= 0.0 ? std::exp(-dz / 4000.0) : Clamp(1.0 - dz / 2000.0, 1.0, 2.5);
  s.precipitationMmH = (backgroundMmH + stormMmH) * altPrecip;
  const double snowFraction = 1.0 - SmoothStep(0.0, 2.0, s.temperatureC);
  // mm water -> cm snow: 1 mm water at a 10:1 ratio is 1 cm of snow.
  s.snowfallCmH = s.precipitationMmH * snowFraction * SnowToLiquidRatio(s.temperatureC) / 10.0;

  // ---- Humidity ------------------------------------------------------------------------------
  s.relativeHumidity01 = Clamp(0.2 + 0.6 * s.cloudCover01 + 0.2 * storm, 0.05, 1.0);

  // ---- Visibility (Koschmieder: extinction coefficients add) ----------------------------------
  double sigma = Extinction(kMaxVisibilityM);
  // In-cloud fog when the mountain is engulfed: a few hundred metres, ~150 m in a storm.
  sigma += SmoothStep(0.7, 1.0, s.cloudCover01) * Extinction(Lerp(800.0, 150.0, storm));
  // Falling snow (Rasmussen et al. 1999): V ~ 1000 m at 1 mm/h water equivalent.
  const double snowWaterMmH = s.precipitationMmH * snowFraction;
  if (snowWaterMmH > 1e-4) sigma += Extinction(1000.0 * std::pow(snowWaterMmH, -0.75));
  const double rainMmH = s.precipitationMmH * (1.0 - snowFraction);
  if (rainMmH > 1e-4) sigma += Extinction(10000.0 * std::pow(rainMmH, -0.6));
  // Blowing snow: loose snow is lifted above ~8 m/s (Li & Pomeroy 1997) and by ~20+ m/s can cut
  // visibility to tens of metres (Mellor 1966). Supply: recent snowfall and the perennially loose
  // high-altitude surface; little drift from warm, wet snow.
  const double looseSnow = Clamp01(0.1 + 0.15 * SmoothStep(5000.0, 7000.0, altitudeM) + 0.75 * recentStorm) *
                           (1.0 - SmoothStep(-3.0, 1.0, s.temperatureC));
  const double blowing = SmoothStep(8.0, 22.0, s.windSpeedMS) * looseSnow;
  sigma += blowing * blowing * Extinction(40.0);
  s.visibilityM = Clamp(3.912 / sigma, 5.0, kMaxVisibilityM);

  // ---- Solar ---------------------------------------------------------------------------------
  const double clearSky = ClearSkyGlobalIrradianceWm2(c.latitudeDeg, DayOfYear(t), hour, altitudeM);
  s.solarWm2 = clearSky * (1.0 - 0.75 * std::pow(s.cloudCover01, 3.4));  // Kasten & Czeplak (1980)

  return s;
}

std::vector<ForecastEntry> WeatherSystem::Forecast(double hoursAhead, double stepHours, double altitudeM) const {
  std::vector<ForecastEntry> out;
  if (!(stepHours > 0.0) || !(hoursAhead > 0.0)) return out;
  const int count = static_cast<int>(std::min(std::floor(hoursAhead / stepHours + 1e-9), 4096.0));
  if (count <= 0) return out;
  out.reserve(static_cast<std::size_t>(count));

  // The truth: a private copy of the system advanced into the future. The real state is untouched
  // and, because the storm schedule and noise are seed-determined, the copy sees exactly the
  // weather that will happen.
  WeatherSystem future = *this;

  // Forecast error, deterministic for (seed, issue time): biases drawn once per forecast (so errors
  // are correlated along the forecast, as in a real model run) plus small per-step noise.
  const auto issueKey = static_cast<std::uint64_t>(std::llround(time_));
  Pcg32 rng(Mix64(seed_ ^ Mix64(issueKey * 0x2545F4914F6CDD1DULL + 0x1234567ULL)), kChanForecast);
  const double tempBias = rng.Normal();
  const double windBias = rng.Normal();
  const double stormBias = rng.Normal();
  const double cloudBias = rng.Normal();

  for (int k = 1; k <= count; ++k) {
    const double leadH = k * stepHours;
    const SimTimeSeconds t = time_ + leadH * kSecondsPerHour;
    future.AdvanceTo(t);
    const WeatherSample truth = future.Evaluate(t, altitudeM, 1.0, false);
    const WeatherSample climo = future.Evaluate(t, altitudeM, 1.0, true);

    // Skill curve: ~0.9 at 24 h, ~0.6 at 72 h, ~0.3 at 7 days.
    const double conf = 1.0 / (1.0 + std::pow(leadH / 95.0, 1.55));
    const double w = 1.0 - conf;  // weight toward climatology and error amplitude

    const double nTemp = rng.Normal();
    const double nWind = rng.Normal();
    const double nStorm = rng.Normal();
    const double nCloud = rng.Normal();

    WeatherSample e;
    e.temperatureC = Lerp(truth.temperatureC, climo.temperatureC, w) + w * (3.0 * tempBias + 1.5 * nTemp);
    const double windMul = std::exp(w * (0.35 * windBias + 0.2 * nWind));
    e.windSpeedMS = std::max(0.0, Lerp(truth.windSpeedMS, climo.windSpeedMS, w) * windMul);
    e.windGustMS = std::max(e.windSpeedMS, Lerp(truth.windGustMS, climo.windGustMS, w) * windMul);
    e.windDirectionDeg = truth.windDirectionDeg;
    e.stormIntensity01 =
        Clamp01(Lerp(truth.stormIntensity01, climo.stormIntensity01, w) + w * (0.2 * stormBias + 0.1 * nStorm));
    const double precipMul = std::exp(w * (0.5 * stormBias + 0.3 * nStorm));
    e.precipitationMmH = std::max(0.0, Lerp(truth.precipitationMmH, climo.precipitationMmH, w) * precipMul);
    e.snowfallCmH = std::max(0.0, Lerp(truth.snowfallCmH, climo.snowfallCmH, w) * precipMul);
    e.cloudCover01 = Clamp01(Lerp(truth.cloudCover01, climo.cloudCover01, w) + w * (0.15 * cloudBias + 0.1 * nCloud));
    e.relativeHumidity01 = Clamp01(Lerp(truth.relativeHumidity01, climo.relativeHumidity01, w));
    // Visibility blends in log space (it spans orders of magnitude).
    e.visibilityM = Clamp(std::exp(Lerp(std::log(truth.visibilityM), std::log(climo.visibilityM), w)), 5.0,
                          kMaxVisibilityM);
    e.solarWm2 = std::max(0.0, Lerp(truth.solarWm2, climo.solarWm2, w));

    ForecastEntry entry;
    entry.time = t;
    entry.expected = e;
    entry.confidence01 = conf;
    out.push_back(entry);
  }
  return out;
}

}  // namespace summit::sim
