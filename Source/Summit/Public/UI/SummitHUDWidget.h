// Base class for the in-game HUD widget (WBP_SummitHUD derives from this).
//
// Pulls a snapshot of the player's vitals, local weather, balance and climb state a few times a second and
// exposes it BlueprintReadOnly; the Blueprint only draws. Diegetic UI (watch, altimeter, breath) can read
// the same values.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Data/SummitTypes.h"
#include "SummitHUDWidget.generated.h"

class ASummitCharacter;

UCLASS(Abstract, Blueprintable)
class SUMMIT_API USummitHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	FSummitVitals Vitals;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	FSummitWeatherSnapshot Weather;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	float BalanceStability01 = 1.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	float SlipRisk01 = 0.f;

	// Lean the cargo demands (deg, + right) and the player's current lean input (-1..1), for the
	// Death Stranding-style balance gauge.
	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	float CargoLateralBiasDeg = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	float LeanInput = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	float CargoMassKg = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	ESummitClimbMode ClimbMode = ESummitClimbMode::Walking;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	ESummitAvalancheDanger AvalancheDanger = ESummitAvalancheDanger::Low;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	bool bOxygenMaskOn = false;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	float OxygenFlowLMin = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	float OxygenHoursRemaining = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	int32 DayOfYear = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|HUD")
	float HourOfDay = 0.f;

	// Seconds between data refreshes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|HUD", meta = (ClampMin = "0"))
	float RefreshIntervalS = 0.1f;

	// Called after every refresh.
	UFUNCTION(BlueprintImplementableEvent, Category = "Summit|HUD")
	void OnSummitDataUpdated();

	// A condition appeared, cleared or worsened/improved by a severity band.
	UFUNCTION(BlueprintImplementableEvent, Category = "Summit|HUD")
	void OnConditionAlert(ESummitCondition Condition, float Severity01, bool bActive);

	UFUNCTION(BlueprintImplementableEvent, Category = "Summit|HUD")
	void OnPlayerDied(ESummitCauseOfDeath Cause);

	UFUNCTION(BlueprintCallable, Category = "Summit|HUD")
	void RefreshNow();

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UFUNCTION()
	void HandleConditionChanged(ESummitCondition Condition, float Severity01, bool bActive);

	UFUNCTION()
	void HandleDeath(ESummitCauseOfDeath Cause);

	void BindToPawn();
	void UnbindFromPawn();

	TWeakObjectPtr<ASummitCharacter> BoundCharacter;
	float RefreshCooldownS = 0.f;
};
