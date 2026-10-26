// Expedition layer: the mountain database and the player's campaign state.
//
// Loads Data/Mountains/peaks.json and routes/*.json (the source of truth; see CLAUDE.md), converts routes to
// summit::sim::Route on demand, and keeps the expedition state that persists across levels: chosen peak and
// route, current camp, day count, the itinerary actually slept, money and a team roster stub. Itineraries
// are checked against Wilderness Medical Society ascent guidelines via summit::sim::EvaluateItinerary.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Data/SummitTypes.h"
#include "SummitExpeditionSubsystem.generated.h"

USTRUCT(BlueprintType)
struct SUMMIT_API FSummitPeakInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	FName Id;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	float ElevationM = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	double LatitudeDeg = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	double LongitudeDeg = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	FString Range;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	TArray<FString> Countries;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	float ProminenceM = 0.f;

	// Climate profile (Data/Mountains/climate/<id>.json) for USummitSettings::ClimateId on this peak's maps.
	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	FName ClimateId;
};

USTRUCT(BlueprintType)
struct SUMMIT_API FSummitCampInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	FName Id;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	float AltitudeM = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	double LatitudeDeg = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	double LongitudeDeg = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	int32 TentCapacity = 0;
};

USTRUCT(BlueprintType)
struct SUMMIT_API FSummitRouteSegmentInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	FName FromCampId;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	FName ToCampId;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	float DistanceM = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	float AscentM = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	float DescentM = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	ESummitTerrainType Terrain = ESummitTerrainType::HardSnow;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	float HoursUp = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	float HoursDown = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	bool bFixedRopes = false;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	TArray<FString> Hazards;
};

USTRUCT(BlueprintType)
struct SUMMIT_API FSummitRouteInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	FName Id;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	FName PeakId;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	FString Name;

	// Bottom to top; the last camp is the summit.
	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	TArray<FSummitCampInfo> Camps;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	TArray<FSummitRouteSegmentInfo> Segments;
};

USTRUCT(BlueprintType)
struct SUMMIT_API FSummitItineraryDay
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Expedition")
	int32 Day = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Expedition")
	FName SleepCampId;

	// Highest point reached that day ("climb high, sleep low").
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Expedition")
	float MaxAltitudeM = 0.f;
};

USTRUCT(BlueprintType)
struct SUMMIT_API FSummitItineraryReport
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	bool bFollowsGuidelines = true;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	float PeakAmsRisk01 = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	int32 RestDays = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Summit|Expedition")
	TArray<FString> Warnings;
};

// Placeholder roster entry until the team/Sherpa system exists.
USTRUCT(BlueprintType)
struct SUMMIT_API FSummitTeamMember
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Expedition")
	FString Name;

	// "Climber", "Sherpa", "Guide", "Cook", "Doctor"...
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Expedition")
	FString Role;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Expedition", meta = (ClampMin = "0", ClampMax = "1"))
	float Skill01 = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Expedition")
	bool bAvailable = true;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSummitExpeditionChangedSignature);

UCLASS()
class SUMMIT_API USummitExpeditionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	// ---- Database -------------------------------------------------------------------------------------

	// (Re)loads peaks.json and routes/*.json from <ProjectDir>/<MountainDataDir>. Returns false if peaks.json
	// is missing or malformed; individual bad route files are skipped with a warning.
	UFUNCTION(BlueprintCallable, Category = "Summit|Expedition")
	bool LoadDatabase();

	UFUNCTION(BlueprintPure, Category = "Summit|Expedition")
	TArray<FSummitPeakInfo> GetPeaks() const { return Peaks; }

	UFUNCTION(BlueprintPure, Category = "Summit|Expedition")
	bool FindPeak(FName PeakId, FSummitPeakInfo& OutPeak) const;

	UFUNCTION(BlueprintPure, Category = "Summit|Expedition")
	TArray<FSummitRouteInfo> GetRoutesForPeak(FName PeakId) const;

	UFUNCTION(BlueprintPure, Category = "Summit|Expedition")
	bool FindRoute(FName RouteId, FSummitRouteInfo& OutRoute) const;

	UFUNCTION(BlueprintPure, Category = "Summit|Expedition")
	bool FindSegment(FName RouteId, FName FromCampId, FName ToCampId, FSummitRouteSegmentInfo& OutSegment) const;

	// ---- Expedition state -----------------------------------------------------------------------------

	// Starts a new expedition at the route's first camp, day 0.
	UFUNCTION(BlueprintCallable, Category = "Summit|Expedition")
	bool SelectExpedition(FName PeakId, FName RouteId);

	UFUNCTION(BlueprintPure, Category = "Summit|Expedition")
	FName GetSelectedPeakId() const { return SelectedPeakId; }

	UFUNCTION(BlueprintPure, Category = "Summit|Expedition")
	FName GetSelectedRouteId() const { return SelectedRouteId; }

	UFUNCTION(BlueprintPure, Category = "Summit|Expedition")
	FName GetCurrentCampId() const { return CurrentCampId; }

	UFUNCTION(BlueprintPure, Category = "Summit|Expedition")
	int32 GetCurrentDay() const { return CurrentDay; }

	UFUNCTION(BlueprintPure, Category = "Summit|Expedition")
	TArray<FSummitItineraryDay> GetItinerary() const { return Itinerary; }

	// The climber reached a camp (does not end the day).
	UFUNCTION(BlueprintCallable, Category = "Summit|Expedition")
	bool ArriveAtCamp(FName CampId);

	// Ends the day: logs where the climber sleeps and the day's high point, then advances the day counter.
	UFUNCTION(BlueprintCallable, Category = "Summit|Expedition")
	void EndDay(FName SleepCampId, float MaxAltitudeReachedM);

	// Checks the itinerary slept so far.
	UFUNCTION(BlueprintCallable, Category = "Summit|Expedition")
	FSummitItineraryReport EvaluateCurrentItinerary() const;

	// Checks a planned itinerary on the selected route (planning screen).
	UFUNCTION(BlueprintCallable, Category = "Summit|Expedition")
	FSummitItineraryReport EvaluatePlannedItinerary(const TArray<FSummitItineraryDay>& Days) const;

	// Bottled O2 logistics (summit::sim::OxygenBottleHours).
	UFUNCTION(BlueprintPure, Category = "Summit|Expedition")
	static float GetOxygenBottleHours(float WaterVolumeL = 4.f, float FillPressureBar = 300.f, float FlowLMin = 2.f);

	// Expedition budget in US dollars (permit, Sherpa support, oxygen, food). Stub economy.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Expedition")
	int32 MoneyUSD = 45000;

	UFUNCTION(BlueprintCallable, Category = "Summit|Expedition")
	bool SpendMoney(int32 AmountUSD);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Expedition")
	TArray<FSummitTeamMember> Team;

	UPROPERTY(BlueprintAssignable, Category = "Summit|Expedition")
	FSummitExpeditionChangedSignature OnExpeditionChanged;

private:
	FSummitItineraryReport Evaluate(const FName RouteId, const TArray<FSummitItineraryDay>& Days) const;

	TArray<FSummitPeakInfo> Peaks;
	TMap<FName, FSummitRouteInfo> Routes;

	FName SelectedPeakId;
	FName SelectedRouteId;
	FName CurrentCampId;
	int32 CurrentDay = 0;
	TArray<FSummitItineraryDay> Itinerary;
};
