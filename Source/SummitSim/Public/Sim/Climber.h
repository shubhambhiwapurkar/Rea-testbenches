#pragma once

#include "Sim/Export.h"

#include <string>
#include <vector>

#include "Sim/Energy.h"
#include "Sim/Physiology.h"
#include "Sim/Terrain.h"
#include "Sim/Thermal.h"
#include "Sim/Weather.h"

namespace summit::sim {

enum class Activity : std::uint8_t {
  Resting,
  Sleeping,
  Walking,       // trekking / glacier walking
  Climbing,      // technical: hands and/or tools on the wall
  Jumaring,      // ascending a fixed rope with an ascender
  Rappelling,
  BreakingTrail, // deep snow
};

struct SUMMITSIM_API ClimberProfile {
  std::string name = "Climber";
  PhysiologyParams physiology;
  EnergyParams energy;
  double skillTechnical01 = 0.5;
  double skillIce01 = 0.5;
  double skillRope01 = 0.5;
};

struct SUMMITSIM_API ClimberGear {
  ClothingSystem clothing;
  OxygenSupply oxygen;
  Medications meds;
  bool crampons = false;
  bool glacierGoggles = true;  // no eye protection at altitude -> snow blindness within hours
  double packMassKg = 15.0;
  double footwearMassKg = 1.6;  // pair. Trail shoe ~0.8, mountaineering boot ~2.0, 8000 m boot ~2.8
};

struct SUMMITSIM_API ClimberEnvironment {
  double altitudeM = 0.0;
  double latitudeDeg = 28.0;
  int dayOfYear = 140;
  WeatherSample weather;
  double exposure01 = 1.0;       // wind exposure at location
  TerrainType terrain = TerrainType::Trail;
  double snowDepthM = 0.0;
  bool inTent = false;
  bool inSleepingBag = false;
  double sleepingBagComfortC = -20.0;
};

struct SUMMITSIM_API ClimberAction {
  Activity activity = Activity::Resting;
  double speedMS = 0.0;          // along-ground speed
  double gradePercent = 0.0;     // rise/run * 100 (vertical climbing: use Climbing activity)
};

enum class Condition : std::uint8_t {
  Hypoxia, AMS, HAPE, HACE, Hypothermia, Frostbite, Dehydration, Exhaustion, Starvation,
  SnowBlindness, Count
};

enum class CauseOfDeath : std::uint8_t { None, Hypothermia, HAPE, HACE, Exhaustion, Fall, Avalanche, Other };

struct SUMMITSIM_API ConditionReport {
  Condition condition;
  double severity01;
};

// Aggregates the per-system models into one climber that the UE layer ticks.
class SUMMITSIM_API Climber {
 public:
  explicit Climber(const ClimberProfile& profile = {});

  // Advances all systems. dt can be large (fast travel/sleep) and is sub-stepped internally to <= 60 s.
  void Step(double dtSeconds, const ClimberEnvironment& env, const ClimberAction& action,
            const ClimberGear& gear);

  const ClimberProfile& Profile() const { return profile_; }
  const Physiology& Physio() const { return physiology_; }
  const ThermalModel& Thermal() const { return thermal_; }
  const EnergySystem& Energy() const { return energy_; }
  Physiology& Physio() { return physiology_; }
  ThermalModel& Thermal() { return thermal_; }
  EnergySystem& Energy() { return energy_; }

  // Current metabolic rate (W) and exertion fraction from the last step.
  double LastMetabolicRateW() const { return lastMetabolicW_; }
  double LastExertion01() const { return lastExertion01_; }

  // Highest speed (m/s) sustainable for the given action right now; gameplay clamps movement to it.
  double MaxSustainableSpeedMS(const ClimberEnvironment& env, const ClimberAction& action,
                               const ClimberGear& gear) const;

  std::vector<ConditionReport> ActiveConditions() const;
  bool IsIncapacitated() const;
  bool IsDead() const { return cause_ != CauseOfDeath::None; }
  CauseOfDeath Cause() const { return cause_; }
  void Kill(CauseOfDeath cause) { cause_ = cause; }

  // Additional accessors.
  double SnowBlindness01() const { return snowBlindness01_; }
  double LastPressureKPa() const { return lastPressureKPa_; }

 private:
  void SubStep(double dt, const ClimberEnvironment& env, const ClimberAction& action, const ClimberGear& gear);

  ClimberProfile profile_;
  Physiology physiology_;
  ThermalModel thermal_;
  EnergySystem energy_;
  double lastMetabolicW_ = 0.0;
  double lastExertion01_ = 0.0;
  double snowBlindness01_ = 0.0;
  double lastPressureKPa_ = 101.325;
  CauseOfDeath cause_ = CauseOfDeath::None;
};

}  // namespace summit::sim
