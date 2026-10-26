// Default game mode: spawns the Summit character and HUD. Point Project Settings > Maps & Modes at a
// Blueprint subclass (BP_SummitGameMode) to swap in BP_SummitCharacter and the HUD widget.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "SummitGameMode.generated.h"

UCLASS()
class SUMMIT_API ASummitGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ASummitGameMode();
};
