#pragma once

#include "Sim/Export.h"

#include <cstdint>

namespace summit::sim {

// Bottled oxygen system (e.g. a Summit Oxygen / Poisk set). flow = 0 means no supplemental O2.
struct SUMMITSIM_API OxygenSupply {
  bool maskOn = false;
  double flowLitersPerMin = 0.0;  // typical: 0.5 sleeping, 2-4 climbing, 4+ emergency
};

struct SUMMITSIM_API Medications {
  bool acetazolamide = false;  // speeds ventilatory acclimatization, AMS prophylaxis
  bool dexamethasone = false;  // treats AMS/HACE symptoms (masks, doesn't acclimatize)
  bool nifedipine = false;     // HAPE treatment/prophylaxis
};

struct SUMMITSIM_API PhysiologyParams {
  double bodyMassKg = 75.0;
  double vo2MaxSeaLevelMlKgMin = 55.0;   // trained mountaineer ~50-65
  double hemoglobinGdl = 15.0;           // sea-level baseline
  double amsSusceptibility = 1.0;        // individual multiplier, 0.5 (resistant) .. 2.0 (susceptible)
  double hapeSusceptibility = 1.0;
  double haceSusceptibility = 1.0;
};

struct SUMMITSIM_API PhysiologyInputs {
  double altitudeM = 0.0;
  double pressureKPa = 101.325;
  double exertion01 = 0.0;   // fraction of *current* (altitude-limited) VO2max being used
  bool sleeping = false;
  OxygenSupply oxygen;
  Medications meds;
  double ambientTempC = 15.0;  // cold air and cold exposure promote HAPE
};

// One evaluation of the ventilation / gas-exchange model (see Physiology.cpp for sources).
struct SUMMITSIM_API GasExchange {
  double effectiveFiO2 = 0.2093;
  double inspiredPO2MmHg = 149.0;
  double paCO2MmHg = 40.0;
  double alveolarPO2MmHg = 100.0;
  double arterialPO2MmHg = 95.0;
  double arterialPH = 7.40;
  double spo2 = 0.98;
  double minuteVentilationLMin = 7.0;   // BTPS
  double vo2MaxMlKgMin = 55.0;          // achievable at this inspired PO2
  double equivalentAltitudeM = 0.0;     // altitude with the same PiO2 breathing air
};

struct SUMMITSIM_API PhysiologyState {
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

  // Additional state (saved with the rest). Exposure altitudes are physiological (equivalent)
  // altitudes, so breathing bottled O2 lowers them.
  double exposureAltitudeM = 0.0;    // time-weighted recent altitude, dominated by sleeping altitude
  double recentMaxAltitudeM = 0.0;   // decaying recent maximum ("climb high, sleep low" credit)
  double equivalentAltitudeM = 0.0;  // pressure altitude with the same PiO2 breathing air
  double arterialPO2MmHg = 95.0;
  double arterialPH = 7.40;
};

class SUMMITSIM_API Physiology {
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

  // FiO2 actually breathed with a real mask set: regulator flow (litres at standard conditions)
  // expands at altitude, and a constant-flow mask with reservoir wastes part of it. Calibrated so
  // that 2 L/min on the summit while climbing ≈ 7,900 m and 4 L/min ≈ 7,000 m.
  static double DeliveredFiO2(const OxygenSupply& oxygen, double pressureKPa, double minuteVentilationLMin);

  // Severinghaus (1979) with the in-vivo Bohr shift (pH and PCO2 correction of the "virtual" PO2).
  static double SaturationFromPO2Shifted(double po2MmHg, double pH, double pco2MmHg);

  // Ventilation, alveolar and arterial gases for given conditions using the current acclimatization
  // state, without advancing time. Useful for previews (e.g. max speed with a different O2 flow).
  GasExchange EvaluateGasExchange(double pressureKPa, double exertion01, bool sleeping,
                                  const OxygenSupply& oxygen, const Medications& meds) const;

  // VO2max (mL/kg/min) at a given inspired PO2 with the current acclimatization and illness state.
  double VO2MaxAtInspiredPO2(double inspiredPO2MmHg) const;

  // 0..1 ventilatory / hematologic adaptation relative to what the given altitude demands
  // (capped at the ~5,400 m ceiling of full acclimatization).
  double VentilatoryAdaptationFor(double altitudeM) const;
  double HematologicAdaptationFor(double altitudeM) const;

  // Altitude above which acclimatization cannot progress (no permanent habitation above it).
  static constexpr double kAcclimatizationCeilingM = 5400.0;

 private:
  PhysiologyParams params_;
  PhysiologyState state_;
};

}  // namespace summit::sim
