#include "Sim/Terrain.h"

#include <cmath>

#include "Sim/Math.h"

namespace summit::sim {

namespace {

// Footprint depth (cm) assumed for snow types when the caller passes no depth.
constexpr double kDefaultSoftSnowFootprintCm = 5.0;
constexpr double kDefaultDeepSnowFootprintCm = 9.0;
constexpr double kMaxHardSnowFootprintCm = 3.0;
constexpr double kMaxSnowTerrainFactor = 20.0;

// Pandolf, Haisman & Goldman (1976) "Metabolic energy expenditure and terrain coefficients for
// walking on snow", Ergonomics 19:683-690: eta = 1.30 + 0.082 D^1.6, D = footprint depth in cm.
// Richmond et al. (2015/2019, Applied Ergonomics) refit the curve with a cubic in sinkage that
// agrees within ~±30 % up to ~25 cm footprints.
double SnowFactor(double footprintCm) {
  const double d = Clamp(footprintCm, 0.0, 60.0);
  return std::fmin(1.30 + 0.082 * std::pow(d, 1.6), kMaxSnowTerrainFactor);
}

int Index(TerrainType type) { return static_cast<int>(type); }

}  // namespace

const char* TerrainTypeName(TerrainType type) {
  switch (type) {
    case TerrainType::Trail: return "Trail";
    case TerrainType::Grass: return "Grass";
    case TerrainType::Scree: return "Scree";
    case TerrainType::Boulder: return "Boulder";
    case TerrainType::Rock: return "Rock";
    case TerrainType::WetRock: return "WetRock";
    case TerrainType::Verglas: return "Verglas";
    case TerrainType::HardSnow: return "HardSnow";
    case TerrainType::SoftSnow: return "SoftSnow";
    case TerrainType::DeepSnow: return "DeepSnow";
    case TerrainType::GlacierIce: return "GlacierIce";
    case TerrainType::Count: break;
  }
  return "Unknown";
}

double PandolfTerrainFactor(TerrainType type, double snowDepthM) {
  // Non-snow coefficients: Soule & Goldman (1972) "Terrain coefficients for energy cost
  // prediction", J Appl Physiol 32:706 and Pandolf et al. (1977): blacktop 1.0, dirt road 1.1,
  // light brush 1.2, heavy brush 1.5, swamp 1.8, loose sand 2.1. Mountain surfaces are mapped onto
  // the closest measured class (scree behaves like loose sand underfoot; talus like heavy brush).
  const double footprintCm = std::fmax(0.0, snowDepthM) * 100.0;
  switch (type) {
    case TerrainType::Trail: return 1.1;
    case TerrainType::Grass: return 1.2;
    case TerrainType::Scree: return 1.8;
    case TerrainType::Boulder: return 1.6;
    case TerrainType::Rock: return 1.2;
    case TerrainType::WetRock: return 1.3;
    case TerrainType::Verglas: return 1.5;   // short, careful steps
    case TerrainType::GlacierIce: return 1.4;
    case TerrainType::HardSnow: return SnowFactor(std::fmin(footprintCm, kMaxHardSnowFootprintCm));
    case TerrainType::SoftSnow:
      return SnowFactor(footprintCm > 0.0 ? footprintCm : kDefaultSoftSnowFootprintCm);
    case TerrainType::DeepSnow:
      return SnowFactor(footprintCm > 0.0 ? footprintCm : kDefaultDeepSnowFootprintCm);
    case TerrainType::Count: break;
  }
  return 1.0;
}

double SurfaceFriction(TerrainType type, bool crampons, double temperatureC) {
  // Static friction, boot sole vs surface. Rubber on dry rock 0.8-1.0 and wet rock ~0.4-0.6
  // (Grönqvist 1995 footwear-friction reviews; climbing-rubber tests). Ice: ~0.02-0.05 near 0 °C
  // because of a meltwater film, rising to ~0.1-0.15 at -20 °C (Bowden & Hughes 1939; Kietzig
  // et al. 2010 "Physics of ice friction", J Appl Phys 107:081101). Packed snow ~0.2-0.35.
  static constexpr double kBoot[] = {
      0.60,  // Trail
      0.45,  // Grass
      0.45,  // Scree (stones roll)
      0.70,  // Boulder
      0.85,  // Rock
      0.40,  // WetRock
      0.05,  // Verglas
      0.30,  // HardSnow
      0.45,  // SoftSnow (the footprint gives mechanical purchase)
      0.60,  // DeepSnow
      0.08,  // GlacierIce
  };
  // With crampons: front points and down points penetrate snow and ice (effective grip 0.8-0.9).
  // On rock they skate; on verglas the points cannot bite into a glaze over rock.
  static constexpr double kCrampon[] = {
      0.60,  // Trail
      0.60,  // Grass
      0.45,  // Scree
      0.50,  // Boulder
      0.50,  // Rock
      0.40,  // WetRock
      0.30,  // Verglas: treacherous even with crampons
      0.90,  // HardSnow
      0.70,  // SoftSnow
      0.70,  // DeepSnow
      0.85,  // GlacierIce
  };
  const int i = Index(type);
  if (i < 0 || i >= Index(TerrainType::Count)) return 0.5;
  double mu = crampons ? kCrampon[i] : kBoot[i];

  const bool icy = type == TerrainType::Verglas || type == TerrainType::GlacierIce;
  if (!crampons && icy) {
    // Colder ice is grippier; close to melting a water film makes it extremely slick.
    mu *= Lerp(0.5, 1.5, SmoothStep(0.0, -20.0, temperatureC));
  }
  if (!crampons && type == TerrainType::HardSnow && temperatureC > -2.0) {
    mu *= 0.75;  // wet, refrozen-then-softening surface
  }
  if (type == TerrainType::WetRock && temperatureC < 0.0) {
    // Wet rock below freezing glazes over: it becomes verglas.
    const double verglas = crampons ? kCrampon[Index(TerrainType::Verglas)] : kBoot[Index(TerrainType::Verglas)];
    mu = Lerp(mu, verglas, SmoothStep(0.0, -3.0, temperatureC));
  }
  return mu;
}

double MaxWalkableSlopeDeg(TerrainType type, bool crampons) {
  // Upright walking limits from mountaineering practice (The Mountaineers, "Mountaineering: The
  // Freedom of the Hills", 9th ed.): scree near its angle of repose (~35°); flat-footed (French)
  // cramponing to ~40-45° on firm snow/ice, steeper needs front-pointing and tools; without
  // crampons hard snow and ice are unsafe beyond gentle angles.
  switch (type) {
    case TerrainType::Trail: return 30.0;
    case TerrainType::Grass: return crampons ? 35.0 : 30.0;
    case TerrainType::Scree: return 35.0;
    case TerrainType::Boulder: return 35.0;
    case TerrainType::Rock: return crampons ? 30.0 : 38.0;
    case TerrainType::WetRock: return 25.0;
    case TerrainType::Verglas: return crampons ? 20.0 : 8.0;
    case TerrainType::HardSnow: return crampons ? 45.0 : 25.0;
    case TerrainType::SoftSnow: return crampons ? 40.0 : 35.0;
    case TerrainType::DeepSnow: return 40.0;
    case TerrainType::GlacierIce: return crampons ? 40.0 : 10.0;
    case TerrainType::Count: break;
  }
  return 30.0;
}

}  // namespace summit::sim
