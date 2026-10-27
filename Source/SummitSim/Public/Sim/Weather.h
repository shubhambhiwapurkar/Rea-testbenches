#pragma once

#include "Sim/Export.h"

#include <array>
#include <cstdint>
#include <vector>

#include "Sim/Random.h"

namespace summit::sim {

// Simulation clock: seconds since 00:00 local solar time on 1 January of the campaign year.
using SimTimeSeconds = double;

SUMMITSIM_API int DayOfYear(SimTimeSeconds t);      // 0..364
SUMMITSIM_API double HourOfDay(SimTimeSeconds t);   // 0..24
SUMMITSIM_API int MonthOfYear(SimTimeSeconds t);    // 0..11

struct SUMMITSIM_API WeatherSample {
  double temperatureC = 0.0;
  double windSpeedMS = 0.0;         // sustained
  double windGustMS = 0.0;
  double windDirectionDeg = 270.0;  // meteorological: direction wind comes FROM
  double visibilityM = 20000.0;
  double precipitationMmH = 0.0;    // water equivalent
  double snowfallCmH = 0.0;
  double cloudCover01 = 0.0;
  double relativeHumidity01 = 0.5;
  double solarWm2 = 0.0;            // incoming shortwave at the surface
  double stormIntensity01 = 0.0;
};

// Per-region climatology. Values are monthly means at `referenceElevationM`.
// Profiles for real ranges live in Data/Mountains/climate/*.json and are loaded by the UE layer.
struct SUMMITSIM_API ClimateProfile {
  double latitudeDeg = 28.0;
  double referenceElevationM = 5364.0;               // e.g. Everest Base Camp
  std::array<double, 12> meanTempC{};                // daily mean at reference elevation
  std::array<double, 12> diurnalRangeC{};            // max-min
  std::array<double, 12> jetStreamWindMS{};          // mean wind near 300 hPa (~9 km)
  std::array<double, 12> stormsPerWeek{};            // synoptic disturbance frequency
  std::array<double, 12> precipitationMmPerDay{};
  double lapseRateCPerKm = 6.5;
  double prevailingWindFromDeg = 270.0;              // westerlies
  // Optional per-month lapse rate (deg C per km). A zero entry falls back to lapseRateCPerKm.
  // Free-air lapse rates over the Himalaya are steeper in the dry season than in the saturated monsoon.
  std::array<double, 12> lapseRateCPerKmByMonth{};
};

// A Himalaya (Khumbu) climatology for tests and the CLI.
SUMMITSIM_API ClimateProfile KhumbuClimate();

// Solar elevation angle above the horizon (deg, negative at night) for local solar time.
// Declination after Cooper (1969); hour angle 15 deg per hour from solar noon.
SUMMITSIM_API double SolarElevationDeg(double latitudeDeg, int dayOfYear, double hourOfDay);

// Clear-sky global shortwave irradiance on a horizontal surface (W/m^2) at a given altitude.
// Zero when the sun is below the horizon.
SUMMITSIM_API double ClearSkyGlobalIrradianceWm2(double latitudeDeg, int dayOfYear, double hourOfDay, double altitudeM);

struct SUMMITSIM_API ForecastEntry {
  SimTimeSeconds time = 0.0;
  WeatherSample expected;
  double confidence01 = 1.0;  // decays with lead time
};

// Stochastic but seeded weather. Storm systems arrive as a Poisson process modulated by
// month, build, peak and decay; the jet stream oscillates on a multi-day period, which creates
// summit "windows". Wind increases with altitude toward jet-stream speed.
class SUMMITSIM_API WeatherSystem {
 public:
  WeatherSystem(const ClimateProfile& climate, std::uint64_t seed);

  // Advance internal synoptic state. Must be called with non-decreasing times.
  void AdvanceTo(SimTimeSeconds t);

  SimTimeSeconds CurrentTime() const { return time_; }

  // Weather right now at a given altitude (m). Spatial variation within a mountain is handled by
  // altitude plus an exposure factor (0 = sheltered couloir, 1 = exposed ridge).
  WeatherSample Sample(double altitudeM, double exposure01 = 1.0) const;

  // In-game forecast as a radio or satellite forecast would give it: the true future plus error that
  // grows with lead time. Deterministic for a given (seed, issue time).
  std::vector<ForecastEntry> Forecast(double hoursAhead, double stepHours, double altitudeM) const;

  const ClimateProfile& Climate() const { return climate_; }

  struct StormCell {
    SimTimeSeconds start = 0.0;
    double buildHours = 12.0;
    double peakHours = 24.0;
    double decayHours = 18.0;
    double intensity01 = 0.5;
  };

 private:
  // Full weather evaluation at time t. `climatological` replaces the stochastic parts (storms, jet
  // oscillation, synoptic anomalies) with their expected values; Forecast() blends toward it.
  WeatherSample Evaluate(SimTimeSeconds t, double altitudeM, double exposure01, bool climatological) const;
  double StormIntensityAt(SimTimeSeconds t) const;
  double JetStreamFactorAt(SimTimeSeconds t) const;
  void ScheduleStormsUntil(SimTimeSeconds t);

  ClimateProfile climate_;
  std::uint64_t seed_;
  Pcg32 rng_;
  SimTimeSeconds time_ = 0.0;
  SimTimeSeconds scheduledUntil_ = 0.0;
  std::vector<StormCell> storms_;
  double jetPhase_ = 0.0;
};

}  // namespace summit::sim
