#include "Sim/Physiology.h"

#include <cmath>
#include <cstddef>

#include "Sim/Atmosphere.h"
#include "Sim/Constants.h"
#include "Sim/Math.h"

namespace summit::sim {

namespace {

constexpr double kDay = kSecondsPerDay;
constexpr double kHour = kSecondsPerHour;

// --- Acclimatization --------------------------------------------------------------------------
// Below ~1,500 m no meaningful adaptation is needed. "Need" is the altitude above that threshold.
constexpr double kAcclimatizationThresholdM = 1500.0;
constexpr double kCeilingM = Physiology::kAcclimatizationCeilingM;
// Ventilatory acclimatization: most of it within 1-2 weeks (Dempsey & Forster 1982; Pikes Peak
// studies, Huey & Powell / Reeves et al. 1993). Acetazolamide speeds it (Swenson 2016; Luks 2019).
constexpr double kVentTauUpS = 2.5 * kDay;
constexpr double kAcetazolamideTauFactor = 0.65;
// Deacclimatization over ~2-3 weeks at low altitude.
constexpr double kVentTauDownS = 14.0 * kDay;
// Hematologic: red-cell mass rises over weeks; Hb +1-2 g/dL in 3-4 weeks (Reynafarje 1959;
// Siebenmann et al. 2015, review in West, High Altitude Medicine and Physiology, 5th ed.).
constexpr double kHemTauUpS = 20.0 * kDay;
constexpr double kHemTauDownS = 30.0 * kDay;
constexpr double kMaxHemoglobinRiseGdl = 2.5;  // fully adapted at the ceiling
// Exposure averaging: during sleep the sleeping altitude dominates within a few hours.
constexpr double kExposureTauSleepS = 4.0 * kHour;
constexpr double kExposureTauAwakeS = 12.0 * kHour;
constexpr double kRecentMaxTauS = 4.0 * kDay;
constexpr double kClimbHighCredit = 0.3;  // share of (recent max - exposure) that drives adaptation

// --- AMS / HAPE / HACE -------------------------------------------------------------------------
// Lake Louise score target grows with the excess of exposure altitude over what the climber is
// adapted to. Calibrated to the WMS guideline (Luks et al. 2019): above 3,000 m, sleeping-altitude
// gains > 500 m/day produce AMS in a large fraction of people; flying to 3,650 m (Lhasa) gives AMS
// in roughly half (Wu et al. 2007); flying straight to > 5,000 m almost always does.
constexpr double kAmsGapThresholdM = 600.0;
constexpr double kAmsScorePerM = 0.003;
constexpr double kAmsMaxScore = 10.0;
constexpr double kAmsRiseTauS = 6.0 * kHour;    // symptoms appear 6-12 h after arrival
constexpr double kAmsFallTauS = 10.0 * kHour;

// --- Death zone ---------------------------------------------------------------------------------
// Deterioration above ~7,000-7,500 m that acclimatization cannot fix (Pugh 1964; West 1983, 2010
// "Deterioration at extreme altitude"; Huey & Eguskitza 2000 on mortality above 8,000 m).
constexpr double kDeteriorationOnsetM = 7000.0;
constexpr double kDeteriorationRecoveryM = 6500.0;

double Need(double altitudeM) { return std::fmax(0.0, altitudeM - kAcclimatizationThresholdM); }

struct Point {
  double x;
  double y;
};

template <std::size_t N>
double Interp(const Point (&table)[N], double x) {
  if (x <= table[0].x) return table[0].y;
  for (std::size_t i = 1; i < N; ++i) {
    if (x <= table[i].x) {
      const double t = (x - table[i - 1].x) / (table[i].x - table[i - 1].x);
      return Lerp(table[i - 1].y, table[i].y, t);
    }
  }
  return table[N - 1].y;
}

// Resting arterial PCO2 (mmHg) vs inspired PO2 (mmHg).
// Acute (unacclimatized) hypoxic ventilatory response: Rahn & Otis (1949) "Man's respiratory
// response during and after acclimatization to high altitude", Am J Physiol 157:445; Operation
// Everest II acute data (Sutton et al. 1988).
constexpr Point kPaCO2Acute[] = {{20, 27.0},  {30, 28.0},  {43, 29.0},  {47, 29.5},  {57, 30.5}, {74, 32.0},
                                 {86, 33.0},  {104, 35.0}, {125, 38.5}, {149, 40.0}, {400, 40.0}};
// Fully acclimatized: Rahn & Otis (1949) acclimatized line; Grocott et al. (2009) NEJM 360:140
// (Caudwell Xtreme Everest, 8,400 m: PaCO2 13.3 mmHg); West et al. (1983) J Appl Physiol 55:678
// (AMREE, summit PACO2 7.5 mmHg). The summit value is set to 12 mmHg rather than 7.5 so that
// saturation keeps falling with altitude (with the Bohr shift, 7.5 mmHg would give a higher SaO2
// on the summit than Caudwell measured at 8,400 m).
constexpr Point kPaCO2Acclimatized[] = {{20, 9.0},   {30, 10.0},  {43, 12.0},  {47, 13.3},  {50, 15.0},
                                        {57, 18.5},  {63, 20.5},  {74, 22.0},  {86, 26.0},  {104, 30.0},
                                        {125, 35.0}, {149, 40.0}, {400, 40.0}};

// VO2max as a fraction of the sea-level value for an acclimatized subject, vs inspired PO2.
// Sources: Pugh et al. (1964) Silver Hut; West et al. (1983) AMREE (summit ≈ 15 mL/kg/min from
// ~62 at sea level); Cymerman et al. (1989) Operation Everest II; Fulco, Rock & Cymerman (1998)
// "Maximal and submaximal exercise performance at altitude", ASEM 69:793 (≈ 7-10 % per 1,000 m
// above ~1,500 m).
constexpr Point kVO2MaxFraction[] = {{20, 0.05},  {30, 0.10},  {43, 0.26},  {47, 0.32}, {50, 0.37},
                                     {57, 0.48},  {63, 0.57},  {74, 0.70},  {80, 0.75}, {95, 0.84},
                                     {110, 0.92}, {125, 0.98}, {149, 1.00}, {250, 1.03}};

// Mask efficiency for a constant-flow mask with reservoir (e.g. Poisk / Summit Oxygen / TopOut):
// part of the flow is lost during exhalation and through leaks. Calibrated against expedition
// guidance: on the summit, 2 L/min "feels like" ~7,800-8,000 m and 4 L/min ~7,000 m while climbing
// (Peacock 1998, BMJ 317:1063; Windsor & Rodway 2006, Br J Hosp Med "Supplemental oxygen and
// sleep at altitude").
constexpr double kMaskEfficiency = 0.55;
constexpr double kReservoirCaptureFraction = 0.5;

double EquivalentAltitudeFromPiO2(double pio2MmHg) {
  const double pbMmHg = pio2MmHg / kO2FractionDryAir + kBodyWaterVapourPressureMmHg;
  return AltitudeForPressureKPa(pbMmHg / kMmHgPerKPa);
}

}  // namespace

Physiology::Physiology(const PhysiologyParams& params) : params_(params) {
  state_.hemoglobinGdl = params_.hemoglobinGdl;
}

double Physiology::SaturationFromPO2(double po2MmHg) {
  const double p = std::fmax(po2MmHg, 0.0);
  return 1.0 / (23400.0 / (p * p * p + 150.0 * p + 1e-9) + 1.0);
}

double Physiology::SaturationFromPO2Shifted(double po2MmHg, double pH, double pco2MmHg) {
  // Severinghaus JW (1979) "Simple, accurate equations for human blood O2 dissociation
  // computations", J Appl Physiol 46:599: virtual PO2 = PO2 * 10^(0.40 (pH-7.40) + 0.06 log10(40/PCO2)).
  // The temperature term is omitted (core temperature ~37 °C).
  const double pco2 = Clamp(pco2MmHg, 3.0, 100.0);
  const double shift = 0.40 * (pH - 7.40) + 0.06 * std::log10(40.0 / pco2);
  return SaturationFromPO2(po2MmHg * std::pow(10.0, shift));
}

double Physiology::AlveolarPO2(double inspiredPO2MmHg, double paCO2MmHg, double respiratoryQuotient) {
  return inspiredPO2MmHg - paCO2MmHg / std::fmax(respiratoryQuotient, 0.5);
}

double Physiology::EffectiveFiO2(double o2FlowLMin, double minuteVentilationLMin) {
  if (o2FlowLMin <= 0.0 || minuteVentilationLMin <= 0.0) return kO2FractionDryAir;
  const double flow = std::fmin(o2FlowLMin, minuteVentilationLMin);
  return (flow + (minuteVentilationLMin - flow) * kO2FractionDryAir) / minuteVentilationLMin;
}

double Physiology::DeliveredFiO2(const OxygenSupply& oxygen, double pressureKPa, double minuteVentilationLMin) {
  if (!oxygen.maskOn || oxygen.flowLitersPerMin <= 0.0) return kO2FractionDryAir;
  // Regulator flow is metered at standard conditions; express it at local pressure and body
  // temperature like the BTPS minute ventilation it mixes into (Boyle + Charles).
  const double expansion = (kSeaLevelPressureKPa / std::fmax(pressureKPa, 1.0)) * (310.15 / 273.15);
  const double offered = oxygen.flowLitersPerMin * expansion * kMaskEfficiency;
  // The reservoir only holds what the climber can inhale: at low ventilation (rest, sleep) the
  // excess spills, so the usable flow saturates at a fraction of the minute ventilation.
  const double capacity = kReservoirCaptureFraction * std::fmax(minuteVentilationLMin, 1.0);
  const double usable = offered / (1.0 + offered / capacity);
  return EffectiveFiO2(usable, minuteVentilationLMin);
}

double Physiology::VentilatoryAdaptationFor(double altitudeM) const {
  const double needed = Need(std::fmin(altitudeM, kCeilingM));
  if (needed < 1.0) return 1.0;
  const double adapted = Clamp01(state_.ventilatoryAcclimatization) * Need(state_.acclimatizedAltitudeM);
  return Clamp01(adapted / needed);
}

double Physiology::HematologicAdaptationFor(double altitudeM) const {
  const double needed = Need(std::fmin(altitudeM, kCeilingM));
  if (needed < 1.0) return 1.0;
  const double adapted = Clamp01(state_.hematologicAcclimatization) * Need(state_.acclimatizedAltitudeM);
  return Clamp01(adapted / needed);
}

double Physiology::VO2MaxAtInspiredPO2(double inspiredPO2MmHg) const {
  const double eqAlt = EquivalentAltitudeFromPiO2(inspiredPO2MmHg);
  double fraction = Interp(kVO2MaxFraction, inspiredPO2MmHg);
  // The table describes acclimatized subjects. Unacclimatized subjects lose a little more
  // (lower CaO2, no hemoconcentration); acclimatization recovers part but never all of the loss.
  const double stress = Clamp01(Need(eqAlt) / Need(kCeilingM));
  const double vent = VentilatoryAdaptationFor(eqAlt);
  const double hem = HematologicAdaptationFor(eqAlt);
  fraction *= 1.0 - 0.08 * (1.0 - vent) * stress - 0.06 * (1.0 - hem) * stress;
  // Illness and deterioration.
  fraction *= 1.0 - 0.5 * Clamp01(state_.hapeSeverity01);
  fraction *= 1.0 - 0.3 * Clamp01(state_.highAltitudeDeterioration01);
  fraction *= 1.0 - 0.012 * Clamp(state_.amsScore, 0.0, 12.0);
  return params_.vo2MaxSeaLevelMlKgMin * std::fmax(fraction, 0.02);
}

GasExchange Physiology::EvaluateGasExchange(double pressureKPa, double exertion01, bool sleeping,
                                            const OxygenSupply& oxygen, const Medications& meds) const {
  GasExchange g;
  const double pbMmHg = pressureKPa * kMmHgPerKPa;
  const double ex = Clamp(exertion01, 0.0, 1.2);
  const double exCapped = std::fmin(ex, 1.0);
  double ve = state_.minuteVentilationLMin > 1.0 ? state_.minuteVentilationLMin : 8.0;
  double vent = 1.0;

  // Ventilation and FiO2 depend on each other through the mask mixing; a few fixed-point passes converge.
  for (int i = 0; i < 5; ++i) {
    g.effectiveFiO2 = DeliveredFiO2(oxygen, pressureKPa, ve);
    g.inspiredPO2MmHg = std::fmax(1.0, g.effectiveFiO2 * (pbMmHg - kBodyWaterVapourPressureMmHg));
    g.equivalentAltitudeM = EquivalentAltitudeFromPiO2(g.inspiredPO2MmHg);
    vent = VentilatoryAdaptationFor(g.equivalentAltitudeM);

    double paco2 = Lerp(Interp(kPaCO2Acute, g.inspiredPO2MmHg), Interp(kPaCO2Acclimatized, g.inspiredPO2MmHg), vent);
    // Acetazolamide: mild metabolic acidosis drives ventilation (Swenson 2016, Eur Respir Rev).
    if (meds.acetazolamide) paco2 *= 0.93;
    // Exercise hyperpnoea beyond metabolic demand, stronger near VO2max (Wasserman 1986).
    paco2 *= 1.0 - 0.12 * exCapped * exCapped;
    // Sleep: hypoventilation raises PaCO2 by ~3 mmHg (Douglas 1982).
    if (sleeping) paco2 *= 1.08;
    g.paCO2MmHg = Clamp(paco2, 6.0, 45.0);

    g.vo2MaxMlKgMin = VO2MaxAtInspiredPO2(g.inspiredPO2MmHg);
    const double vo2 = std::fmax(sleeping ? 3.3 : 3.5, ex * g.vo2MaxMlKgMin) * params_.bodyMassKg;  // mL/min
    const double rq = 0.82 + 0.15 * exCapped;
    const double vco2 = rq * vo2;
    // Alveolar ventilation equation: VA[L/min BTPS] = 0.863 VCO2[mL/min STPD] / PaCO2.
    const double va = 0.863 * vco2 / g.paCO2MmHg;
    const double deadSpaceFraction = 0.30 - 0.12 * exCapped;
    ve = va / (1.0 - deadSpaceFraction);
  }
  g.minuteVentilationLMin = ve;

  g.alveolarPO2MmHg = std::fmax(1.0, AlveolarPO2(g.inspiredPO2MmHg, g.paCO2MmHg, 0.85));
  // A-a gradient: ~5 mmHg at rest (Grocott 2009 measured 5.4 at 8,400 m); diffusion limitation
  // widens it with exercise in hypoxia (Wagner et al. 1987, OEII); HAPE floods alveoli.
  const double hypoxia01 = Clamp01((80.0 - g.alveolarPO2MmHg) / 50.0);
  double aaGradient = 5.0 + ex * (2.0 + 6.0 * hypoxia01) + 25.0 * Clamp01(state_.hapeSeverity01);
  // Periodic breathing during sleep at altitude adds desaturation (West 2010; Bloch 2010).
  if (sleeping) aaGradient += 2.5 * hypoxia01;
  g.arterialPO2MmHg = std::fmax(5.0, g.alveolarPO2MmHg - aaGradient);

  // Acid-base: acute respiratory alkalosis buffers ~0.2 mmol/L HCO3 per mmHg PCO2; chronic renal
  // compensation ~0.5 (Brackett 1965; Krapf 1991). Henderson-Hasselbalch.
  double hco3 = 24.0 - (0.2 + 0.3 * vent) * (40.0 - g.paCO2MmHg);
  if (meds.acetazolamide) hco3 -= 3.0;
  hco3 = std::fmax(hco3, 4.0);
  g.arterialPH = 6.1 + std::log10(hco3 / (0.03 * g.paCO2MmHg));
  g.spo2 = Clamp(SaturationFromPO2Shifted(g.arterialPO2MmHg, g.arterialPH, g.paCO2MmHg), 0.05, 0.995);
  return g;
}

void Physiology::Step(double dtSeconds, const PhysiologyInputs& in) {
  if (dtSeconds <= 0.0) return;
  PhysiologyState& s = state_;

  // 1. Gas exchange with the current adaptation.
  const GasExchange g = EvaluateGasExchange(in.pressureKPa, in.exertion01, in.sleeping, in.oxygen, in.meds);
  s.effectiveFiO2 = g.effectiveFiO2;
  s.inspiredPO2MmHg = g.inspiredPO2MmHg;
  s.paCO2MmHg = g.paCO2MmHg;
  s.alveolarPO2MmHg = g.alveolarPO2MmHg;
  s.arterialPO2MmHg = g.arterialPO2MmHg;
  s.arterialPH = g.arterialPH;
  s.spo2 = g.spo2;
  s.minuteVentilationLMin = g.minuteVentilationLMin;
  s.equivalentAltitudeM = g.equivalentAltitudeM;
  const double eqAlt = g.equivalentAltitudeM;

  // 2. Exposure history (physiological altitude).
  if (s.exposureAltitudeM <= 0.0 && s.recentMaxAltitudeM <= 0.0) {
    s.exposureAltitudeM = std::fmax(eqAlt, 0.0);
    s.recentMaxAltitudeM = s.exposureAltitudeM;
  }
  s.exposureAltitudeM = ApproachExp(s.exposureAltitudeM, eqAlt, dtSeconds,
                                    in.sleeping ? kExposureTauSleepS : kExposureTauAwakeS);
  s.recentMaxAltitudeM = std::fmax(eqAlt, ApproachExp(s.recentMaxAltitudeM, s.exposureAltitudeM, dtSeconds, kRecentMaxTauS));

  // 3. Acclimatization. Work in "adapted metres above 1,500 m" so that external edits of the
  // public fractions are honoured, then write the fractions back.
  {
    const double target = std::fmin(kCeilingM, s.exposureAltitudeM +
                                                   kClimbHighCredit * (s.recentMaxAltitudeM - s.exposureAltitudeM));
    const double needTarget = Need(target);
    const double needAccl = Need(s.acclimatizedAltitudeM);
    double ventAdapted = Clamp01(s.ventilatoryAcclimatization) * needAccl;
    double hemAdapted = Clamp01(s.hematologicAcclimatization) * needAccl;

    const double ventTauUp = kVentTauUpS * (in.meds.acetazolamide ? kAcetazolamideTauFactor : 1.0);
    ventAdapted = ApproachExp(ventAdapted, needTarget, dtSeconds, needTarget > ventAdapted ? ventTauUp : kVentTauDownS);
    hemAdapted = ApproachExp(hemAdapted, needTarget, dtSeconds, needTarget > hemAdapted ? kHemTauUpS : kHemTauDownS);

    double accl = s.acclimatizedAltitudeM;
    if (target > accl) {
      accl = target;
    } else {
      accl = ApproachExp(accl, target, dtSeconds, kVentTauDownS);
    }
    accl = std::fmax(accl, kAcclimatizationThresholdM + std::fmax(ventAdapted, hemAdapted));
    accl = std::fmin(accl, kCeilingM);
    s.acclimatizedAltitudeM = accl;
    const double needNew = Need(accl);
    s.ventilatoryAcclimatization = needNew > 1.0 ? Clamp01(ventAdapted / needNew) : 0.0;
    s.hematologicAcclimatization = needNew > 1.0 ? Clamp01(hemAdapted / needNew) : 0.0;
    s.hemoglobinGdl = params_.hemoglobinGdl + kMaxHemoglobinRiseGdl * Clamp01(hemAdapted / Need(kCeilingM));
  }

  // 4. Altitude illness.
  const double ventAdaptedM = Clamp01(s.ventilatoryAcclimatization) * Need(s.acclimatizedAltitudeM);
  // Tolerance altitude: adaptation keeps paying off for AMS above the ceiling (rotations to Camp 3
  // let climbers sleep at Camp 4 without severe AMS), even though performance no longer improves.
  const double toleranceM = kAcclimatizationThresholdM + ventAdaptedM + std::fmax(0.0, ventAdaptedM - 2500.0);
  const double gap = s.exposureAltitudeM - toleranceM;
  const double exCapped = Clamp01(in.exertion01);
  const double det = Clamp01(s.highAltitudeDeterioration01);

  double amsUnmasked = params_.amsSusceptibility * kAmsScorePerM * std::fmax(0.0, gap - kAmsGapThresholdM);
  amsUnmasked += 3.0 * det + 6.0 * s.haceSeverity01 + 2.0 * s.hapeSeverity01;
  amsUnmasked *= 1.0 + 0.25 * exCapped;            // exertion on arrival worsens AMS (Roach 2000)
  if (in.meds.acetazolamide) amsUnmasked *= 0.75;  // prophylaxis (Low et al. 2012 meta-analysis)
  amsUnmasked = Clamp(amsUnmasked, 0.0, kAmsMaxScore);
  // Dexamethasone relieves symptoms without acclimatizing (Ferrazzini 1987; Hackett 1988).
  const double amsTarget = in.meds.dexamethasone ? 0.5 * amsUnmasked : amsUnmasked;
  s.amsScore = ApproachExp(s.amsScore, amsTarget, dtSeconds, amsTarget > s.amsScore ? kAmsRiseTauS : kAmsFallTauS);
  const double amsForProgression = in.meds.dexamethasone ? s.amsScore * 2.0 : s.amsScore;

  // Descent is the cure: recovery accelerates with metres descended from the recent maximum.
  const double descentBonus = std::fmax(0.0, s.recentMaxAltitudeM - eqAlt) / 500.0;
  const bool onOxygen = in.oxygen.maskOn && in.oxygen.flowLitersPerMin > 0.0;

  // HACE: end stage of untreated severe AMS or very fast ascent; ataxia then coma over 1-3 days
  // (Hackett & Roach 2004, High Alt Med Biol 5:136).
  {
    double drive = params_.haceSusceptibility *
                   (std::fmax(0.0, amsForProgression - 7.0) / 8.0 + 0.25 * std::fmax(0.0, gap - 3000.0) / 1500.0 +
                    0.4 * std::fmax(0.0, det - 0.5));
    if (in.meds.dexamethasone) drive *= 0.35;
    // Once the drivers are gone the brain recovers, faster with descent, dexamethasone and O2.
    const double tau = 1.5 * kDay / (1.0 + descentBonus + (in.meds.dexamethasone ? 1.0 : 0.0) + (onOxygen ? 0.5 : 0.0));
    const double recovery = drive < 0.01 ? s.haceSeverity01 / tau : 0.0;
    s.haceSeverity01 = Clamp01(s.haceSeverity01 + (drive / kDay - recovery) * dtSeconds);
  }
  // HAPE: fast ascent, exertion and cold raise pulmonary artery pressure; worsens with hypoxaemia
  // until descent or O2 (Bärtsch & Swenson 2013, NEJM 368:2294; nifedipine: Bärtsch 1991).
  {
    const double cold01 = Clamp01(-in.ambientTempC / 30.0);
    double drive = params_.hapeSusceptibility *
                   (0.15 * std::fmax(0.0, gap - 2500.0) / 1500.0 + 0.15 * std::fmax(0.0, amsForProgression - 8.0) / 2.0 +
                    0.4 * std::fmax(0.0, det - 0.5)) *
                   (1.0 + 0.8 * exCapped + 0.5 * cold01);
    // Established HAPE feeds on its own hypoxaemia (hypoxic pulmonary vasoconstriction), so left
    // at altitude it worsens; descent or O2 that restores SpO2 breaks the cycle.
    const double feedback = 0.5 * s.hapeSeverity01 * SmoothStep(0.85, 0.60, s.spo2);
    if (in.meds.nifedipine) drive *= 0.4;
    const double tau = 2.0 * kDay /
                       (1.0 + descentBonus + (in.meds.nifedipine ? 0.5 : 0.0) + (s.spo2 > 0.9 ? 1.0 : 0.0));
    const double recovery = drive < 0.01 ? s.hapeSeverity01 / tau : 0.0;
    s.hapeSeverity01 = Clamp01(s.hapeSeverity01 + ((drive + feedback) / kDay - recovery) * dtSeconds);
  }

  // 5. Death-zone deterioration: accumulates above ~7,000 m and only recovers lower down.
  if (eqAlt > kDeteriorationOnsetM) {
    const double excessKm = (eqAlt - kDeteriorationOnsetM) / 1000.0;
    s.highAltitudeDeterioration01 =
        Clamp01(s.highAltitudeDeterioration01 + 0.12 * std::pow(excessKm, 1.3) * dtSeconds / kDay);
  } else if (eqAlt < kDeteriorationRecoveryM) {
    const double tau = (eqAlt < 5500.0 ? 3.0 : 6.0) * kDay;
    s.highAltitudeDeterioration01 = ApproachExp(s.highAltitudeDeterioration01, 0.0, dtSeconds, tau);
  }
  if (in.altitudeM >= kDeathZoneAltitudeM) s.secondsInDeathZone += dtSeconds;
}

double Physiology::CurrentVO2MaxMlKgMin() const { return VO2MaxAtInspiredPO2(state_.inspiredPO2MmHg); }

double Physiology::CognitiveImpairment01() const {
  // Hypoxic impairment starts around SpO2 85-90 % and is severe below ~50 % (Virués-Ortega 2004
  // review "Neuropsychological functioning associated with high-altitude exposure").
  const double hypoxic = 0.75 * SmoothStep(0.90, 0.40, state_.spo2);
  return Clamp01(hypoxic + state_.haceSeverity01 + 0.03 * state_.amsScore + 0.2 * state_.highAltitudeDeterioration01);
}

double Physiology::MaxSustainableExertion01() const {
  // Self-paced multi-hour work: ~0.65 of VO2max near sea level, ~0.55 at extreme altitude
  // (Pugh 1964; Fulco et al. 1998). AMS and deterioration already act through VO2max; pulmonary
  // and cerebral oedema also wreck the ability to keep going.
  const double alt = state_.equivalentAltitudeM;
  double f = 0.65 - 0.10 * SmoothStep(1500.0, 7500.0, alt);
  f -= 0.20 * state_.hapeSeverity01;
  f -= 0.20 * state_.haceSeverity01;
  return Clamp(f, 0.25, 0.70);
}

double Physiology::EquivalentAltitudeM(double pressureKPa) const {
  return EquivalentAltitudeFromPiO2(InspiredPO2MmHg(pressureKPa, state_.effectiveFiO2));
}

}  // namespace summit::sim
