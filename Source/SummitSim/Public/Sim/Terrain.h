#pragma once

#include <cstdint>

namespace summit::sim {

// Surface classes the gameplay layer maps physical materials onto.
enum class TerrainType : std::uint8_t {
  Trail,        // packed dirt path (approach trek)
  Grass,
  Scree,        // loose small rock
  Boulder,      // talus / moraine
  Rock,         // solid rock, dry
  WetRock,
  Verglas,      // thin ice glaze on rock
  HardSnow,     // firm, wind-packed snow / névé
  SoftSnow,     // fresh / unconsolidated
  DeepSnow,     // trail-breaking, knee-deep or more
  GlacierIce,   // blue / water ice
  Count
};

const char* TerrainTypeName(TerrainType type);

// Pandolf terrain coefficient eta (Pandolf 1977; Soule & Goldman 1972 for snow/sand).
// snowDepthM is only used for snow types (footprint depression).
double PandolfTerrainFactor(TerrainType type, double snowDepthM = 0.0);

// Effective static friction coefficient between boot (optionally with crampons) and surface.
double SurfaceFriction(TerrainType type, bool crampons, double temperatureC);

// Steepest slope (deg) a climber can walk upright on before needing hands/tools.
double MaxWalkableSlopeDeg(TerrainType type, bool crampons);

}  // namespace summit::sim
