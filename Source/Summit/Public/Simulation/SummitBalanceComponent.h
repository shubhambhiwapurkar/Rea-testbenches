// Load carriage and balance (the Death Stranding layer), via summit::sim::BalanceModel.
//
// Reads the cargo layout, the slope under the feet relative to the direction of travel, crosswind from
// the local weather, fatigue and hypoxic impairment from the climber component, and the player's lean
// and brace input. Publishes stability and tilt for UI and procedural animation, and raises stumble/fall
// events. The character listens to OnFallen and switches to the Fallen climb mode.
//
// Balance is a real-time motor skill, so it steps on real DeltaTime (not the clock's time scale).

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Data/SummitTypes.h"
#include "Simulation/SummitSimHandles.h"
#include "SummitBalanceComponent.generated.h"

class USummitCargoComponent;
class USummitClimberComponent;
class USummitWeatherSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSummitBalanceEventSignature);

UCLASS(ClassGroup = (Summit), meta = (BlueprintSpawnableComponent))
class SUMMIT_API USummitBalanceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USummitBalanceComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// ---- Body (applied at BeginPlay) ------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Balance", meta = (Units = "Meters"))
	float HeightM = 1.78f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Balance", meta = (Units = "Meters"))
	float StanceHalfWidthM = 0.15f;

	// Seed for the balance model's disturbance noise (deterministic replays).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Balance")
	int32 Seed = 1;

	// Trekking poles or an axe used as a third point of contact.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Balance")
	bool bUsingPoles = false;

	// ---- Input (set by the character every frame) -----------------------------------------------------

	// -1 lean left .. +1 lean right (analog triggers).
	UFUNCTION(BlueprintCallable, Category = "Summit|Balance")
	void SetLeanInput(float Lean) { LeanInput = FMath::Clamp(Lean, -1.f, 1.f); }

	UFUNCTION(BlueprintCallable, Category = "Summit|Balance")
	void SetBracing(bool bInBracing) { bBracing = bInBracing; }

	// Only steps while upright (Walking/Scrambling); other modes freeze the model.
	UFUNCTION(BlueprintCallable, Category = "Summit|Balance")
	void SetClimbMode(ESummitClimbMode NewMode) { ClimbMode = NewMode; }

	// Get up after a fall. Resets the model's state.
	UFUNCTION(BlueprintCallable, Category = "Summit|Balance")
	void Recover();

	// ---- Output ---------------------------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Summit|Balance")
	float GetStability01() const { return Stability01; }

	UFUNCTION(BlueprintPure, Category = "Summit|Balance")
	float GetSlipRisk01() const { return SlipRisk01; }

	// + leaning right (degrees). Feed to the AnimBP for procedural lean.
	UFUNCTION(BlueprintPure, Category = "Summit|Balance")
	float GetLateralTiltDeg() const { return LateralTiltDeg; }

	// + leaning forward (degrees).
	UFUNCTION(BlueprintPure, Category = "Summit|Balance")
	float GetForwardTiltDeg() const { return ForwardTiltDeg; }

	UFUNCTION(BlueprintPure, Category = "Summit|Balance")
	bool IsStumbling() const { return bStumbling; }

	UFUNCTION(BlueprintPure, Category = "Summit|Balance")
	bool IsFallen() const { return bFallen; }

	// Recommended speed (cm/s) for the current slope, load and footing; the character clamps to it.
	UFUNCTION(BlueprintPure, Category = "Summit|Balance")
	float GetSafeSpeedCmS() const { return SafeSpeedCmS; }

	// Lean (degrees, + right) the player must hold to cancel the cargo's static lateral imbalance.
	UFUNCTION(BlueprintPure, Category = "Summit|Balance")
	float GetCargoLateralBiasDeg() const { return CargoLateralBiasDeg; }

	// Combined body+cargo CoM relative to the point between the feet, actor-local cm.
	UFUNCTION(BlueprintPure, Category = "Summit|Balance")
	FVector GetCombinedCoMCm() const { return CombinedCoMCm; }

	UFUNCTION(BlueprintPure, Category = "Summit|Balance")
	float GetSlopeAlongDeg() const { return SlopeAlongDeg; }

	UFUNCTION(BlueprintPure, Category = "Summit|Balance")
	float GetSlopeAcrossDeg() const { return SlopeAcrossDeg; }

	UPROPERTY(BlueprintAssignable, Category = "Summit|Balance")
	FSummitBalanceEventSignature OnStumbleStarted;

	UPROPERTY(BlueprintAssignable, Category = "Summit|Balance")
	FSummitBalanceEventSignature OnStumbleRecovered;

	UPROPERTY(BlueprintAssignable, Category = "Summit|Balance")
	FSummitBalanceEventSignature OnFallen;

private:
	UFUNCTION()
	void HandleCargoChanged();

	void PushCargo();
	void ComputeSlope(const FVector& TravelDir2D, const FVector& FloorNormal);

	TSimPtr<summit::sim::BalanceModel> Balance;

	UPROPERTY(Transient)
	TObjectPtr<USummitCargoComponent> Cargo;

	UPROPERTY(Transient)
	TObjectPtr<USummitClimberComponent> ClimberComp;

	UPROPERTY(Transient)
	TObjectPtr<USummitWeatherSubsystem> WeatherSubsystem;

	ESummitClimbMode ClimbMode = ESummitClimbMode::Walking;
	float LeanInput = 0.f;
	bool bBracing = false;

	float Stability01 = 1.f;
	float SlipRisk01 = 0.f;
	float LateralTiltDeg = 0.f;
	float ForwardTiltDeg = 0.f;
	float SafeSpeedCmS = 600.f;
	float CargoLateralBiasDeg = 0.f;
	float SlopeAlongDeg = 0.f;
	float SlopeAcrossDeg = 0.f;
	FVector CombinedCoMCm = FVector::ZeroVector;
	bool bStumbling = false;
	bool bFallen = false;
};
