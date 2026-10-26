#include "Climbing/SummitFixedRope.h"

#include "Data/SummitSimConversions.h"

#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

#include "Sim/Random.h"
#include "Sim/Rope.h"

ASummitFixedRope::ASummitFixedRope()
{
	PrimaryActorTick.bCanEverTick = false;

	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("Spline"));
	RootComponent = Spline;
	Spline->SetMobility(EComponentMobility::Static);

	Tags.Add(TEXT("FixedRope"));

	// A fresh line usually has an anchor at each end.
	FSummitAnchor Bottom;
	Bottom.AnchorType = ESummitAnchorType::SnowPicket;
	Bottom.RatedStrengthKN = 5.f;
	Bottom.DistanceAlongRopeCm = 0.f;
	Anchors.Add(Bottom);
}

void ASummitFixedRope::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RebuildVisuals();
}

void ASummitFixedRope::BeginPlay()
{
	Super::BeginPlay();
	FallRng.Reset(new summit::sim::Pcg32(static_cast<std::uint64_t>(static_cast<uint32>(FallSeed))));
}

void ASummitFixedRope::RebuildVisuals()
{
	for (USplineMeshComponent* Old : SegmentMeshes)
	{
		if (IsValid(Old))
		{
			Old->DestroyComponent();
		}
	}
	SegmentMeshes.Reset();

	if (!RopeMesh || !Spline)
	{
		return;
	}

	// The rope mesh is authored as a 1 cm diameter cylinder along +X, so scale = diameter in cm.
	const float Scale = DiameterMm / 10.f;
	const int32 NumSegments = Spline->GetNumberOfSplineSegments();
	for (int32 Index = 0; Index < NumSegments; ++Index)
	{
		FVector StartPos, StartTangent, EndPos, EndTangent;
		Spline->GetLocationAndTangentAtSplinePoint(Index, StartPos, StartTangent, ESplineCoordinateSpace::Local);
		Spline->GetLocationAndTangentAtSplinePoint(Index + 1, EndPos, EndTangent, ESplineCoordinateSpace::Local);

		USplineMeshComponent* Segment = NewObject<USplineMeshComponent>(this);
		// Marked as construction-script-created so the editor cleans it up on the next rerun.
		Segment->CreationMethod = EComponentCreationMethod::UserConstructionScript;
		Segment->SetMobility(EComponentMobility::Movable);
		Segment->SetupAttachment(Spline);
		Segment->SetStaticMesh(RopeMesh);
		if (RopeMaterial)
		{
			Segment->SetMaterial(0, RopeMaterial);
		}
		Segment->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Segment->SetStartScale(FVector2D(Scale, Scale), false);
		Segment->SetEndScale(FVector2D(Scale, Scale), false);
		Segment->SetStartAndEnd(StartPos, StartTangent, EndPos, EndTangent, false);
		Segment->RegisterComponent();
		Segment->UpdateMesh();
		SegmentMeshes.Add(Segment);
	}
}

float ASummitFixedRope::GetRopeLength() const
{
	return Spline ? Spline->GetSplineLength() : 0.f;
}

float ASummitFixedRope::FindClosestDistance(const FVector& WorldLocation) const
{
	if (!Spline)
	{
		return 0.f;
	}
	const float Key = Spline->FindInputKeyClosestToWorldLocation(WorldLocation);
	return Spline->GetDistanceAlongSplineAtSplineInputKey(Key);
}

FVector ASummitFixedRope::GetLocationAtDistance(float Distance) const
{
	return Spline ? Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World) : GetActorLocation();
}

FVector ASummitFixedRope::GetDirectionAtDistance(float Distance) const
{
	return Spline ? Spline->GetDirectionAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World) : FVector::UpVector;
}

float ASummitFixedRope::GetUphillSign() const
{
	if (!Spline)
	{
		return 1.f;
	}
	const float StartZ = Spline->GetLocationAtDistanceAlongSpline(0.f, ESplineCoordinateSpace::World).Z;
	const float EndZ = Spline->GetLocationAtDistanceAlongSpline(GetRopeLength(), ESplineCoordinateSpace::World).Z;
	return EndZ >= StartZ ? 1.f : -1.f;
}

FVector ASummitFixedRope::GetUphillDirectionAtDistance(float Distance) const
{
	return GetDirectionAtDistance(Distance) * GetUphillSign();
}

int32 ASummitFixedRope::FindAnchorBelow(float Distance) const
{
	const float Sign = GetUphillSign();
	int32 Best = INDEX_NONE;
	float BestGap = TNumericLimits<float>::Max();
	for (int32 Index = 0; Index < Anchors.Num(); ++Index)
	{
		// Gap measured toward the low end of the rope.
		const float Gap = (Distance - Anchors[Index].DistanceAlongRopeCm) * Sign;
		if (Gap >= 0.f && Gap < BestGap)
		{
			BestGap = Gap;
			Best = Index;
		}
	}
	return Best;
}

float ASummitFixedRope::GetAnchorStrengthKN(int32 AnchorIndex, float TemperatureC) const
{
	if (!Anchors.IsValidIndex(AnchorIndex))
	{
		return 0.f;
	}
	return static_cast<float>(summit::sim::AnchorEffectiveStrengthKN(SummitConv::ToSim(Anchors[AnchorIndex]),
		MediumQuality01, TemperatureC));
}

FSummitRopeFallResult ASummitFixedRope::SimulateFall(float Distance, float FallDistanceM, float ClimberMassKg, float TemperatureC)
{
	if (!FallRng)
	{
		FallRng.Reset(new summit::sim::Pcg32(static_cast<std::uint64_t>(static_cast<uint32>(FallSeed))));
	}

	summit::sim::RopeSpec Rope;
	Rope.diameterMm = DiameterMm;
	Rope.dynamic = bDynamicRope;
	Rope.wear01 = Wear01;

	// Without a recorded anchor below, assume an unknown-quality expedition anchor at the rope's low end.
	summit::sim::Anchor Anchor;
	Anchor.type = summit::sim::AnchorType::FixedLine;
	float AnchorDistance = GetUphillSign() > 0.f ? 0.f : GetRopeLength();
	const int32 AnchorIndex = FindAnchorBelow(Distance);
	if (Anchors.IsValidIndex(AnchorIndex))
	{
		Anchor = SummitConv::ToSim(Anchors[AnchorIndex]);
		AnchorDistance = Anchors[AnchorIndex].DistanceAlongRopeCm;
	}

	// Slope of the line at the fall point: steep ice faces (Lhotse ~40-50°) vs. near-vertical steps.
	const FVector Dir = GetUphillDirectionAtDistance(Distance);
	const double SlopeDeg = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(FMath::Abs(Dir.Z), 0.0, 1.0)));

	summit::sim::FallScenario Scenario;
	Scenario.climberMassKg = ClimberMassKg;
	// A slip on a fixed line drops the climber twice the slack/sliding distance onto the jumar/cow's tail.
	Scenario.distanceAboveProtectionM = FMath::Max(0.0f, FallDistanceM * 0.5f);
	Scenario.ropeOutM = FMath::Max(0.5, SummitConv::CmToM(FMath::Abs(Distance - AnchorDistance)));
	Scenario.slackM = 0.3;
	Scenario.slopeAngleDeg = SlopeDeg;
	Scenario.belayDynamic01 = 0.0;   // anchors are fixed; nothing gives
	Scenario.sharpEdge = false;

	const summit::sim::FallOutcome Outcome = summit::sim::ResolveFall(Scenario, Rope, Anchor, MediumQuality01,
		TemperatureC, *FallRng);

	FSummitRopeFallResult Result;
	Result.FallDistanceM = static_cast<float>(Outcome.fallDistanceM);
	Result.FallFactor = static_cast<float>(Outcome.fallFactor);
	Result.PeakForceKN = static_cast<float>(Outcome.peakForceKN);
	Result.bAnchorFailed = Outcome.anchorFailed;
	Result.bRopeCut = Outcome.ropeCut;
	Result.InjurySeverity01 = static_cast<float>(Outcome.injurySeverity01);
	return Result;
}
