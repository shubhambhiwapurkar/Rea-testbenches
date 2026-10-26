// Climbing state, surface detection and limb IK targets.
//
// Responsibilities:
//  * Ground checks while walking: hands-down Scrambling on steep rock, and the transition to Sliding when
//    snow/ice is steeper than the sim's walkable limit for the current boots/crampons.
//  * Wall climbing: finds climbable surfaces (USummitPhysicalMaterial::bClimbable or a rock/ice terrain type,
//    or a "Climbable" actor/component tag), chooses RockClimbing vs IceClimbing (ice needs axes and crampons),
//    tracks the wall normal, places four limbs on the surface in a diagonal gait for the AnimBP's IK, drains
//    grip stamina through the climber component and lets go when it runs out. Mantles onto ledges at the top.
//  * Fixed ropes: attach/detach, jumaring up and rappelling down with realistic pauses at anchors (unclip,
//    reclip) and for switching between ascender and descender.
//  * Self-arrest while Sliding, using summit::sim::SelfArrestSuccessProbability.
// The movement itself is integrated by USummitCharacterMovementComponent.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Data/SummitTypes.h"
#include "Simulation/SummitSimHandles.h"
#include "SummitClimbingComponent.generated.h"

class ASummitCharacter;
class ASummitFixedRope;
class USummitClimberComponent;
struct FHitResult;

// One IK target for the AnimBP (Control Rig or two-bone IK). World space.
USTRUCT(BlueprintType)
struct SUMMIT_API FSummitLimbTarget
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Climbing")
	FVector Location = FVector::ZeroVector;

	// X axis points into the surface, Z axis along the wall's up direction.
	UPROPERTY(BlueprintReadOnly, Category = "Summit|Climbing")
	FRotator Rotation = FRotator::ZeroRotator;

	// IK blend weight 0..1 (0 while the limb is in flight between holds or when not climbing).
	UPROPERTY(BlueprintReadOnly, Category = "Summit|Climbing")
	float Alpha = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Climbing")
	bool bPlanted = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSummitMantleSignature, FVector, TargetLocation);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSummitClimbEventSignature);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSummitSelfArrestSignature, bool, bSuccess, float, Probability);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSummitRopeAnchorSignature, int32, AnchorIndex);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSummitImpactSignature, float, ImpactSpeedMS);

UCLASS(ClassGroup = (Summit), meta = (BlueprintSpawnableComponent))
class SUMMIT_API USummitClimbingComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USummitClimbingComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// ---- Tuning ---------------------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Climbing")
	FName ClimbableTag = TEXT("Climbable");

	// How far ahead of the chest a wall can be grabbed / tracked.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Climbing", meta = (ClampMin = "10"))
	float ReachCm = 110.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Climbing", meta = (ClampMin = "1"))
	float TraceRadiusCm = 10.f;

	// Base wall-climbing speed for a skilled, fresh climber at sea level (cm/s). Real technical ground goes
	// at a few metres per minute; altitude and fatigue slow it further via the climber component.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Climbing", meta = (ClampMin = "1"))
	float MaxClimbSpeedCmS = 45.f;

	// Grip stamina drain (fraction of short-term stamina per second) hanging still on a vertical wall,
	// and the additional drain at full movement input.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Climbing|Stamina", meta = (ClampMin = "0"))
	float StaticGripDrainPerS = 0.012f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Climbing|Stamina", meta = (ClampMin = "0"))
	float MovingGripDrainPerS = 0.03f;

	// Limb gait: a limb re-places when the body has moved this far from its planted hold.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Climbing|IK", meta = (ClampMin = "5"))
	float LimbStepThresholdCm = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Climbing|IK", meta = (ClampMin = "0.05"))
	float LimbMoveTimeS = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Climbing|IK")
	float LimbSurfaceOffsetCm = 3.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Climbing", meta = (ClampMin = "0.1"))
	float MantleDurationS = 1.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope", meta = (ClampMin = "10"))
	float RopeGrabRadiusCm = 150.f;

	// Unclip/reclip at each intermediate anchor. Doubles with numb hands or hypoxic confusion.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope", meta = (ClampMin = "0"))
	float AnchorChangeoverS = 4.f;

	// Switching between ascender and descender (a minute or two in real life; shortened for play).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope", meta = (ClampMin = "0"))
	float DeviceChangeoverS = 15.f;

	// Controlled rappel speed limit (cm/s). Fixed-line rappels on 8 mm rope with a figure-8 are slow.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope", meta = (ClampMin = "1"))
	float MaxRappelSpeedCmS = 100.f;

	// Scrambling starts when the ground ahead exceeds this fraction of the walkable limit on rock/boulders.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Climbing", meta = (ClampMin = "0", ClampMax = "1"))
	float ScrambleSlopeFraction = 0.75f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Slide")
	int32 SelfArrestSeed = 7;

	// Minimum seconds between self-arrest attempts.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Slide", meta = (ClampMin = "0"))
	float SelfArrestRetryS = 0.6f;

	// ---- IK output (AnimBP reads these) --------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Climbing|IK")
	FSummitLimbTarget LeftHand;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Climbing|IK")
	FSummitLimbTarget RightHand;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Climbing|IK")
	FSummitLimbTarget LeftFoot;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Climbing|IK")
	FSummitLimbTarget RightFoot;

	// True when the targets above are meaningful (on a wall or a rope).
	UPROPERTY(BlueprintReadOnly, Category = "Summit|Climbing|IK")
	bool bLimbTargetsValid = false;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Climbing")
	FVector WallNormal = FVector::ForwardVector;

	// ---- Events ---------------------------------------------------------------------------------------

	// Mantle begins; Blueprint can add a Motion Warping target at TargetLocation and play the montage.
	UPROPERTY(BlueprintAssignable, Category = "Summit|Climbing")
	FSummitMantleSignature OnMantleStarted;

	// Grip gave out (or the wall ended) and the climber dropped off.
	UPROPERTY(BlueprintAssignable, Category = "Summit|Climbing")
	FSummitClimbEventSignature OnLetGo;

	UPROPERTY(BlueprintAssignable, Category = "Summit|Slide")
	FSummitSelfArrestSignature OnSelfArrestAttempt;

	UPROPERTY(BlueprintAssignable, Category = "Summit|Rope")
	FSummitRopeAnchorSignature OnRopeAnchorReached;

	UPROPERTY(BlueprintAssignable, Category = "Summit|Slide")
	FSummitImpactSignature OnSlideImpact;

	// ---- Actions --------------------------------------------------------------------------------------

	// Move input in the climbing plane: X = right, Y = up (also up/down along a rope).
	UFUNCTION(BlueprintCallable, Category = "Summit|Climbing")
	void SetClimbInput(FVector2D Input) { ClimbInput = Input.ClampAxes(-1.f, 1.f); }

	UFUNCTION(BlueprintCallable, Category = "Summit|Climbing")
	bool TryStartClimb();

	// Leave the wall. bDrop = true lets go (falling); false steps off onto the ground if there is any.
	UFUNCTION(BlueprintCallable, Category = "Summit|Climbing")
	void StopClimbing(bool bDrop);

	UFUNCTION(BlueprintCallable, Category = "Summit|Rope")
	bool TryAttachToRope();

	UFUNCTION(BlueprintCallable, Category = "Summit|Rope")
	void DetachFromRope();

	UFUNCTION(BlueprintCallable, Category = "Summit|Slide")
	bool TrySelfArrest();

	// Called by the character when balance is lost: on steep snow/ice a fall becomes a slide.
	UFUNCTION(BlueprintPure, Category = "Summit|Slide")
	bool ShouldFallBecomeSlide() const;

	// Called by the character on any climb-mode change so internal state stays coherent.
	void HandleClimbModeChanged(ESummitClimbMode OldMode, ESummitClimbMode NewMode);

	// ---- Queries for the movement component -----------------------------------------------------------

	bool HasWall() const { return bHasWall; }
	FVector GetWallNormal() const { return WallNormal; }
	float GetWallDistanceCm() const { return WallDistanceCm; }
	FVector2D GetClimbInput() const { return ClimbInput; }
	bool IsMantling() const { return bMantling; }

	// Current wall speed limit (cm/s), scaled by skill, stamina and the sim's sustainable pace.
	float GetClimbSpeedCmS() const;

	UFUNCTION(BlueprintPure, Category = "Summit|Rope")
	ASummitFixedRope* GetAttachedRope() const;

	float GetRopeDistance() const { return RopeDistanceCm; }

	// Moves the attachment along the rope; handles anchors and the rope ends.
	void SetRopeDistance(float NewDistanceCm);

	// +1 moving up the rope, -1 down, 0 holding (no input, or busy at an anchor/changeover).
	float GetRopeTravelSign() const;
	float GetRopeSpeedCmS() const;

	bool IsArresting() const { return bArresting; }
	void NotifySlideStopped();
	void NotifySlideImpact(float ImpactSpeedCmS);

private:
	ASummitCharacter* GetSummitCharacter() const;
	ESummitClimbMode GetMode() const;
	void RequestMode(ESummitClimbMode NewMode);

	bool TraceWall(const FVector& Start, const FVector& Direction, float Distance, FHitResult& OutHit) const;
	bool IsClimbableHit(const FHitResult& Hit, ESummitTerrainType& OutTerrain) const;
	ESummitTerrainType TerrainFromHit(const FHitResult& Hit) const;
	bool CanClimbTerrain(ESummitTerrainType Terrain, ESummitClimbMode& OutMode) const;
	void GetWallBasis(FVector& OutRight, FVector& OutUp) const;

	void TickGround(float DeltaTime);
	void TickWall(float DeltaTime);
	void TickRope(float DeltaTime);
	void TickSlide(float DeltaTime);
	void TickMantle(float DeltaTime);
	bool TryMantle();

	void UpdateWallLimbs(float DeltaTime);
	void UpdateRopeLimbs();
	void ClearLimbs();
	FSummitLimbTarget& Limb(int32 Index);

	UPROPERTY(Transient)
	TObjectPtr<USummitClimberComponent> ClimberComp;

	TWeakObjectPtr<ASummitFixedRope> AttachedRope;
	TSimPtr<summit::sim::Pcg32> ArrestRng;

	FVector2D ClimbInput = FVector2D::ZeroVector;

	// Wall
	bool bHasWall = false;
	float WallDistanceCm = 0.f;
	ESummitTerrainType WallTerrain = ESummitTerrainType::Rock;

	// Limb gait (index 0 LH, 1 RH, 2 LF, 3 RF)
	FVector LimbFrom[4];
	FVector LimbTo[4];
	float LimbMoveAlpha[4] = { 1.f, 1.f, 1.f, 1.f };
	int32 NextLimbToMove = 0;

	// Mantle
	bool bMantling = false;
	float MantleAlpha = 0.f;
	FVector MantleStart = FVector::ZeroVector;
	FVector MantleTarget = FVector::ZeroVector;

	// Rope
	float RopeDistanceCm = 0.f;
	float RopeBusyS = 0.f;               // anchor or device changeover in progress
	ESummitClimbMode PendingRopeMode = ESummitClimbMode::Jumaring;
	bool bDeviceChangePending = false;

	// Slide
	float SlideTimeS = 0.f;
	float LastArrestAttemptS = -100.f;
	bool bArresting = false;
};
