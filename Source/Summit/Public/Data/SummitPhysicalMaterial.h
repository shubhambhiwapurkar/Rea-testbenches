// Physical material carrying the mountain surface class the simulation needs.
//
// Assign instances to landscape layers and meshes (PM_Trail, PM_HardSnow, PM_GlacierIce, ...). The climber
// and balance components line-trace the floor and read Terrain/SnowDepth/Roughness from here. If a
// surface uses a plain UPhysicalMaterial instead, the components fall back to its SurfaceType
// (SurfaceType1..11 are mapped to ESummitTerrainType in DefaultEngine.ini).

#pragma once

#include "CoreMinimal.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Data/SummitTypes.h"
#include "SummitPhysicalMaterial.generated.h"

UCLASS(BlueprintType, meta = (DisplayName = "Summit Physical Material"))
class SUMMIT_API USummitPhysicalMaterial : public UPhysicalMaterial
{
	GENERATED_BODY()

public:
	USummitPhysicalMaterial();

	// Surface class fed to Pandolf terrain factors, boot/crampon friction and walkable-slope limits.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit")
	ESummitTerrainType Terrain = ESummitTerrainType::Trail;

	// Depth of loose snow over the firm base (metres). Only meaningful for snow types; drives
	// footprint sink-in cost (Soule & Goldman) and trail-breaking.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit", meta = (ClampMin = "0", ClampMax = "3", Units = "Meters"))
	float SnowDepthM = 0.f;

	// Micro-roughness that disturbs balance (boulders, sastrugi, penitentes): 0 smooth .. 1 chaotic.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit", meta = (ClampMin = "0", ClampMax = "1"))
	float Roughness01 = 0.1f;

	// Whether hands/tools can be placed on this surface (rock/ice walls). Meshes can also use the
	// "Climbable" actor/component tag instead.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit")
	bool bClimbable = false;
};
