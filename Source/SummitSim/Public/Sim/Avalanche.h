#pragma once

#include <cstdint>

namespace summit::sim {

struct WeatherSample;

// European Avalanche Danger Scale (EAWS).
enum class DangerLevel : std::uint8_t { Low = 1, Moderate = 2, Considerable = 3, High = 4, VeryHigh = 5 };

struct SnowpackState {
  double newSnow24hCm = 0.0;
  double newSnow72hCm = 0.0;
  double windLoading01 = 0.0;     // wind-transported snow deposited on lee slopes
  double warming01 = 0.0;         // rapid warming / strong sun (wet-snow instability)
  double persistentWeakLayer01 = 0.0;  // buried surface hoar / facets, slow to heal
  double baseDepthCm = 150.0;
  double hoursSinceLoading = 999.0;    // instability peaks during and just after loading
};

struct SlopeInfo {
  double angleDeg = 30.0;
  double aspectDeg = 0.0;         // direction the slope faces (0 = N)
  double elevationM = 6000.0;
  bool leeward = false;           // relative to recent wind
  bool convex = false;            // tension zones
  bool terrainTrap = false;       // gully, crevasse, cliff below (raises consequence, not likelihood)
};

// Release-angle dependence: slab avalanches cluster at 30-45° with a peak around 38-40°
// (Schweizer & Jamieson 2001; McClung & Schaerer, The Avalanche Handbook).
double SlopeAngleFactor(double angleDeg);

DangerLevel RegionalDanger(const SnowpackState& snow);

// Natural release probability per hour for a slope.
double NaturalReleaseProbabilityPerHour(const SnowpackState& snow, const SlopeInfo& slope);

// Probability that a party crossing the slope triggers it. `partySize` climbers crossing together,
// `spacedOut` = crossing one at a time (reduces load and exposure).
double HumanTriggerProbability(const SnowpackState& snow, const SlopeInfo& slope, int partySize,
                               bool spacedOut);

// Evolves the snowpack from weather. Weak layers heal slowly; new-snow instability settles over
// ~2-3 days at cold temperatures and faster when warm.
class SnowpackModel {
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
};

}  // namespace summit::sim
