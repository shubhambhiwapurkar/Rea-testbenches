// Headless driver for the simulation core. Runs realistic scenarios end to end and prints a log,
// so the models can be sanity-checked against expedition reports without opening the editor.
//
//   summit_sim_cli everest [--no-o2] [--elite] [--flow L/min] [--seed N] [--day DOY] [--start-hour H]
//   summit_sim_cli trek    [--fast] [--seed N]
//   summit_sim_cli forecast [--seed N] [--day DOY]
//   summit_sim_cli windows  [--seed N]   list pre-monsoon summit windows (night-to-noon wind < 15 m/s)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Sim/Atmosphere.h"
#include "Sim/Avalanche.h"
#include "Sim/Balance.h"
#include "Sim/Climber.h"
#include "Sim/Constants.h"
#include "Sim/Expedition.h"
#include "Sim/Thermal.h"
#include "Sim/Weather.h"

using namespace summit::sim;

namespace {

struct Args {
  std::string scenario = "everest";
  bool noO2 = false;
  double flow = 3.0;
  unsigned long long seed = 7;
  int dayOfYear = 140;  // 20 May: the classic pre-monsoon summit window
  double startHour = 22.0;
  bool fast = false;
  bool elite = false;
};

Args Parse(int argc, char** argv) {
  Args a;
  if (argc > 1) a.scenario = argv[1];
  for (int i = 2; i < argc; ++i) {
    const auto is = [&](const char* flag) { return std::strcmp(argv[i], flag) == 0; };
    const auto next = [&]() { return i + 1 < argc ? argv[++i] : "0"; };
    if (is("--no-o2")) a.noO2 = true;
    else if (is("--flow")) a.flow = std::atof(next());
    else if (is("--seed")) a.seed = std::strtoull(next(), nullptr, 10);
    else if (is("--day")) a.dayOfYear = std::atoi(next());
    else if (is("--start-hour")) a.startHour = std::atof(next());
    else if (is("--fast")) a.fast = true;
    else if (is("--elite")) a.elite = true;
  }
  return a;
}

const char* ConditionName(Condition c) {
  switch (c) {
    case Condition::Hypoxia: return "hypoxia";
    case Condition::AMS: return "AMS";
    case Condition::HAPE: return "HAPE";
    case Condition::HACE: return "HACE";
    case Condition::Hypothermia: return "hypothermia";
    case Condition::Frostbite: return "frostbite";
    case Condition::Dehydration: return "dehydration";
    case Condition::Exhaustion: return "exhaustion";
    case Condition::Starvation: return "starvation";
    case Condition::SnowBlindness: return "snow-blindness";
    default: return "?";
  }
}

const char* DeathName(CauseOfDeath c) {
  switch (c) {
    case CauseOfDeath::Hypothermia: return "hypothermia";
    case CauseOfDeath::HAPE: return "HAPE";
    case CauseOfDeath::HACE: return "HACE";
    case CauseOfDeath::Exhaustion: return "exhaustion";
    case CauseOfDeath::Fall: return "fall";
    case CauseOfDeath::Avalanche: return "avalanche";
    default: return "other";
  }
}

void PrintConditions(const Climber& c) {
  bool any = false;
  for (const auto& r : c.ActiveConditions()) {
    if (r.severity01 < 0.05) continue;
    std::printf("%s%s %.2f", any ? ", " : "  [", ConditionName(r.condition), r.severity01);
    any = true;
  }
  if (any) std::printf("]");
}

void LogHeader() {
  std::printf("%-6s %7s %6s %5s %6s %5s %5s %5s %6s %5s %6s %5s %5s %5s\n", "time", "alt_m", "airC",
              "wind", "chillC", "SpO2", "eqAlt", "core", "handC", "AMS", "glyc", "dehyd", "fatig",
              "vm/h");
}

void LogRow(SimTimeSeconds t, double altitudeM, const WeatherSample& w, const Climber& c,
            double vertSpeedMH, double pressureKPa) {
  const auto& p = c.Physio().State();
  const auto& th = c.Thermal().State();
  const auto& e = c.Energy().State();
  const double hour = HourOfDay(t);
  std::printf("%02d:%02d  %7.0f %6.1f %5.1f %6.1f %4.0f%% %5.0f %5.1f %6.1f %5.1f %6.0f %5.2f %5.2f %5.0f",
              static_cast<int>(hour), static_cast<int>((hour - static_cast<int>(hour)) * 60.0), altitudeM,
              w.temperatureC, w.windSpeedMS, WindChillC(w.temperatureC, w.windSpeedMS * 3.6),
              p.spo2 * 100.0, c.Physio().EquivalentAltitudeM(pressureKPa), th.coreTempC,
              th.extremityTempC[static_cast<int>(BodyPart::Hands)], p.amsScore, e.glycogenKcal,
              e.hydrationDeficitL, e.fatigue01, vertSpeedMH);
  PrintConditions(c);
  std::printf("\n");
}

// Pre-acclimatize a climber as if they had completed two rotations to Camp 3 (~7200 m).
void ApplyFullRotationAcclimatization(Climber& c) {
  auto& s = c.Physio().MutableState();
  s.ventilatoryAcclimatization = 0.95;
  s.hematologicAcclimatization = 0.8;
  s.acclimatizedAltitudeM = 5400.0;
  s.hemoglobinGdl = 17.0;
}

// Everest summit push from the South Col (C4, 7906 m), the scenario every expedition report describes.
int RunEverest(const Args& args) {
  WeatherSystem weather(KhumbuClimate(), args.seed);
  const SimTimeSeconds start = args.dayOfYear * kSecondsPerDay + args.startHour * kSecondsPerHour;
  weather.AdvanceTo(start);

  ClimberProfile profile;
  profile.name = "Climber";
  if (args.elite) {
    // Messner/Habeler class: very high aerobic capacity, light build.
    profile.physiology.vo2MaxSeaLevelMlKgMin = 66.0;
    profile.physiology.bodyMassKg = 68.0;
    profile.energy.bodyMassKg = 68.0;
  }
  Climber climber(profile);
  ApplyFullRotationAcclimatization(climber);

  ClimberGear gear;
  gear.clothing.torsoClo = 7.0;  // 8000 m down suit
  gear.clothing.handsClo = 3.0;  // expedition mitts
  gear.clothing.feetClo = 3.5;   // triple boots (e.g. La Sportiva Olympus Mons)
  gear.clothing.faceCoverage01 = 0.85;
  gear.clothing.windproof01 = 0.9;
  gear.crampons = true;
  gear.glacierGoggles = true;
  gear.packMassKg = args.noO2 ? 4.5 : 9.0;  // O2 bottles (when used), water, spare mitts
  gear.footwearMassKg = 2.8;  // 8000 m triple boots (pair)
  gear.oxygen.maskOn = !args.noO2;
  gear.oxygen.flowLitersPerMin = args.noO2 ? 0.0 : args.flow;

  // South Col → Balcony → South Summit → Hillary Step → summit: ~943 m up over ~1.9 km of route.
  constexpr double kC4AltitudeM = 7906.0;
  constexpr double kSummitAltitudeM = 8848.86;
  constexpr double kRouteGrade = (kSummitAltitudeM - kC4AltitudeM) / 1900.0;
  const double turnaroundHour = 13.0;

  ClimberEnvironment env;
  env.latitudeDeg = 27.988;
  env.dayOfYear = args.dayOfYear;
  env.terrain = TerrainType::HardSnow;
  env.exposure01 = 0.9;
  env.altitudeM = kC4AltitudeM;

  ClimberAction action;
  action.activity = Activity::Jumaring;  // fixed lines run almost the entire way

  std::printf("Everest South Col summit push, day %d, start %02.0f:00, %s, %s climber, seed %llu\n",
              args.dayOfYear, args.startHour, args.noO2 ? "NO supplemental O2" : "supplemental O2",
              args.elite ? "elite" : "strong", args.seed);
  if (!args.noO2) std::printf("O2 flow %.1f L/min\n", args.flow);
  std::printf("PB at summit %.1f kPa (ISA would say %.1f kPa); water boils at %.1f C\n\n",
              BarometricPressureKPa(kSummitAltitudeM, env.latitudeDeg, args.dayOfYear),
              IsaPressureKPa(kSummitAltitudeM),
              WaterBoilingPointC(BarometricPressureKPa(kSummitAltitudeM, env.latitudeDeg, args.dayOfYear)));
  LogHeader();

  constexpr double dt = 60.0;
  bool summited = false;
  bool descending = false;
  double sinceLogS = 0.0;
  double altitudeAtLastLog = env.altitudeM;
  SimTimeSeconds t = start;
  SimTimeSeconds summitTime = 0.0;
  double balconyStopS = 0.0;
  BalanceModel balance;
  balance.SetCargo({CargoItem{"pack", gear.packMassKg, Vec3(0.0, -0.15, 0.0), true}});

  for (int step = 0; step < 36 * 60; ++step) {
    weather.AdvanceTo(t);
    env.weather = weather.Sample(env.altitudeM, env.exposure01);
    env.dayOfYear = DayOfYear(t);
    const double pressure = BarometricPressureKPa(env.altitudeM, env.latitudeDeg, env.dayOfYear);

    if (!descending && HourOfDay(t) >= turnaroundHour && HourOfDay(t) < 20.0) {
      descending = true;
      std::printf("--- turnaround time reached at %.0f m ---\n", env.altitudeM);
    }

    action.gradePercent = (descending ? -kRouteGrade : kRouteGrade) * 100.0;
    action.activity = descending ? Activity::Walking : Activity::Jumaring;

    // Real summit days are not continuous: a 5-10 min breather every hour (drink, eat a gel), a
    // ~20 min stop at the Balcony (~8,400 m) to swap O2 bottles, and ~30 min on top.
    const double minuteOfHour = std::fmod((t - start) / 60.0, 60.0);
    bool stopped = minuteOfHour < 8.0 && t - start > 1800.0;
    if (!descending && env.altitudeM >= 8400.0 && balconyStopS < 20.0 * 60.0) {
      stopped = true;
      balconyStopS += dt;
    }
    if (summited && t - summitTime < 30.0 * 60.0) stopped = true;
    if (stopped && minuteOfHour < 1.0) {
      climber.Energy().Eat(120.0);
      climber.Energy().Drink(0.2);
    }

    if (stopped) {
      action.activity = Activity::Resting;
      action.speedMS = 0.0;
    } else if (descending) {
      // Descent is limited by footing and downclimbing on ~26 degree hard snow, not by oxygen.
      BalanceInputs footing;
      footing.slopeAlongDeg = -RadToDeg(std::atan(kRouteGrade));
      footing.terrain = env.terrain;
      footing.crampons = gear.crampons;
      footing.temperatureC = env.weather.temperatureC;
      footing.lateralWindMS = env.weather.windSpeedMS * 0.5;
      footing.gustiness01 = 0.5;
      footing.roughness01 = 0.5;
      footing.fatigue01 = climber.Energy().State().fatigue01;
      footing.cognitiveImpairment01 = climber.Physio().CognitiveImpairment01();
      const double footingLimit = balance.SafeSpeedMS(footing);
      action.speedMS = std::fmin(climber.MaxSustainableSpeedMS(env, action, gear), footingLimit) * 0.85;
    } else {
      // Experienced climbers hold ~85 % of their maximal sustainable pace (the "rest step").
      action.speedMS = climber.MaxSustainableSpeedMS(env, action, gear) * 0.85;
    }

    climber.Step(dt, env, action, gear);
    const double dz = action.speedMS * dt * kRouteGrade / std::sqrt(1.0 + kRouteGrade * kRouteGrade);
    if (action.activity != Activity::Resting) env.altitudeM += descending ? -dz : dz;

    if (!descending && env.altitudeM >= kSummitAltitudeM) {
      env.altitudeM = kSummitAltitudeM;
      summited = true;
      descending = true;
      summitTime = t;
      std::printf("*** SUMMIT at %02d:%02d ***\n", static_cast<int>(HourOfDay(t)),
                  static_cast<int>((HourOfDay(t) - static_cast<int>(HourOfDay(t))) * 60.0));
    }

    t += dt;
    sinceLogS += dt;
    if (sinceLogS >= 3600.0) {
      LogRow(t, env.altitudeM, env.weather, climber, (env.altitudeM - altitudeAtLastLog), pressure);
      altitudeAtLastLog = env.altitudeM;
      sinceLogS = 0.0;
    }

    if (climber.IsDead()) {
      std::printf("\nDIED at %.0f m: %s\n", env.altitudeM, DeathName(climber.Cause()));
      return 2;
    }
    if (climber.IsIncapacitated()) {
      std::printf("\nINCAPACITATED at %.0f m, rescue needed\n", env.altitudeM);
      return 3;
    }
    if (descending && env.altitudeM <= kC4AltitudeM) {
      std::printf("\nBack at the South Col after %.1f h.%s\n", (t - start) / 3600.0,
                  summited ? "" : " (no summit)");
      break;
    }
  }

  const auto& th = climber.Thermal();
  std::printf("Frostbite  hands:%d feet:%d face:%d   (0 none, 1 frostnip, 2 superficial, 3 deep)\n",
              static_cast<int>(th.Frostbite(BodyPart::Hands)), static_cast<int>(th.Frostbite(BodyPart::Feet)),
              static_cast<int>(th.Frostbite(BodyPart::Face)));
  if (summited) std::printf("Summit reached %.1f h after leaving C4.\n", (summitTime - start) / 3600.0);
  return summited ? 0 : 1;
}

// Lukla → EBC trek. The standard itinerary acclimatizes; --fast skips the rest days.
int RunTrek(const Args& args) {
  struct Night { const char* place; double sleepM; double maxM; double km; };
  const std::vector<Night> standard = {
      {"Phakding", 2610, 2860, 8}, {"Namche Bazaar", 3440, 3440, 10}, {"Namche (rest, hike Everest View)", 3440, 3880, 4},
      {"Deboche", 3820, 3870, 10}, {"Dingboche", 4410, 4410, 11}, {"Dingboche (rest, Nangkartshang)", 4410, 5080, 5},
      {"Lobuche", 4940, 4940, 8}, {"Gorak Shep", 5164, 5364, 8}, {"Gorak Shep (Kala Patthar)", 5164, 5644, 4},
      {"Everest Base Camp", 5364, 5364, 4}};
  const std::vector<Night> fast = {{"Namche Bazaar", 3440, 3440, 18}, {"Dingboche", 4410, 4410, 21},
                                   {"Gorak Shep", 5164, 5164, 16}, {"Everest Base Camp", 5364, 5364, 4}};
  const auto& plan = args.fast ? fast : standard;

  WeatherSystem weather(KhumbuClimate(), args.seed);
  Climber climber;
  ClimberGear gear;
  gear.clothing.torsoClo = 2.5;
  gear.packMassKg = 8.0;

  ClimberEnvironment env;
  env.latitudeDeg = 27.9;
  env.terrain = TerrainType::Trail;
  env.altitudeM = 2860.0;

  std::printf("Lukla -> Everest Base Camp trek (%s itinerary)\n\n", args.fast ? "FAST, no rest days" : "standard");
  std::printf("%-3s %-34s %6s %6s %6s %5s %5s %6s\n", "day", "sleep", "sleepM", "SpO2", "AMS", "HAPE", "HACE",
              "acclim");

  SimTimeSeconds t = 95 * kSecondsPerDay + 7 * kSecondsPerHour;  // early April
  for (std::size_t day = 0; day < plan.size(); ++day) {
    const Night& n = plan[day];
    // 7 h walking day: up to the high point, then down to the sleeping altitude.
    const double walkS = 7.0 * kSecondsPerHour;
    const double startM = env.altitudeM;
    for (double s = 0; s < walkS; s += 60.0) {
      weather.AdvanceTo(t);
      env.weather = weather.Sample(env.altitudeM, 0.5);
      env.dayOfYear = DayOfYear(t);
      // First 70% of the day climbs to the day's high point, the rest descends to the sleeping altitude.
      const double frac = (s + 60.0) / walkS;
      const double target = frac < 0.7 ? startM + (n.maxM - startM) * (frac / 0.7)
                                       : n.maxM + (n.sleepM - n.maxM) * ((frac - 0.7) / 0.3);
      ClimberAction a;
      a.activity = Activity::Walking;
      a.speedMS = n.km * 1000.0 / walkS;
      a.gradePercent = (target - env.altitudeM) / (a.speedMS * 60.0) * 100.0;
      climber.Step(60.0, env, a, gear);
      env.altitudeM = target;
      t += 60.0;
    }
    climber.Energy().Eat(3000.0);
    climber.Energy().Drink(3.5);
    // Evening and night in a lodge / tent.
    env.inTent = true;
    env.inSleepingBag = true;
    env.altitudeM = n.sleepM;
    ClimberAction rest;
    rest.activity = Activity::Resting;
    climber.Step(4 * kSecondsPerHour, env, rest, gear);
    ClimberAction sleep;
    sleep.activity = Activity::Sleeping;
    climber.Step(13 * kSecondsPerHour, env, sleep, gear);
    t += 17 * kSecondsPerHour;
    env.inTent = false;
    env.inSleepingBag = false;

    const auto& p = climber.Physio().State();
    std::printf("%-3zu %-34s %6.0f %5.0f%% %6.1f %5.2f %5.2f %6.2f", day + 1, n.place, n.sleepM, p.spo2 * 100.0,
                p.amsScore, p.hapeSeverity01, p.haceSeverity01, p.ventilatoryAcclimatization);
    PrintConditions(climber);
    std::printf("\n");
    if (climber.IsDead()) {
      std::printf("\nDIED: %s\n", DeathName(climber.Cause()));
      return 2;
    }
  }
  return 0;
}

int RunForecast(const Args& args) {
  WeatherSystem weather(KhumbuClimate(), args.seed);
  // Spin up the snowpack over the preceding 30 days so the danger rating reflects recent weather.
  SnowpackModel snowpack;
  const SimTimeSeconds issue = args.dayOfYear * kSecondsPerDay + 6 * kSecondsPerHour;
  for (SimTimeSeconds t = issue - 30 * kSecondsPerDay; t <= issue; t += kSecondsPerHour) {
    weather.AdvanceTo(t);
    snowpack.Step(kSecondsPerHour, weather.Sample(6400.0, 0.6), 0.6);
  }
  std::printf("Summit (8849 m) forecast issued day %d 06:00, seed %llu\n\n", args.dayOfYear, args.seed);
  std::printf("%-10s %7s %7s %7s %8s %6s %5s\n", "when", "tempC", "windMS", "gustMS", "snowCmH", "cloud", "conf");
  for (const auto& f : weather.Forecast(7 * 24.0, 12.0, 8849.0)) {
    std::printf("d%-3d %02.0f:00 %7.1f %7.1f %7.1f %8.2f %6.2f %5.2f\n", DayOfYear(f.time), HourOfDay(f.time),
                f.expected.temperatureC, f.expected.windSpeedMS, f.expected.windGustMS, f.expected.snowfallCmH,
                f.expected.cloudCover01, f.confidence01);
  }
  std::printf("\nAvalanche danger in the Western Cwm (6400 m) at issue time: %d/5\n",
              static_cast<int>(RegionalDanger(snowpack.State())));
  return 0;
}

// Scans the pre-monsoon season for summit windows the way expedition forecasters do: a summit push
// needs the jet stream off the summit from departure (~22:00) until the descent (~14:00).
int RunWindows(const Args& args) {
  WeatherSystem weather(KhumbuClimate(), args.seed);
  std::printf("Everest summit windows, seed %llu (max summit wind 22:00-14:00 below 15 m/s)\n\n", args.seed);
  int found = 0;
  for (int day = 120; day <= 165; ++day) {
    double maxWind = 0.0;
    double minTemp = 100.0;
    for (double h = -2.0; h <= 14.0; h += 1.0) {
      weather.AdvanceTo(day * kSecondsPerDay + h * kSecondsPerHour);
      const WeatherSample w = weather.Sample(8848.0, 1.0);
      maxWind = std::fmax(maxWind, w.windSpeedMS);
      minTemp = std::fmin(minTemp, w.temperatureC);
    }
    if (maxWind < 15.0) {
      std::printf("  day %3d: max wind %4.1f m/s, min temp %5.1f C  ->  summit_sim_cli everest --seed %llu --day %d\n",
                  day, maxWind, minTemp, args.seed, day - 1);
      ++found;
    }
  }
  if (found == 0) std::printf("  no window this season: expeditions go home without a summit attempt\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = Parse(argc, argv);
  if (args.scenario == "everest") return RunEverest(args);
  if (args.scenario == "trek") return RunTrek(args);
  if (args.scenario == "forecast") return RunForecast(args);
  if (args.scenario == "windows") return RunWindows(args);
  std::printf("usage: summit_sim_cli everest|trek|forecast|windows [options]\n");
  return 64;
}
