// Creates the HUD widget for the local player.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "SummitHUD.generated.h"

class USummitHUDWidget;

UCLASS()
class SUMMIT_API ASummitHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// Set to WBP_SummitHUD in a Blueprint subclass (or BP_SummitGameMode's HUD class).
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Summit|HUD")
	TSubclassOf<USummitHUDWidget> HUDWidgetClass;

	UFUNCTION(BlueprintPure, Category = "Summit|HUD")
	USummitHUDWidget* GetHUDWidget() const { return HUDWidget; }

private:
	UPROPERTY(Transient)
	TObjectPtr<USummitHUDWidget> HUDWidget;
};
