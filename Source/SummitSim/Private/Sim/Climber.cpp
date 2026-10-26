#include "Sim/Climber.h"

#include <cmath>

#include "Sim/Atmosphere.h"
#include "Sim/Constants.h"
#include "Sim/Math.h"

namespace summit::sim {

namespace {

constexpr double kMaxSubStepS = 60.0;
// 1 MET = 3.5 mL O2/kg/min ≈ 1.17 W/kg (Ainsworth et al. 2011, Compendium of Physical Activities).
constexpr double kWattsPerMetPerKg = 3.5 * 20.1 / 60.0;
constexpr double kRestingMets = 1.3;   // sitting quietly, awake (Compendium code 07021)
constexpr double kSleepingMets = 0.95; // Compendium code 07030
constexpr double kRappellingMets = 3.0;
constexpr double kMaxExertion = 1.2;
constexpr double kMaxWalkingSpeedMS = 2.2;  // walk-run transition (Hreljac 1993)

// Mass on the feet costs ~4.5-6x the same mass on the torso at normal walking speed (Soule & Goldman
// 1969, J Appl Physiol 27:687; Legg & Mahanty 1986), because the legs must swing it. That swing cost
// scales roughly with speed squared; at the slow pace of high-altitude ascent the vertical work
// dominates and foot mass costs about what it weighs. Pandolf's data were collected in ~1.6 kg
// military boots, so only footwear above that baseline is converted to equivalent pack load.
constexpr double kFootMassLoadMultiplierAtWalkingSpeed = 5.0;
constexpr double kPandolfBaselineFootwearKg = 1.6;
constexpr double kCramponPairMassKg = 0.9;

// Multi-layer cold-weather clothing raises the cost of moving by ~10-16 % beyond its weight through
// friction between layers and restricted joints (Teitlebaum & Goldman 1972, J Appl Physiol 32:743;
// Dorman & Havenith 2009, Eur J Appl Physiol 105:463). Those figures were measured at normal walking
// pace; the penalty comes from limb movement, so it fades at the slow plod of extreme altitude.
double ClothingHobblingFactor(const ClothingSystem& clothing, double speedMS) {
  return 1.0 + 0.16 * SmoothStep(3.0, 8.0, clothing.torsoClo) * Clamp01(std::fabs(speedMS) / 1.34);
}

double EffectiveLoadKg(const ClimberGear& gear, double speedMS) {
  const double feetKg = std::fmax(0.0, gear.footwearMassKg) + (gear.crampons ? kCramponPairMassKg : 0.0);
  const double extraFeetKg = std::fmax(0.0, feetKg - kPandolfBaselineFootwearKg);
  const double v = Clamp01(std::fabs(speedMS) / 1.34);
  const double multiplier = 1.0 + (kFootMassLoadMultiplierAtWalkingSpeed - 1.0) * v * v;
  return std::fmax(0.0, gear.packMassKg) + multiplier * extraFeetKg;
}

bool IsSnowOrIce(TerrainType t) {
  return t == TerrainType::HardSnow || t == TerrainType::SoftSnow || t == TerrainType::DeepSnow ||
         t == TerrainType::GlacierIce || t == TerrainType::Verglas;
}

bool IsActive(Activity a) { return a != Activity::Resting && a != Activity::Sleeping; }

double TerrainFactorFor(const ClimberEnvironment& env, Activity activity) {
  double eta = PandolfTerrainFactor(env.terrain, env.snowDepthM);
  if (activity == Activity::BreakingTrail) eta = std::fmax(eta, PandolfTerrainFactor(TerrainType::DeepSnow, env.snowDepthM));
  return eta;
}

// Vertical fraction of along-route speed. A grade of 0 for rope/technical activities means
// the caller is giving vertical speed directly.
double VerticalFraction(double gradePercent) {
  if (gradePercent <= 0.0) return 1.0;
  return std::sin(std::atan(gradePercent / 100.0));
}

struct TechnicalCost {
  double baseW;            // holding on, clipping, balancing
  double efficiency;       // mechanical efficiency of lifting body + load
};

// Technical ascent: a static cost plus the cost of lifting body and pack. Rock/ice climbing runs
// ~8-12 METs at typical rates (Ainsworth 2011: rock climbing ascending 7.5-8 METs; Sheel 2004
// "Physiology of sport rock climbing", Br J Sports Med 38:355). Muscular efficiency for uphill
// work is ~20-25 % walking (Pandolf's 0.35 V G term ≈ 28 %), lower for arm-assisted ascent.
TechnicalCost TechnicalCostFor(Activity activity, const ClimberProfile& p, TerrainType terrain, double massKg,
                               double standingW) {
  if (activity == Activity::Climbing) {
    const bool ice = IsSnowOrIce(terrain);
    const double skill = ice ? p.skillIce01 : p.skillTechnical01;
    return {2.5 * kWattsPerMetPerKg * massKg, 0.10 + 0.06 * Clamp01(skill)};
  }
  // Jumaring a fixed line: mostly legs; the ascender takes the arm-holding cost off, so the static
  // part is just standing with the pack.
  return {standingW, 0.13 + 0.06 * Clamp01(p.skillRope01)};
}

// On fixed lines below ~45 degrees climbers walk with the ascender sliding along as a safety rather
// than hauling on it, so the cost is Pandolf walking plus clipping past anchors (~10 %).
constexpr double kWalkedFixedLineMaxGradePercent = 100.0;
constexpr double kFixedLineWalkingOverhead = 1.10;

bool IsWalkedFixedLine(const ClimberAction& action) {
  return action.activity == Activity::Jumaring && action.gradePercent < kWalkedFixedLineMaxGradePercent;
}

double StandingW(double massKg, double loadKg) {
  return 1.5 * massKg + 2.0 * (massKg + loadKg) * (loadKg / massKg) * (loadKg / massKg);
}

}  // namespace

Climber::Climber(const ClimberProfile& profile)
    : profile_(profile), physiology_(profile.physiology), energy_(profile.energy) {}

namespace {

double MetabolicRateW(const ClimberProfile& profile, const ClimberEnvironment& env, const ClimberAction& action,
                      double massKg, double loadKg) {
  switch (action.activity) {
    case Activity::Resting: return kRestingMets * kWattsPerMetPerKg * massKg;
    case Activity::Sleeping: return kSleepingMets * kWattsPerMetPerKg * massKg;
    case Activity::Walking:
    case Activity::BreakingTrail:
      return PandolfMetabolicRateW(massKg, loadKg, action.speedMS, action.gradePercent,
                                   TerrainFactorFor(env, action.activity));
    case Activity::Climbing:
    case Activity::Jumaring: {
      if (IsWalkedFixedLine(action)) {
        return kFixedLineWalkingOverhead * PandolfMetabolicRateW(massKg, loadKg, action.speedMS, action.gradePercent,
                                                                 TerrainFactorFor(env, Activity::Walking));
      }
      const TechnicalCost c = TechnicalCostFor(action.activity, profile, env.terrain, massKg, StandingW(massKg, loadKg));
      const double vVert = std::fmax(0.0, action.speedMS) * VerticalFraction(action.gradePercent);
      return c.baseW + (massKg + loadKg) * kGravity * vVert / c.efficiency;
    }
    case Activity::Rappelling:
      return kRappellingMets * kWattsPerMetPerKg * massKg * (massKg + loadKg) / massKg;
  }
  return kRestingMets * kWattsPerMetPerKg * massKg;
}

}  // namespace

void Climber::Step(double dtSeconds, const ClimberEnvironment& env, const ClimberAction& action,
                   const ClimberGear& gear) {
  if (dtSeconds <= 0.0 || IsDead()) return;
  const int n = static_cast<int>(std::ceil(dtSeconds / kMaxSubStepS));
  const double dt = dtSeconds / n;
  for (int i = 0; i < n && !IsDead(); ++i) SubStep(dt, env, action, gear);
}

void Climber::SubStep(double dt, const ClimberEnvironment& env, const ClimberAction& action, const ClimberGear& gear) {
  const double pressure = BarometricPressureKPa(env.altitudeM, env.latitudeDeg, env.dayOfYear);
  lastPressureKPa_ = pressure;
  const double mass = std::fmax(30.0, profile_.physiology.bodyMassKg - energy_.State().bodyMassLostKg);
  const double load = EffectiveLoadKg(gear, action.speedMS);
  const bool sleeping = action.activity == Activity::Sleeping;

  // 1. Metabolic demand and exertion relative to the oxygen-limited ceiling.
  const double hobbling = IsActive(action.activity) ? ClothingHobblingFactor(gear.clothing, action.speedMS) : 1.0;
  const double metabolicW = hobbling * MetabolicRateW(profile_, env, action, mass, load);
  const double vo2Required = MetabolicRateToVO2MlKgMin(metabolicW, mass);
  const GasExchange preview = physiology_.EvaluateGasExchange(pressure, lastExertion01_, sleeping, gear.oxygen, gear.meds);
  const double coreT = thermal_.State().coreTempC;
  // Cold muscle and a cooling core cut maximal power (Oksa 2002; Pugh 1966).
  const double thermalPerf = 0.4 + 0.6 * SmoothStep(31.0, 35.5, coreT);
  const double rawVo2Max = std::fmax(preview.vo2MaxMlKgMin, 1.0);
  const double effVo2Max = rawVo2Max * energy_.PerformanceMultiplier() * thermalPerf;
  const double exertionRaw = std::fmin(kMaxExertion, vo2Required / rawVo2Max);
  const double exertionEff = std::fmin(kMaxExertion, vo2Required / std::fmax(effVo2Max, 0.5));

  // 2. Physiology (gas exchange, acclimatization, altitude illness).
  PhysiologyInputs pin;
  pin.altitudeM = env.altitudeM;
  pin.pressureKPa = pressure;
  pin.exertion01 = exertionRaw;
  pin.sleeping = sleeping;
  pin.oxygen = gear.oxygen;
  pin.meds = gear.meds;
  pin.ambientTempC = env.weather.temperatureC;
  physiology_.Step(dt, pin);
  const PhysiologyState& ps = physiology_.State();

  // 3. Thermal balance. About 20 % of the metabolic rate of locomotion is external work; at rest
  // all of it is heat.
  const double albedo = IsSnowOrIce(env.terrain) ? 0.8 : 0.2;
  ThermalInputs tin;
  tin.env.airTempC = env.weather.temperatureC;
  const double gustExcess = std::fmax(0.0, env.weather.windGustMS - env.weather.windSpeedMS);
  tin.env.windMS = (env.weather.windSpeedMS + 0.3 * gustExcess) * Clamp01(env.exposure01);
  // Body sees the incoming beam plus snow-reflected light from below (about half the view).
  tin.env.solarWm2 = std::fmax(0.0, env.weather.solarWm2) * (1.0 + 0.5 * albedo);
  tin.env.relativeHumidity01 = env.weather.relativeHumidity01;
  tin.env.inTent = env.inTent;
  tin.env.inSleepingBag = env.inSleepingBag;
  tin.env.sleepingBagComfortC = env.sleepingBagComfortC;
  tin.clothing = gear.clothing;
  tin.metabolicHeatW = metabolicW * (IsActive(action.activity) ? 0.8 : 1.0);
  tin.bodyMassKg = mass;
  tin.spo2 = ps.spo2;
  tin.hydrationDeficitL = energy_.State().hydrationDeficitL;
  tin.minuteVentilationLMin = ps.minuteVentilationLMin;
  tin.pressureKPa = pressure;
  tin.glycogenFraction01 =
      Clamp01(energy_.State().glycogenKcal / std::fmax(profile_.energy.glycogenCapacityKcal, 1.0));
  thermal_.Step(dt, tin);

  // 4. Energy, water, fatigue. Appetite and sleep follow the physiological altitude.
  EnergyInputs ein;
  ein.metabolicRateW = metabolicW + thermal_.State().shivering01 * 3.5 * 1.17 * mass;
  ein.exertion01 = exertionEff;
  ein.ambientTempC = env.inTent ? env.weather.temperatureC + 5.0 : env.weather.temperatureC;
  ein.relativeHumidity01 = env.weather.relativeHumidity01;
  ein.minuteVentilationLMin = ps.minuteVentilationLMin;
  ein.sleeping = sleeping;
  ein.altitudeM = ps.equivalentAltitudeM;
  energy_.Step(dt, ein);

  // 5. Snow blindness (UV photokeratitis): UV rises ~10 % per 1,000 m and snow reflects ~80 %
  // of it; unprotected eyes burn within a few hours of strong sun (Hackett & Roach; WMS 2019).
  const bool eyesExposed = !gear.glacierGoggles && !env.inTent && !sleeping;
  const double uvSolar = std::fmax(0.0, env.weather.solarWm2) * (1.0 + albedo);
  if (eyesExposed && uvSolar > 150.0) {
    const double ratePerHour = (uvSolar / 1400.0) * (1.0 + 0.1 * std::fmax(0.0, env.altitudeM) / 1000.0) / 3.0;
    snowBlindness01_ = Clamp01(snowBlindness01_ + ratePerHour * dt / kSecondsPerHour);
  } else {
    snowBlindness01_ = ApproachExp(snowBlindness01_, 0.0, dt, 24.0 * kSecondsPerHour);  // heals in 1-2 days
  }

  lastMetabolicW_ = metabolicW;
  lastExertion01_ = exertionEff;

  // 6. Death.
  const ThermalState& ts = thermal_.State();
  const EnergyState& es = energy_.State();
  const double glyFrac = Clamp01(es.glycogenKcal / std::fmax(profile_.energy.glycogenCapacityKcal, 1.0));
  const double dehydration = es.hydrationDeficitL / mass;
  if (ts.coreTempC < 24.0) {
    Kill(CauseOfDeath::Hypothermia);  // asystole / VF below ~24 °C (Brown et al. 2012, NEJM 367:1930)
  } else if (ps.haceSeverity01 >= 1.0) {
    Kill(CauseOfDeath::HACE);
  } else if (ps.hapeSeverity01 >= 1.0) {
    Kill(CauseOfDeath::HAPE);
  } else if (glyFrac < 0.02 && es.fatigue01 > 0.95 &&
             (ts.coreTempC < 33.0 || ps.highAltitudeDeterioration01 > 0.8 || dehydration > 0.08)) {
    Kill(CauseOfDeath::Exhaustion);  // the classic death-zone collapse
  } else if (ps.highAltitudeDeterioration01 >= 1.0 && es.fatigue01 > 0.9) {
    Kill(CauseOfDeath::Exhaustion);
  } else if (dehydration > 0.15) {
    Kill(CauseOfDeath::Other);
  }
}

double Climber::MaxSustainableSpeedMS(const ClimberEnvironment& env, const ClimberAction& action,
                                      const ClimberGear& gear) const {
  if (IsDead() || IsIncapacitated()) return 0.0;
  if (action.activity == Activity::Resting || action.activity == Activity::Sleeping) return 0.0;
  if (action.activity == Activity::Rappelling) return 0.3 + 0.4 * Clamp01(profile_.skillRope01);

  const double pressure = BarometricPressureKPa(env.altitudeM, env.latitudeDeg, env.dayOfYear);
  const double mass = std::fmax(30.0, profile_.physiology.bodyMassKg - energy_.State().bodyMassLostKg);
  // Technical ascent is slow, so foot mass counts at roughly its weight there.
  const double slowLoad = EffectiveLoadKg(gear, 0.0);
  const double sustainable = physiology_.MaxSustainableExertion01();
  const GasExchange g = physiology_.EvaluateGasExchange(pressure, sustainable, false, gear.oxygen, gear.meds);
  const double thermalPerf = 0.4 + 0.6 * SmoothStep(31.0, 35.5, thermal_.State().coreTempC);
  const double vo2Target = sustainable * g.vo2MaxMlKgMin * energy_.PerformanceMultiplier() * thermalPerf;
  const double targetW = vo2Target * mass * kWattsPerMetPerKg / 3.5;

  if ((action.activity == Activity::Climbing || action.activity == Activity::Jumaring) && !IsWalkedFixedLine(action)) {
    const TechnicalCost c = TechnicalCostFor(action.activity, profile_, env.terrain, mass, StandingW(mass, slowLoad));
    // Technical ascent runs at ~0.2-0.4 m/s along the route; evaluate clothing hobbling there.
    const double budgetW = targetW / ClothingHobblingFactor(gear.clothing, 0.3);
    const double vVert = std::fmax(0.0, budgetW - c.baseW) * c.efficiency / ((mass + slowLoad) * kGravity);
    return std::fmin(vVert / VerticalFraction(action.gradePercent), 1.0);
  }

  // Walking / trail breaking: largest speed whose Pandolf cost fits the budget. A coarse scan then
  // bisection handles the non-monotonic Santee downhill curve.
  const bool fixedLine = IsWalkedFixedLine(action);
  const double eta = TerrainFactorFor(env, fixedLine ? Activity::Walking : action.activity);
  const double overhead = fixedLine ? kFixedLineWalkingOverhead : 1.0;
  const auto cost = [&](double v) {
    return overhead * ClothingHobblingFactor(gear.clothing, v) *
           PandolfMetabolicRateW(mass, EffectiveLoadKg(gear, v), v, action.gradePercent, eta);
  };
  double budgetW = targetW;
  if (cost(0.0) >= budgetW) {
    // Exhausted or severely hypoxic: the sustainable budget no longer covers standing still, but a
    // climber can still shuffle (above sustainable effort) while VO2max exceeds the standing cost.
    // This is how spent climbers get themselves down; only when even that fails do they stop.
    const double maxW = targetW / std::fmax(sustainable, 1e-3);
    if (maxW <= cost(0.0)) return 0.0;
    budgetW = cost(0.0) + 0.3 * (maxW - cost(0.0));
  }
  double lo = 0.0;
  double hi = -1.0;
  constexpr double kScanStep = 0.05;
  for (double v = kScanStep; v <= kMaxWalkingSpeedMS + 1e-9; v += kScanStep) {
    if (cost(v) > budgetW) {
      hi = v;
      break;
    }
    lo = v;
  }
  if (hi < 0.0) return kMaxWalkingSpeedMS;
  for (int i = 0; i < 30; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (cost(mid) > budgetW) hi = mid; else lo = mid;
  }
  return lo;
}

std::vector<ConditionReport> Climber::ActiveConditions() const {
  std::vector<ConditionReport> out;
  const PhysiologyState& p = physiology_.State();
  const ThermalState& t = thermal_.State();
  const EnergyState& e = energy_.State();
  const double mass = std::fmax(30.0, profile_.physiology.bodyMassKg - e.bodyMassLostKg);
  const double glyFrac = Clamp01(e.glycogenKcal / std::fmax(profile_.energy.glycogenCapacityKcal, 1.0));

  const auto add = [&out](Condition c, double severity) {
    if (severity > 0.01) out.push_back({c, Clamp01(severity)});
  };
  add(Condition::Hypoxia, (0.90 - p.spo2) / 0.40);                 // 90 % -> 0, 50 % -> 1
  add(Condition::AMS, p.amsScore >= 1.0 ? p.amsScore / 9.0 : 0.0);  // LLS >= 3 is AMS, >= 6 severe
  add(Condition::HAPE, p.hapeSeverity01);
  add(Condition::HACE, p.haceSeverity01);
  add(Condition::Hypothermia, t.coreTempC < 36.0 ? (36.0 - t.coreTempC) / 12.0 : 0.0);
  double frost = 0.0;
  for (int i = 0; i < 3; ++i) {
    frost = std::fmax(frost, t.frostbiteDamage01[i]);
    if (t.surfaceTempC[i] <= 0.0) frost = std::fmax(frost, 0.05);  // frostnip
  }
  add(Condition::Frostbite, frost);
  add(Condition::Dehydration, e.hydrationDeficitL / (0.06 * mass));  // 6 % body mass = severe
  add(Condition::Exhaustion, std::fmax(e.fatigue01 >= 0.3 ? e.fatigue01 : 0.0, (1.0 - e.shortTermStamina01) * 0.5));
  add(Condition::Starvation, std::fmax(SmoothStep(0.25, 0.0, glyFrac), e.bodyMassLostKg / 15.0));
  add(Condition::SnowBlindness, snowBlindness01_);
  return out;
}

bool Climber::IsIncapacitated() const {
  if (IsDead()) return true;
  return thermal_.State().coreTempC < 30.0 || physiology_.State().haceSeverity01 > 0.7 ||
         energy_.State().fatigue01 >= 0.98;
}

}  // namespace summit::sim
