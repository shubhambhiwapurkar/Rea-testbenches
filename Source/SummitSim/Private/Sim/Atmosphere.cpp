#include "Sim/Atmosphere.h"

#include <cmath>

#include "Sim/Constants.h"
#include "Sim/Math.h"

namespace summit::sim {

namespace {

// West (1996) model-atmosphere coefficients, PB in Torr and h in km.
constexpr double kWestA = 6.63268;
constexpr double kWestB = 0.1112;
constexpr double kWestC = 0.00149;

// Latitude correction. Pressure at altitude falls toward the poles because the troposphere is
// colder and shallower there (West 1996; West 1983 "Barometric pressures on Mt. Everest";
// Denali 6190 m, 63°N: observed ≈ 349 Torr ≈ 46.5 kPa against ≈ 361 Torr predicted at 28°N).
// Deficit fraction = kLatitudeDeficitPerKm * h[km] * g(lat), g = 0 at 28°, 1 at 63°.
constexpr double kLatitudeDeficitPerKm = 0.0055;
constexpr double kReferenceLatitudeDeg = 28.0;
constexpr double kDenaliLatitudeDeg = 63.0;

// Seasonal correction. On Everest the summit pressure swings from ~245 Torr in midwinter to
// ~255 Torr in midsummer (West 1999, J Appl Physiol 86:1062-1066, radiosonde climatology).
// The West equation is anchored to May/June, so the correction is zero on day 140.
constexpr double kSeasonalAmplitudePerKm = 0.02 / 8.848;  // fractional, per km of altitude
constexpr double kPressurePeakDay = 200.0;               // mid-July (northern hemisphere)
constexpr double kWestReferenceDay = 140.0;              // 20 May

double SinSq(double deg) {
  const double s = std::sin(DegToRad(deg));
  return s * s;
}

}  // namespace

double BarometricPressureKPa(double altitudeM, double latitudeDeg, int dayOfYear) {
  const double h = Clamp(altitudeM, -500.0, 12000.0) / 1000.0;
  const double pbTorr = std::exp(kWestA - kWestB * h - kWestC * h * h);
  double kPa = pbTorr / kMmHgPerKPa;

  if (h <= 0.0) return kPa;

  // Latitude: smooth in sin^2(latitude), slightly higher pressure in the tropics, lower poleward.
  const double absLat = std::fabs(latitudeDeg);
  const double g = Clamp((SinSq(absLat) - SinSq(kReferenceLatitudeDeg)) /
                             (SinSq(kDenaliLatitudeDeg) - SinSq(kReferenceLatitudeDeg)),
                         -0.4, 1.5);
  const double latitudeFactor = 1.0 - kLatitudeDeficitPerKm * h * g;

  // Season: southern hemisphere is shifted by half a year.
  const double peakDay = latitudeDeg >= 0.0 ? kPressurePeakDay : kPressurePeakDay - 182.5;
  const double phase = std::cos(2.0 * kPi * (dayOfYear - peakDay) / 365.0);
  const double refPhase = std::cos(2.0 * kPi * (kWestReferenceDay - peakDay) / 365.0);
  // Seasonal swing scales with latitude: negligible in the deep tropics.
  const double seasonLatScale = Clamp(absLat / kReferenceLatitudeDeg, 0.0, 1.5);
  const double seasonFactor = 1.0 + kSeasonalAmplitudePerKm * h * seasonLatScale * (phase - refPhase);

  kPa *= latitudeFactor * seasonFactor;
  return kPa;
}

double AltitudeForPressureKPa(double pressureKPa) {
  // Analytic inverse of the West (1996) quadratic: c h^2 + b h + (ln PB - a) = 0.
  const double pb = std::fmax(pressureKPa, 1e-3) * kMmHgPerKPa;
  const double k = std::log(pb) - kWestA;
  const double disc = kWestB * kWestB - 4.0 * kWestC * k;
  if (disc <= 0.0) return 12000.0;  // beyond the vertex of the fit; far above any summit
  const double hKm = (-kWestB + std::sqrt(disc)) / (2.0 * kWestC);
  return hKm * 1000.0;
}

double IsaPressureKPa(double altitudeM) {
  // ICAO Doc 7488 / ISO 2533: troposphere lapse 6.5 K/km to 11 km, isothermal 216.65 K above.
  constexpr double kT0 = 288.15;
  constexpr double kLapse = 0.0065;
  constexpr double kExponent = kGravity / (kGasConstantDryAir * kLapse);  // ≈ 5.2559
  constexpr double kTropopauseM = 11000.0;
  constexpr double kTropopauseT = kT0 - kLapse * kTropopauseM;  // 216.65 K
  if (altitudeM <= kTropopauseM) {
    return kSeaLevelPressureKPa * std::pow(1.0 - kLapse * altitudeM / kT0, kExponent);
  }
  const double p11 = kSeaLevelPressureKPa * std::pow(kTropopauseT / kT0, kExponent);
  return p11 * std::exp(-kGravity * (altitudeM - kTropopauseM) / (kGasConstantDryAir * kTropopauseT));
}

double AirDensityKgM3(double pressureKPa, double temperatureC) {
  const double tK = std::fmax(temperatureC + kCelsiusToKelvin, 1.0);
  return pressureKPa * 1000.0 / (kGasConstantDryAir * tK);
}

double InspiredPO2MmHg(double pressureKPa, double fio2) {
  const double pb = pressureKPa * kMmHgPerKPa;
  return std::fmax(0.0, fio2 * (pb - kBodyWaterVapourPressureMmHg));
}

double WaterBoilingPointC(double pressureKPa) {
  // Antoine equation for water, 1-100 °C (NIST / Dortmund Data Bank constants):
  //   log10(P[mmHg]) = 8.07131 - 1730.63 / (233.426 + T)
  const double pMmHg = std::fmax(pressureKPa, 0.01) * kMmHgPerKPa;
  return 1730.63 / (8.07131 - std::log10(pMmHg)) - 233.426;
}

}  // namespace summit::sim
