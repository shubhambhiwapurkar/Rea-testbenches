#pragma once

#include <string>
#include <vector>

#include "Sim/Terrain.h"

namespace summit::sim {

struct Camp {
  std::string id;          // e.g. "ebc", "c1"
  std::string name;        // "Everest Base Camp"
  double altitudeM = 0.0;
  double latitudeDeg = 0.0;
  double longitudeDeg = 0.0;
  int tentCapacity = 0;
};

struct RouteSegment {
  std::string fromCampId;
  std::string toCampId;
  double distanceM = 0.0;
  double ascentM = 0.0;
  double descentM = 0.0;
  TerrainType dominantTerrain = TerrainType::HardSnow;
  double typicalHoursUp = 0.0;    // for an acclimatized, fit climber
  double typicalHoursDown = 0.0;
  bool fixedRopes = false;
  std::vector<std::string> hazards;   // "icefall", "crevasses", "serac", "rockfall", "avalanche", "cornice"
};

struct Route {
  std::string id;
  std::string peakId;
  std::string name;
  std::vector<Camp> camps;                 // ordered from bottom to top, last = summit
  std::vector<RouteSegment> segments;
  const Camp* FindCamp(const std::string& id) const;
};

// One night of an itinerary: where the climber sleeps and the highest point reached that day.
struct ItineraryDay {
  int day = 0;
  std::string sleepCampId;
  double maxAltitudeM = 0.0;
};

struct ItineraryReport {
  bool followsGuidelines = true;
  double peakAmsRisk01 = 0.0;
  int restDays = 0;
  std::vector<std::string> warnings;
};

// Checks an itinerary against Wilderness Medical Society guidelines (Luks et al. 2019):
// above 3000 m, do not raise sleeping altitude by more than 500 m per day, and add a rest day
// every 3-4 days. "Climb high, sleep low" rotations are rewarded.
ItineraryReport EvaluateItinerary(const Route& route, const std::vector<ItineraryDay>& days);

// Bottled O2 logistics: litres in a bottle at a fill pressure, and how long it lasts at a flow.
double OxygenBottleLiters(double waterVolumeL, double fillPressureBar);
double OxygenBottleHours(double waterVolumeL, double fillPressureBar, double flowLMin);

}  // namespace summit::sim
