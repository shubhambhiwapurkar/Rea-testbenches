#include "Sim/Expedition.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Sim/Math.h"

namespace summit::sim {

// File-specific namespace (not anonymous) so Unreal unity builds cannot collide with other sim files.
namespace expedition_detail {

// Wilderness Medical Society practice guidelines for acute altitude illness, 2019 update
// (Luks AM et al., Wilderness Environ Med 30(4S):S3-S18):
//  - above 3000 m, do not increase sleeping elevation by more than 500 m per day;
//  - include a rest day (no increase in sleeping elevation) every 3-4 days;
//  - older formulations of the same guidance: a rest day for every ~1000 m gained.
constexpr double kGuidelineThresholdM = 3000.0;
constexpr double kMaxDailySleepGainM = 500.0;
constexpr int kMaxAscentDaysWithoutRest = 3;
constexpr double kGainPerRestDayM = 1000.0;

// Heuristic AMS model (game calibration, documented against the literature):
//  - the body acclimatizes roughly 400 m of sleeping altitude per night of exposure;
//  - "climb high, sleep low" day trips add extra acclimatization (WMS 2019; Muza et al. 2010);
//  - a one-night jump of ~1500 m above the acclimatized level gives ~60 % AMS, as in rapid ascents to
//    4559 m (Schneider et al. 2002, 58 % AMS in non-preacclimatized climbers ascending in < 2 days);
//  - a 500 m/day profile gives ~20-30 %, close to the AMS incidence of guideline-following treks.
constexpr double kNightlyAcclimatizationM = 400.0;
constexpr double kRotationCreditFraction = 0.3;
constexpr double kRotationCreditMaxM = 300.0;
constexpr double kBaselineRisk = 0.05;
constexpr double kExcessRiskScale = 0.6;
constexpr double kExcessForFullRiskM = 1500.0;

std::string CampLabel(const Camp* camp, const std::string& id) {
  if (camp == nullptr) return "'" + id + "'";
  return camp->name.empty() ? camp->id : camp->name;
}

std::string FormatMeters(double m) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f m", m);
  return buf;
}

// Real-gas correction for oxygen: compressibility Z(P) at ~15-20 °C, a quadratic fit to the NIST
// Chemistry WebBook isotherm (Z ~0.975 at 150 bar, ~0.99 at 200 bar, ~1.05 at 300 bar). Above ~250 bar
// oxygen is less compressible than an ideal gas, so a 300 bar fill holds ~5 % less than V x P.
double OxygenCompressibility(double pressureBar) {
  const double p = Clamp(pressureBar, 0.0, 400.0);
  return 1.0 - 5.167e-4 * p + 2.333e-6 * p * p;
}

}  // namespace expedition_detail

const Camp* Route::FindCamp(const std::string& campId) const {
  for (const Camp& camp : camps) {
    if (camp.id == campId) return &camp;
  }
  return nullptr;
}

ItineraryReport EvaluateItinerary(const Route& route, const std::vector<ItineraryDay>& days) {
  using namespace expedition_detail;
  ItineraryReport report;
  if (days.empty()) return report;

  bool havePrev = false;
  double prevSleepM = 0.0;
  double acclimatizedM = 0.0;
  int ascentDaysSinceRest = 0;
  double gainSinceRestM = 0.0;
  bool restWarningIssued = false;

  for (const ItineraryDay& day : days) {
    const Camp* camp = route.FindCamp(day.sleepCampId);
    const std::string dayLabel = "Day " + std::to_string(day.day);
    if (camp == nullptr) {
      report.warnings.push_back(dayLabel + ": unknown camp " + CampLabel(nullptr, day.sleepCampId) +
                                " on route " + route.id + "; night skipped");
      continue;
    }
    const double sleepM = camp->altitudeM;
    const double maxM = std::max(day.maxAltitudeM, sleepM);
    const std::string where = dayLabel + " (" + CampLabel(camp, day.sleepCampId) + ", " + FormatMeters(sleepM) + ")";

    if (!havePrev) {
      // The first night is the starting point: the climber is taken as acclimatized to it (they
      // trekked in), so no gain is counted.
      havePrev = true;
      prevSleepM = sleepM;
      acclimatizedM = sleepM;
      continue;
    }

    // Only gains above the 3000 m guideline threshold count.
    const double gainM = sleepM > kGuidelineThresholdM ? sleepM - std::max(prevSleepM, kGuidelineThresholdM) : 0.0;

    if (gainM > kMaxDailySleepGainM) {
      report.followsGuidelines = false;
      report.warnings.push_back(where + ": sleeping altitude rises " + FormatMeters(gainM) +
                                " in one day; WMS advises at most 500 m/day above 3000 m");
    }

    if (gainM > 0.0) {
      ++ascentDaysSinceRest;
      gainSinceRestM += gainM;
      if (!restWarningIssued && ascentDaysSinceRest > kMaxAscentDaysWithoutRest &&
          gainSinceRestM > kGainPerRestDayM) {
        report.followsGuidelines = false;
        restWarningIssued = true;
        report.warnings.push_back(where + ": " + std::to_string(ascentDaysSinceRest) +
                                  " consecutive days of higher sleeping altitude (+" + FormatMeters(gainSinceRestM) +
                                  ") without a rest day; WMS advises a rest day every 3-4 days");
      }
    } else if (sleepM > kGuidelineThresholdM || prevSleepM > kGuidelineThresholdM) {
      // A rest day: no increase in sleeping altitude while at altitude (descents and rotations count).
      ++report.restDays;
      ascentDaysSinceRest = 0;
      gainSinceRestM = 0.0;
      restWarningIssued = false;
    }

    // AMS heuristic: excess of tonight's sleeping altitude over the acclimatized altitude.
    const double floorM = std::max(acclimatizedM, kGuidelineThresholdM);
    const double excessM = std::max(0.0, sleepM - floorM);
    const double nightRisk = Clamp01(kBaselineRisk + kExcessRiskScale * SmoothStep(0.0, kExcessForFullRiskM, excessM));
    if (sleepM > kGuidelineThresholdM) report.peakAmsRisk01 = std::max(report.peakAmsRisk01, nightRisk);

    // Acclimatization progresses toward tonight's altitude; "climb high, sleep low" adds credit.
    if (sleepM > acclimatizedM) {
      acclimatizedM = std::min(sleepM, acclimatizedM + kNightlyAcclimatizationM);
    }
    if (maxM > sleepM) {
      const double credit = std::min(kRotationCreditMaxM, kRotationCreditFraction * (maxM - sleepM));
      acclimatizedM = std::max(acclimatizedM, std::min(maxM, acclimatizedM + credit));
    }
    prevSleepM = sleepM;
  }
  return report;
}

double OxygenBottleLiters(double waterVolumeL, double fillPressureBar) {
  // Free gas volume at ~1 atm: water volume x fill pressure (bar), divided by Z. A Poisk 4 L bottle at
  // 270-300 bar holds ~1050-1150 L, matching the ~1000-1200 L quoted by expedition outfitters.
  if (waterVolumeL <= 0.0 || fillPressureBar <= 0.0) return 0.0;
  return waterVolumeL * fillPressureBar / expedition_detail::OxygenCompressibility(fillPressureBar);
}

double OxygenBottleHours(double waterVolumeL, double fillPressureBar, double flowLMin) {
  // Duration at a constant regulator flow. A 4 L / 300 bar bottle at 2 L/min lasts ~9.5 h, which is
  // why summit pushes on Everest budget 2-3 bottles per climber. Non-positive flow is invalid -> 0.
  if (flowLMin <= 0.0) return 0.0;
  return OxygenBottleLiters(waterVolumeL, fillPressureBar) / (flowLMin * 60.0);
}

}  // namespace summit::sim
