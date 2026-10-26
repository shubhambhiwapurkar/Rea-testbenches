// Weather, sun and snowpack for the current world.
//
// Owns summit::sim::WeatherSystem (seeded, deterministic) and summit::sim::SnowpackModel, advances them from
// the world clock, and answers point queries ("what is the weather here?") by converting world Z to altitude
// through USummitSettings. Optionally drives presentation actors (sun, fog, material parameter collection);
// that code is null-safe so the subsystem runs in an empty test map.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Data/SummitTypes.h"
#include "Simulation/SummitSimHandles.h"
#include "SummitWeatherSubsystem.generated.h"

class ADirectionalLight;
class AExponentialHeightFog;
class UMaterialParameterCollection;
class USummitWorldClockSubsystem;

namespace summit::sim
{
	struct WeatherSample;
}

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSummitWeatherUpdatedSignature);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSummitAvalancheDangerSignature, ESummitAvalancheDanger, NewDanger);

UCLASS()
class SUMMIT_API USummitWeatherSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem / UWorldSubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// ---- Queries -------------------------------------------------------------------------------------

	// Weather at a world location. Exposure01: 0 = sheltered couloir/tent lee, 1 = exposed ridge.
	UFUNCTION(BlueprintPure, Category = "Summit|Weather")
	FSummitWeatherSnapshot SampleAtLocation(const FVector& WorldLocation, float Exposure01 = 1.f) const;

	UFUNCTION(BlueprintPure, Category = "Summit|Weather")
	FSummitWeatherSnapshot SampleAtAltitude(float AltitudeM, float Exposure01 = 1.f) const;

	// Native sample for components that feed the sim directly (avoids a float round-trip).
	void SampleSim(double AltitudeM, double Exposure01, summit::sim::WeatherSample& OutSample) const;

	// Barometric pressure (kPa) at an altitude for this map's latitude and the current day of year.
	UFUNCTION(BlueprintPure, Category = "Summit|Weather")
	float GetPressureKPa(float AltitudeM) const;

	UFUNCTION(BlueprintPure, Category = "Summit|Weather")
	float WorldZToAltitudeM(double WorldZCm) const;

	// Radio forecast for the given altitude: truth plus error growing with lead time.
	UFUNCTION(BlueprintCallable, Category = "Summit|Weather")
	TArray<FSummitForecastEntry> GetForecast(float HoursAhead = 72.f, float StepHours = 6.f, float AltitudeM = 8000.f) const;

	// Regional avalanche danger (EAWS) from the snowpack.
	UFUNCTION(BlueprintPure, Category = "Summit|Avalanche")
	ESummitAvalancheDanger GetAvalancheDanger() const { return CurrentDanger; }

	UFUNCTION(BlueprintPure, Category = "Summit|Avalanche")
	float GetNewSnow24hCm() const;

	UFUNCTION(BlueprintPure, Category = "Summit|Avalanche")
	float GetNewSnow72hCm() const;

	// Probability that a party crossing a slope releases it (for route decisions and scripted slopes).
	UFUNCTION(BlueprintPure, Category = "Summit|Avalanche")
	float GetHumanTriggerProbability(float SlopeAngleDeg, float AspectDeg, float AltitudeM, bool bLeeward,
		bool bConvex, int32 PartySize, bool bSpacedOut) const;

	// Sun position for this map's latitude and the clock's day and solar hour (degrees).
	// Azimuth is clockwise from north; elevation is above the horizon (negative at night).
	UFUNCTION(BlueprintPure, Category = "Summit|Sun")
	void GetSunPosition(float& OutElevationDeg, float& OutAzimuthDeg) const;

	// Pure solar geometry, exposed for tests and tools. DayOfYear 0-based, HourOfDay local solar time.
	static void ComputeSunPosition(double LatitudeDeg, int32 DayOfYear, double HourOfDay,
		double& OutElevationDeg, double& OutAzimuthDeg);

	// Snapshot at the local player's pawn, refreshed every weather update. Cheap for UI to poll.
	UFUNCTION(BlueprintPure, Category = "Summit|Weather")
	FSummitWeatherSnapshot GetPlayerWeather() const { return PlayerWeather; }

	// ---- Configuration -------------------------------------------------------------------------------

	// Altitude whose weather drives the regional snowpack (start-zone elevation of the slopes that matter).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Avalanche")
	float SnowpackReferenceAltitudeM = 6000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Avalanche", meta = (ClampMin = "0", ClampMax = "1"))
	float SnowpackWindExposure01 = 0.7f;

	// Yaw (deg) of geographic north in world space. Tools/terrain imports heightmaps with +X = east and
	// +Y = south, so north is -Y, i.e. yaw -90. Azimuths map to world yaw as NorthYawDeg + azimuth.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Sun")
	float NorthYawDeg = -90.f;

	// Actor tags used to find presentation actors in the level.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Presentation")
	FName SunActorTag = TEXT("Sun");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Presentation")
	FName FogActorTag = TEXT("SummitFog");

	UPROPERTY(BlueprintAssignable, Category = "Summit|Weather")
	FSummitWeatherUpdatedSignature OnWeatherUpdated;

	UPROPERTY(BlueprintAssignable, Category = "Summit|Avalanche")
	FSummitAvalancheDangerSignature OnAvalancheDangerChanged;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void BuildClimate();
	void AdvanceSimTo(double SimTimeSeconds);
	void HandleClockJump(double FromSimSeconds, double JumpSeconds);
	void RefreshPlayerWeather();
	void FindSceneActors();
	void DriveScene();

	TSimPtr<summit::sim::WeatherSystem> Weather;
	TSimPtr<summit::sim::SnowpackModel> Snowpack;

	UPROPERTY(Transient)
	TObjectPtr<USummitWorldClockSubsystem> Clock;

	// Presentation actors are owned by the level; weak so a streamed-out sun never dangles.
	TWeakObjectPtr<ADirectionalLight> SunActor;
	TWeakObjectPtr<AExponentialHeightFog> FogActor;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> WeatherMPC;

	FSummitWeatherSnapshot PlayerWeather;
	ESummitAvalancheDanger CurrentDanger = ESummitAvalancheDanger::Low;

	double LastUpdateSimSeconds = -1.0;
	double LatitudeDeg = 28.0;
	float AuthoredFogDensity = 0.02f;
	float SceneLookupCooldownS = 0.f;
	bool bSceneActorsSearched = false;
};
