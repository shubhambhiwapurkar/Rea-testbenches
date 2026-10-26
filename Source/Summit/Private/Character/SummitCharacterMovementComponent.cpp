#include "Character/SummitCharacterMovementComponent.h"

#include "Climbing/SummitClimbingComponent.h"
#include "Climbing/SummitFixedRope.h"
#include "Simulation/SummitClimberComponent.h"

#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"

USummitCharacterMovementComponent::USummitCharacterMovementComponent()
{
	// Mountaineering pace: a loaded trekker walks ~1.0-1.5 m/s on a trail. The character clamps this further
	// every frame from the sim's sustainable speed and the balance model's safe speed.
	MaxWalkSpeed = 160.f;
	MaxWalkSpeedCrouched = 60.f;
	BrakingDecelerationWalking = 800.f;
	GroundFriction = 6.f;
	// A climber in 8000 m boots with a pack barely leaves the ground.
	JumpZVelocity = 300.f;
	AirControl = 0.05f;
	MaxStepHeight = 40.f;
	bOrientRotationToMovement = true;
	RotationRate = FRotator(0.f, 270.f, 0.f);
	// Overwritten per frame from summit::sim::MaxWalkableSlopeDeg for the current surface.
	SetWalkableFloorAngle(45.f);
	NavAgentProps.bCanCrouch = true;
}

void USummitCharacterMovementComponent::BeginPlay()
{
	Super::BeginPlay();
	CacheComponents();
}

void USummitCharacterMovementComponent::CacheComponents()
{
	if (const AActor* Owner = GetOwner())
	{
		ClimbingComp = Owner->FindComponentByClass<USummitClimbingComponent>();
		ClimberComp = Owner->FindComponentByClass<USummitClimberComponent>();
	}
}

bool USummitCharacterMovementComponent::IsInSummitMode(ESummitCustomMovement Mode) const
{
	return MovementMode == MOVE_Custom && CustomMovementMode == static_cast<uint8>(Mode);
}

void USummitCharacterMovementComponent::EnterSummitMode(ESummitCustomMovement Mode)
{
	if (Mode == ESummitCustomMovement::None)
	{
		if (MovementMode == MOVE_Custom)
		{
			SetMovementMode(MOVE_Falling);
		}
		return;
	}
	SetMovementMode(MOVE_Custom, static_cast<uint8>(Mode));
}

float USummitCharacterMovementComponent::GetMaxSpeed() const
{
	if (MovementMode == MOVE_Custom)
	{
		switch (static_cast<ESummitCustomMovement>(CustomMovementMode))
		{
		case ESummitCustomMovement::Climb:
			return ClimbingComp ? ClimbingComp->GetClimbSpeedCmS() : 40.f;
		case ESummitCustomMovement::Rope:
			return ClimbingComp ? ClimbingComp->GetRopeSpeedCmS() : 50.f;
		case ESummitCustomMovement::Slide:
			return 5000.f;   // physics-limited (drag), not input-limited
		default:
			break;
		}
	}
	return Super::GetMaxSpeed();
}

float USummitCharacterMovementComponent::GetMaxBrakingDeceleration() const
{
	if (MovementMode == MOVE_Custom)
	{
		return 2000.f;
	}
	return Super::GetMaxBrakingDeceleration();
}

void USummitCharacterMovementComponent::OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode)
{
	Super::OnMovementModeChanged(PreviousMovementMode, PreviousCustomMode);

	if (!ClimbingComp || !ClimberComp)
	{
		CacheComponents();
	}

	if (MovementMode == MOVE_Custom)
	{
		const ESummitCustomMovement Mode = static_cast<ESummitCustomMovement>(CustomMovementMode);
		// Climbing and rope work face the surface/rope; rotation is set explicitly by the phys functions.
		bOrientRotationToMovement = false;
		if (Mode == ESummitCustomMovement::Climb || Mode == ESummitCustomMovement::Rope)
		{
			Velocity = FVector::ZeroVector;
		}
		// Slide keeps the momentum it entered with (a stumble at speed slides faster).
	}
	else if (PreviousMovementMode == MOVE_Custom)
	{
		bOrientRotationToMovement = true;
	}
}

void USummitCharacterMovementComponent::PhysCustom(float DeltaTime, int32 Iterations)
{
	// Keeps the Blueprint UpdateCustomMovement hook working.
	Super::PhysCustom(DeltaTime, Iterations);

	if (DeltaTime < MIN_TICK_TIME || !CharacterOwner || !UpdatedComponent)
	{
		return;
	}

	switch (static_cast<ESummitCustomMovement>(CustomMovementMode))
	{
	case ESummitCustomMovement::Climb:
		PhysClimb(DeltaTime, Iterations);
		break;
	case ESummitCustomMovement::Rope:
		PhysRope(DeltaTime, Iterations);
		break;
	case ESummitCustomMovement::Slide:
		PhysSlide(DeltaTime, Iterations);
		break;
	default:
		SetMovementMode(MOVE_Falling);
		StartNewPhysics(DeltaTime, Iterations);
		break;
	}
}

void USummitCharacterMovementComponent::MoveWithSlide(const FVector& Delta, const FQuat& NewRotation, FHitResult& OutFirstHit)
{
	FHitResult Hit(1.f);
	SafeMoveUpdatedComponent(Delta, NewRotation, /*bSweep*/ true, Hit);
	OutFirstHit = Hit;
	if (Hit.IsValidBlockingHit())
	{
		SlideAlongSurface(Delta, 1.f - Hit.Time, Hit.Normal, Hit, /*bHandleImpact*/ true);
	}
}

// ---- Wall climbing -------------------------------------------------------------------------------------

void USummitCharacterMovementComponent::PhysClimb(float DeltaTime, int32 Iterations)
{
	if (!ClimbingComp)
	{
		SetMovementMode(MOVE_Falling);
		StartNewPhysics(DeltaTime, Iterations);
		return;
	}

	// The climbing component moves the actor kinematically during a mantle.
	if (ClimbingComp->IsMantling())
	{
		Velocity = FVector::ZeroVector;
		return;
	}
	if (!ClimbingComp->HasWall())
	{
		ClimbingComp->StopClimbing(/*bDrop*/ true);
		return;
	}

	const FVector N = ClimbingComp->GetWallNormal();
	FVector Up = FVector::VectorPlaneProject(FVector::UpVector, N).GetSafeNormal();
	if (Up.IsNearlyZero())
	{
		Up = UpdatedComponent->GetForwardVector();
	}
	const FVector Right = FVector::CrossProduct(Up, -N).GetSafeNormal();

	const FVector2D Input = ClimbingComp->GetClimbInput();
	FVector Desired = Right * Input.X + Up * Input.Y;
	if (Desired.SizeSquared() > 1.0)
	{
		Desired.Normalize();
	}
	Desired *= GetMaxSpeed();
	Velocity = FMath::VInterpTo(Velocity, Desired, DeltaTime, ClimbResponsiveness);

	// Hold the body a hand's width off the wall. The wall trace is a sphere sweep from the capsule centre, so
	// the surface distance is the sweep distance plus the sphere radius.
	const float CapsuleRadius = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float SurfaceDistance = ClimbingComp->GetWallDistanceCm() + ClimbingComp->TraceRadiusCm;
	const float StandoffError = SurfaceDistance - (CapsuleRadius + 5.f);
	const FVector Correction = -N * StandoffError * FMath::Min(1.f, WallStickStrength * DeltaTime);

	const FVector FaceDir = (-N).GetSafeNormal2D();
	const FQuat Rotation = FaceDir.IsNearlyZero() ? UpdatedComponent->GetComponentQuat() : FaceDir.Rotation().Quaternion();

	FHitResult Hit;
	MoveWithSlide(Velocity * DeltaTime + Correction, Rotation, Hit);
}

// ---- Fixed rope ----------------------------------------------------------------------------------------

void USummitCharacterMovementComponent::PhysRope(float DeltaTime, int32 Iterations)
{
	ASummitFixedRope* Rope = ClimbingComp ? ClimbingComp->GetAttachedRope() : nullptr;
	if (!Rope)
	{
		if (ClimbingComp)
		{
			ClimbingComp->DetachFromRope();
		}
		if (MovementMode == MOVE_Custom)
		{
			SetMovementMode(MOVE_Falling);
		}
		StartNewPhysics(DeltaTime, Iterations);
		return;
	}

	const float TravelSign = ClimbingComp->GetRopeTravelSign();
	const float Along = ClimbingComp->GetRopeDistance() + Rope->GetUphillSign() * TravelSign * GetMaxSpeed() * DeltaTime;
	ClimbingComp->SetRopeDistance(Along);

	// Reaching the end of the line detaches (mode change); continue with whatever physics that selected.
	if (!IsInSummitMode(ESummitCustomMovement::Rope))
	{
		StartNewPhysics(DeltaTime, Iterations);
		return;
	}

	const float Distance = ClimbingComp->GetRopeDistance();
	const FVector UpRope = Rope->GetUphillDirectionAtDistance(Distance);
	const float HalfHeight = CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();

	// Rope lies on the slope roughly at foot level; the harness hangs a cow's tail below the device.
	const FVector Target = Rope->GetLocationAtDistance(Distance) - UpRope * RopeBodyOffsetCm
		+ FVector::UpVector * (HalfHeight * 0.9f);
	const FVector Delta = Target - UpdatedComponent->GetComponentLocation();

	const FVector FaceDir = UpRope.GetSafeNormal2D();
	const FQuat Rotation = FaceDir.IsNearlyZero() ? UpdatedComponent->GetComponentQuat() : FaceDir.Rotation().Quaternion();

	const FVector OldLocation = UpdatedComponent->GetComponentLocation();
	FHitResult Hit;
	MoveWithSlide(Delta, Rotation, Hit);
	Velocity = (UpdatedComponent->GetComponentLocation() - OldLocation) / DeltaTime;
}

// ---- Sliding -------------------------------------------------------------------------------------------

void USummitCharacterMovementComponent::PhysSlide(float DeltaTime, int32 Iterations)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const UCapsuleComponent* Capsule = CharacterOwner->GetCapsuleComponent();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const FVector Location = UpdatedComponent->GetComponentLocation();

	FCollisionQueryParams Params(SCENE_QUERY_STAT(SummitSlideFloor), false, CharacterOwner);
	FHitResult Floor;
	if (!World->LineTraceSingleByChannel(Floor, Location, Location - FVector::UpVector * (HalfHeight + 60.f), ECC_Visibility, Params))
	{
		// Launched off a step or cliff: free fall. The character's Landed() decides what happens next.
		SetMovementMode(MOVE_Falling);
		StartNewPhysics(DeltaTime, Iterations);
		return;
	}

	const FVector N = Floor.ImpactNormal;
	const double CosTheta = FMath::Clamp(N.Z, 0.0, 1.0);
	const double SinTheta = FMath::Sqrt(FMath::Max(0.0, 1.0 - CosTheta * CosTheta));
	const double SlopeDeg = FMath::RadiansToDegrees(FMath::Acos(CosTheta));
	const FVector DownSlope = FVector::VectorPlaneProject(-FVector::UpVector, N).GetSafeNormal();
	const double G = FMath::Abs(GetGravityZ());

	const double StaticFriction = ClimberComp ? ClimberComp->GetFloorFriction() : 0.3;
	const double KineticFriction = FMath::Min(StaticFriction * SlideKineticFrictionScale, static_cast<double>(SlideMaxKineticFriction));

	// Gravity along the slope, then all resistive terms against the direction of motion.
	Velocity = FVector::VectorPlaneProject(Velocity, N);
	Velocity += DownSlope * (G * SinTheta * DeltaTime);

	const double Speed = Velocity.Size();
	double Resist = KineticFriction * G * CosTheta + SlideDragPerCm * Speed * Speed;
	const bool bArresting = ClimbingComp && ClimbingComp->IsArresting();
	if (bArresting)
	{
		Resist += ArrestDecelerationCmS2;
	}
	const double NewSpeed = FMath::Max(0.0, Speed - Resist * DeltaTime);
	Velocity = Speed > KINDA_SMALL_NUMBER ? Velocity * (NewSpeed / Speed) : FVector::ZeroVector;

	// Stop when slow on ground the climber could stand on, or when an arrest has bled off the speed.
	const double MaxWalk = ClimberComp ? ClimberComp->GetMaxWalkableSlopeDeg() : 35.0;
	if (NewSpeed < SlideStopSpeedCmS && (SlopeDeg < MaxWalk || bArresting))
	{
		Velocity = FVector::ZeroVector;
		if (ClimbingComp)
		{
			ClimbingComp->NotifySlideStopped();
		}
		return;
	}

	// Keep contact with the slope: the capsule's lower hemisphere touches a plane tilted by theta at a vertical
	// distance of (HalfHeight - Radius) + Radius / cos(theta) from the centre.
	const double ContactDistance = (HalfHeight - Radius) + Radius / FMath::Max(CosTheta, 0.2);
	const double Gap = Floor.Distance - ContactDistance;
	FVector Delta = Velocity * DeltaTime;
	if (Gap > 1.0)
	{
		Delta += -FVector::UpVector * FMath::Min(Gap, 10.0);
	}

	const FVector FaceDir = Velocity.GetSafeNormal2D();
	const FQuat Rotation = FaceDir.IsNearlyZero() ? UpdatedComponent->GetComponentQuat() : FaceDir.Rotation().Quaternion();

	FHitResult Hit;
	MoveWithSlide(Delta, Rotation, Hit);

	// Obstacles (rocks, crevasse lips, seracs), not the slope itself, cause impacts.
	if (Hit.IsValidBlockingHit() && FVector::DotProduct(Hit.ImpactNormal, N) < 0.7)
	{
		const double ImpactSpeed = FMath::Max(0.0, -FVector::DotProduct(Velocity, Hit.ImpactNormal));
		Velocity = FVector::VectorPlaneProject(Velocity, Hit.ImpactNormal);
		if (ImpactSpeed > 300.0 && ClimbingComp)
		{
			ClimbingComp->NotifySlideImpact(static_cast<float>(ImpactSpeed));
		}
	}
}
