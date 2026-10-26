// A fixed line: the expedition ropes strung up the Lhotse Face, the Cheeks of Denali, the Bottleneck.
//
// The spline defines the line (place points at the anchors). Anchors carry their type, rated strength and
// age, and the rope spec defaults to a static line, so a fall onto an old anchor can be resolved with the
// sim's rope mechanics (summit::sim::ResolveFall). Visuals are spline meshes generated along the spline.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Data/SummitTypes.h"
#include "Simulation/SummitSimHandles.h"
#include "SummitFixedRope.generated.h"

class USplineComponent;
class USplineMeshComponent;
class UStaticMesh;
class UMaterialInterface;

USTRUCT(BlueprintType)
struct SUMMIT_API FSummitRopeFallResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Rope")
	float FallDistanceM = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Rope")
	float FallFactor = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Rope")
	float PeakForceKN = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Rope")
	bool bAnchorFailed = false;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Rope")
	bool bRopeCut = false;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Rope")
	float InjurySeverity01 = 0.f;
};

UCLASS(Blueprintable)
class SUMMIT_API ASummitFixedRope : public AActor
{
	GENERATED_BODY()

public:
	ASummitFixedRope();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Rope")
	TObjectPtr<USplineComponent> Spline;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope")
	TArray<FSummitAnchor> Anchors;

	// Fixed lines are usually 8-10 mm static or semi-static rope.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope", meta = (ClampMin = "5", ClampMax = "13"))
	float DiameterMm = 9.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope")
	bool bDynamicRope = false;

	// UV, abrasion and crampon damage accumulated by a season of traffic.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope", meta = (ClampMin = "0", ClampMax = "1"))
	float Wear01 = 0.2f;

	// Quality of the medium the anchors sit in (ice/snow hardness, rock soundness).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope", meta = (ClampMin = "0", ClampMax = "1"))
	float MediumQuality01 = 0.7f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope")
	int32 FallSeed = 1;

	// Visuals: a thin cylinder mesh along +X is deformed along each spline segment.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope|Visual")
	TObjectPtr<UStaticMesh> RopeMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope|Visual")
	TObjectPtr<UMaterialInterface> RopeMaterial;

	// ---- Queries (world space, cm) --------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Summit|Rope")
	float GetRopeLength() const;

	UFUNCTION(BlueprintPure, Category = "Summit|Rope")
	float FindClosestDistance(const FVector& WorldLocation) const;

	UFUNCTION(BlueprintPure, Category = "Summit|Rope")
	FVector GetLocationAtDistance(float Distance) const;

	// Unit tangent at a distance, pointing toward increasing distance.
	UFUNCTION(BlueprintPure, Category = "Summit|Rope")
	FVector GetDirectionAtDistance(float Distance) const;

	// Unit tangent pointing up the rope (toward the higher end).
	UFUNCTION(BlueprintPure, Category = "Summit|Rope")
	FVector GetUphillDirectionAtDistance(float Distance) const;

	// +1 if distance increases uphill, -1 otherwise.
	UFUNCTION(BlueprintPure, Category = "Summit|Rope")
	float GetUphillSign() const;

	// Index of the nearest anchor below the given distance (toward the low end), or INDEX_NONE.
	UFUNCTION(BlueprintPure, Category = "Summit|Rope")
	int32 FindAnchorBelow(float Distance) const;

	// Effective strength of an anchor right now (kN).
	UFUNCTION(BlueprintPure, Category = "Summit|Rope")
	float GetAnchorStrengthKN(int32 AnchorIndex, float TemperatureC) const;

	// Resolves a fall while clipped to the line at Distance, using the anchor below. Deterministic per seed.
	UFUNCTION(BlueprintCallable, Category = "Summit|Rope")
	FSummitRopeFallResult SimulateFall(float Distance, float FallDistanceM, float ClimberMassKg, float TemperatureC);

private:
	void RebuildVisuals();

	UPROPERTY(Transient)
	TArray<TObjectPtr<USplineMeshComponent>> SegmentMeshes;

	TSimPtr<summit::sim::Pcg32> FallRng;
};
