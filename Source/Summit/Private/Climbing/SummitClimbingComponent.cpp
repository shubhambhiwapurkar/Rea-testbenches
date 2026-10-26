#include "Climbing/SummitClimbingComponent.h"

#include "Character/SummitCharacter.h"
#include "Character/SummitCharacterMovementComponent.h"
#include "Climbing/SummitFixedRope.h"
#include "Data/SummitPhysicalMaterial.h"
#include "Data/SummitSimConversions.h"
#include "Simulation/SummitCargoComponent.h"
#include "Simulation/SummitClimberComponent.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#include "Sim/Random.h"
#include "Sim/Rope.h"
#include "Sim/Terrain.h"

namespace
{
	enum ELimbIndex : int32 { LimbLeftHand = 0, LimbRightHand = 1, LimbLeftFoot = 2, LimbRightFoot = 3, LimbCount = 4 };

	// Neutral limb positions relative to the capsule centre in the wall plane (cm): X = right, Y = up.
	// Roughly a 1.78 m climber with hands just above head height and feet below the hips.
	const FVector2D kLimbOffsets[LimbCount] = {
		FVector2D(-25.0, 65.0), FVector2D(25.0, 65.0), FVector2D(-18.0, -85.0), FVector2D(18.0, -85.0)
	};

	// Diagonal gait (left hand, right foot, right hand, left foot): the classic stable climbing sequence that
	// keeps three points of contact while one limb moves.
	const int32 kGaitOrder[LimbCount] = { LimbLeftHand, LimbRightFoot, LimbRightHand, LimbLeftFoot };

	bool IsRocky(ESummitTerrainType T)
	{
		return T == ESummitTerrainType::Rock || T == ESummitTerrainType::WetRock || T == ESummitTerrainType::Boulder
			|| T == ESummitTerrainType::Scree;
	}

	// Surfaces on which a fallen climber starts to slide rather than lying still.
	bool IsSlippery(ESummitTerrainType T)
	{
		return T == ESummitTerrainType::HardSnow || T == ESummitTerrainType::SoftSnow
			|| T == ESummitTerrainType::GlacierIce || T == ESummitTerrainType::Verglas;
	}

	bool IsIceOrHardSnow(ESummitTerrainType T)
	{
		return T == ESummitTerrainType::HardSnow || T == ESummitTerrainType::GlacierIce || T == ESummitTerrainType::Verglas;
	}

	bool IsWallMode(ESummitClimbMode M)
	{
		return M == ESummitClimbMode::RockClimbing || M == ESummitClimbMode::IceClimbing;
	}

	bool IsRopeMode(ESummitClimbMode M)
	{
		return M == ESummitClimbMode::Jumaring || M == ESummitClimbMode::Rappelling;
	}

	double SlopeDegFromNormal(const FVector& N)
	{
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(N.Z, -1.0, 1.0)));
	}
}

USummitClimbingComponent::USummitClimbingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	for (int32 Index = 0; Index < LimbCount; ++Index)
	{
		LimbFrom[Index] = FVector::ZeroVector;
		LimbTo[Index] = FVector::ZeroVector;
	}
}

void USummitClimbingComponent::BeginPlay()
{
	Super::BeginPlay();
	if (AActor* Owner = GetOwner())
	{
		ClimberComp = Owner->FindComponentByClass<USummitClimberComponent>();
	}
	ArrestRng.Reset(new summit::sim::Pcg32(static_cast<std::uint64_t>(static_cast<uint32>(SelfArrestSeed))));
}

ASummitCharacter* USummitClimbingComponent::GetSummitCharacter() const
{
	return Cast<ASummitCharacter>(GetOwner());
}

ESummitClimbMode USummitClimbingComponent::GetMode() const
{
	const ASummitCharacter* Char = GetSummitCharacter();
	return Char ? Char->GetClimbMode() : ESummitClimbMode::Walking;
}

void USummitClimbingComponent::RequestMode(ESummitClimbMode NewMode)
{
	if (ASummitCharacter* Char = GetSummitCharacter())
	{
		Char->SetClimbMode(NewMode);
	}
}

FSummitLimbTarget& USummitClimbingComponent::Limb(int32 Index)
{
	switch (Index)
	{
	case LimbLeftHand: return LeftHand;
	case LimbRightHand: return RightHand;
	case LimbLeftFoot: return LeftFoot;
	default: return RightFoot;
	}
}

// ---- Mode bookkeeping ----------------------------------------------------------------------------------

void USummitClimbingComponent::HandleClimbModeChanged(ESummitClimbMode OldMode, ESummitClimbMode NewMode)
{
	if (!IsWallMode(NewMode))
	{
		bHasWall = false;
		bMantling = false;
	}
	else if (!IsWallMode(OldMode))
	{
		// Fresh on the wall: every limb needs a hold.
		for (int32 Index = 0; Index < LimbCount; ++Index)
		{
			Limb(Index).bPlanted = false;
			LimbMoveAlpha[Index] = 1.f;
		}
		NextLimbToMove = 0;
	}

	if (!IsRopeMode(NewMode))
	{
		AttachedRope.Reset();
		RopeBusyS = 0.f;
		bDeviceChangePending = false;
	}

	if (NewMode == ESummitClimbMode::Sliding)
	{
		SlideTimeS = 0.f;
		LastArrestAttemptS = -100.f;
		bArresting = false;
	}

	if (!IsWallMode(NewMode) && !IsRopeMode(NewMode))
	{
		ClearLimbs();
	}
	ClimbInput = FVector2D::ZeroVector;
}

void USummitClimbingComponent::ClearLimbs()
{
	bLimbTargetsValid = false;
	for (int32 Index = 0; Index < LimbCount; ++Index)
	{
		FSummitLimbTarget& L = Limb(Index);
		L.Alpha = 0.f;
		L.bPlanted = false;
	}
}

// ---- Tick ----------------------------------------------------------------------------------------------

void USummitClimbingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	switch (GetMode())
	{
	case ESummitClimbMode::Walking:
	case ESummitClimbMode::Scrambling:
		TickGround(DeltaTime);
		break;
	case ESummitClimbMode::RockClimbing:
	case ESummitClimbMode::IceClimbing:
		if (bMantling)
		{
			TickMantle(DeltaTime);
		}
		else
		{
			TickWall(DeltaTime);
		}
		break;
	case ESummitClimbMode::Jumaring:
	case ESummitClimbMode::Rappelling:
		TickRope(DeltaTime);
		break;
	case ESummitClimbMode::Sliding:
		TickSlide(DeltaTime);
		break;
	default:
		ClearLimbs();
		break;
	}
}

void USummitClimbingComponent::TickGround(float /*DeltaTime*/)
{
	ClearLimbs();

	const ASummitCharacter* Char = GetSummitCharacter();
	if (!Char || !ClimberComp || !ClimberComp->HasFloor())
	{
		return;
	}
	const UCharacterMovementComponent* Move = Char->GetCharacterMovement();
	if (!Move || !Move->IsMovingOnGround())
	{
		return;
	}

	const ESummitTerrainType Terrain = ClimberComp->GetFloorTerrain();
	const double SlopeDeg = SlopeDegFromNormal(ClimberComp->GetFloorNormal());
	const double MaxWalk = ClimberComp->GetMaxWalkableSlopeDeg();

	// Steeper than boots/crampons can hold on snow or ice: the feet go and the climber slides.
	if (IsSlippery(Terrain) && SlopeDeg > MaxWalk)
	{
		RequestMode(ESummitClimbMode::Sliding);
		return;
	}

	// Hands-down scrambling on steep rock, with a little hysteresis so the pose does not flicker.
	const double ScrambleOn = MaxWalk * ScrambleSlopeFraction;
	const double ScrambleOff = ScrambleOn - 5.0;
	const ESummitClimbMode Mode = GetMode();
	if (Mode == ESummitClimbMode::Walking && IsRocky(Terrain) && SlopeDeg > ScrambleOn)
	{
		RequestMode(ESummitClimbMode::Scrambling);
	}
	else if (Mode == ESummitClimbMode::Scrambling && (!IsRocky(Terrain) || SlopeDeg < ScrambleOff))
	{
		RequestMode(ESummitClimbMode::Walking);
	}
}

// ---- Surface queries -----------------------------------------------------------------------------------

bool USummitClimbingComponent::TraceWall(const FVector& Start, const FVector& Direction, float Distance, FHitResult& OutHit) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SummitClimbTrace), /*bTraceComplex*/ false, GetOwner());
	Params.bReturnPhysicalMaterial = true;
	const FVector End = Start + Direction.GetSafeNormal() * Distance;
	return World->SweepSingleByChannel(OutHit, Start, End, FQuat::Identity, ECC_Visibility,
		FCollisionShape::MakeSphere(TraceRadiusCm), Params);
}

ESummitTerrainType USummitClimbingComponent::TerrainFromHit(const FHitResult& Hit) const
{
	const UPhysicalMaterial* PhysMat = Hit.PhysMaterial.Get();
	if (const USummitPhysicalMaterial* SummitMat = Cast<USummitPhysicalMaterial>(PhysMat))
	{
		return SummitMat->Terrain;
	}
	ESummitTerrainType Terrain = ESummitTerrainType::Rock;
	if (PhysMat)
	{
		SummitConv::TerrainFromSurfaceType(PhysMat->SurfaceType.GetValue(), Terrain);
	}
	return Terrain;
}

bool USummitClimbingComponent::IsClimbableHit(const FHitResult& Hit, ESummitTerrainType& OutTerrain) const
{
	if (!Hit.bBlockingHit)
	{
		return false;
	}
	OutTerrain = TerrainFromHit(Hit);

	if (const USummitPhysicalMaterial* SummitMat = Cast<USummitPhysicalMaterial>(Hit.PhysMaterial.Get()))
	{
		if (SummitMat->bClimbable)
		{
			return true;
		}
	}

	const AActor* HitActor = Hit.GetActor();
	const UPrimitiveComponent* HitComp = Hit.GetComponent();
	if ((HitActor && HitActor->ActorHasTag(ClimbableTag)) || (HitComp && HitComp->ComponentHasTag(ClimbableTag)))
	{
		return true;
	}

	// Without explicit markup, solid rock and ice are climbable; loose and soft surfaces are not.
	return OutTerrain == ESummitTerrainType::Rock || OutTerrain == ESummitTerrainType::WetRock
		|| OutTerrain == ESummitTerrainType::Boulder || OutTerrain == ESummitTerrainType::GlacierIce
		|| OutTerrain == ESummitTerrainType::Verglas || OutTerrain == ESummitTerrainType::HardSnow;
}

bool USummitClimbingComponent::CanClimbTerrain(ESummitTerrainType Terrain, ESummitClimbMode& OutMode) const
{
	switch (Terrain)
	{
	case ESummitTerrainType::Rock:
	case ESummitTerrainType::WetRock:
	case ESummitTerrainType::Boulder:
		OutMode = ESummitClimbMode::RockClimbing;
		return true;

	case ESummitTerrainType::GlacierIce:
	case ESummitTerrainType::Verglas:
	case ESummitTerrainType::HardSnow:
		// Front-pointing needs both: tools for the hands and crampons for the feet.
		OutMode = ESummitClimbMode::IceClimbing;
		return ClimberComp && ClimberComp->bIceAxes && ClimberComp->bCrampons;

	default:
		return false;
	}
}

void USummitClimbingComponent::GetWallBasis(FVector& OutRight, FVector& OutUp) const
{
	// Up = world up projected into the wall plane; Right completes the frame facing into the wall (-Normal).
	OutUp = FVector::VectorPlaneProject(FVector::UpVector, WallNormal).GetSafeNormal();
	if (OutUp.IsNearlyZero())
	{
		// Horizontal roof: fall back to the actor's forward as "up the wall".
		const AActor* Owner = GetOwner();
		OutUp = Owner ? Owner->GetActorForwardVector() : FVector::ForwardVector;
	}
	OutRight = FVector::CrossProduct(OutUp, -WallNormal).GetSafeNormal();
}

// ---- Wall climbing -------------------------------------------------------------------------------------

bool USummitClimbingComponent::TryStartClimb()
{
	ASummitCharacter* Char = GetSummitCharacter();
	if (!Char || !ClimberComp || ClimberComp->IsDead())
	{
		return false;
	}

	const ESummitClimbMode Mode = GetMode();
	const bool bFromRope = IsRopeMode(Mode);
	if (Mode != ESummitClimbMode::Walking && Mode != ESummitClimbMode::Scrambling && !bFromRope)
	{
		return false;
	}

	const FVector Chest = Char->GetActorLocation() + FVector(0.0, 0.0, 30.0);
	FHitResult Hit;
	if (!TraceWall(Chest, Char->GetActorForwardVector(), ReachCm, Hit))
	{
		return false;
	}

	ESummitTerrainType Terrain;
	ESummitClimbMode NewMode;
	if (!IsClimbableHit(Hit, Terrain) || !CanClimbTerrain(Terrain, NewMode))
	{
		return false;
	}

	// Only walls steeper than walkable are climbed; gentler ground is walked or scrambled.
	const double SurfaceDeg = SlopeDegFromNormal(Hit.ImpactNormal);
	const double MaxWalk = summit::sim::MaxWalkableSlopeDeg(SummitConv::ToSim(Terrain), ClimberComp->bCrampons);
	if (SurfaceDeg < MaxWalk - 5.0)
	{
		return false;
	}

	if (bFromRope)
	{
		AttachedRope.Reset();
	}

	WallNormal = Hit.ImpactNormal;
	WallTerrain = Terrain;
	WallDistanceCm = Hit.Distance;
	bHasWall = true;
	RequestMode(NewMode);
	return true;
}

void USummitClimbingComponent::StopClimbing(bool bDrop)
{
	if (!IsWallMode(GetMode()))
	{
		return;
	}
	bHasWall = false;
	bMantling = false;
	if (bDrop)
	{
		OnLetGo.Broadcast();
	}
	// The character maps leaving a custom mode to MOVE_Falling; with ground underfoot it lands at once.
	RequestMode(ESummitClimbMode::Walking);
}

float USummitClimbingComponent::GetClimbSpeedCmS() const
{
	if (!ClimberComp)
	{
		return MaxClimbSpeedCmS;
	}
	const bool bIce = GetMode() == ESummitClimbMode::IceClimbing;
	const float Skill = bIce ? ClimberComp->GetSkillIce01() : ClimberComp->GetSkillTechnical01();
	const float Stamina = ClimberComp->GetStamina01();

	float Speed = MaxClimbSpeedCmS * (0.5f + 0.5f * Skill) * (0.35f + 0.65f * Stamina);

	// The sim's sustainable pace for the Climbing activity carries altitude, fatigue and illness.
	const float Sustainable = ClimberComp->GetMaxSustainableSpeedCmS();
	if (Sustainable > 0.f)
	{
		Speed = FMath::Min(Speed, FMath::Max(Sustainable * 1.5f, 8.f));
	}
	return FMath::Max(Speed, 5.f);
}

void USummitClimbingComponent::TickWall(float DeltaTime)
{
	const AActor* Owner = GetOwner();
	if (!Owner || !ClimberComp)
	{
		return;
	}

	const FVector Center = Owner->GetActorLocation();
	FHitResult Hit;
	ESummitTerrainType Terrain = WallTerrain;
	const bool bWallAhead = TraceWall(Center, -WallNormal, ReachCm, Hit) && IsClimbableHit(Hit, Terrain);

	if (!bWallAhead)
	{
		// The wall ended: usually the top. Mantle if there is somewhere to stand, otherwise we come off.
		if (!TryMantle())
		{
			StopClimbing(/*bDrop*/ true);
		}
		return;
	}

	// Smooth the normal so the capsule does not jitter over rough geometry.
	const float Blend = FMath::Clamp(DeltaTime * 10.f, 0.f, 1.f);
	WallNormal = (WallNormal + (Hit.ImpactNormal - WallNormal) * Blend).GetSafeNormal();
	WallDistanceCm = Hit.Distance;

	// Rock to ice (or back): switch technique, or come off if the gear is missing.
	if (Terrain != WallTerrain)
	{
		ESummitClimbMode NewMode;
		if (!CanClimbTerrain(Terrain, NewMode))
		{
			StopClimbing(true);
			return;
		}
		WallTerrain = Terrain;
		if (NewMode != GetMode())
		{
			RequestMode(NewMode);
		}
	}

	// Topping out: pushing up with no wall at head height.
	if (ClimbInput.Y > 0.5f)
	{
		const float HalfHeight = Owner->GetSimpleCollisionHalfHeight();
		FHitResult HeadHit;
		if (!TraceWall(Center + FVector::UpVector * (HalfHeight + 20.f), -WallNormal, ReachCm, HeadHit) && TryMantle())
		{
			return;
		}
	}

	// Grip cost. Slabs (normal tilted up) let the feet carry the weight; overhangs load the arms.
	const double NZ = WallNormal.Z;
	double Steepness = 1.0;
	if (NZ > 0.0)
	{
		Steepness = 1.0 - 0.7 * FMath::Clamp(NZ / 0.7, 0.0, 1.0);
	}
	else
	{
		Steepness = 1.0 + 2.0 * FMath::Clamp(-NZ, 0.0, 1.0);
	}

	double TerrainFactor = 1.0;
	switch (WallTerrain)
	{
	case ESummitTerrainType::WetRock: TerrainFactor = 1.4; break;
	case ESummitTerrainType::Verglas: TerrainFactor = 1.5; break;   // thin ice: poor tool and boot placements
	case ESummitTerrainType::GlacierIce: TerrainFactor = 0.8; break; // hanging on well-placed tools
	case ESummitTerrainType::HardSnow: TerrainFactor = 0.7; break;
	default: break;
	}

	double LoadFactor = 1.0;
	if (const USummitCargoComponent* Cargo = Owner->FindComponentByClass<USummitCargoComponent>())
	{
		LoadFactor = 1.0 + 1.5 * Cargo->GetTotalMassKg() / FMath::Max(40.0, static_cast<double>(ClimberComp->BodyMassKg));
	}

	const bool bIce = GetMode() == ESummitClimbMode::IceClimbing;
	const double Skill = bIce ? ClimberComp->GetSkillIce01() : ClimberComp->GetSkillTechnical01();
	const double SkillFactor = 1.3 - 0.6 * Skill;

	// Cold hands lose grip strength and dexterity quickly below ~15 °C skin temperature.
	const double HandsC = ClimberComp->GetVitals().HandsTempC;
	const double ColdFactor = 1.0 + FMath::Clamp((15.0 - HandsC) / 15.0, 0.0, 1.0);

	const double Drain = (StaticGripDrainPerS + MovingGripDrainPerS * ClimbInput.Size())
		* Steepness * TerrainFactor * LoadFactor * SkillFactor * ColdFactor * DeltaTime;
	if (!ClimberComp->ConsumeStamina(static_cast<float>(Drain)))
	{
		StopClimbing(/*bDrop*/ true);
		return;
	}

	UpdateWallLimbs(DeltaTime);
}

void USummitClimbingComponent::UpdateWallLimbs(float DeltaTime)
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	FVector Right, Up;
	GetWallBasis(Right, Up);
	const FVector Center = Owner->GetActorLocation();
	const FRotator LimbRot = FRotationMatrix::MakeFromXZ(-WallNormal, Up).Rotator();
	bLimbTargetsValid = true;

	// Advance limbs in flight.
	bool bAnyInFlight = false;
	for (int32 Index = 0; Index < LimbCount; ++Index)
	{
		FSummitLimbTarget& L = Limb(Index);
		if (LimbMoveAlpha[Index] < 1.f)
		{
			LimbMoveAlpha[Index] = FMath::Min(1.f, LimbMoveAlpha[Index] + DeltaTime / LimbMoveTimeS);
			const float Smooth = FMath::SmoothStep(0.f, 1.f, LimbMoveAlpha[Index]);
			// Arc the limb slightly off the wall while it travels.
			const float Lift = FMath::Sin(Smooth * UE_PI) * 8.f;
			L.Location = FMath::Lerp(LimbFrom[Index], LimbTo[Index], Smooth) + WallNormal * Lift;
			L.Alpha = 0.6f + 0.4f * Smooth;
			L.bPlanted = LimbMoveAlpha[Index] >= 1.f;
			bAnyInFlight |= !L.bPlanted;
		}
		else if (L.bPlanted)
		{
			L.Location = LimbTo[Index];
			L.Alpha = 1.f;
		}
	}

	// One limb at a time, in diagonal order: three points of contact.
	if (bAnyInFlight)
	{
		return;
	}
	for (int32 Step = 0; Step < LimbCount; ++Step)
	{
		const int32 GaitSlot = (NextLimbToMove + Step) % LimbCount;
		const int32 Index = kGaitOrder[GaitSlot];
		FSummitLimbTarget& L = Limb(Index);

		const FVector Ideal = Center + Right * kLimbOffsets[Index].X + Up * kLimbOffsets[Index].Y;
		if (L.bPlanted && FVector::Dist(LimbTo[Index], Ideal) < LimbStepThresholdCm)
		{
			continue;
		}

		FHitResult Hold;
		ESummitTerrainType HoldTerrain;
		if (TraceWall(Ideal + WallNormal * 50.f, -WallNormal, 100.f, Hold) && IsClimbableHit(Hold, HoldTerrain))
		{
			LimbFrom[Index] = L.bPlanted ? LimbTo[Index] : Ideal + WallNormal * 15.f;
			LimbTo[Index] = Hold.ImpactPoint + Hold.ImpactNormal * LimbSurfaceOffsetCm;
			LimbMoveAlpha[Index] = 0.f;
			L.Rotation = FRotationMatrix::MakeFromXZ(-Hold.ImpactNormal, Up).Rotator();
			L.bPlanted = false;
			NextLimbToMove = (GaitSlot + 1) % LimbCount;
			return;
		}

		// Nothing to hold here: leave the limb hanging (IK off) and try the next one.
		L.bPlanted = false;
		L.Alpha = 0.f;
		L.Rotation = LimbRot;
	}
}

bool USummitClimbingComponent::TryMantle()
{
	ASummitCharacter* Char = GetSummitCharacter();
	UWorld* World = GetWorld();
	if (!Char || !World || bMantling)
	{
		return false;
	}

	const UCapsuleComponent* Capsule = Char->GetCapsuleComponent();
	const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 90.f;
	const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 35.f;

	const FVector Center = Char->GetActorLocation();
	const FVector Into = (-WallNormal).GetSafeNormal2D();
	const FVector Start = Center + FVector::UpVector * (HalfHeight * 2.f + 30.f) + Into * (Radius * 2.f + 20.f);
	const FVector End = Start - FVector::UpVector * (HalfHeight * 2.f + 80.f);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(SummitMantle), false, Char);
	FHitResult Ledge;
	if (!World->LineTraceSingleByChannel(Ledge, Start, End, ECC_Visibility, Params))
	{
		return false;
	}

	// Must be somewhere a person can stand.
	const double MaxWalk = ClimberComp ? ClimberComp->GetMaxWalkableSlopeDeg() : 45.0;
	if (SlopeDegFromNormal(Ledge.ImpactNormal) > MaxWalk)
	{
		return false;
	}

	const FVector Target = Ledge.ImpactPoint + FVector::UpVector * (HalfHeight + 2.f);
	if (World->OverlapBlockingTestByChannel(Target, FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeCapsule(Radius, HalfHeight), Params))
	{
		return false;
	}

	bMantling = true;
	MantleAlpha = 0.f;
	MantleStart = Center;
	MantleTarget = Target;
	OnMantleStarted.Broadcast(Target);
	return true;
}

void USummitClimbingComponent::TickMantle(float DeltaTime)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// Two phases: pull up the wall, then roll over the lip. Movement is kinematic; the AnimBP plays a
	// mantle montage (optionally motion-warped to MantleTarget via OnMantleStarted).
	MantleAlpha = FMath::Min(1.f, MantleAlpha + DeltaTime / MantleDurationS);
	const FVector Lip(MantleStart.X, MantleStart.Y, MantleTarget.Z);
	constexpr float PullPhase = 0.6f;
	FVector Location;
	if (MantleAlpha < PullPhase)
	{
		Location = FMath::Lerp(MantleStart, Lip, FMath::SmoothStep(0.f, 1.f, MantleAlpha / PullPhase));
	}
	else
	{
		Location = FMath::Lerp(Lip, MantleTarget, FMath::SmoothStep(0.f, 1.f, (MantleAlpha - PullPhase) / (1.f - PullPhase)));
	}
	Owner->SetActorLocation(Location, /*bSweep*/ false);

	if (MantleAlpha >= 1.f)
	{
		bMantling = false;
		bHasWall = false;
		RequestMode(ESummitClimbMode::Walking);
	}
}

// ---- Fixed ropes ---------------------------------------------------------------------------------------

bool USummitClimbingComponent::TryAttachToRope()
{
	const AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World || (ClimberComp && ClimberComp->IsDead()))
	{
		return false;
	}

	const ESummitClimbMode Mode = GetMode();
	if (Mode != ESummitClimbMode::Walking && Mode != ESummitClimbMode::Scrambling && !IsWallMode(Mode))
	{
		return false;
	}

	const FVector Location = Owner->GetActorLocation();
	ASummitFixedRope* Best = nullptr;
	float BestDistanceSq = FMath::Square(RopeGrabRadiusCm);
	float BestAlong = 0.f;
	for (TActorIterator<ASummitFixedRope> It(World); It; ++It)
	{
		ASummitFixedRope* Rope = *It;
		const float Along = Rope->FindClosestDistance(Location);
		const float DistSq = FVector::DistSquared(Rope->GetLocationAtDistance(Along), Location);
		if (DistSq < BestDistanceSq)
		{
			Best = Rope;
			BestDistanceSq = DistSq;
			BestAlong = Along;
		}
	}
	if (!Best)
	{
		return false;
	}

	// Clipping in at the top of a line means going down; anywhere else means going up.
	const float Length = Best->GetRopeLength();
	const float HeightAlong = Best->GetUphillSign() > 0.f ? BestAlong : Length - BestAlong;
	const bool bNearTop = HeightAlong > Length - 300.f;
	const ESummitClimbMode NewMode = (bNearTop || ClimbInput.Y < -0.2f) ? ESummitClimbMode::Rappelling : ESummitClimbMode::Jumaring;

	bHasWall = false;
	RequestMode(NewMode);
	// Attachment is set after the mode change so the rope-mode bookkeeping cannot clear it.
	AttachedRope = Best;
	RopeDistanceCm = BestAlong;
	RopeBusyS = 2.f;   // clipping the ascender/descender and safety
	return true;
}

void USummitClimbingComponent::DetachFromRope()
{
	if (!IsRopeMode(GetMode()))
	{
		AttachedRope.Reset();
		return;
	}
	AttachedRope.Reset();
	RequestMode(ESummitClimbMode::Walking);
}

float USummitClimbingComponent::GetRopeTravelSign() const
{
	if (RopeBusyS > 0.f || bDeviceChangePending)
	{
		return 0.f;
	}
	const ESummitClimbMode Mode = GetMode();
	if (Mode == ESummitClimbMode::Jumaring && ClimbInput.Y > 0.2f)
	{
		return 1.f;
	}
	if (Mode == ESummitClimbMode::Rappelling && ClimbInput.Y < -0.2f)
	{
		return -1.f;
	}
	return 0.f;
}

float USummitClimbingComponent::GetRopeSpeedCmS() const
{
	const float Input = FMath::Abs(static_cast<float>(ClimbInput.Y));
	if (GetMode() == ESummitClimbMode::Rappelling)
	{
		return MaxRappelSpeedCmS * Input;
	}
	// Jumaring pace is pure aerobic work; the sim's sustainable speed for the Jumaring activity sets it.
	const float Sustainable = ClimberComp ? ClimberComp->GetMaxSustainableSpeedCmS() : 50.f;
	return FMath::Max(Sustainable, 5.f) * Input;
}

void USummitClimbingComponent::SetRopeDistance(float NewDistanceCm)
{
	ASummitFixedRope* Rope = AttachedRope.Get();
	if (!Rope)
	{
		return;
	}

	const float Length = Rope->GetRopeLength();
	const float Old = RopeDistanceCm;
	float New = FMath::Clamp(NewDistanceCm, 0.f, Length);

	// Stop at the first intermediate anchor crossed: the climber must unclip and reclip around it.
	const float Travel = New - Old;
	if (FMath::Abs(Travel) > KINDA_SMALL_NUMBER)
	{
		int32 Crossed = INDEX_NONE;
		float CrossedAt = New;
		for (int32 Index = 0; Index < Rope->Anchors.Num(); ++Index)
		{
			const float A = Rope->Anchors[Index].DistanceAlongRopeCm;
			const bool bAhead = Travel > 0.f ? (A > Old + 0.5f && A <= New) : (A < Old - 0.5f && A >= New);
			const bool bCloser = Travel > 0.f ? A < CrossedAt : A > CrossedAt;
			if (bAhead && (Crossed == INDEX_NONE || bCloser))
			{
				Crossed = Index;
				CrossedAt = A;
			}
		}
		if (Crossed != INDEX_NONE && CrossedAt > 0.5f && CrossedAt < Length - 0.5f)
		{
			New = CrossedAt;
			float Impairment = 1.f;
			if (ClimberComp)
			{
				const FSummitVitals V = ClimberComp->GetVitals();
				Impairment += V.CognitiveImpairment01 + FMath::Clamp((10.f - V.HandsTempC) / 20.f, 0.f, 1.f);
			}
			RopeBusyS = AnchorChangeoverS * Impairment;
			OnRopeAnchorReached.Broadcast(Crossed);
		}
	}
	RopeDistanceCm = New;

	// Rope ends: step off at the top after jumaring, or at the bottom after rappelling.
	const float Sign = Rope->GetUphillSign();
	const bool bAtTop = Sign > 0.f ? New >= Length - 1.f : New <= 1.f;
	const bool bAtBottom = Sign > 0.f ? New <= 1.f : New >= Length - 1.f;
	const ESummitClimbMode Mode = GetMode();
	if ((bAtTop && Mode == ESummitClimbMode::Jumaring) || (bAtBottom && Mode == ESummitClimbMode::Rappelling))
	{
		DetachFromRope();
	}
}

void USummitClimbingComponent::TickRope(float DeltaTime)
{
	if (!AttachedRope.IsValid())
	{
		DetachFromRope();
		return;
	}

	RopeBusyS = FMath::Max(0.f, RopeBusyS - DeltaTime);
	if (bDeviceChangePending && RopeBusyS <= 0.f)
	{
		bDeviceChangePending = false;
		// Rope-to-rope mode changes keep the attachment (HandleClimbModeChanged only clears it when leaving
		// rope modes).
		RequestMode(PendingRopeMode);
	}

	// Input against the current device asks for a changeover (ascender <-> descender).
	const ESummitClimbMode Mode = GetMode();
	ESummitClimbMode Wanted = Mode;
	if (ClimbInput.Y > 0.2f)
	{
		Wanted = ESummitClimbMode::Jumaring;
	}
	else if (ClimbInput.Y < -0.2f)
	{
		Wanted = ESummitClimbMode::Rappelling;
	}
	if (Wanted != Mode && !bDeviceChangePending && RopeBusyS <= 0.f)
	{
		float Impairment = 1.f;
		if (ClimberComp)
		{
			Impairment += ClimberComp->GetCognitiveImpairment01();
		}
		bDeviceChangePending = true;
		PendingRopeMode = Wanted;
		RopeBusyS = DeviceChangeoverS * Impairment;
	}

	UpdateRopeLimbs();
}

void USummitClimbingComponent::UpdateRopeLimbs()
{
	const ASummitFixedRope* Rope = AttachedRope.Get();
	const AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Rope || !Owner || !World)
	{
		ClearLimbs();
		return;
	}

	bLimbTargetsValid = true;
	const float Sign = Rope->GetUphillSign();
	const FVector UpRope = Rope->GetUphillDirectionAtDistance(RopeDistanceCm);
	const FRotator HandRot = FRotationMatrix::MakeFromX(UpRope).Rotator();

	// Jumaring: both hands above (ascender, then safety). Rappelling: guide hand above, brake hand below.
	const bool bRappel = GetMode() == ESummitClimbMode::Rappelling;
	const float LeftAlong = RopeDistanceCm + Sign * (bRappel ? 30.f : 45.f);
	const float RightAlong = RopeDistanceCm + Sign * (bRappel ? -35.f : 75.f);
	LeftHand.Location = Rope->GetLocationAtDistance(FMath::Clamp(LeftAlong, 0.f, Rope->GetRopeLength()));
	RightHand.Location = Rope->GetLocationAtDistance(FMath::Clamp(RightAlong, 0.f, Rope->GetRopeLength()));
	LeftHand.Rotation = HandRot;
	RightHand.Rotation = HandRot;
	LeftHand.Alpha = RightHand.Alpha = 1.f;
	LeftHand.bPlanted = RightHand.bPlanted = true;

	// Feet on the slope under the body.
	const FVector Center = Owner->GetActorLocation();
	const FVector Right = FVector::CrossProduct(FVector::UpVector, UpRope.GetSafeNormal2D()).GetSafeNormal();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SummitRopeFeet), false, Owner);
	const float Sides[2] = { -18.f, 18.f };
	FSummitLimbTarget* Feet[2] = { &LeftFoot, &RightFoot };
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const FVector Start = Center + Right * Sides[Index];
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, Start, Start - FVector::UpVector * 160.f, ECC_Visibility, Params))
		{
			Feet[Index]->Location = Hit.ImpactPoint + Hit.ImpactNormal * LimbSurfaceOffsetCm;
			Feet[Index]->Rotation = FRotationMatrix::MakeFromZX(Hit.ImpactNormal, UpRope).Rotator();
			Feet[Index]->Alpha = 1.f;
			Feet[Index]->bPlanted = true;
		}
		else
		{
			Feet[Index]->Alpha = 0.f;
			Feet[Index]->bPlanted = false;
		}
	}
}

// ---- Sliding and self-arrest ---------------------------------------------------------------------------

void USummitClimbingComponent::TickSlide(float DeltaTime)
{
	SlideTimeS += DeltaTime;
	ClearLimbs();
}

bool USummitClimbingComponent::ShouldFallBecomeSlide() const
{
	if (!ClimberComp || !ClimberComp->HasFloor())
	{
		return false;
	}
	// On hard snow and ice a body that goes down on a slope of more than ~25-30° starts to slide.
	return IsSlippery(ClimberComp->GetFloorTerrain()) && SlopeDegFromNormal(ClimberComp->GetFloorNormal()) > 28.0;
}

bool USummitClimbingComponent::TrySelfArrest()
{
	if (GetMode() != ESummitClimbMode::Sliding || bArresting || !ClimberComp || !ArrestRng)
	{
		return false;
	}
	if (SlideTimeS - LastArrestAttemptS < SelfArrestRetryS)
	{
		return false;
	}
	LastArrestAttemptS = SlideTimeS;

	const ESummitTerrainType Terrain = ClimberComp->GetFloorTerrain();
	const double SlopeDeg = SlopeDegFromNormal(ClimberComp->GetFloorNormal());

	// Reaction time is the time already spent sliding: the longer it takes, the faster the slide and the
	// lower the odds (summit::sim models this).
	double Probability = summit::sim::SelfArrestSuccessProbability(SlopeDeg, IsIceOrHardSnow(Terrain),
		ClimberComp->GetSkillIce01(), SlideTimeS, ClimberComp->GetFatigue01());

	// Without an axe only a boot/hand arrest is possible, which rarely works on anything firm.
	if (!ClimberComp->bIceAxes)
	{
		Probability *= 0.25;
	}

	const bool bSuccess = ArrestRng->NextDouble() < Probability;
	bArresting = bSuccess;
	OnSelfArrestAttempt.Broadcast(bSuccess, static_cast<float>(Probability));
	return bSuccess;
}

void USummitClimbingComponent::NotifySlideStopped()
{
	bArresting = false;
	// Face-down in the snow holding the axe: the player has to get up.
	RequestMode(ESummitClimbMode::Fallen);
}

void USummitClimbingComponent::NotifySlideImpact(float ImpactSpeedCmS)
{
	const float SpeedMS = static_cast<float>(SummitConv::CmsToMs(ImpactSpeedCmS));
	OnSlideImpact.Broadcast(SpeedMS);

	// Blunt trauma thresholds for hitting rock/ice while sliding: ~12 m/s is commonly fatal (comparable to a
	// 7 m free fall); above ~5 m/s the slide is stopped hard and the climber is down.
	if (SpeedMS > 12.f && ClimberComp)
	{
		ClimberComp->KillClimber(ESummitCauseOfDeath::Fall);
	}
	if (SpeedMS > 5.f)
	{
		bArresting = false;
		RequestMode(ESummitClimbMode::Fallen);
	}
}
