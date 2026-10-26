#include "Sim/Expedition.h"
#include "TestFramework.h"

using namespace summit::sim;

namespace expedition_tests {

// Khumbu approach with rounded altitudes (enough to exercise the WMS rules).
Route KhumbuRoute() {
  Route r;
  r.id = "everest_south_col";
  r.peakId = "everest";
  r.name = "South Col";
  auto camp = [](const char* id, const char* name, double alt) {
    Camp c;
    c.id = id;
    c.name = name;
    c.altitudeM = alt;
    return c;
  };
  r.camps = {camp("lukla", "Lukla", 2860.0),       camp("namche", "Namche Bazaar", 3440.0),
             camp("tengboche", "Tengboche", 3860.0), camp("pheriche", "Pheriche", 4250.0),
             camp("lobuche", "Lobuche", 4700.0),     camp("gorakshep", "Gorak Shep", 5150.0),
             camp("ebc", "Everest Base Camp", 5364.0)};
  return r;
}

ItineraryDay Night(int day, const char* camp, double maxAlt = 0.0) {
  ItineraryDay d;
  d.day = day;
  d.sleepCampId = camp;
  d.maxAltitudeM = maxAlt;
  return d;
}

bool AnyWarningContains(const ItineraryReport& r, const std::string& a, const std::string& b) {
  for (const std::string& w : r.warnings) {
    if (w.find(a) != std::string::npos && w.find(b) != std::string::npos) return true;
  }
  return false;
}

}  // namespace expedition_tests

using namespace expedition_tests;

TEST_CASE("Expedition: FindCamp") {
  const Route r = KhumbuRoute();
  const Camp* c = r.FindCamp("namche");
  CHECK(c != nullptr);
  if (c != nullptr) CHECK_NEAR(c->altitudeM, 3440.0, 1e-9);
  CHECK(r.FindCamp("c4") == nullptr);
  CHECK(Route{}.FindCamp("lukla") == nullptr);
}

TEST_CASE("Expedition: a WMS-compliant trek passes") {
  const Route r = KhumbuRoute();
  const std::vector<ItineraryDay> days = {
      Night(1, "lukla"),     Night(2, "namche"),   Night(3, "namche", 3880.0),  // rest + climb high
      Night(4, "tengboche"), Night(5, "pheriche"), Night(6, "pheriche", 4750.0),
      Night(7, "lobuche"),   Night(8, "gorakshep"), Night(9, "gorakshep", 5550.0),
      Night(10, "ebc")};
  const ItineraryReport rep = EvaluateItinerary(r, days);
  CHECK(rep.followsGuidelines);
  CHECK(rep.warnings.empty());
  CHECK(rep.restDays == 3);
  CHECK_RANGE(rep.peakAmsRisk01, 0.05, 0.3);
}

TEST_CASE("Expedition: too-fast ascent is flagged by day and camp") {
  const Route r = KhumbuRoute();
  const std::vector<ItineraryDay> days = {Night(1, "lukla"), Night(2, "namche"), Night(3, "pheriche"),
                                          Night(4, "gorakshep")};
  const ItineraryReport rep = EvaluateItinerary(r, days);
  CHECK(!rep.followsGuidelines);
  CHECK(rep.warnings.size() >= 2);
  CHECK(AnyWarningContains(rep, "Day 3", "Pheriche"));
  CHECK(AnyWarningContains(rep, "Day 4", "Gorak Shep"));
  CHECK(rep.peakAmsRisk01 > 0.5);
  CHECK(rep.restDays == 0);
}

TEST_CASE("Expedition: AMS risk ranks itineraries sensibly") {
  const Route r = KhumbuRoute();
  const std::vector<ItineraryDay> slow = {Night(1, "lukla"), Night(2, "namche"), Night(3, "namche"),
                                          Night(4, "tengboche"), Night(5, "pheriche")};
  const std::vector<ItineraryDay> fast = {Night(1, "lukla"), Night(2, "namche"), Night(3, "pheriche")};
  const std::vector<ItineraryDay> heli = {Night(1, "lukla"), Night(2, "ebc")};
  const double pSlow = EvaluateItinerary(r, slow).peakAmsRisk01;
  const double pFast = EvaluateItinerary(r, fast).peakAmsRisk01;
  const double pHeli = EvaluateItinerary(r, heli).peakAmsRisk01;
  CHECK(pSlow < pFast);
  CHECK(pFast < pHeli);
  CHECK(pHeli > 0.6);   // ~2400 m jump overnight: most people get AMS
}

TEST_CASE("Expedition: climb high, sleep low lowers AMS risk") {
  const Route r = KhumbuRoute();
  const std::vector<ItineraryDay> plain = {Night(1, "namche"), Night(2, "tengboche"), Night(3, "tengboche"),
                                           Night(4, "lobuche")};
  const std::vector<ItineraryDay> rotation = {Night(1, "namche"), Night(2, "tengboche"),
                                              Night(3, "tengboche", 4700.0), Night(4, "lobuche")};
  CHECK(EvaluateItinerary(r, rotation).peakAmsRisk01 < EvaluateItinerary(r, plain).peakAmsRisk01);
}

TEST_CASE("Expedition: missing rest days are flagged") {
  Route r;
  r.id = "ladder";
  for (int i = 0; i < 8; ++i) {
    Camp c;
    c.id = "c" + std::to_string(i);
    c.name = "Camp " + std::to_string(i);
    c.altitudeM = 3000.0 + 400.0 * i;
    r.camps.push_back(c);
  }
  std::vector<ItineraryDay> days;
  for (int i = 0; i < 8; ++i) days.push_back(Night(i + 1, ("c" + std::to_string(i)).c_str()));
  const ItineraryReport rep = EvaluateItinerary(r, days);   // +400 m/day, never resting
  CHECK(!rep.followsGuidelines);
  CHECK(rep.restDays == 0);
  CHECK(AnyWarningContains(rep, "rest day", "Camp"));
  // No warning about the daily rate: 400 m/day is within the limit.
  CHECK(!AnyWarningContains(rep, "500 m/day", "Camp"));
}

TEST_CASE("Expedition: unknown camps are reported, below 3000 m is ignored") {
  Route r = KhumbuRoute();
  const ItineraryReport rep = EvaluateItinerary(r, {Night(1, "lukla"), Night(2, "atlantis")});
  CHECK(AnyWarningContains(rep, "Day 2", "atlantis"));

  Route low;
  Camp a, b;
  a.id = "a";
  a.altitudeM = 1000.0;
  b.id = "b";
  b.altitudeM = 2900.0;
  low.camps = {a, b};
  const ItineraryReport lowRep = EvaluateItinerary(low, {Night(1, "a"), Night(2, "b")});
  CHECK(lowRep.followsGuidelines);
  CHECK(lowRep.warnings.empty());
  CHECK(EvaluateItinerary(r, {}).followsGuidelines);
}

TEST_CASE("Oxygen: Poisk 4 L bottle volumes with real-gas correction") {
  const double at300 = OxygenBottleLiters(4.0, 300.0);
  CHECK_RANGE(at300, 1100.0, 1170.0);   // ~5 % below the ideal 1200 L
  CHECK(at300 < 4.0 * 300.0);
  CHECK_RANGE(OxygenBottleLiters(4.0, 270.0), 1000.0, 1080.0);
  // Close to ideal at moderate pressure.
  CHECK_NEAR(OxygenBottleLiters(4.0, 200.0) / 800.0, 1.0, 0.03);
  CHECK_NEAR(OxygenBottleLiters(0.0, 300.0), 0.0, 1e-12);
  CHECK(OxygenBottleLiters(4.0, 300.0) > OxygenBottleLiters(4.0, 250.0));
}

TEST_CASE("Oxygen: bottle duration at expedition flow rates") {
  CHECK_RANGE(OxygenBottleHours(4.0, 300.0, 2.0), 9.0, 10.0);
  CHECK_NEAR(OxygenBottleHours(4.0, 300.0, 4.0), 0.5 * OxygenBottleHours(4.0, 300.0, 2.0), 1e-9);
  CHECK_RANGE(OxygenBottleHours(4.0, 300.0, 0.5), 36.0, 40.0);   // sleeping flow
  CHECK_NEAR(OxygenBottleHours(4.0, 300.0, 0.0), 0.0, 1e-12);
}
