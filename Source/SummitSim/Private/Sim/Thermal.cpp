#include "Sim/Thermal.h"

#include <cmath>

#include "Sim/Constants.h"
#include "Sim/Energy.h"
#include "Sim/Math.h"

namespace summit::sim {

namespace {

constexpr double kMaxSubStepS = 10.0;
constexpr double kCloToM2KW = 0.155;               // ISO 9920
constexpr double kTissueHeatCapacity = 3490.0;     // J/(kg K), Gagge 1971
constexpr double kReferenceHeightM = 1.78;
constexpr double kCoreMassFraction = 0.80;
constexpr double kSkinMassFraction = 0.15;         // vasoconstricted shell (Gagge's alpha in the cold)
constexpr double kTissueFreezingC = -0.55;         // Danielsson 1996
constexpr double kLatentHeatSlowdown = 2.0;        // freezing tissue water holds temperature near -0.55 °C
constexpr double kRestingMetabolismWPerKg = 1.17;  // 1 MET = 3.5 mL O2/kg/min

// Extremity nodes: share of body-surface area, tissue mass per 75 kg, and perfusion conductance
// range (W/K = rho c Q). Finger blood flow ranges from ~5 to ~200 mL/min per hand
// (Sendowski 1997; Daanen 2003 "Finger cold-induced vasodilation: a review", Eur J Appl Physiol
// 89:411). The face vasoconstricts much less than hands and feet.
struct ExtremitySpec {
  double areaFraction;
  double massKg75;
  double kMinWK;
  double kMaxWK;
  double convectionBoost;  // small-radius geometry: fingers and nose shed heat faster
  double tissueResistance; // m2K/W, node centre to skin surface (skin + subcutis conduction)
};
constexpr ExtremitySpec kExtremities[3] = {
    {0.050, 0.45, 0.3, 15.0, 1.4, 0.030},  // Hands (fingers and superficial hand tissue at risk)
    {0.070, 1.00, 0.5, 15.0, 1.0, 0.030},  // Feet (toes and forefoot)
    {0.025, 0.25, 0.8, 6.0, 1.2, 0.020},   // Face (nose, cheeks, ears)
};

// DuBois & DuBois (1916) body-surface area with a reference height.
double BodySurfaceAreaM2(double massKg) {
  return 0.2025 * std::pow(std::fmax(massKg, 20.0), 0.425) * std::pow(kReferenceHeightM, 0.725);
}

// Forced convection on a clothed body, ASHRAE / Gagge: hc = 8.6 v^0.53 (min ~3 W/m2K still air),
// scaled for air density as (P/P0)^0.55 (Kandjov 1999, convective heat transfer at altitude).
double ConvectiveCoefficient(double windMS, double pressureKPa) {
  const double hc = std::fmax(3.0, 8.6 * std::pow(std::fmax(windMS, 0.0), 0.53));
  return hc * std::pow(Clamp(pressureKPa / kSeaLevelPressureKPa, 0.2, 1.2), 0.55);
}

// Linear radiative coefficient hr = 4 eps sigma Tm^3.
double RadiativeCoefficient(double meanTempC) {
  const double tk = meanTempC + kCelsiusToKelvin;
  return 4.0 * 0.95 * kStefanBoltzmann * tk * tk * tk;
}

// Wind and body motion strip insulation from clothing (Havenith & Nilsson 2004, "Correction of
// clothing insulation for movement and wind effects", Eur J Appl Physiol 92:636); a windproof
// shell prevents most of it.
double WindInsulationFactor(double windMS, double windproof01) {
  const double permeability = 1.0 - 0.85 * Clamp01(windproof01);
  return std::exp(-0.06 * permeability * Clamp(windMS, 0.0, 25.0));
}

// Wet insulation loses most of its value (down collapses; ISO 9920 wet-clothing data).
double WetInsulationFactor(double wetness01) { return 1.0 - 0.6 * Clamp01(wetness01); }

// Sleeping-bag insulation from its comfort rating: ISO 23537 / EN 13537 define the comfort
// temperature as the air temperature at which a resting sleeper is in heat balance; with ~66 W of
// dry heat loss over ~1.9 m2 at 33 °C skin this is R ≈ (33 - Tcomfort) / 34 m2K/W.
double SleepingBagResistance(double comfortC) { return std::fmax(0.3, (33.0 - comfortC) / 34.0); }

}  // namespace

double WindChillC(double airTempC, double windKmh) {
  // Environment Canada / US NWS JAG/TI (2001) wind chill index.
  if (airTempC > 10.0 || windKmh < 4.8) return airTempC;
  const double v016 = std::pow(windKmh, 0.16);
  return 13.12 + 0.6215 * airTempC - 11.37 * v016 + 0.3965 * airTempC * v016;
}

double ThermalModel::EffectiveExposureTempC(const ThermalEnvironment& env) {
  if (env.inTent) return env.airTempC + 5.0 + std::fmin(8.0, 0.008 * env.solarWm2);
  return WindChillC(env.airTempC, env.windMS * 3.6);
}

HypothermiaStage ThermalModel::Hypothermia() const {
  const double t = state_.coreTempC;
  if (t >= 35.0) return HypothermiaStage::None;
  if (t >= 32.0) return HypothermiaStage::Mild;
  if (t >= 28.0) return HypothermiaStage::Moderate;
  return HypothermiaStage::Severe;
}

FrostbiteGrade ThermalModel::Frostbite(BodyPart part) const {
  const int i = static_cast<int>(part);
  if (i < 0 || i >= 3) return FrostbiteGrade::None;
  const double d = state_.frostbiteDamage01[i];
  if (d >= 0.5) return FrostbiteGrade::Deep;
  if (d >= 0.15) return FrostbiteGrade::Superficial;
  if (d >= 0.01 || state_.surfaceTempC[i] <= 0.0) return FrostbiteGrade::Frostnip;
  return FrostbiteGrade::None;
}

void ThermalModel::Step(double dtSeconds, const ThermalInputs& in) {
  if (dtSeconds <= 0.0) return;
  ThermalState& s = state_;
  const int n = static_cast<int>(std::ceil(dtSeconds / kMaxSubStepS));
  const double dt = dtSeconds / n;

  const double mass = std::fmax(in.bodyMassKg, 20.0);
  const double bsa = BodySurfaceAreaM2(mass);
  const double restingW = kRestingMetabolismWPerKg * mass;
  const ClothingSystem& cl = in.clothing;
  const ThermalEnvironment& env = in.env;

  // Micro-environment. A tent stops the wind, holds a few degrees of body/stove heat and turns
  // sunshine into a greenhouse; the sleeping bag adds insulation everywhere it covers.
  double airT = env.airTempC;
  double wind = std::fmax(env.windMS, 0.0);
  double solar = std::fmax(env.solarWm2, 0.0);
  if (env.inTent) {
    airT += 5.0 + std::fmin(8.0, 0.008 * solar);
    wind = 0.3;
    solar = 0.0;
  }
  const double radiantT = env.inTent ? airT : airT - 5.0;  // cold high-altitude sky
  const double hc = ConvectiveCoefficient(wind, in.pressureKPa);
  const double hr = RadiativeCoefficient(0.5 * (airT + radiantT) + 5.0);
  const double h = hc + hr;
  const double operativeT = (hc * airT + hr * radiantT) / h;
  // Absorbed shortwave (absorptivity ~0.7, ~25 % of the surface facing the direct + snow-reflected
  // beam) raises the effective outer temperature: the sol-air temperature.
  const double solarAbsorbedWm2 = 0.7 * 0.25 * solar;
  const double bagR = env.inSleepingBag ? SleepingBagResistance(env.sleepingBagComfortC) : 0.0;

  const double windF = WindInsulationFactor(wind, cl.windproof01);
  const double wetF = WetInsulationFactor(cl.wetness01);
  const double torsoClo = std::fmax(cl.torsoClo, 0.0);
  const double fcl = std::fmin(2.5, 1.0 + 0.28 * torsoClo);  // ISO 9920 clothing area factor
  // Clothing worn inside a sleeping bag is compressed and counts about half.
  const double torsoR = torsoClo * kCloToM2KW * windF * wetF * (env.inSleepingBag ? 0.5 : 1.0);
  const double bodyR = torsoR + bagR + 1.0 / (fcl * h);

  double extremityArea = 0.0;
  for (const auto& e : kExtremities) extremityArea += e.areaFraction;
  const double bodyArea = bsa * (1.0 - extremityArea);

  // Extremity insulation.
  double extR[3];
  {
    const double handsR = std::fmax(cl.handsClo, 0.0) * kCloToM2KW * windF * wetF;
    const double feetR = std::fmax(cl.feetClo, 0.0) * kCloToM2KW * wetF;  // boots are windproof
    double faceCover = Clamp01(cl.faceCoverage01);
    if (env.inSleepingBag) faceCover = std::fmax(faceCover, 0.7);  // hood drawn tight
    const double faceR = faceCover * 1.0 * kCloToM2KW * windF;     // balaclava + goggles ≈ 1 clo
    extR[0] = handsR + bagR + 1.0 / (kExtremities[0].convectionBoost * hc + hr);
    extR[1] = feetR + bagR + 1.0 / (kExtremities[1].convectionBoost * hc + hr);
    extR[2] = faceR + 0.3 * bagR + 1.0 / (kExtremities[2].convectionBoost * hc + hr);
  }

  // Respiratory heat loss: warming inspired air to ~34 °C and saturating it (latent heat).
  const double airDensityBody = in.pressureKPa * 1000.0 / (kGasConstantDryAir * 310.15);
  const double massFlow = std::fmax(in.minuteVentilationLMin, 0.0) / 60000.0 * airDensityBody;
  const double respSensible = massFlow * 1005.0 * std::fmax(0.0, 34.0 - airT);
  const double respWaterLPerH = EnergySystem::RespiratoryWaterLossLPerHour(
      in.minuteVentilationLMin, airT, env.inTent ? 0.5 : env.relativeHumidity01);
  const double respLatent = respWaterLPerH / kSecondsPerHour * 2.43e6;
  const double respW = respSensible + respLatent;

  const double coreC = kCoreMassFraction * mass * kTissueHeatCapacity;
  const double skinC = kSkinMassFraction * mass * kTissueHeatCapacity;
  const double massScale = mass / 75.0;
  const double hydrationFrac = std::fmax(in.hydrationDeficitL, 0.0) / mass;

  double lastNet = 0.0;
  for (int step = 0; step < n; ++step) {
    const double tc = s.coreTempC;
    const double tsk = s.meanSkinTempC;

    // --- Shivering (Gagge 1971 drive, capped at ~4.5x resting total; Tikuisis 1991 max ~5x).
    // Hypoxia blunts it (Johnston et al. 1996, J Appl Physiol 80:422), glycogen depletion limits it
    // (Haman 2004), and it fails below ~31 °C core.
    const double drive = Clamp01(1.6 * std::fmax(0.0, 36.8 - tc) + 0.06 * std::fmax(0.0, 32.0 - tsk));
    const double shivMax = 3.5 * restingW;
    double shiv = shivMax * drive;
    shiv *= 0.4 + 0.6 * SmoothStep(0.5, 0.9, in.spo2);
    shiv *= 0.3 + 0.7 * SmoothStep(0.0, 0.2, in.glycogenFraction01);
    shiv *= SmoothStep(30.0, 32.5, tc);
    // Shivering cannot be sustained indefinitely: muscle fatigue cuts its capacity over hours
    // (Tikuisis, Bell & Jacobs 1991, J Appl Physiol 70:1996; Haman 2004).
    shiv *= 1.0 - 0.7 * s.shiveringFatigue01;
    // Working muscles already make the heat; shivering only adds what exercise does not cover.
    shiv = std::fmax(0.0, shiv - 0.6 * std::fmax(0.0, in.metabolicHeatW - restingW));
    s.shivering01 = shivMax > 0.0 ? shiv / shivMax : 0.0;
    if (s.shivering01 > 0.05) {
      s.shiveringFatigue01 = Clamp01(s.shiveringFatigue01 + s.shivering01 * dt / (3.0 * kSecondsPerHour));
    } else {
      s.shiveringFatigue01 = ApproachExp(s.shiveringFatigue01, 0.0, dt, 4.0 * kSecondsPerHour);
    }
    const double production = std::fmax(in.metabolicHeatW, 0.0) + shiv;

    // --- Core-to-skin transfer with Gagge's skin blood flow control.
    const double skbf = Clamp((6.3 + 200.0 * std::fmax(0.0, tc - 36.8)) / (1.0 + 0.5 * std::fmax(0.0, 33.7 - tsk)), 0.5, 90.0);
    const double kCoreSkin = bsa * (1.0 - extremityArea) * (5.28 + 1.163 * skbf);
    const double coreToSkin = kCoreSkin * (tc - tsk);

    // --- Skin to environment (dry), plus evaporation (insensible + regulatory sweating).
    const double outerT = operativeT + solarAbsorbedWm2 / (fcl * h);
    const double dryLoss = bodyArea * (tsk - outerT) / bodyR;
    const double sweatW = std::fmin(115.0 * bsa * std::fmax(0.0, tc - 37.0), 400.0) / (1.0 + 0.15 * torsoClo);
    const double evap = 0.06 * restingW + sweatW;

    // --- Extremities.
    double toExtremities = 0.0;
    double extremityLoss = 0.0;
    // Central drive: a cooling core or cooling skin shuts the peripheral shunts (AVAs) first.
    const double central = SmoothStep(35.8, 37.2, tc) * (0.15 + 0.85 * SmoothStep(27.0, 34.0, tsk));
    const double hypoxic = 0.45 + 0.55 * SmoothStep(0.60, 0.90, in.spo2);
    const double hydration = 1.0 - 0.4 * SmoothStep(0.01, 0.05, hydrationFrac);
    for (int i = 0; i < 3; ++i) {
      const ExtremitySpec& spec = kExtremities[i];
      const double te = s.extremityTempC[i];
      // Local cold constricts the vessels of the cold tissue itself.
      const double local = 0.1 + 0.9 * SmoothStep(10.0, 34.0, te);
      // Cold-induced vasodilation ("hunting reaction"), blunted by hypoxia and core cooling
      // (Daanen 2003; Felicijan et al. 2008, High Alt Med Biol 9:22).
      const double civd = 0.25 * SmoothStep(12.0, 4.0, te) * SmoothStep(36.5, 37.2, tc) * SmoothStep(0.7, 0.9, in.spo2);
      const double perf = Clamp01(central * hypoxic * hydration * local + civd);
      s.perfusion01[i] = perf;
      const double kPerf = (spec.kMinWK + (spec.kMaxWK - spec.kMinWK) * perf) * massScale;
      const double area = bsa * spec.areaFraction;
      const double solarGain = i == 2 ? 0.7 * 0.5 * solar * area * (1.0 - Clamp01(cl.faceCoverage01)) : 0.0;
      const double in_ = kPerf * (tc - te);
      const double out = area * (te - operativeT) / extR[i] - solarGain;
      double dT = (in_ - out) * dt / (spec.massKg75 * massScale * kTissueHeatCapacity);
      if (te <= kTissueFreezingC && dT < 0.0) dT /= kLatentHeatSlowdown;
      s.extremityTempC[i] = te + dT;
      toExtremities += in_;
      extremityLoss += out;
      // The skin surface sits between the node and the environment: exposed skin freezes long
      // before the deeper tissue does (Danielsson 1996; Osczevski & Bluestein 2005 wind chill model).
      const double rOut = extR[i];
      const double surface = operativeT + (s.extremityTempC[i] - operativeT) * rOut / (spec.tissueResistance + rOut);
      s.surfaceTempC[i] = surface;
      // Freezing injury accumulates while tissue is below its freezing point; deeper cold, faster.
      const double frozenT = 0.5 * (surface + s.extremityTempC[i]);  // mean of the outer tissue layer
      if (frozenT < kTissueFreezingC) {
        const double rate = 0.08 + 0.3 * (kTissueFreezingC - frozenT);  // per hour
        s.frostbiteDamage01[i] = Clamp01(s.frostbiteDamage01[i] + rate * dt / kSecondsPerHour);
      }
    }

    s.coreTempC = tc + (production - respW - coreToSkin - toExtremities) * dt / coreC;
    s.meanSkinTempC = tsk + (coreToSkin - dryLoss - evap) * dt / skinC;
    s.heatProductionW = production;
    lastNet = respW + dryLoss + evap + extremityLoss - production;
  }
  s.respiratoryHeatLossW = respW;
  lastNetHeatLossW_ = lastNet;
}

}  // namespace summit::sim
