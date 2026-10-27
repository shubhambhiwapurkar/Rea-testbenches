// Builds the playable Everest map from the terrain pipeline output, so nobody has to click through the
// Landscape editor:
//   UnrealEditor-Cmd Summit.uproject -run=SummitSetup [-Terrain=<dir with everest.json/.r16>] [-Map=/Game/Maps/L_Everest]
// Creates M_SummitLandscape (slope/altitude snow-rock material), then the map: landscape at real scale, sun,
// sky atmosphere anchored at sea level, clouds, fog, sky light, a PlayerStart at Everest Base Camp and the
// Summit game mode. Re-running overwrites both assets.

#pragma once

#include "Commandlets/Commandlet.h"
#include "SummitSetupCommandlet.generated.h"

UCLASS()
class USummitSetupCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	USummitSetupCommandlet();
	virtual int32 Main(const FString& Params) override;
};
