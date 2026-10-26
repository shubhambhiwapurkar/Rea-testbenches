// The player climber.
//
// A thin coordinator: owns the camera rig and the gameplay components, routes Enhanced Input to them, holds
// the authoritative ESummitClimbMode, and clamps walking speed to what the body (sim sustainable speed) and
// footing (balance safe speed) allow. Game logic lives in the components:
//   USummitClimberComponent  physiology/thermal/energy sim       USummitCargoComponent    pack contents
//   USummitBalanceComponent  load-carriage balance sim           USummitClimbingComponent walls, ropes, slides
// Input assets are soft references assigned in BP_SummitCharacter.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Data/SummitTypes.h"
#include "SummitCharacter.generated.h"

class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class USpringArmComponent;
class USummitBalanceComponent;
class USummitCargoComponent;
class USummitCharacterMovementComponent;
class USummitClimberComponent;
class USummitClimbingComponent;
struct FInputActionValue;

UCLASS(Blueprintable)
class SUMMIT_API ASummitCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ASummitCharacter(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void NotifyControllerChanged() override;
	virtual void Landed(const FHitResult& Hit) override;
	virtual void OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode = 0) override;

	// ---- Components -----------------------------------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Components")
	TObjectPtr<USummitClimberComponent> Climber;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Components")
	TObjectPtr<USummitCargoComponent> Cargo;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Components")
	TObjectPtr<USummitBalanceComponent> Balance;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Components")
	TObjectPtr<USummitClimbingComponent> Climbing;

	UFUNCTION(BlueprintPure, Category = "Summit|Movement")
	USummitCharacterMovementComponent* GetSummitMovement() const;

	// ---- Input assets (assign in the Blueprint) ------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Input")
	TSoftObjectPtr<UInputMappingContext> DefaultMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Input")
	TSoftObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Input")
	TSoftObjectPtr<UInputAction> LookAction;

	// Analog triggers (Axis1D 0..1), Death Stranding style: hold to counter-lean against load and wind.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Input")
	TSoftObjectPtr<UInputAction> LeanLeftAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Input")
	TSoftObjectPtr<UInputAction> LeanRightAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Input")
	TSoftObjectPtr<UInputAction> BraceAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Input")
	TSoftObjectPtr<UInputAction> InteractAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Input")
	TSoftObjectPtr<UInputAction> ClimbAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Input")
	TSoftObjectPtr<UInputAction> UseAxeAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Input")
	TSoftObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Input")
	TSoftObjectPtr<UInputAction> ToggleOxygenAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Input")
	TSoftObjectPtr<UInputAction> OpenInventoryAction;

	// ---- Tuning ---------------------------------------------------------------------------------------

	// Hard ceiling on walking speed (cm/s), before the sim and balance clamps. ~2 m/s is a fast, unloaded walk.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement", meta = (ClampMin = "10"))
	float MaxWalkSpeedCeilingCmS = 200.f;

	// Short bursts above the sustainable pace are possible while anaerobic stamina lasts.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement", meta = (ClampMin = "1", ClampMax = "2"))
	float BurstSpeedMultiplier = 1.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement", meta = (ClampMin = "0", ClampMax = "1"))
	float BraceSpeedMultiplier = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement", meta = (ClampMin = "0", ClampMax = "1"))
	float ScrambleSpeedMultiplier = 0.45f;

	// Seconds on the ground before the climber can get up after a fall.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement", meta = (ClampMin = "0"))
	float MinFallenTimeS = 1.5f;

	// Landing speeds (m/s). ~7 m/s is a 2.5 m drop: the climber goes down. ~14 m/s (10 m) is usually fatal.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement", meta = (ClampMin = "0"))
	float HardLandingSpeedMS = 7.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement", meta = (ClampMin = "0"))
	float FatalLandingSpeedMS = 14.f;

	// ---- Climb mode -----------------------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Summit|Movement")
	ESummitClimbMode GetClimbMode() const { return ClimbMode; }

	// The single place locomotion state changes. Updates components and CharacterMovement, then fires
	// OnClimbModeChanged for the AnimBP.
	UFUNCTION(BlueprintCallable, Category = "Summit|Movement")
	void SetClimbMode(ESummitClimbMode NewMode);

	UFUNCTION(BlueprintCallable, Category = "Summit|Movement")
	bool TryRecoverFromFall();

	UFUNCTION(BlueprintPure, Category = "Summit|Input")
	float GetLeanInput() const { return LeanRightValue - LeanLeftValue; }

	UFUNCTION(BlueprintPure, Category = "Summit|Input")
	bool IsBracing() const { return bBraceHeld || (LeanLeftValue > 0.5f && LeanRightValue > 0.5f); }

	// ---- Blueprint hooks ------------------------------------------------------------------------------

	UFUNCTION(BlueprintImplementableEvent, Category = "Summit|Movement")
	void OnClimbModeChanged(ESummitClimbMode OldMode, ESummitClimbMode NewMode);

	// Balance lost or hard landing: play the fall montage / partial ragdoll here.
	UFUNCTION(BlueprintImplementableEvent, Category = "Summit|Movement")
	void OnFallenDown(bool bHardImpact);

	UFUNCTION(BlueprintImplementableEvent, Category = "Summit|Movement")
	void OnRecoveredFromFall();

	UFUNCTION(BlueprintImplementableEvent, Category = "Summit|Vitals")
	void OnClimberDied(ESummitCauseOfDeath Cause);

	// Interact with nothing climbing-related in reach: camps, caches, tents, teammates.
	UFUNCTION(BlueprintImplementableEvent, Category = "Summit|Input")
	void OnInteract();

	UFUNCTION(BlueprintImplementableEvent, Category = "Summit|Input")
	void OnOpenInventory();

protected:
	// Input handlers
	void HandleMove(const FInputActionValue& Value);
	void HandleMoveCompleted(const FInputActionValue& Value);
	void HandleLook(const FInputActionValue& Value);
	void HandleLeanLeft(const FInputActionValue& Value);
	void HandleLeanRight(const FInputActionValue& Value);
	void HandleBraceStarted(const FInputActionValue& Value);
	void HandleBraceCompleted(const FInputActionValue& Value);
	void HandleInteract(const FInputActionValue& Value);
	void HandleClimb(const FInputActionValue& Value);
	void HandleUseAxeStarted(const FInputActionValue& Value);
	void HandleUseAxeCompleted(const FInputActionValue& Value);
	void HandleJump(const FInputActionValue& Value);
	void HandleToggleOxygen(const FInputActionValue& Value);
	void HandleOpenInventory(const FInputActionValue& Value);

private:
	UFUNCTION()
	void HandleBalanceFallen();

	UFUNCTION()
	void HandleClimberDeath(ESummitCauseOfDeath Cause);

	void EnterFallen(bool bHardImpact);
	void UpdateMovementLimits();

	ESummitClimbMode ClimbMode = ESummitClimbMode::Walking;
	float LeanLeftValue = 0.f;
	float LeanRightValue = 0.f;
	bool bBraceHeld = false;
	float FallenTimeS = 0.f;
	bool bDead = false;
	bool bChangingMode = false;
};
