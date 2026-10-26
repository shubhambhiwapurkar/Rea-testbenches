#pragma once

#include <cstdint>

namespace summit::sim {

// Bottled oxygen system (e.g. a Summit Oxygen / Poisk set). flow = 0 means no supplemental O2.
struct OxygenSupply {
  bool maskOn = false;
  double flowLitersPerMin = 0.0;  // typical: 0.5 sleeping, 2-4 climbing, 4+ emergency
};

struct Medications {
  bool acetazolamide = false;  // speeds ventilatory acclimatization, AMS prophylaxis
  bool dexamethasone = false;  // treats AMS/HACE symptoms (masks, doesn't acclimatize)
  bool nifedipine = false;     // HAPE treatment/prophylaxis
};

struct PhysiologyParams {
  double bodyMassKg = 75.0;
  double vo2MaxSeaLevelMlKgMin = 55.0;   // trained mountaineer ~50-65
  double hemoglobinGdl = 15.0;           // sea-level baseline
  double amsSusceptibility = 1.0;        // individual multiplier, 0.5 (resistant) .. 2.0 (susceptible)
  double hapeSusceptibility = 1.0;
  double haceSusceptibility = 1.0;
};

struct PhysiologyInputs {
  double altitudeM = 0.0;
  double pressureKPa = 101.325;
  double exertion01 = 0.0;   // fraction of *current* (altitude-limited) VO2max being used
  bool sleeping = false;
  OxygenSupply oxygen;
  Medications meds;
};

struct PhysiologyState {
  // Acclimatization. Ventilatory: days-scale. Hematologic: weeks-scale. Both 0..1 relative to full
  // adaptation to `acclimatizedAltitudeM`.
  double ventilatoryAcclimatization = 0.0;
  double hematologicAcclimatization = 0.0;
  double acclimatizedAltitudeM = 0.0;   // altitude the body is currently adapted to
  double hemoglobinGdl = 15.0;

  // Gas exchange (updated every step)
  double effectiveFiO2 = 0.2093;        // after supplemental O2 mixing
  double inspiredPO2MmHg = 149.0;
  double paCO2MmHg = 40.0;              // falls with hyperventilation
  double alveolarPO2MmHg = 100.0;
  double spo2 = 0.98;                   // 0..1
  double minuteVentilationLMin = 7.0;

  // Altitude illness (0 = none). amsScore mirrors the Lake Louise score (0..12, >=3 = AMS).
  double amsScore = 0.0;
  double hapeSeverity01 = 0.0;
  double haceSeverity01 = 0.0;

  // Cumulative deterioration above ~7500 m that acclimatization cannot fix.
  double highAltitudeDeterioration01 = 0.0;
  double secondsInDeathZone = 0.0;
};

class Physiology {
 public:
  explicit Physiology(const PhysiologyParams& params = {});

  void Step(double dtSeconds, const PhysiologyInputs& in);

  const PhysiologyParams& Params() const { return params_; }
  const PhysiologyState& State() const { return state_; }
  PhysiologyState& MutableState() { return state_; }

  // VO2max achievable right now (mL/kg/min): falls with lower inspired PO2 and rises with acclimatization.
  // Calibration: ~15 mL/kg/min on the Everest summit breathing air for a 55-60 sea-level athlete
  // (West et al. 1983, AMREE; Operation Everest II).
  double CurrentVO2MaxMlKgMin() const;

  // 0 = clear-headed, 1 = severe impairment (hypoxic plus HACE). Drives UI blur and slow inputs.
  double CognitiveImpairment01() const;

  // Fraction of current VO2max that can be held for hours (≈0.5-0.6 at altitude).
  double MaxSustainableExertion01() const;

  // Pressure altitude equivalent of breathing the current effective FiO2 ("feels like" altitude).
  double EquivalentAltitudeM(double pressureKPa) const;

  // Severinghaus (1979) oxygen-haemoglobin dissociation: SO2 = 1 / (23400/(PO2^3 + 150 PO2) + 1).
  static double SaturationFromPO2(double po2MmHg);

  // Alveolar gas equation: PAO2 = PiO2 - PaCO2 / RQ (simplified form).
  static double AlveolarPO2(double inspiredPO2MmHg, double paCO2MmHg, double respiratoryQuotient = 0.85);

  // Effective FiO2 with an open-circuit mask: O2 flow mixes with entrained ambient air.
  static double EffectiveFiO2(double o2FlowLMin, double minuteVentilationLMin);

 private:
  PhysiologyParams params_;
  PhysiologyState state_;
};

}  // namespace summit::sim
