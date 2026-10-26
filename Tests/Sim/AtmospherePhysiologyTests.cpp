// Atmosphere and physiology checked against published field data.

#include "Sim/Atmosphere.h"
#include "Sim/Constants.h"
#include "Sim/Physiology.h"
#include "TestFramework.h"

using namespace summit::sim;

namespace {

PhysiologyInputs InputsAt(double altitudeM, double exertion = 0.0, bool sleeping = false) {
  PhysiologyInputs in;
  in.altitudeM = altitudeM;
  in.pressureKPa = BarometricPressureKPa(altitudeM);
  in.exertion01 = exertion;
  in.sleeping = sleeping;
  in.ambientTempC = -10.0;
  return in;
}

void Hold(Physiology& p, double hours, const PhysiologyInputs& in) {
  for (double t = 0.0; t < hours * 3600.0; t += 60.0) p.Step(60.0, in);
}

// A climber who has completed the usual rotations (as the CLI and the UE layer set it up).
Physiology FullyAcclimatized(double vo2MaxSeaLevel = 60.0) {
  PhysiologyParams params;
  params.vo2MaxSeaLevelMlKgMin = vo2MaxSeaLevel;
  Physiology p(params);
  auto& s = p.MutableState();
  s.acclimatizedAltitudeM = Physiology::kAcclimatizationCeilingM;
  s.ventilatoryAcclimatization = 1.0;
  s.hematologicAcclimatization = 1.0;
  return p;
}

const OxygenSupply kNoOxygen{};
const Medications kNoMeds{};

}  // namespace

// --- Atmosphere ---------------------------------------------------------------------------------

TEST_CASE("Atmosphere: West 1996 Everest summit pressure ~253 Torr (33.7 kPa) in May") {
  const double pb = BarometricPressureKPa(8848.0, 28.0, 140);
  CHECK_NEAR(pb, 33.7, 0.3);
  CHECK_NEAR(pb * kMmHgPerKPa, 253.0, 2.5);
}

TEST_CASE("Atmosphere: Everest Base Camp 5364 m ~53-54 kPa") {
  CHECK_RANGE(BarometricPressureKPa(5364.0), 53.0, 54.0);
}

TEST_CASE("Atmosphere: lower pressure at Denali latitude (63N, 6190 m: observed ~46.5 kPa)") {
  const double denali = BarometricPressureKPa(6190.0, 63.0, 160);
  const double himalaya = BarometricPressureKPa(6190.0, 28.0, 160);
  CHECK_NEAR(denali, 46.5, 0.6);
  CHECK(himalaya - denali > 1.0);
  CHECK_RANGE(himalaya, 47.5, 48.5);
}

TEST_CASE("Atmosphere: winter is a few percent lower at extreme altitude") {
  const double may = BarometricPressureKPa(8848.0, 28.0, 140);
  const double january = BarometricPressureKPa(8848.0, 28.0, 15);
  const double july = BarometricPressureKPa(8848.0, 28.0, 200);
  const double drop = (may - january) / may;
  CHECK_RANGE(drop, 0.02, 0.05);
  CHECK(july > may);
  // Sea level is not affected by the altitude-scaled corrections.
  CHECK_NEAR(BarometricPressureKPa(0.0, 63.0, 15), BarometricPressureKPa(0.0, 28.0, 140), 1e-9);
}

TEST_CASE("Atmosphere: ISA reference values") {
  CHECK_NEAR(IsaPressureKPa(0.0), 101.325, 1e-6);
  CHECK_NEAR(IsaPressureKPa(11000.0), 22.63, 0.05);
  CHECK_NEAR(IsaPressureKPa(20000.0), 5.47, 0.05);
  // ISA underestimates Himalayan pressure (West 1996): ~31.4 vs 33.7 kPa on the summit.
  CHECK(IsaPressureKPa(8848.0) < BarometricPressureKPa(8848.0) - 1.5);
}

TEST_CASE("Atmosphere: density, inspired PO2 and the West inverse") {
  CHECK_NEAR(AirDensityKgM3(101.325, 15.0), 1.225, 0.003);
  CHECK_NEAR(InspiredPO2MmHg(kSeaLevelPressureKPa), 149.2, 0.5);
  CHECK_NEAR(InspiredPO2MmHg(BarometricPressureKPa(8848.0)), 43.0, 1.0);  // West 1983: PiO2 ~43 mmHg
  for (double h : {0.0, 2860.0, 5364.0, 8848.0}) CHECK_NEAR(AltitudeForPressureKPa(BarometricPressureKPa(h)), h, 1.0);
}

TEST_CASE("Atmosphere: boiling point of water (Antoine)") {
  CHECK_NEAR(WaterBoilingPointC(101.325), 100.0, 0.1);
  CHECK_NEAR(WaterBoilingPointC(33.7), 71.5, 1.0);
  CHECK(WaterBoilingPointC(53.4) < 84.0);
}

// --- Static gas-exchange equations -----------------------------------------------------------

TEST_CASE("Physiology: Severinghaus 1979 dissociation curve") {
  CHECK_NEAR(Physiology::SaturationFromPO2(40.0), 0.75, 0.01);
  CHECK_NEAR(Physiology::SaturationFromPO2(100.0), 0.977, 0.003);
  CHECK_NEAR(Physiology::SaturationFromPO2(27.0), 0.50, 0.02);
  // Alkalosis shifts the curve left (Bohr): Caudwell 8,400 m, pH 7.53 and PaCO2 13.3 mmHg.
  CHECK(Physiology::SaturationFromPO2Shifted(24.6, 7.53, 13.3) > Physiology::SaturationFromPO2(24.6) + 0.08);
  CHECK_NEAR(Physiology::SaturationFromPO2Shifted(24.6, 7.53, 13.3), 0.54, 0.04);
}

TEST_CASE("Physiology: alveolar gas equation reproduces West 1983 summit PAO2 ~35 mmHg") {
  CHECK_NEAR(Physiology::AlveolarPO2(43.0, 7.5), 35.0, 1.5);
  CHECK_NEAR(Physiology::AlveolarPO2(149.0, 40.0), 102.0, 1.0);
}

TEST_CASE("Physiology: open-circuit mask mixing formula") {
  CHECK_NEAR(Physiology::EffectiveFiO2(0.0, 40.0), kO2FractionDryAir, 1e-12);
  CHECK_NEAR(Physiology::EffectiveFiO2(2.0, 40.0), (2.0 + 38.0 * kO2FractionDryAir) / 40.0, 1e-12);
  CHECK_NEAR(Physiology::EffectiveFiO2(50.0, 40.0), 1.0, 1e-12);
  OxygenSupply off;
  off.flowLitersPerMin = 3.0;  // mask not worn
  CHECK_NEAR(Physiology::DeliveredFiO2(off, 33.7, 60.0), kO2FractionDryAir, 1e-12);
}

// --- Field gas exchange --------------------------------------------------------------------------

TEST_CASE("Physiology: resting SpO2 at sea level, Lukla (unacclimatized) and EBC (acclimatized)") {
  Physiology fresh;
  const auto sea = fresh.EvaluateGasExchange(kSeaLevelPressureKPa, 0.0, false, kNoOxygen, kNoMeds);
  CHECK_RANGE(sea.spo2, 0.97, 0.99);
  CHECK_NEAR(sea.paCO2MmHg, 40.0, 0.5);
  CHECK_NEAR(sea.arterialPH, 7.40, 0.02);
  const auto lukla = fresh.EvaluateGasExchange(BarometricPressureKPa(2860.0), 0.0, false, kNoOxygen, kNoMeds);
  CHECK_RANGE(lukla.spo2, 0.90, 0.94);
  Physiology acc = FullyAcclimatized();
  const auto ebc = acc.EvaluateGasExchange(BarometricPressureKPa(5364.0), 0.0, false, kNoOxygen, kNoMeds);
  CHECK_RANGE(ebc.spo2, 0.80, 0.88);
  const auto ebcFresh = fresh.EvaluateGasExchange(BarometricPressureKPa(5364.0), 0.0, false, kNoOxygen, kNoMeds);
  CHECK(ebcFresh.spo2 < ebc.spo2 - 0.05);  // acclimatization raises SpO2
  CHECK(ebcFresh.paCO2MmHg > ebc.paCO2MmHg + 5.0);
}

TEST_CASE("Physiology: Caudwell Xtreme Everest arterial gases at 8,400 m (Grocott 2009)") {
  Physiology acc = FullyAcclimatized();
  const auto g = acc.EvaluateGasExchange(BarometricPressureKPa(8400.0), 0.0, false, kNoOxygen, kNoMeds);
  CHECK_RANGE(g.arterialPO2MmHg, 22.0, 28.0);  // measured 24.6
  CHECK_RANGE(g.paCO2MmHg, 11.5, 15.0);        // measured 13.3
  CHECK_RANGE(g.spo2, 0.48, 0.62);             // measured 54 %
  CHECK_RANGE(g.arterialPH, 7.48, 7.58);       // measured 7.53
}

TEST_CASE("Physiology: summit hyperventilation and monotonic desaturation") {
  Physiology acc = FullyAcclimatized();
  const auto summit = acc.EvaluateGasExchange(BarometricPressureKPa(8848.0), 0.0, false, kNoOxygen, kNoMeds);
  const auto g8400 = acc.EvaluateGasExchange(BarometricPressureKPa(8400.0), 0.0, false, kNoOxygen, kNoMeds);
  CHECK(summit.paCO2MmHg < g8400.paCO2MmHg);  // PaCO2 keeps falling (West 1983: 7.5 mmHg)
  CHECK_RANGE(summit.paCO2MmHg, 7.0, 13.0);
  CHECK_RANGE(summit.alveolarPO2MmHg, 27.0, 37.0);  // West 1983: ~35
  CHECK(summit.spo2 < g8400.spo2);
  double previous = 1.0;
  for (double h = 0.0; h <= 8848.0; h += 250.0) {
    const double s = acc.EvaluateGasExchange(BarometricPressureKPa(h), 0.0, false, kNoOxygen, kNoMeds).spo2;
    CHECK(s <= previous + 1e-9);
    previous = s;
  }
}

TEST_CASE("Physiology: exercise and sleep desaturate more than rest at altitude") {
  Physiology acc = FullyAcclimatized();
  const double p = BarometricPressureKPa(6400.0);
  const auto rest = acc.EvaluateGasExchange(p, 0.0, false, kNoOxygen, kNoMeds);
  const auto work = acc.EvaluateGasExchange(p, 0.7, false, kNoOxygen, kNoMeds);
  const auto sleep = acc.EvaluateGasExchange(p, 0.0, true, kNoOxygen, kNoMeds);
  CHECK(work.spo2 < rest.spo2 - 0.02);
  CHECK(sleep.spo2 < rest.spo2 - 0.02);
  CHECK(work.minuteVentilationLMin > 4.0 * rest.minuteVentilationLMin);
  CHECK(work.paCO2MmHg < rest.paCO2MmHg);
}

TEST_CASE("Physiology: VO2max ~15 mL/kg/min on the summit and ~7-10 % per 1,000 m") {
  Physiology acc = FullyAcclimatized(60.0);
  const auto summit = acc.EvaluateGasExchange(BarometricPressureKPa(8848.0), 0.6, false, kNoOxygen, kNoMeds);
  CHECK_RANGE(summit.vo2MaxMlKgMin, 13.5, 17.0);
  const double at1500 = acc.VO2MaxAtInspiredPO2(InspiredPO2MmHg(BarometricPressureKPa(1500.0)));
  const double at4500 = acc.VO2MaxAtInspiredPO2(InspiredPO2MmHg(BarometricPressureKPa(4500.0)));
  const double perKm = (1.0 - at4500 / at1500) / 3.0;
  CHECK_RANGE(perKm, 0.06, 0.11);
  // Acclimatization recovers part of the loss.
  Physiology fresh(acc.Params());
  CHECK(fresh.VO2MaxAtInspiredPO2(74.0) < acc.VO2MaxAtInspiredPO2(74.0));
  CHECK(fresh.VO2MaxAtInspiredPO2(74.0) > 0.8 * acc.VO2MaxAtInspiredPO2(74.0));
  // Sustainable fraction for hours.
  Hold(acc, 0.5, InputsAt(8000.0, 0.5));
  CHECK_RANGE(acc.MaxSustainableExertion01(), 0.45, 0.65);
}

TEST_CASE("Physiology: supplemental O2 equivalent altitude on the summit") {
  Physiology acc = FullyAcclimatized(60.0);
  const double p = BarometricPressureKPa(8848.0);
  OxygenSupply two{true, 2.0};
  OxygenSupply four{true, 4.0};
  const auto g2 = acc.EvaluateGasExchange(p, 0.6, false, two, kNoMeds);
  const auto g4 = acc.EvaluateGasExchange(p, 0.6, false, four, kNoMeds);
  CHECK_RANGE(g2.equivalentAltitudeM, 7600.0, 8150.0);  // expedition doctrine: ~7,800-8,000 m
  CHECK_RANGE(g4.equivalentAltitudeM, 6700.0, 7400.0);  // ~7,000 m
  const auto air = acc.EvaluateGasExchange(p, 0.6, false, kNoOxygen, kNoMeds);
  CHECK(g4.spo2 > air.spo2 + 0.08);
  CHECK(g4.vo2MaxMlKgMin > 1.5 * air.vo2MaxMlKgMin);

  // Sleeping O2 at 0.5 L/min works because ventilation is low asleep.
  const double p7900 = BarometricPressureKPa(7900.0);
  OxygenSupply sleepFlow{true, 0.5};
  const auto asleepAir = acc.EvaluateGasExchange(p7900, 0.0, true, kNoOxygen, kNoMeds);
  const auto asleepO2 = acc.EvaluateGasExchange(p7900, 0.0, true, sleepFlow, kNoMeds);
  CHECK(asleepO2.equivalentAltitudeM < asleepAir.equivalentAltitudeM - 800.0);
  CHECK(asleepO2.spo2 > asleepAir.spo2 + 0.05);

  // EquivalentAltitudeM after a step.
  PhysiologyInputs in = InputsAt(8848.0, 0.6);
  in.oxygen = two;
  acc.Step(60.0, in);
  CHECK_NEAR(acc.EquivalentAltitudeM(p), acc.State().equivalentAltitudeM, 1.0);
  CHECK(acc.State().effectiveFiO2 > kO2FractionDryAir);
  Physiology plain;
  plain.Step(60.0, InputsAt(4000.0));
  CHECK_NEAR(plain.EquivalentAltitudeM(BarometricPressureKPa(4000.0)), 4000.0, 5.0);
}

// --- Acclimatization -----------------------------------------------------------------------------

TEST_CASE("Physiology: ventilatory acclimatization takes days, hematologic weeks") {
  Physiology p;
  const PhysiologyInputs at4300 = InputsAt(4300.0, 0.1);
  Hold(p, 24.0, at4300);
  CHECK(p.State().ventilatoryAcclimatization < 0.5);
  Hold(p, 6.0 * 24.0, at4300);
  CHECK(p.State().ventilatoryAcclimatization > 0.8);
  CHECK(p.State().hematologicAcclimatization < 0.5);
  Hold(p, 21.0 * 24.0, at4300);  // 28 days total
  const double hbRise = p.State().hemoglobinGdl - p.Params().hemoglobinGdl;
  CHECK_RANGE(hbRise, 1.0, 2.5);
  CHECK_NEAR(p.State().acclimatizedAltitudeM, 4300.0, 50.0);

  // Deacclimatization over 2-3 weeks at sea level.
  const double adaptedBefore = p.State().ventilatoryAcclimatization * (p.State().acclimatizedAltitudeM - 1500.0);
  Hold(p, 21.0 * 24.0, InputsAt(0.0));
  const double adaptedAfter = p.State().ventilatoryAcclimatization * (p.State().acclimatizedAltitudeM - 1500.0);
  CHECK(adaptedAfter < 0.5 * adaptedBefore);
}

TEST_CASE("Physiology: acetazolamide speeds ventilatory acclimatization") {
  Physiology plain;
  Physiology diamox;
  PhysiologyInputs in = InputsAt(4000.0, 0.1);
  Hold(plain, 72.0, in);
  in.meds.acetazolamide = true;
  Hold(diamox, 72.0, in);
  CHECK(diamox.State().ventilatoryAcclimatization > plain.State().ventilatoryAcclimatization + 0.05);
  CHECK(diamox.State().amsScore <= plain.State().amsScore);
}

TEST_CASE("Physiology: acclimatization cannot exceed the ~5,400 m ceiling") {
  Physiology p;
  Hold(p, 40.0 * 24.0, InputsAt(6400.0, 0.1));
  CHECK(p.State().acclimatizedAltitudeM <= Physiology::kAcclimatizationCeilingM + 1e-6);
  CHECK(p.State().ventilatoryAcclimatization > 0.9);
}

TEST_CASE("Physiology: death-zone deterioration accumulates over days and recovers lower down") {
  Physiology p = FullyAcclimatized();
  Hold(p, 72.0, InputsAt(8000.0, 0.1));
  const double det = p.State().highAltitudeDeterioration01;
  CHECK_RANGE(det, 0.2, 0.7);
  CHECK_NEAR(p.State().secondsInDeathZone, 72.0 * 3600.0, 60.0);
  Hold(p, 48.0, InputsAt(6800.0, 0.1));  // between 6,500 and 7,000 m: no recovery
  CHECK_NEAR(p.State().highAltitudeDeterioration01, det, 0.01);
  Hold(p, 7.0 * 24.0, InputsAt(5300.0, 0.1));
  CHECK(p.State().highAltitudeDeterioration01 < 0.5 * det);
}

// --- Altitude illness ----------------------------------------------------------------------------

TEST_CASE("Physiology: flying to 5,300 m produces AMS; descent relieves it") {
  Physiology p;
  double peak = 0.0;
  for (int h = 0; h < 36; ++h) {
    Hold(p, 1.0, InputsAt(5300.0, 0.05, h >= 14 && h < 24));
    if (p.State().amsScore > peak) peak = p.State().amsScore;
  }
  CHECK(peak >= 3.0);
  const double before = p.State().amsScore;
  Physiology descended = p;
  Hold(descended, 24.0, InputsAt(3500.0, 0.05));
  Hold(p, 24.0, InputsAt(5300.0, 0.05));
  CHECK(descended.State().amsScore < 0.5 * before);
  CHECK(descended.State().amsScore < p.State().amsScore);
}

TEST_CASE("Physiology: susceptibility scales AMS; dexamethasone masks symptoms") {
  PhysiologyParams resistant;
  resistant.amsSusceptibility = 0.5;
  PhysiologyParams susceptible;
  susceptible.amsSusceptibility = 2.0;
  Physiology a(resistant);
  Physiology b(susceptible);
  Physiology c(susceptible);
  PhysiologyInputs in = InputsAt(4200.0, 0.1);
  Hold(a, 12.0, in);
  Hold(b, 12.0, in);
  in.meds.dexamethasone = true;
  Hold(c, 12.0, in);
  CHECK(b.State().amsScore > 2.0 * a.State().amsScore);
  CHECK(c.State().amsScore < 0.7 * b.State().amsScore);
}

TEST_CASE("Physiology: HAPE progresses over days at altitude, nifedipine slows it, descent and O2 cure it") {
  PhysiologyParams prone;
  prone.hapeSusceptibility = 2.0;
  Physiology untreated(prone);
  Physiology nifedipine(prone);
  PhysiologyInputs in = InputsAt(5500.0, 0.5);
  in.ambientTempC = -20.0;
  Hold(untreated, 24.0, in);
  CHECK(untreated.State().hapeSeverity01 > 0.05);
  CHECK(untreated.State().hapeSeverity01 < 1.0);  // hours to days, not minutes
  in.meds.nifedipine = true;
  Hold(nifedipine, 24.0, in);
  CHECK(nifedipine.State().hapeSeverity01 < untreated.State().hapeSeverity01);

  Physiology sick(prone);
  sick.MutableState().hapeSeverity01 = 0.5;
  sick.MutableState().recentMaxAltitudeM = 5500.0;
  sick.MutableState().exposureAltitudeM = 5500.0;
  Physiology descend = sick;
  Physiology oxygen = sick;
  const auto spo2Sick = sick.EvaluateGasExchange(BarometricPressureKPa(5500.0), 0.0, false, kNoOxygen, kNoMeds).spo2;
  const auto spo2Well = Physiology(prone).EvaluateGasExchange(BarometricPressureKPa(5500.0), 0.0, false, kNoOxygen, kNoMeds).spo2;
  CHECK(spo2Sick < spo2Well - 0.1);  // flooded alveoli widen the A-a gradient
  Hold(descend, 48.0, InputsAt(3500.0, 0.0));
  CHECK(descend.State().hapeSeverity01 < 0.2);
  PhysiologyInputs o2In = InputsAt(5500.0, 0.0);
  o2In.oxygen = OxygenSupply{true, 4.0};
  o2In.meds.nifedipine = true;
  Hold(oxygen, 48.0, o2In);
  CHECK(oxygen.State().hapeSeverity01 < 0.5);
}

TEST_CASE("Physiology: dexamethasone and descent treat HACE") {
  Physiology a;
  a.MutableState().haceSeverity01 = 0.5;
  a.MutableState().acclimatizedAltitudeM = 5000.0;
  a.MutableState().ventilatoryAcclimatization = 1.0;
  Physiology b = a;
  PhysiologyInputs in = InputsAt(5000.0, 0.0);
  Hold(a, 24.0, in);
  in.meds.dexamethasone = true;
  Hold(b, 24.0, in);
  CHECK(a.State().haceSeverity01 < 0.5);
  CHECK(b.State().haceSeverity01 < a.State().haceSeverity01);
  CHECK(a.CognitiveImpairment01() > Physiology().CognitiveImpairment01());
}

TEST_CASE("Physiology: cognitive impairment rises with hypoxia") {
  Physiology acc = FullyAcclimatized();
  Hold(acc, 0.2, InputsAt(5364.0));
  const double ebc = acc.CognitiveImpairment01();
  Hold(acc, 0.2, InputsAt(8848.0));
  const double summit = acc.CognitiveImpairment01();
  CHECK(summit > ebc + 0.2);
  CHECK_RANGE(summit, 0.3, 0.9);
}
