#pragma once

#include "Sim/Export.h"

#include <cstdint>

namespace summit::sim {

struct WeatherSample;

// European Avalanche Danger Scale (EAWS).
enum class DangerLevel : std::uint8_t { Low = 1, Moderate = 2, Considerable = 3, High = 4, VeryHigh = 5 };

struct SUMMITSIM_API SnowpackState {
  double newSnow24hCm = 0.0;
  double newSnow72hCm = 0.0;
  double windLoading01 = 0.0;     // wind-transported snow deposited on lee slopes
  double warming01 = 0.0;         // rapid warming / strong sun (wet-snow instability)
  double persistentWeakLayer01 = 0.0;  // buried surface hoar / facets, slow to heal
  double baseDepthCm = 150.0;
  double hoursSinceLoading = 999.0;    // instability peaks during and just after loading
  // New snow that has not yet settled and bonded (cm). Grows with snowfall and decays with a time
  // scale of ~2-3 days when cold, faster near 0 deg C. Outlives the 72 h window for slow settling.
  double unsettledNewSnowCm = 0.0;
  double surfaceHoar01 = 0.0;          // surface hoar on the snow surface; becomes a weak layer when buried
};

struct SUMMITSIM_API SlopeInfo {
  double angleDeg = 30.0;
  double aspectDeg = 0.0;         // direction the slope faces (0 = N)
  double elevationM = 6000.0;
  bool leeward = false;           // relative to recent wind
  bool convex = false;            // tension zones
  bool terrainTrap = false;       // gully, crevasse, cliff below (raises consequence, not likelihood)
};

// Release-angle dependence: slab avalanches cluster at 30-45° with a peak around 38-40°
// (Schweizer & Jamieson 2001; McClung & Schaerer, The Avalanche Handbook).
SUMMITSIM_API double SlopeAngleFactor(double angleDeg);

SUMMITSIM_API DangerLevel RegionalDanger(const SnowpackState& snow);

// Continuous danger index on the EAWS scale (1.0 .. 5.0). RegionalDanger() rounds it.
SUMMITSIM_API double DangerIndex(const SnowpackState& snow);

// Natural release probability per hour for a slope.
SUMMITSIM_API double NaturalReleaseProbabilityPerHour(const SnowpackState& snow, const SlopeInfo& slope);

// Probability that a party crossing the slope triggers it. `partySize` climbers crossing together,
// `spacedOut` = crossing one at a time (reduces load and exposure).
SUMMITSIM_API double HumanTriggerProbability(const SnowpackState& snow, const SlopeInfo& slope, int partySize,
                               bool spacedOut);

// Evolves the snowpack from weather. Weak layers heal slowly; new-snow instability settles over
// ~2-3 days at cold temperatures and faster when warm.
class SUMMITSIM_API SnowpackModel {
 public:
  SnowpackModel() = default;
  explicit SnowpackModel(const SnowpackState& initial) : state_(initial) {}

  void Step(double dtSeconds, const WeatherSample& weather, double windExposure01);

  const SnowpackState& State() const { return state_; }
  SnowpackState& MutableState() { return state_; }

 private:
  SnowpackState state_;
  double snow24Accum_[24] = {};   // hourly ring buffer, cm
  double snow72Accum_[72] = {};
  int hourIndex_ = 0;
  double hourAccumulatorS_ = 0.0;
  double currentHourSnowCm_ = 0.0;
  double tempEmaC_ = 0.0;     // ~24 h running mean air temperature, for rapid-warming detection
  bool tempEmaValid_ = false;

  void StepChunk(double dtSeconds, const WeatherSample& weather, double windExposure01);
};

}  // namespace summit::sim
