// Project-wide SUMMIT settings (Project Settings > Game > Summit).

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SummitSettings.generated.h"

class UMaterialParameterCollection;

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Summit"))
class SUMMIT_API USummitSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	USummitSettings();

	static const USummitSettings* Get() { return GetDefault<USummitSettings>(); }

	// World Z (cm) that corresponds to 0 m above mean sea level. Real mountains are huge: a landscape tile
	// whose origin sits at 5000 m would use -500000 here, keeping world coordinates near the origin for
	// float precision while the sim sees true altitude.
	UPROPERTY(Config, EditAnywhere, Category = "World", meta = (Units = "Centimeters"))
	double SeaLevelWorldZCm = 0.0;

	// Constant added to every computed altitude (m). Lets a test map without real terrain pretend to be at
	// Camp 2 (e.g. 6400) without moving geometry. Usually 0.
	UPROPERTY(Config, EditAnywhere, Category = "World", meta = (Units = "Meters"))
	double AltitudeOffsetM = 0.0;

	// Latitude of the map, used for solar position, pressure correction and day length.
	UPROPERTY(Config, EditAnywhere, Category = "World", meta = (ClampMin = "-90", ClampMax = "90"))
	double LatitudeDeg = 27.988;

	// Longitude, only used for display and expedition data lookups (sim time is local solar time).
	UPROPERTY(Config, EditAnywhere, Category = "World", meta = (ClampMin = "-180", ClampMax = "180"))
	double LongitudeDeg = 86.925;

	// Climate profile id (Data/Mountains/climate/<id>.json). "khumbu" uses the built-in summit::sim::KhumbuClimate.
	UPROPERTY(Config, EditAnywhere, Category = "Weather")
	FName ClimateId = TEXT("khumbu");

	UPROPERTY(Config, EditAnywhere, Category = "Weather")
	int32 WeatherSeed = 8848;

	// Mountain database root, relative to the project directory.
	UPROPERTY(Config, EditAnywhere, Category = "Data")
	FString MountainDataDir = TEXT("Data/Mountains");

	// Game seconds per real second. 1 = real time: a climb takes as long as it really does. Long waits
	// (sleeping, fast travel between camps) use UWorldClock::AdvanceBy instead of a high scale.
	UPROPERTY(Config, EditAnywhere, Category = "Time", meta = (ClampMin = "0", ClampMax = "3600"))
	float TimeScale = 1.f;

	// Campaign start: day of year (0 = 1 Jan) and local solar hour. Default mid-May, pre-monsoon window.
	UPROPERTY(Config, EditAnywhere, Category = "Time", meta = (ClampMin = "0", ClampMax = "364"))
	int32 StartDayOfYear = 130;

	UPROPERTY(Config, EditAnywhere, Category = "Time", meta = (ClampMin = "0", ClampMax = "24"))
	float StartHour = 6.f;

	// Collection the weather subsystem writes WindSpeed, SnowIntensity, CloudCover, StormIntensity (and a few
	// more scalars) into each frame. Materials and Niagara systems read it.
	UPROPERTY(Config, EditAnywhere, Category = "Weather|Presentation")
	TSoftObjectPtr<UMaterialParameterCollection> WeatherParameterCollection;

	// Fog density at zero visibility; scaled down to the scene's authored density in clear air.
	UPROPERTY(Config, EditAnywhere, Category = "Weather|Presentation", meta = (ClampMin = "0"))
	float WhiteoutFogDensity = 0.35f;

	// Seconds of game time between weather/snowpack updates. The synoptic state changes slowly; sampling
	// every frame is pointless and would make the snowpack ring buffers do needless work.
	UPROPERTY(Config, EditAnywhere, Category = "Weather", meta = (ClampMin = "0.1"))
	float WeatherUpdateIntervalS = 1.f;

	// World Z (cm) -> altitude above mean sea level (m). The single place this mapping is defined.
	double WorldZToAltitudeM(double WorldZCm) const { return (WorldZCm - SeaLevelWorldZCm) / 100.0 + AltitudeOffsetM; }
	double AltitudeMToWorldZ(double AltitudeM) const { return (AltitudeM - AltitudeOffsetM) * 100.0 + SeaLevelWorldZCm; }
};
