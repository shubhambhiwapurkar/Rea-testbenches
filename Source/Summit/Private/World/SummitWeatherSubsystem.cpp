#include "World/SummitWeatherSubsystem.h"

#include "Data/SummitSettings.h"
#include "Data/SummitSimConversions.h"
#include "Summit.h"
#include "World/SummitWorldClockSubsystem.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Sim/Atmosphere.h"
#include "Sim/Avalanche.h"
#include "Sim/Constants.h"
#include "Sim/Weather.h"

namespace
{
	// Largest step used when catching up the synoptic state and snowpack. The snowpack keeps hourly ring
	// buffers, so steps well under an hour keep its new-snow totals accurate after a long sleep.
	constexpr double kMaxWeatherStepS = 900.0;

	// How often (real seconds) to retry finding presentation actors that are missing (streaming levels).
	constexpr float kSceneLookupIntervalS = 2.f;

	// Reads a 12-element numeric array into a monthly table; leaves Out untouched if the field is absent
	// or malformed so the Khumbu defaults survive partial climate files.
	void ReadMonthly(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, std::array<double, 12>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Obj->TryGetArrayField(Field, Values) && Values && Values->Num() == 12)
		{
			for (int32 Month = 0; Month < 12; ++Month)
			{
				double V = 0.0;
				if ((*Values)[Month].IsValid() && (*Values)[Month]->TryGetNumber(V))
				{
					Out[Month] = V;
				}
			}
		}
	}

	// Loads Data/Mountains/climate/<id>.json on top of the Khumbu profile.
	// Schema (all optional): latitude_deg, reference_elevation_m, lapse_rate_c_per_km, prevailing_wind_from_deg,
	// and 12-element arrays mean_temp_c, diurnal_range_c, jet_stream_wind_ms, storms_per_week,
	// precipitation_mm_per_day.
	bool LoadClimateJson(const FString& Path, summit::sim::ClimateProfile& InOut)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			UE_LOG(LogSummit, Warning, TEXT("Weather: could not parse climate file %s"), *Path);
			return false;
		}

		Root->TryGetNumberField(TEXT("latitude_deg"), InOut.latitudeDeg);
		Root->TryGetNumberField(TEXT("reference_elevation_m"), InOut.referenceElevationM);
		Root->TryGetNumberField(TEXT("lapse_rate_c_per_km"), InOut.lapseRateCPerKm);
		Root->TryGetNumberField(TEXT("prevailing_wind_from_deg"), InOut.prevailingWindFromDeg);
		ReadMonthly(Root, TEXT("mean_temp_c"), InOut.meanTempC);
		ReadMonthly(Root, TEXT("diurnal_range_c"), InOut.diurnalRangeC);
		ReadMonthly(Root, TEXT("jet_stream_wind_ms"), InOut.jetStreamWindMS);
		ReadMonthly(Root, TEXT("storms_per_week"), InOut.stormsPerWeek);
		ReadMonthly(Root, TEXT("precipitation_mm_per_day"), InOut.precipitationMmPerDay);
		return true;
	}
}

bool USummitWeatherSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void USummitWeatherSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The clock must exist first: we read time from it and integrate its jumps.
	Clock = Collection.InitializeDependency<USummitWorldClockSubsystem>();
	if (Clock)
	{
		Clock->OnTimeJumpedNative.AddUObject(this, &USummitWeatherSubsystem::HandleClockJump);
	}

	LatitudeDeg = USummitSettings::Get()->LatitudeDeg;
	BuildClimate();

	if (Clock)
	{
		AdvanceSimTo(Clock->GetSimTimeSeconds());
	}
}

void USummitWeatherSubsystem::Deinitialize()
{
	if (Clock)
	{
		Clock->OnTimeJumpedNative.RemoveAll(this);
	}
	Clock = nullptr;
	Weather.Reset();
	Snowpack.Reset();
	Super::Deinitialize();
}

void USummitWeatherSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	const USummitSettings* Settings = USummitSettings::Get();
	if (!Settings->WeatherParameterCollection.IsNull())
	{
		WeatherMPC = Settings->WeatherParameterCollection.LoadSynchronous();
	}

	FindSceneActors();
	RefreshPlayerWeather();
	DriveScene();
}

TStatId USummitWeatherSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USummitWeatherSubsystem, STATGROUP_Tickables);
}

void USummitWeatherSubsystem::BuildClimate()
{
	const USummitSettings* Settings = USummitSettings::Get();

	summit::sim::ClimateProfile Climate = summit::sim::KhumbuClimate();
	if (!Settings->ClimateId.IsNone() && Settings->ClimateId != FName(TEXT("khumbu")))
	{
		const FString Path = FPaths::Combine(FPaths::ProjectDir(), Settings->MountainDataDir, TEXT("climate"),
			Settings->ClimateId.ToString() + TEXT(".json"));
		if (!LoadClimateJson(Path, Climate))
		{
			UE_LOG(LogSummit, Warning, TEXT("Weather: climate '%s' not found at %s, using Khumbu"),
				*Settings->ClimateId.ToString(), *Path);
		}
	}

	const std::uint64_t Seed = static_cast<std::uint64_t>(static_cast<uint32>(Settings->WeatherSeed));
	Weather.Reset(new summit::sim::WeatherSystem(Climate, Seed));
	Snowpack.Reset(new summit::sim::SnowpackModel());
	LastUpdateSimSeconds = -1.0;
}

void USummitWeatherSubsystem::AdvanceSimTo(double SimTimeSeconds)
{
	if (!Weather || !Snowpack)
	{
		return;
	}

	// WeatherSystem only moves forward. A backwards clock (loading a save) rebuilds it from the seed,
	// which reproduces the same weather because the model is deterministic.
	if (LastUpdateSimSeconds >= 0.0 && SimTimeSeconds < LastUpdateSimSeconds)
	{
		BuildClimate();
	}

	if (LastUpdateSimSeconds < 0.0)
	{
		Weather->AdvanceTo(SimTimeSeconds);
		LastUpdateSimSeconds = SimTimeSeconds;
	}

	double Current = LastUpdateSimSeconds;
	while (Current < SimTimeSeconds)
	{
		const double Next = FMath::Min(SimTimeSeconds, Current + kMaxWeatherStepS);
		Weather->AdvanceTo(Next);
		const summit::sim::WeatherSample Sample =
			Weather->Sample(SnowpackReferenceAltitudeM, SnowpackWindExposure01);
		Snowpack->Step(Next - Current, Sample, SnowpackWindExposure01);
		Current = Next;
	}
	LastUpdateSimSeconds = SimTimeSeconds;

	const ESummitAvalancheDanger NewDanger = SummitConv::FromSim(summit::sim::RegionalDanger(Snowpack->State()));
	if (NewDanger != CurrentDanger)
	{
		CurrentDanger = NewDanger;
		OnAvalancheDangerChanged.Broadcast(CurrentDanger);
	}
}

void USummitWeatherSubsystem::HandleClockJump(double FromSimSeconds, double JumpSeconds)
{
	AdvanceSimTo(FromSimSeconds + JumpSeconds);
	RefreshPlayerWeather();
	DriveScene();
	OnWeatherUpdated.Broadcast();
}

void USummitWeatherSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Clock)
	{
		return;
	}

	SceneLookupCooldownS -= DeltaTime;
	if ((!SunActor.IsValid() || !FogActor.IsValid()) && SceneLookupCooldownS <= 0.f)
	{
		FindSceneActors();
	}

	const double Now = Clock->GetSimTimeSeconds();
	const double Interval = FMath::Max(0.1, static_cast<double>(USummitSettings::Get()->WeatherUpdateIntervalS));
	if (LastUpdateSimSeconds < 0.0 || Now - LastUpdateSimSeconds >= Interval || Now < LastUpdateSimSeconds)
	{
		AdvanceSimTo(Now);
		RefreshPlayerWeather();
		OnWeatherUpdated.Broadcast();
	}

	// The sun moves every frame at accelerated time scales; it is cheap, so drive it unconditionally.
	DriveScene();
}

// ---- Queries -------------------------------------------------------------------------------------------

float USummitWeatherSubsystem::WorldZToAltitudeM(double WorldZCm) const
{
	return static_cast<float>(USummitSettings::Get()->WorldZToAltitudeM(WorldZCm));
}

void USummitWeatherSubsystem::SampleSim(double AltitudeM, double Exposure01, summit::sim::WeatherSample& OutSample) const
{
	if (Weather)
	{
		OutSample = Weather->Sample(AltitudeM, FMath::Clamp(Exposure01, 0.0, 1.0));
	}
	else
	{
		OutSample = summit::sim::WeatherSample();
	}
}

FSummitWeatherSnapshot USummitWeatherSubsystem::SampleAtAltitude(float AltitudeM, float Exposure01) const
{
	summit::sim::WeatherSample Sample;
	SampleSim(AltitudeM, Exposure01, Sample);
	return SummitConv::FromSim(Sample, AltitudeM, GetPressureKPa(AltitudeM));
}

FSummitWeatherSnapshot USummitWeatherSubsystem::SampleAtLocation(const FVector& WorldLocation, float Exposure01) const
{
	return SampleAtAltitude(WorldZToAltitudeM(WorldLocation.Z), Exposure01);
}

float USummitWeatherSubsystem::GetPressureKPa(float AltitudeM) const
{
	const int32 Day = Clock ? Clock->GetDayOfYear() : 140;
	return static_cast<float>(summit::sim::BarometricPressureKPa(AltitudeM, LatitudeDeg, Day));
}

TArray<FSummitForecastEntry> USummitWeatherSubsystem::GetForecast(float HoursAhead, float StepHours, float AltitudeM) const
{
	TArray<FSummitForecastEntry> Result;
	if (!Weather || HoursAhead <= 0.f || StepHours <= 0.f)
	{
		return Result;
	}

	const double Issued = Weather->CurrentTime();
	const float PressureKPa = GetPressureKPa(AltitudeM);
	const std::vector<summit::sim::ForecastEntry> Entries = Weather->Forecast(HoursAhead, StepHours, AltitudeM);
	Result.Reserve(static_cast<int32>(Entries.size()));
	for (const summit::sim::ForecastEntry& Entry : Entries)
	{
		FSummitForecastEntry& Out = Result.AddDefaulted_GetRef();
		Out.HoursAhead = static_cast<float>((Entry.time - Issued) / summit::sim::kSecondsPerHour);
		Out.SimTimeHours = static_cast<float>(Entry.time / summit::sim::kSecondsPerHour);
		Out.Expected = SummitConv::FromSim(Entry.expected, AltitudeM, PressureKPa);
		Out.Confidence01 = static_cast<float>(Entry.confidence01);
	}
	return Result;
}

float USummitWeatherSubsystem::GetNewSnow24hCm() const
{
	return Snowpack ? static_cast<float>(Snowpack->State().newSnow24hCm) : 0.f;
}

float USummitWeatherSubsystem::GetNewSnow72hCm() const
{
	return Snowpack ? static_cast<float>(Snowpack->State().newSnow72hCm) : 0.f;
}

float USummitWeatherSubsystem::GetHumanTriggerProbability(float SlopeAngleDeg, float AspectDeg, float AltitudeM,
	bool bLeeward, bool bConvex, int32 PartySize, bool bSpacedOut) const
{
	if (!Snowpack)
	{
		return 0.f;
	}

	summit::sim::SlopeInfo Slope;
	Slope.angleDeg = SlopeAngleDeg;
	Slope.aspectDeg = AspectDeg;
	Slope.elevationM = AltitudeM;
	Slope.leeward = bLeeward;
	Slope.convex = bConvex;
	return static_cast<float>(summit::sim::HumanTriggerProbability(Snowpack->State(), Slope,
		FMath::Max(1, PartySize), bSpacedOut));
}

// ---- Sun -----------------------------------------------------------------------------------------------

void USummitWeatherSubsystem::ComputeSunPosition(double InLatitudeDeg, int32 DayOfYear, double HourOfDay,
	double& OutElevationDeg, double& OutAzimuthDeg)
{
	// Declination: Cooper (1969), delta = 23.44° sin(360° (284 + N) / 365), N = 1-based day of year.
	// Hour angle: 15° per hour from local solar noon (sim time is local solar time, so no equation of time).
	// Elevation and azimuth from the standard spherical-astronomy relations (e.g. Duffie & Beckman,
	// Solar Engineering of Thermal Processes, ch. 1). Accuracy ~1°, ample for lighting.
	const double N = static_cast<double>(DayOfYear + 1);
	const double Decl = FMath::DegreesToRadians(23.44 * FMath::Sin(FMath::DegreesToRadians(360.0 * (284.0 + N) / 365.0)));
	const double Lat = FMath::DegreesToRadians(InLatitudeDeg);
	const double HourAngle = FMath::DegreesToRadians(15.0 * (HourOfDay - 12.0));

	const double SinElev = FMath::Sin(Lat) * FMath::Sin(Decl) + FMath::Cos(Lat) * FMath::Cos(Decl) * FMath::Cos(HourAngle);
	const double Elev = FMath::Asin(FMath::Clamp(SinElev, -1.0, 1.0));

	// Azimuth measured from south (positive west), then shifted to clockwise-from-north.
	const double AzFromSouth = FMath::Atan2(FMath::Sin(HourAngle),
		FMath::Cos(HourAngle) * FMath::Sin(Lat) - FMath::Tan(Decl) * FMath::Cos(Lat));

	OutElevationDeg = FMath::RadiansToDegrees(Elev);
	OutAzimuthDeg = FMath::Fmod(FMath::RadiansToDegrees(AzFromSouth) + 180.0 + 360.0, 360.0);
}

void USummitWeatherSubsystem::GetSunPosition(float& OutElevationDeg, float& OutAzimuthDeg) const
{
	double Elev = 0.0;
	double Az = 0.0;
	const int32 Day = Clock ? Clock->GetDayOfYear() : 140;
	const double Hour = Clock ? static_cast<double>(Clock->GetHourOfDay()) : 12.0;
	ComputeSunPosition(LatitudeDeg, Day, Hour, Elev, Az);
	OutElevationDeg = static_cast<float>(Elev);
	OutAzimuthDeg = static_cast<float>(Az);
}

// ---- Presentation --------------------------------------------------------------------------------------

void USummitWeatherSubsystem::RefreshPlayerWeather()
{
	const APawn* Pawn = UGameplayStatics::GetPlayerPawn(this, 0);
	if (Pawn)
	{
		PlayerWeather = SampleAtLocation(Pawn->GetActorLocation(), 1.f);
	}
	else
	{
		PlayerWeather = SampleAtAltitude(SnowpackReferenceAltitudeM, 1.f);
	}
}

void USummitWeatherSubsystem::FindSceneActors()
{
	SceneLookupCooldownS = kSceneLookupIntervalS;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (!SunActor.IsValid())
	{
		for (TActorIterator<ADirectionalLight> It(World); It; ++It)
		{
			if (It->ActorHasTag(SunActorTag))
			{
				SunActor = *It;
				break;
			}
		}
	}

	if (!FogActor.IsValid())
	{
		AExponentialHeightFog* Fallback = nullptr;
		for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
		{
			if (It->ActorHasTag(FogActorTag))
			{
				FogActor = *It;
				break;
			}
			if (!Fallback)
			{
				Fallback = *It;
			}
		}
		// A level usually has exactly one height fog; use it even if untagged.
		if (!FogActor.IsValid() && Fallback)
		{
			FogActor = Fallback;
		}
		if (FogActor.IsValid() && FogActor->GetComponent())
		{
			AuthoredFogDensity = FogActor->GetComponent()->FogDensity;
		}
	}
}

void USummitWeatherSubsystem::DriveScene()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	float SunElevDeg = 0.f;
	float SunAzDeg = 0.f;
	GetSunPosition(SunElevDeg, SunAzDeg);

	// Sun: a directional light points along the light's travel, i.e. away from the sun. It must be Movable
	// to rotate at runtime; static/stationary suns are left alone rather than spamming warnings.
	if (ADirectionalLight* Sun = SunActor.Get())
	{
		const USceneComponent* Root = Sun->GetRootComponent();
		if (Root && Root->Mobility == EComponentMobility::Movable)
		{
			const float SunYaw = NorthYawDeg + SunAzDeg;
			Sun->SetActorRotation(FRotator(-SunElevDeg, SunYaw + 180.f, 0.f));
		}
	}

	// Fog: map visibility to density on a log scale between the artist's clear-air density and whiteout.
	// Meteorological visibility follows Koschmieder (V = 3.912 / extinction), so extinction and therefore
	// fog density grow as 1/V; a log ramp from 20 km to 50 m gives a perceptually even transition.
	if (AExponentialHeightFog* Fog = FogActor.Get())
	{
		if (UExponentialHeightFogComponent* FogComp = Fog->GetComponent())
		{
			const double Vis = FMath::Clamp(static_cast<double>(PlayerWeather.VisibilityM), 50.0, 20000.0);
			const double T = 1.0 - (FMath::Loge(Vis / 50.0) / FMath::Loge(20000.0 / 50.0));
			const float Whiteout = USummitSettings::Get()->WhiteoutFogDensity;
			FogComp->SetFogDensity(FMath::Lerp(AuthoredFogDensity, Whiteout, static_cast<float>(T * T)));
		}
	}

	// Material parameter collection read by snow/cloud materials and Niagara (via MPC sampling in materials
	// or by Blueprint forwarding). Missing parameters are silently ignored by the instance.
	if (WeatherMPC)
	{
		if (UMaterialParameterCollectionInstance* MPC = World->GetParameterCollectionInstance(WeatherMPC))
		{
			MPC->SetScalarParameterValue(TEXT("WindSpeed"), PlayerWeather.WindSpeedMS);
			MPC->SetScalarParameterValue(TEXT("WindGust"), PlayerWeather.WindGustMS);
			MPC->SetScalarParameterValue(TEXT("WindDirection"), PlayerWeather.WindDirectionDeg);
			// 5 cm/h of snowfall is a heavy Himalayan storm; normalise so 1 = heavy.
			MPC->SetScalarParameterValue(TEXT("SnowIntensity"), FMath::Clamp(PlayerWeather.SnowfallCmH / 5.f, 0.f, 1.f));
			MPC->SetScalarParameterValue(TEXT("CloudCover"), PlayerWeather.CloudCover01);
			MPC->SetScalarParameterValue(TEXT("StormIntensity"), PlayerWeather.StormIntensity01);
			MPC->SetScalarParameterValue(TEXT("Temperature"), PlayerWeather.TemperatureC);
			MPC->SetScalarParameterValue(TEXT("Visibility"), PlayerWeather.VisibilityM);
			MPC->SetScalarParameterValue(TEXT("SunElevation"), SunElevDeg);

			// World-space vector the wind blows TOWARD (meteorological direction is where it comes FROM).
			const float ToYawRad = FMath::DegreesToRadians(NorthYawDeg + PlayerWeather.WindDirectionDeg + 180.f);
			const FLinearColor WindVector(FMath::Cos(ToYawRad) * PlayerWeather.WindSpeedMS,
				FMath::Sin(ToYawRad) * PlayerWeather.WindSpeedMS, 0.f, PlayerWeather.WindGustMS);
			MPC->SetVectorParameterValue(TEXT("WindVector"), WindVector);
		}
	}
}
