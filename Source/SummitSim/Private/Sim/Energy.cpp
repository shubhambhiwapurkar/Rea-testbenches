#include "Sim/Energy.h"

#include <cmath>

#include "Sim/Constants.h"
#include "Sim/Math.h"

namespace summit::sim {

namespace {

constexpr double kHour = kSecondsPerHour;

// 1 L O2 ≈ 20.1 kJ for a mixed diet (Weir 1949; McArdle, Katch & Katch, Exercise Physiology).
constexpr double kJoulesPerLiterO2 = 20100.0;

// Body-tissue energy density for weight change (Wishnofsky 1958: ~7,700 kcal/kg).
constexpr double kKcalPerKgTissue = 7700.0;
// Glycogen is stored with ~3 g water per g; 1 kcal of glycogen ≈ 1 g of body mass.
constexpr double kKgPerKcalGlycogen = 0.001;

// Intake. Normal maximum voluntary intake ~4,000 kcal/day; above ~5,000 m appetite and absorption
// fall (Westerterp et al. 1992 "Energy expenditure climbing Mt Everest"; Boyer & Blume 1984
// "Weight loss and changes in body composition at high altitude", J Appl Physiol 57:1580;
// Kayser 1992). Expeditions typically lose 5-10 kg over two months.
constexpr double kMaxDailyIntakeKcal = 4000.0;
constexpr double kStomachEmptyingTauS = 2.0 * kHour;
constexpr double kMealCarbShare = 0.55;
constexpr double kLipogenesisEfficiency = 0.75;

// Fluids: insensible skin loss ~0.6 L/day, obligatory urine ~1 L/day, metabolic water
// ~0.13 mL/kcal (Institute of Medicine 2005, DRI for water; Consolazio 1968).
constexpr double kInsensibleSkinLPerHour = 0.025;
constexpr double kUrineLPerHour = 0.04;
constexpr double kMetabolicWaterLPerKcal = 0.00013;

// Exhaled air leaves near 35 °C and saturated during mouth breathing in exercise (McCutchan &
// Taylor 1951; Ferrus et al. 1980, "Respiratory water loss", Respir Physiol 39:367).
constexpr double kExhaledTempC = 35.0;

double AbsoluteHumidityGM3(double vapourPressureKPa, double temperatureC) {
  // Ideal gas for water vapour: rho = e M / (R T) = 2166.7 * e[kPa] / T[K] g/m^3.
  return 2166.7 * vapourPressureKPa / (temperatureC + kCelsiusToKelvin);
}

}  // namespace

double PandolfMetabolicRateW(double bodyMassKg, double loadKg, double speedMS, double gradePercent,
                             double terrainFactor) {
  // Pandolf, Givoni & Goldman (1977) J Appl Physiol 43:577-581.
  const double w = std::fmax(bodyMassKg, 1.0);
  const double l = std::fmax(loadKg, 0.0);
  const double v = std::fmax(speedMS, 0.0);
  const double g = gradePercent;
  const double eta = std::fmax(terrainFactor, 0.5);
  const double standing = 1.5 * w + 2.0 * (w + l) * (l / w) * (l / w);
  double m = standing + eta * (w + l) * (1.5 * v * v + 0.35 * v * g);
  if (g < 0.0) {
    // Santee et al. (2003) downhill correction, Aviat Space Environ Med 74:956:
    //   CF = eta [ G (W+L) V / 3.5 - (W+L)(G+6)^2 / W + (25 - V^2) ],  M = Pandolf - CF.
    const double cf = eta * (g * (w + l) * v / 3.5 - (w + l) * (g + 6.0) * (g + 6.0) / w + (25.0 - v * v));
    m -= cf;
  }
  return std::fmax(m, standing);
}

double MetabolicRateToVO2MlKgMin(double metabolicW, double bodyMassKg) {
  const double litersPerMin = metabolicW * 60.0 / kJoulesPerLiterO2;
  return litersPerMin * 1000.0 / std::fmax(bodyMassKg, 1.0);
}

EnergySystem::EnergySystem(const EnergyParams& params) : params_(params) {
  state_.glycogenKcal = params_.glycogenCapacityKcal;
  state_.fatReserveKcal = params_.fatReserveKcal;
}

double EnergySystem::SaturationVapourPressureKPa(double temperatureC) {
  // Buck AL (1996), Buck Research CR-1A manual (improved Magnus form).
  const double t = temperatureC;
  if (t >= 0.0) return 0.61121 * std::exp((18.678 - t / 234.5) * (t / (257.14 + t)));
  return 0.61115 * std::exp((23.036 - t / 333.7) * (t / (279.82 + t)));
}

double EnergySystem::RespiratoryWaterLossLPerHour(double minuteVentilationLMin, double ambientTempC,
                                                  double relativeHumidity01) {
  const double exhaled = AbsoluteHumidityGM3(SaturationVapourPressureKPa(kExhaledTempC), kExhaledTempC);
  const double ambient =
      AbsoluteHumidityGM3(Clamp01(relativeHumidity01) * SaturationVapourPressureKPa(ambientTempC), ambientTempC);
  const double m3PerHour = std::fmax(minuteVentilationLMin, 0.0) * 60.0 / 1000.0;
  return std::fmax(0.0, m3PerHour * (exhaled - ambient) / 1000.0);  // g -> kg ≈ L
}

double EnergySystem::AppetiteFactor01(double altitudeM) {
  return 1.0 - 0.45 * SmoothStep(3500.0, 7500.0, altitudeM);
}

double EnergySystem::SleepQuality01(double altitudeM) {
  // Periodic (Cheyne-Stokes) breathing and frequent arousals become near-universal above
  // ~4,000-5,000 m (West et al. 1986 "Nocturnal periodic breathing at altitudes of 6300 and 8050 m";
  // Bloch et al. 2010).
  return 1.0 - 0.45 * SmoothStep(2500.0, 7500.0, altitudeM);
}

double EnergySystem::RemainingAppetiteKcal() const {
  return std::fmax(0.0, kMaxDailyIntakeKcal * state_.appetite01 - state_.recentIntakeKcal);
}

void EnergySystem::Eat(double kcal) {
  const double accepted = std::fmin(std::fmax(kcal, 0.0), RemainingAppetiteKcal());
  state_.lastMealAcceptedKcal = accepted;
  state_.recentIntakeKcal += accepted;
  // Intestinal absorption falls modestly at extreme altitude (Boyer & Blume 1984).
  const double absorption = 0.95 - 0.10 * SmoothStep(5000.0, 7000.0, lastAltitudeM_);
  state_.stomachKcal += accepted * absorption;
}

void EnergySystem::Drink(double liters) {
  state_.hydrationDeficitL = std::fmax(0.0, state_.hydrationDeficitL - std::fmax(liters, 0.0));
}

void EnergySystem::Step(double dtSeconds, const EnergyInputs& in) {
  if (dtSeconds <= 0.0) return;
  EnergyState& s = state_;
  lastAltitudeM_ = in.altitudeM;
  const double hours = dtSeconds / kHour;
  const double ex = Clamp(in.exertion01, 0.0, 1.2);
  const double glyCap = std::fmax(params_.glycogenCapacityKcal, 1.0);
  const double bodyMass = std::fmax(params_.bodyMassKg - s.bodyMassLostKg, 20.0);

  s.appetite01 = AppetiteFactor01(in.altitudeM);
  s.recentIntakeKcal = ApproachExp(s.recentIntakeKcal, 0.0, dtSeconds, kSecondsPerDay);

  // --- Fuel ---------------------------------------------------------------------------------
  const double spentKcal = std::fmax(in.metabolicRateW, 0.0) * dtSeconds / kKcalToJoule;
  const double absorbed = s.stomachKcal * (1.0 - std::exp(-dtSeconds / kStomachEmptyingTauS));
  s.stomachKcal -= absorbed;

  // Carbohydrate share of oxidation rises with intensity ("crossover concept", Brooks & Mercier
  // 1994; Romijn et al. 1993: fat dominates at 25 % VO2max, CHO ~70-80 % at 85 %). It falls when
  // glycogen runs out, which is what caps intensity ("hitting the wall").
  const double glyFrac = Clamp01(s.glycogenKcal / glyCap);
  double choShare = in.sleeping ? 0.30 : 0.35 + 0.55 * SmoothStep(0.25, 0.95, ex);
  choShare *= SmoothStep(0.0, 0.15, glyFrac) * 0.9 + 0.1;

  s.glycogenKcal += absorbed * kMealCarbShare - choShare * spentKcal;
  s.fatReserveKcal += absorbed * (1.0 - kMealCarbShare) - (1.0 - choShare) * spentKcal;
  if (s.glycogenKcal > glyCap) {
    s.fatReserveKcal += (s.glycogenKcal - glyCap) * kLipogenesisEfficiency;
    s.glycogenKcal = glyCap;
  }
  if (s.glycogenKcal < 0.0) {
    s.fatReserveKcal += s.glycogenKcal;  // shortfall met by fat / gluconeogenesis
    s.glycogenKcal = 0.0;
  }
  s.fatReserveKcal = std::fmax(s.fatReserveKcal, 0.0);
  s.bodyMassLostKg = (params_.fatReserveKcal - s.fatReserveKcal) / kKcalPerKgTissue +
                     (glyCap - s.glycogenKcal) * kKgPerKcalGlycogen;

  // --- Water --------------------------------------------------------------------------------
  const double respiratory = RespiratoryWaterLossLPerHour(in.minuteVentilationLMin, in.ambientTempC, in.relativeHumidity01);
  // Sweat: rises with work rate and warmth; in the cold under a down suit it is modest
  // (~0.1-0.3 L/h while climbing; Sawka et al. 2007 ACSM position stand on fluid replacement).
  const double sweat = (0.25 + 0.015 * std::fmax(0.0, in.ambientTempC + 10.0)) *
                       Clamp01((in.metabolicRateW - 200.0) / 500.0);
  const double lossL = (respiratory + sweat + kInsensibleSkinLPerHour + kUrineLPerHour) * hours;
  const double gainL = kMetabolicWaterLPerKcal * spentKcal;
  s.hydrationDeficitL = std::fmax(0.0, s.hydrationDeficitL + lossL - gainL);
  const double dehydrationFrac = s.hydrationDeficitL / bodyMass;

  // --- Short-term (anaerobic) stamina: drains above ~90 % VO2max within minutes ----------------
  if (ex > 0.9) {
    s.shortTermStamina01 -= (ex - 0.85) * 0.019 * dtSeconds;
  } else {
    s.shortTermStamina01 += (0.9 - ex) / 0.9 * dtSeconds / 90.0;
  }
  s.shortTermStamina01 = Clamp01(s.shortTermStamina01);

  // --- Sleep and fatigue ----------------------------------------------------------------------
  const double quality = SleepQuality01(in.altitudeM);
  if (in.sleeping) {
    s.lastSleepQuality01 = quality;
    s.sleepDebtHours -= quality * hours;
    // A full night of good sleep clears most accumulated fatigue (tau ~6 h); poor sleep at
    // altitude recovers much less.
    s.fatigue01 = ApproachExp(s.fatigue01, 0.0, dtSeconds, 6.0 * kHour / std::fmax(quality, 0.1));
  } else {
    s.sleepDebtHours += 0.5 * hours;
    const double glyLow = 1.0 - SmoothStep(0.0, 0.2, Clamp01(s.glycogenKcal / glyCap));
    // Calibrated so a 12 h summit day at ~65 % VO2max ends near 0.8 and an 8 h trekking day at
    // ~50 % near 0.2-0.3.
    double rate = 0.32 * std::pow(std::fmax(0.0, ex - 0.3), 1.5);
    rate *= (1.0 + glyLow) * (1.0 + 10.0 * dehydrationFrac);
    rate += 0.004 * std::fmax(0.0, s.sleepDebtHours - 8.0);
    s.fatigue01 += rate * hours;
    if (ex < 0.3) {
      // Sitting rest recovers slowly.
      s.fatigue01 -= s.fatigue01 * (0.3 - ex) / 0.3 * dtSeconds / (30.0 * kHour);
    }
  }
  s.sleepDebtHours = Clamp(s.sleepDebtHours, 0.0, 72.0);
  s.fatigue01 = Clamp01(s.fatigue01);
}

double EnergySystem::PerformanceMultiplier() const {
  const EnergyState& s = state_;
  const double glyFrac = Clamp01(s.glycogenKcal / std::fmax(params_.glycogenCapacityKcal, 1.0));
  // Fat-only metabolism supports roughly 50-60 % of VO2max (Coyle 1986).
  const double glycogen = 0.55 + 0.45 * SmoothStep(0.0, 0.2, glyFrac);
  // Endurance falls measurably beyond ~2 % body-mass water deficit (Cheuvront & Kenefick 2014).
  const double bodyMass = std::fmax(params_.bodyMassKg - s.bodyMassLostKg, 20.0);
  const double pct = s.hydrationDeficitL / bodyMass;
  const double hydration = 1.0 - Clamp((pct - 0.01) * 8.0, 0.0, 0.7);
  const double fatigue = 1.0 - 0.5 * std::pow(Clamp01(s.fatigue01), 1.5);
  return Clamp(glycogen * hydration * fatigue, 0.1, 1.0);
}

}  // namespace summit::sim
