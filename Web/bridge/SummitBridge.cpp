// WebAssembly bridge between the browser game (Web/) and the simulation core (Source/SummitSim).
//
// The game writes inputs into a flat array of doubles, calls one exported function, and reads the
// outputs back from the same array. No strings or structs cross the boundary, so the JavaScript side
// needs no glue code beyond a Float64Array view. The field order below is the single source of truth:
// build_wasm.sh generates Web/js/simLayout.js from the IO_FIELDS list.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include "Sim/Atmosphere.h"
#include "Sim/Avalanche.h"
#include "Sim/Balance.h"
#include "Sim/Climber.h"
#include "Sim/Constants.h"
#include "Sim/Expedition.h"
#include "Sim/Math.h"
#include "Sim/Rope.h"
#include "Sim/Terrain.h"
#include "Sim/Thermal.h"
#include "Sim/Weather.h"

using namespace summit::sim;

#define IO_FIELDS(X)                                                                                   \
  /* ---- setup (read by sim_init) ---- */                                                             \
  X(SEED) X(VO2MAX) X(BODY_MASS) X(SKILL_TECH) X(SKILL_ICE) X(SKILL_ROPE) X(AMS_SUSCEPT)               \
  X(ACCLIM_PRESET) X(LATITUDE)                                                                         \
  /* ---- per-tick inputs ---- */                                                                      \
  X(TIME) X(ALT) X(EXPOSURE) X(SUNLIT) X(TERRAIN) X(SNOW_DEPTH) X(IN_TENT) X(IN_BAG) X(BAG_COMFORT)    \
  X(ACTIVITY) X(SPEED) X(GRADE) X(PACK_KG) X(FOOTWEAR_KG) X(CRAMPONS) X(GOGGLES) X(TORSO_CLO)          \
  X(HANDS_CLO) X(FEET_CLO) X(FACE_COVER) X(WINDPROOF) X(WETNESS) X(O2_MASK) X(O2_FLOW) X(MED_ACETA)    \
  X(MED_DEX) X(MED_NIFE) X(SLOPE_ALONG) X(SLOPE_ACROSS) X(POLES) X(LATERAL_WIND) X(ROUGHNESS)          \
  X(LEAN) X(BRACING)                                                                                   \
  /* ---- weather outputs ---- */                                                                      \
  X(W_TEMP) X(W_WIND) X(W_GUST) X(W_DIR) X(W_VIS) X(W_PRECIP) X(W_SNOW) X(W_CLOUD) X(W_RH) X(W_SOLAR)  \
  X(W_STORM) X(W_PRESSURE) X(W_WINDCHILL) X(W_BOILING) X(W_DENSITY) X(SUN_ELEV) X(SUN_AZ)              \
  X(DAY_OF_YEAR) X(HOUR)                                                                               \
  /* ---- climber outputs ---- */                                                                      \
  X(SPO2) X(EQ_ALT) X(VE) X(VO2MAX_NOW) X(COGNITIVE) X(SUSTAINABLE) X(AMS) X(HAPE) X(HACE)             \
  X(ACCLIM_VENT) X(ACCLIM_HEMA) X(ACCLIM_ALT) X(DETERIORATION) X(DEATHZONE_S) X(CORE) X(SKIN)          \
  X(HAND_T) X(FOOT_T) X(FACE_T) X(FB_HAND) X(FB_FOOT) X(FB_FACE) X(FBG_HAND) X(FBG_FOOT) X(FBG_FACE)   \
  X(SHIVER) X(HYPOTHERMIA) X(GLYCOGEN) X(FAT) X(DEHYDRATION) X(STAMINA) X(FATIGUE) X(SLEEP_DEBT)       \
  X(MASS_LOST) X(APPETITE) X(APPETITE_KCAL) X(PERFORMANCE) X(METABOLIC_W) X(EXERTION) X(SNOWBLIND)     \
  X(MAX_SPEED) X(DEAD) X(INCAPACITATED)                                                                \
  X(COND_HYPOXIA) X(COND_AMS) X(COND_HAPE) X(COND_HACE) X(COND_HYPOTHERMIA) X(COND_FROSTBITE)          \
  X(COND_DEHYDRATION) X(COND_EXHAUSTION) X(COND_STARVATION) X(COND_SNOWBLIND)                          \
  /* ---- balance outputs ---- */                                                                      \
  X(B_TILT_LAT) X(B_TILT_FWD) X(B_STABILITY) X(B_SLIP) X(B_STUMBLING) X(B_FALLEN) X(B_SAFE_SPEED)      \
  X(B_CARGO_BIAS) X(B_CARGO_KG) X(MAX_WALK_SLOPE) X(FRICTION) X(B_IMBALANCE_LAT) X(B_IMBALANCE_FWD)    \
  /* ---- snowpack outputs ---- */                                                                     \
  X(S_NEW24) X(S_NEW72) X(S_WINDLOAD) X(S_WARMING) X(S_DANGER_INDEX) X(S_DANGER_LEVEL)                 \
  X(AV_NATURAL_PER_HOUR) X(AV_HUMAN_TRIGGER)

enum Io : int {
#define X(name) name,
  IO_FIELDS(X)
#undef X
  IO_COUNT
};

namespace {

constexpr int kAuxSize = 2048;

double g_io[IO_COUNT + 8];
double g_aux[kAuxSize];

struct World {
  WeatherSystem weather;
  SnowpackModel snow;
  Climber climber;
  BalanceModel balance;
  Pcg32 rng;
  double latitude;

  World(std::uint64_t seed, const ClimberProfile& profile, const BodyGeometry& body, double lat)
      : weather(KhumbuClimate(), seed), climber(profile), balance(body, seed ^ 0x9e3779b97f4a7c15ULL),
        rng(seed ^ 0x51ed270b0f1a2d3cULL), latitude(lat) {}
};

World* g_world = nullptr;

bool Flag(Io i) { return g_io[i] > 0.5; }

TerrainType TerrainFromIo() {
  const int t = static_cast<int>(g_io[TERRAIN] + 0.5);
  if (t < 0 || t >= static_cast<int>(TerrainType::Count)) return TerrainType::HardSnow;
  return static_cast<TerrainType>(t);
}

ClimberGear GearFromIo() {
  ClimberGear gear;
  gear.clothing.torsoClo = g_io[TORSO_CLO];
  gear.clothing.handsClo = g_io[HANDS_CLO];
  gear.clothing.feetClo = g_io[FEET_CLO];
  gear.clothing.faceCoverage01 = g_io[FACE_COVER];
  gear.clothing.windproof01 = g_io[WINDPROOF];
  gear.clothing.wetness01 = g_io[WETNESS];
  gear.oxygen.maskOn = Flag(O2_MASK);
  gear.oxygen.flowLitersPerMin = Flag(O2_MASK) ? g_io[O2_FLOW] : 0.0;
  gear.meds.acetazolamide = Flag(MED_ACETA);
  gear.meds.dexamethasone = Flag(MED_DEX);
  gear.meds.nifedipine = Flag(MED_NIFE);
  gear.crampons = Flag(CRAMPONS);
  gear.glacierGoggles = Flag(GOGGLES);
  gear.packMassKg = g_io[PACK_KG];
  gear.footwearMassKg = g_io[FOOTWEAR_KG];
  return gear;
}

ClimberAction ActionFromIo() {
  ClimberAction action;
  const int a = static_cast<int>(g_io[ACTIVITY] + 0.5);
  action.activity = (a >= 0 && a <= static_cast<int>(Activity::BreakingTrail)) ? static_cast<Activity>(a)
                                                                               : Activity::Resting;
  action.speedMS = g_io[SPEED];
  action.gradePercent = g_io[GRADE];
  return action;
}

// Solar azimuth (deg clockwise from north) with the same Cooper (1969) declination the sim uses for
// elevation, so the rendered sun and the sim's solar heating agree.
double SolarAzimuthDeg(double latitudeDeg, int dayOfYear, double hourOfDay) {
  const double decl = DegToRad(23.45) * std::sin(2.0 * kPi * (284.0 + dayOfYear + 1.0) / 365.0);
  const double lat = DegToRad(latitudeDeg);
  const double hourAngle = DegToRad(15.0 * (hourOfDay - 12.0));
  const double az = std::atan2(std::sin(hourAngle), std::cos(hourAngle) * std::sin(lat) - std::tan(decl) * std::cos(lat));
  double deg = RadToDeg(az) + 180.0;
  while (deg < 0.0) deg += 360.0;
  while (deg >= 360.0) deg -= 360.0;
  return deg;
}

void WriteWeather(const WeatherSample& w, double altitudeM, double time) {
  World& world = *g_world;
  const int day = DayOfYear(time);
  const double hour = HourOfDay(time);
  const double pressure = BarometricPressureKPa(altitudeM, world.latitude, day);
  g_io[W_TEMP] = w.temperatureC;
  g_io[W_WIND] = w.windSpeedMS;
  g_io[W_GUST] = w.windGustMS;
  g_io[W_DIR] = w.windDirectionDeg;
  g_io[W_VIS] = w.visibilityM;
  g_io[W_PRECIP] = w.precipitationMmH;
  g_io[W_SNOW] = w.snowfallCmH;
  g_io[W_CLOUD] = w.cloudCover01;
  g_io[W_RH] = w.relativeHumidity01;
  g_io[W_SOLAR] = w.solarWm2;
  g_io[W_STORM] = w.stormIntensity01;
  g_io[W_PRESSURE] = pressure;
  g_io[W_WINDCHILL] = WindChillC(w.temperatureC, w.windSpeedMS * 3.6);
  g_io[W_BOILING] = WaterBoilingPointC(pressure);
  g_io[W_DENSITY] = AirDensityKgM3(pressure, w.temperatureC);
  g_io[SUN_ELEV] = SolarElevationDeg(world.latitude, day, hour);
  g_io[SUN_AZ] = SolarAzimuthDeg(world.latitude, day, hour);
  g_io[DAY_OF_YEAR] = day;
  g_io[HOUR] = hour;
}

void WriteClimber(const ClimberEnvironment& env, const ClimberAction& action, const ClimberGear& gear) {
  const Climber& c = g_world->climber;
  const PhysiologyState& p = c.Physio().State();
  const ThermalState& t = c.Thermal().State();
  const EnergyState& e = c.Energy().State();
  g_io[SPO2] = p.spo2;
  g_io[EQ_ALT] = p.equivalentAltitudeM;
  g_io[VE] = p.minuteVentilationLMin;
  g_io[VO2MAX_NOW] = c.Physio().CurrentVO2MaxMlKgMin();
  g_io[COGNITIVE] = c.Physio().CognitiveImpairment01();
  g_io[SUSTAINABLE] = c.Physio().MaxSustainableExertion01();
  g_io[AMS] = p.amsScore;
  g_io[HAPE] = p.hapeSeverity01;
  g_io[HACE] = p.haceSeverity01;
  g_io[ACCLIM_VENT] = p.ventilatoryAcclimatization;
  g_io[ACCLIM_HEMA] = p.hematologicAcclimatization;
  g_io[ACCLIM_ALT] = p.acclimatizedAltitudeM;
  g_io[DETERIORATION] = p.highAltitudeDeterioration01;
  g_io[DEATHZONE_S] = p.secondsInDeathZone;
  g_io[CORE] = t.coreTempC;
  g_io[SKIN] = t.meanSkinTempC;
  g_io[HAND_T] = t.extremityTempC[0];
  g_io[FOOT_T] = t.extremityTempC[1];
  g_io[FACE_T] = t.extremityTempC[2];
  g_io[FB_HAND] = t.frostbiteDamage01[0];
  g_io[FB_FOOT] = t.frostbiteDamage01[1];
  g_io[FB_FACE] = t.frostbiteDamage01[2];
  g_io[FBG_HAND] = static_cast<double>(c.Thermal().Frostbite(BodyPart::Hands));
  g_io[FBG_FOOT] = static_cast<double>(c.Thermal().Frostbite(BodyPart::Feet));
  g_io[FBG_FACE] = static_cast<double>(c.Thermal().Frostbite(BodyPart::Face));
  g_io[SHIVER] = t.shivering01;
  g_io[HYPOTHERMIA] = static_cast<double>(c.Thermal().Hypothermia());
  g_io[GLYCOGEN] = e.glycogenKcal;
  g_io[FAT] = e.fatReserveKcal;
  g_io[DEHYDRATION] = e.hydrationDeficitL;
  g_io[STAMINA] = e.shortTermStamina01;
  g_io[FATIGUE] = e.fatigue01;
  g_io[SLEEP_DEBT] = e.sleepDebtHours;
  g_io[MASS_LOST] = e.bodyMassLostKg;
  g_io[APPETITE] = e.appetite01;
  g_io[APPETITE_KCAL] = c.Energy().RemainingAppetiteKcal();
  g_io[PERFORMANCE] = c.Energy().PerformanceMultiplier();
  g_io[METABOLIC_W] = c.LastMetabolicRateW();
  g_io[EXERTION] = c.LastExertion01();
  g_io[SNOWBLIND] = c.SnowBlindness01();
  // "How fast could I go from here?" must have an answer while standing still, so a resting climber
  // is asked about walking.
  ClimberAction query = action;
  if (query.activity == Activity::Resting || query.activity == Activity::Sleeping) query.activity = Activity::Walking;
  g_io[MAX_SPEED] = c.MaxSustainableSpeedMS(env, query, gear);
  g_io[DEAD] = static_cast<double>(c.Cause());
  g_io[INCAPACITATED] = c.IsIncapacitated() ? 1.0 : 0.0;
  for (int i = 0; i < static_cast<int>(Condition::Count); ++i) g_io[COND_HYPOXIA + i] = 0.0;
  for (const ConditionReport& r : c.ActiveConditions()) {
    g_io[COND_HYPOXIA + static_cast<int>(r.condition)] = r.severity01;
  }
}

void WriteSnow() {
  const SnowpackState& s = g_world->snow.State();
  g_io[S_NEW24] = s.newSnow24hCm;
  g_io[S_NEW72] = s.newSnow72hCm;
  g_io[S_WINDLOAD] = s.windLoading01;
  g_io[S_WARMING] = s.warming01;
  g_io[S_DANGER_INDEX] = DangerIndex(s);
  g_io[S_DANGER_LEVEL] = static_cast<double>(RegionalDanger(s));
}

template <typename T>
int CopyOut(const T& value, int offset) {
  static_assert(std::is_trivially_copyable<T>::value, "state must be plain data");
  static_assert(sizeof(T) % sizeof(double) == 0, "state must be made of doubles");
  const int n = static_cast<int>(sizeof(T) / sizeof(double));
  g_aux[offset] = n;
  std::memcpy(&g_aux[offset + 1], &value, sizeof(T));
  return offset + 1 + n;
}

template <typename T>
int CopyIn(T& value, int offset) {
  const int n = static_cast<int>(sizeof(T) / sizeof(double));
  // A save written by an older build with a different state size is ignored rather than misread.
  if (static_cast<int>(g_aux[offset]) == n) std::memcpy(&value, &g_aux[offset + 1], sizeof(T));
  return offset + 1 + static_cast<int>(g_aux[offset]);
}

}  // namespace

#define EXPORT(name) __attribute__((export_name(#name))) name

extern "C" {

double* EXPORT(sim_io)() { return g_io; }
double* EXPORT(sim_aux)() { return g_aux; }
int EXPORT(sim_io_count)() { return IO_COUNT; }

// Creates the world. Reads the setup block plus TIME (the start of play). The snowpack is spun up over
// the preceding ten days so avalanche danger reflects recent weather from the first minute.
void EXPORT(sim_init)() {
  delete g_world;
  ClimberProfile profile;
  profile.physiology.vo2MaxSeaLevelMlKgMin = g_io[VO2MAX] > 0.0 ? g_io[VO2MAX] : 55.0;
  profile.physiology.bodyMassKg = g_io[BODY_MASS] > 0.0 ? g_io[BODY_MASS] : 75.0;
  profile.physiology.amsSusceptibility = g_io[AMS_SUSCEPT] > 0.0 ? g_io[AMS_SUSCEPT] : 1.0;
  profile.energy.bodyMassKg = profile.physiology.bodyMassKg;
  profile.skillTechnical01 = g_io[SKILL_TECH];
  profile.skillIce01 = g_io[SKILL_ICE];
  profile.skillRope01 = g_io[SKILL_ROPE];
  BodyGeometry body;
  body.bodyMassKg = profile.physiology.bodyMassKg;
  const auto seed = static_cast<std::uint64_t>(g_io[SEED]);
  g_world = new World(seed, profile, body, g_io[LATITUDE] != 0.0 ? g_io[LATITUDE] : 27.988);

  // Acclimatization the climber arrives with. 0: flew in. 1: walked in over ten days (trek to base
  // camp). 2: finished the rotations to Camp 3 and is ready for a summit push.
  PhysiologyState& p = g_world->climber.Physio().MutableState();
  const int preset = static_cast<int>(g_io[ACCLIM_PRESET] + 0.5);
  if (preset == 1) {
    p.ventilatoryAcclimatization = 0.85;
    p.hematologicAcclimatization = 0.25;
    p.acclimatizedAltitudeM = 5000.0;
    p.exposureAltitudeM = 5000.0;
    p.recentMaxAltitudeM = 5300.0;
    p.hemoglobinGdl = 15.6;
  } else if (preset == 2) {
    p.ventilatoryAcclimatization = 0.95;
    p.hematologicAcclimatization = 0.8;
    p.acclimatizedAltitudeM = 5400.0;
    p.exposureAltitudeM = 5400.0;
    p.recentMaxAltitudeM = 7200.0;
    p.hemoglobinGdl = 17.0;
  }

  const double start = g_io[TIME];
  const double spinUp = 10.0 * kSecondsPerDay;
  for (double t = std::fmax(0.0, start - spinUp); t < start; t += kSecondsPerHour) {
    g_world->weather.AdvanceTo(t);
    g_world->snow.Step(kSecondsPerHour, g_world->weather.Sample(6400.0, 0.6), 0.6);
  }
  g_world->weather.AdvanceTo(start);
}

// One frame. dtSim advances the body and snowpack (game time, may be large under time-lapse);
// dtReal advances balance, which the player feels in real time.
void EXPORT(sim_tick)(double dtSim, double dtReal) {
  if (g_world == nullptr) return;
  World& world = *g_world;
  const double time = g_io[TIME];
  world.weather.AdvanceTo(time);
  const double altitude = g_io[ALT];
  const double exposure = Clamp01(g_io[EXPOSURE]);
  WeatherSample sample = world.weather.Sample(altitude, exposure);
  WriteWeather(sample, altitude, time);

  ClimberEnvironment env;
  env.altitudeM = altitude;
  env.latitudeDeg = world.latitude;
  env.dayOfYear = DayOfYear(time);
  env.weather = sample;
  env.weather.solarWm2 *= Clamp01(g_io[SUNLIT]);  // terrain shadow from the game's horizon maps
  env.exposure01 = exposure;
  env.terrain = TerrainFromIo();
  env.snowDepthM = g_io[SNOW_DEPTH];
  env.inTent = Flag(IN_TENT);
  env.inSleepingBag = Flag(IN_BAG);
  env.sleepingBagComfortC = g_io[BAG_COMFORT];
  const ClimberGear gear = GearFromIo();
  const ClimberAction action = ActionFromIo();

  if (dtSim > 0.0) {
    world.climber.Step(dtSim, env, action, gear);
    world.snow.Step(dtSim, sample, exposure);
  }
  WriteClimber(env, action, gear);
  WriteSnow();

  BalanceInputs b;
  b.slopeAlongDeg = g_io[SLOPE_ALONG];
  b.slopeAcrossDeg = g_io[SLOPE_ACROSS];
  b.speedMS = g_io[SPEED];
  b.terrain = env.terrain;
  b.crampons = gear.crampons;
  b.usingPoles = Flag(POLES);
  b.temperatureC = sample.temperatureC;
  b.lateralWindMS = g_io[LATERAL_WIND];
  b.gustiness01 = sample.windSpeedMS > 0.1 ? Clamp01((sample.windGustMS / sample.windSpeedMS - 1.0) / 0.6) : 0.0;
  b.roughness01 = g_io[ROUGHNESS];
  b.fatigue01 = world.climber.Energy().State().fatigue01;
  b.cognitiveImpairment01 = world.climber.Physio().CognitiveImpairment01();
  b.leanInput = g_io[LEAN];
  b.bracing = Flag(BRACING);
  b.airDensityKgM3 = g_io[W_DENSITY];
  if (dtReal > 0.0) world.balance.Step(dtReal, b);
  const BalanceState& bs = world.balance.State();
  g_io[B_TILT_LAT] = bs.lateralTiltRad;
  g_io[B_TILT_FWD] = bs.forwardTiltRad;
  g_io[B_STABILITY] = bs.stability01;
  g_io[B_IMBALANCE_LAT] = bs.lateralImbalance;
  g_io[B_IMBALANCE_FWD] = bs.forwardImbalance;
  g_io[B_SLIP] = bs.slipRisk01;
  g_io[B_STUMBLING] = bs.stumbling ? 1.0 : 0.0;
  g_io[B_FALLEN] = bs.fallen ? 1.0 : 0.0;
  g_io[B_SAFE_SPEED] = world.balance.SafeSpeedMS(b);
  g_io[B_CARGO_BIAS] = world.balance.CargoLateralBiasRad();
  g_io[B_CARGO_KG] = world.balance.CargoMassKg();
  g_io[MAX_WALK_SLOPE] = MaxWalkableSlopeDeg(env.terrain, gear.crampons);
  g_io[FRICTION] = SurfaceFriction(env.terrain, gear.crampons, sample.temperatureC);
}

void EXPORT(sim_eat)(double kcal) {
  if (g_world != nullptr) g_world->climber.Energy().Eat(kcal);
}

void EXPORT(sim_drink)(double liters) {
  if (g_world != nullptr) g_world->climber.Energy().Drink(liters);
}

void EXPORT(sim_recover)() {
  if (g_world != nullptr) g_world->balance.Recover();
}

void EXPORT(sim_kill)(int cause) {
  if (g_world != nullptr) g_world->climber.Kill(static_cast<CauseOfDeath>(cause));
}

// Cargo: aux = [massKg, x, y, z, strapped] * count.
void EXPORT(sim_set_cargo)(int count) {
  if (g_world == nullptr) return;
  std::vector<CargoItem> cargo;
  for (int i = 0; i < count && (i + 1) * 5 <= kAuxSize; ++i) {
    CargoItem item;
    item.massKg = g_aux[i * 5];
    item.offsetM = Vec3(g_aux[i * 5 + 1], g_aux[i * 5 + 2], g_aux[i * 5 + 3]);
    item.strapped = g_aux[i * 5 + 4] > 0.5;
    cargo.push_back(item);
  }
  g_world->balance.SetCargo(cargo);
}

// Radio forecast for an altitude. aux = [time, temp, wind, gust, snowCmH, cloud, visibility,
// confidence] * returned count.
int EXPORT(sim_forecast)(double hoursAhead, double stepHours, double altitudeM) {
  if (g_world == nullptr) return 0;
  const std::vector<ForecastEntry> entries = g_world->weather.Forecast(hoursAhead, stepHours, altitudeM);
  int n = 0;
  for (const ForecastEntry& f : entries) {
    if ((n + 1) * 8 > kAuxSize) break;
    double* out = &g_aux[n * 8];
    out[0] = f.time;
    out[1] = f.expected.temperatureC;
    out[2] = f.expected.windSpeedMS;
    out[3] = f.expected.windGustMS;
    out[4] = f.expected.snowfallCmH;
    out[5] = f.expected.cloudCover01;
    out[6] = f.expected.visibilityM;
    out[7] = f.confidence01;
    ++n;
  }
  return n;
}

// Avalanche odds for the slope the player is standing on. Writes AV_NATURAL_PER_HOUR and
// AV_HUMAN_TRIGGER (per crossing).
void EXPORT(sim_avalanche)(double angleDeg, double aspectDeg, double elevationM, int leeward, int convex) {
  if (g_world == nullptr) return;
  SlopeInfo slope;
  slope.angleDeg = angleDeg;
  slope.aspectDeg = aspectDeg;
  slope.elevationM = elevationM;
  slope.leeward = leeward != 0;
  slope.convex = convex != 0;
  const SnowpackState& s = g_world->snow.State();
  g_io[AV_NATURAL_PER_HOUR] = NaturalReleaseProbabilityPerHour(s, slope);
  g_io[AV_HUMAN_TRIGGER] = HumanTriggerProbability(s, slope, 1, true);
}

double EXPORT(sim_self_arrest)(double slopeDeg, int hardSurface, double skill01, double reactionS, double fatigue01) {
  return SelfArrestSuccessProbability(slopeDeg, hardSurface != 0, skill01, reactionS, fatigue01);
}

double EXPORT(sim_fall_injury)(double fallDistanceM, double slopeDeg) {
  return UnprotectedFallInjury01(fallDistanceM, slopeDeg);
}

// A fall held by a fixed line. Returns injury 0..1; aux[0] = peak force kN, aux[1] = anchor failed.
double EXPORT(sim_roped_fall)(double massKg, double aboveAnchorM, double ropeOutM, double slopeDeg, double anchorAgeDays,
                              double temperatureC) {
  if (g_world == nullptr) return 0.0;
  FallScenario sc;
  sc.climberMassKg = massKg;
  sc.distanceAboveProtectionM = aboveAnchorM;
  sc.ropeOutM = ropeOutM;
  sc.slopeAngleDeg = slopeDeg;
  sc.belayDynamic01 = 0.0;
  RopeSpec fixedLine;
  fixedLine.dynamic = false;
  Anchor anchor;
  anchor.type = AnchorType::FixedLine;
  anchor.ratedStrengthKN = NominalAnchorStrengthKN(AnchorType::FixedLine);
  anchor.ageDays = anchorAgeDays;
  const FallOutcome out = ResolveFall(sc, fixedLine, anchor, 0.7, temperatureC, g_world->rng);
  g_aux[0] = out.peakForceKN;
  g_aux[1] = out.anchorFailed ? 1.0 : 0.0;
  return out.injurySeverity01;
}

double EXPORT(sim_random)() { return g_world != nullptr ? g_world->rng.NextDouble() : 0.0; }

double EXPORT(sim_pressure)(double altitudeM, int dayOfYear) {
  return BarometricPressureKPa(altitudeM, g_world != nullptr ? g_world->latitude : 27.988, dayOfYear);
}

double EXPORT(sim_bottle_liters)(double waterVolumeL, double fillPressureBar) {
  return OxygenBottleLiters(waterVolumeL, fillPressureBar);
}

// Save and load the climber's body. aux = [count, values...] for physiology, thermal, energy.
int EXPORT(sim_export_state)() {
  if (g_world == nullptr) return 0;
  int o = 0;
  o = CopyOut(g_world->climber.Physio().State(), o);
  o = CopyOut(g_world->climber.Thermal().State(), o);
  o = CopyOut(g_world->climber.Energy().State(), o);
  return o;
}

void EXPORT(sim_import_state)() {
  if (g_world == nullptr) return;
  int o = 0;
  o = CopyIn(g_world->climber.Physio().MutableState(), o);
  o = CopyIn(g_world->climber.Thermal().MutableState(), o);
  o = CopyIn(g_world->climber.Energy().MutableState(), o);
}

}  // extern "C"
