// Character movement with mountaineering custom modes.
//
// Walking stays on the stock CMC (its walkable floor angle is driven every frame from the sim's
// summit::sim::MaxWalkableSlopeDeg for the current terrain and crampons). Three MOVE_Custom sub-modes cover
// what the stock component cannot:
//   Climb: on a rock/ice wall, moving in the wall plane, held at a standoff from the surface.
//   Rope:  attached to an ASummitFixedRope, moving along its spline (jumaring up / rappelling down).
//   Slide: uncontrolled slide on steep snow/ice, gravity minus friction and drag, with self-arrest braking.
// State and decisions (which wall, which rope, input, stamina) live in USummitClimbingComponent; this class
// only integrates motion.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "SummitCharacterMovementComponent.generated.h"

class USummitClimbingComponent;
class USummitClimberComponent;

UENUM(BlueprintType)
enum class ESummitCustomMovement : uint8
{
	None = 0 UMETA(Hidden),
	Climb = 1,
	Rope = 2,
	Slide = 3
};

UCLASS()
class SUMMIT_API USummitCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	USummitCharacterMovementComponent();

	virtual void BeginPlay() override;
	virtual float GetMaxSpeed() const override;
	virtual float GetMaxBrakingDeceleration() const override;

	UFUNCTION(BlueprintPure, Category = "Summit|Movement")
	bool IsInSummitMode(ESummitCustomMovement Mode) const;

	// Switches to MOVE_Custom with the given sub-mode (None exits to falling, which lands into walking).
	void EnterSummitMode(ESummitCustomMovement Mode);

	// How quickly wall-climbing velocity follows input (1/s). Climbing is deliberate, not twitchy.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement|Climb", meta = (ClampMin = "0.1"))
	float ClimbResponsiveness = 6.f;

	// Fraction of the standoff error corrected per second (keeps the capsule glued to the wall).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement|Climb", meta = (ClampMin = "0"))
	float WallStickStrength = 8.f;

	// How far down the rope from the ascender/descender the harness hangs (cow's tail length, roughly).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement|Rope", meta = (ClampMin = "0"))
	float RopeBodyOffsetCm = 40.f;

	// Sliding body on snow: kinetic friction is a fraction of the boot's static friction, capped. Clothing on
	// hard snow is slippery (~0.1-0.3); this is why unarrested slides on 40° névé are so deadly.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement|Slide", meta = (ClampMin = "0", ClampMax = "1"))
	float SlideKineticFrictionScale = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement|Slide", meta = (ClampMin = "0", ClampMax = "1"))
	float SlideMaxKineticFriction = 0.3f;

	// Quadratic drag (1/cm): a = k v². 9e-5 gives a terminal speed of ~25 m/s on a 45° slope.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement|Slide", meta = (ClampMin = "0"))
	float SlideDragPerCm = 9.0e-5f;

	// Braking from a successful axe arrest (cm/s²), ~0.5 g with the pick driven in and body weight on it.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement|Slide", meta = (ClampMin = "0"))
	float ArrestDecelerationCmS2 = 500.f;

	// Below this speed on walkable ground (or while arresting) a slide ends.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Movement|Slide", meta = (ClampMin = "0"))
	float SlideStopSpeedCmS = 30.f;

protected:
	virtual void PhysCustom(float DeltaTime, int32 Iterations) override;
	virtual void OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode) override;

private:
	void PhysClimb(float DeltaTime, int32 Iterations);
	void PhysRope(float DeltaTime, int32 Iterations);
	void PhysSlide(float DeltaTime, int32 Iterations);

	// Moves with sweep and slides along whatever blocks. OutFirstHit is the first blocking hit (before sliding).
	void MoveWithSlide(const FVector& Delta, const FQuat& NewRotation, FHitResult& OutFirstHit);

	void CacheComponents();

	UPROPERTY(Transient)
	TObjectPtr<USummitClimbingComponent> ClimbingComp;

	UPROPERTY(Transient)
	TObjectPtr<USummitClimberComponent> ClimberComp;
};
