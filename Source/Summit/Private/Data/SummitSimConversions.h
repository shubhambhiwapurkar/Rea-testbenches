// Boundary between Unreal types and the summit::sim core.
//
// PRIVATE header: include it only from .cpp files. It pulls in the sim headers (std::vector, std::string)
// that we deliberately keep out of reflected public headers.
//
// All unit conversion between Unreal (centimetres, cm/s, X-forward/Y-right) and the sim (metres, m/s,
// x-right/y-forward) happens here so gameplay code never mixes them.

#pragma once

#include "CoreMinimal.h"
#include "Data/SummitTypes.h"
#include "Engine/EngineTypes.h"   // EPhysicalSurface

#include "Sim/Atmosphere.h"
#include "Sim/Avalanche.h"
#include "Sim/Balance.h"
#include "Sim/Climber.h"
#include "Sim/Rope.h"
#include "Sim/Terrain.h"
#include "Sim/Thermal.h"
#include "Sim/Weather.h"

#include <string>

namespace SummitConv
{
	// ---------------------------------------------------------------- units

	inline constexpr double CmPerM = 100.0;

	inline double CmToM(double Cm) { return Cm / CmPerM; }
	inline double MToCm(double M) { return M * CmPerM; }
	inline double CmsToMs(double CmS) { return CmS / CmPerM; }
	inline double MsToCms(double MS) { return MS * CmPerM; }

	// Unreal actor-local (X forward, Y right, Z up; cm) -> sim body frame (x right, y forward, z up; m).
	inline summit::sim::Vec3 LocalCmToSimM(const FVector& LocalCm)
	{
		return summit::sim::Vec3(CmToM(LocalCm.Y), CmToM(LocalCm.X), CmToM(LocalCm.Z));
	}

	inline FVector SimMToLocalCm(const summit::sim::Vec3& V)
	{
		return FVector(MToCm(V.y), MToCm(V.x), MToCm(V.z));
	}

	// ---------------------------------------------------------------- strings

	inline std::string ToStd(const FString& S)
	{
		const FTCHARToUTF8 Conv(*S);
		return std::string(Conv.Get(), static_cast<size_t>(Conv.Length()));
	}

	inline FString FromStd(const std::string& S)
	{
		return FString(UTF8_TO_TCHAR(S.c_str()));
	}

	// ---------------------------------------------------------------- enums
	// The UENUMs mirror the sim enums value-for-value, so a cast is enough. The static_asserts catch drift
	// if someone appends to a sim enum without updating SummitTypes.h.

	static_assert(static_cast<int>(summit::sim::TerrainType::Count) == static_cast<int>(ESummitTerrainType::GlacierIce) + 1,
		"ESummitTerrainType is out of sync with summit::sim::TerrainType");
	static_assert(static_cast<int>(summit::sim::Condition::Count) == static_cast<int>(ESummitCondition::SnowBlindness) + 1,
		"ESummitCondition is out of sync with summit::sim::Condition");
	static_assert(static_cast<int>(summit::sim::AnchorType::Count) == static_cast<int>(ESummitAnchorType::FixedLine) + 1,
		"ESummitAnchorType is out of sync with summit::sim::AnchorType");

	inline summit::sim::TerrainType ToSim(ESummitTerrainType T) { return static_cast<summit::sim::TerrainType>(static_cast<uint8>(T)); }
	inline ESummitTerrainType FromSim(summit::sim::TerrainType T) { return static_cast<ESummitTerrainType>(static_cast<uint8>(T)); }

	inline summit::sim::Activity ToSim(ESummitActivity A) { return static_cast<summit::sim::Activity>(static_cast<uint8>(A)); }
	inline ESummitActivity FromSim(summit::sim::Activity A) { return static_cast<ESummitActivity>(static_cast<uint8>(A)); }

	inline ESummitCondition FromSim(summit::sim::Condition C) { return static_cast<ESummitCondition>(static_cast<uint8>(C)); }

	inline ESummitCauseOfDeath FromSim(summit::sim::CauseOfDeath C) { return static_cast<ESummitCauseOfDeath>(static_cast<uint8>(C)); }

	inline ESummitAvalancheDanger FromSim(summit::sim::DangerLevel D) { return static_cast<ESummitAvalancheDanger>(static_cast<uint8>(D)); }

	inline summit::sim::AnchorType ToSim(ESummitAnchorType A) { return static_cast<summit::sim::AnchorType>(static_cast<uint8>(A)); }

	// Physical surface fallback: SurfaceType1..11 are configured in DefaultEngine.ini in TerrainType order.
	inline bool TerrainFromSurfaceType(EPhysicalSurface Surface, ESummitTerrainType& OutTerrain)
	{
		const int32 Index = static_cast<int32>(Surface) - 1;
		if (Index >= 0 && Index < static_cast<int32>(summit::sim::TerrainType::Count))
		{
			OutTerrain = static_cast<ESummitTerrainType>(Index);
			return true;
		}
		return false;
	}

	// ---------------------------------------------------------------- structs

	inline summit::sim::ClothingSystem ToSim(const FSummitClothing& C)
	{
		summit::sim::ClothingSystem Out;
		Out.torsoClo = C.TorsoClo;
		Out.handsClo = C.HandsClo;
		Out.feetClo = C.FeetClo;
		Out.faceCoverage01 = C.FaceCoverage01;
		Out.windproof01 = C.Windproof01;
		Out.wetness01 = C.Wetness01;
		return Out;
	}

	inline summit::sim::CargoItem ToSim(const FSummitCargoItem& Item)
	{
		summit::sim::CargoItem Out;
		Out.id = ToStd(Item.Id.ToString());
		Out.massKg = Item.MassKg;
		Out.offsetM = LocalCmToSimM(Item.AttachOffsetCm);
		Out.strapped = Item.bStrapped;
		return Out;
	}

	inline summit::sim::Anchor ToSim(const FSummitAnchor& A)
	{
		summit::sim::Anchor Out;
		Out.type = ToSim(A.AnchorType);
		Out.ratedStrengthKN = A.RatedStrengthKN;
		Out.placementQuality01 = A.PlacementQuality01;
		Out.ageDays = A.AgeDays;
		return Out;
	}

	inline FSummitWeatherSnapshot FromSim(const summit::sim::WeatherSample& W, double AltitudeM, double PressureKPa)
	{
		FSummitWeatherSnapshot Out;
		Out.AltitudeM = static_cast<float>(AltitudeM);
		Out.TemperatureC = static_cast<float>(W.temperatureC);
		Out.WindChillC = static_cast<float>(summit::sim::WindChillC(W.temperatureC, W.windSpeedMS * 3.6));
		Out.PressureKPa = static_cast<float>(PressureKPa);
		Out.WindSpeedMS = static_cast<float>(W.windSpeedMS);
		Out.WindGustMS = static_cast<float>(W.windGustMS);
		Out.WindDirectionDeg = static_cast<float>(W.windDirectionDeg);
		Out.VisibilityM = static_cast<float>(W.visibilityM);
		Out.PrecipitationMmH = static_cast<float>(W.precipitationMmH);
		Out.SnowfallCmH = static_cast<float>(W.snowfallCmH);
		Out.CloudCover01 = static_cast<float>(W.cloudCover01);
		Out.RelativeHumidity01 = static_cast<float>(W.relativeHumidity01);
		Out.SolarWm2 = static_cast<float>(W.solarWm2);
		Out.StormIntensity01 = static_cast<float>(W.stormIntensity01);
		return Out;
	}

	// Inverse of the above, used when the weather subsystem is absent and a snapshot was authored by hand.
	inline summit::sim::WeatherSample ToSim(const FSummitWeatherSnapshot& S)
	{
		summit::sim::WeatherSample Out;
		Out.temperatureC = S.TemperatureC;
		Out.windSpeedMS = S.WindSpeedMS;
		Out.windGustMS = S.WindGustMS;
		Out.windDirectionDeg = S.WindDirectionDeg;
		Out.visibilityM = S.VisibilityM;
		Out.precipitationMmH = S.PrecipitationMmH;
		Out.snowfallCmH = S.SnowfallCmH;
		Out.cloudCover01 = S.CloudCover01;
		Out.relativeHumidity01 = S.RelativeHumidity01;
		Out.solarWm2 = S.SolarWm2;
		Out.stormIntensity01 = S.StormIntensity01;
		return Out;
	}
}
