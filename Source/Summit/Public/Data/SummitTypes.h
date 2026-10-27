// Blueprint-facing mirrors of the simulation core's enums and state.
//
// The sim (Source/SummitSim/Public/Sim) is pure C++17 in namespace summit::sim and is not visible to
// UHT/Blueprint. These types are the reflected surface the rest of the game (AnimBPs, UMG, data assets)
// works with. Conversion to and from the sim types lives in Private/Data/SummitSimConversions.h so sim
// headers never leak into public gameplay headers.
//
// Units: everything here is in the sim's SI units (metres, m/s, kg, °C) unless the name ends in Cm,
// because these values are read by UI and designers, who think in metres and °C. Unreal-space vectors
// (attach offsets, IK targets) stay in centimetres.

#pragma once

#include "CoreMinimal.h"
#include "SummitTypes.generated.h"

// Mirrors summit::sim::TerrainType (same order; values are cast directly, so keep them in sync).
// Physical surface types SurfaceType1..SurfaceType11 in DefaultEngine.ini use the same order + 1.
UENUM(BlueprintType)
enum class ESummitTerrainType : uint8
{
	Trail,
	Grass,
	Scree,
	Boulder,
	Rock,
	WetRock,
	Verglas,
	HardSnow,
	SoftSnow,
	DeepSnow,
	GlacierIce
};

// Mirrors summit::sim::Activity.
UENUM(BlueprintType)
enum class ESummitActivity : uint8
{
	Resting,
	Sleeping,
	Walking,
	Climbing,
	Jumaring,
	Rappelling,
	BreakingTrail
};

// Mirrors summit::sim::Condition.
UENUM(BlueprintType)
enum class ESummitCondition : uint8
{
	Hypoxia,
	AMS,
	HAPE,
	HACE,
	Hypothermia,
	Frostbite,
	Dehydration,
	Exhaustion,
	Starvation,
	SnowBlindness
};

// Mirrors summit::sim::CauseOfDeath.
UENUM(BlueprintType)
enum class ESummitCauseOfDeath : uint8
{
	None,
	Hypothermia,
	HAPE,
	HACE,
	Exhaustion,
	Fall,
	Avalanche,
	Other
};

// Mirrors summit::sim::DangerLevel (EAWS 1..5). Index 0 is unused so the numeric value matches the scale.
UENUM(BlueprintType)
enum class ESummitAvalancheDanger : uint8
{
	None = 0 UMETA(Hidden),
	Low = 1,
	Moderate = 2,
	Considerable = 3,
	High = 4,
	VeryHigh = 5
};

// Mirrors summit::sim::AnchorType.
UENUM(BlueprintType)
enum class ESummitAnchorType : uint8
{
	IceScrew,
	SnowPicket,
	Deadman,
	Cam,
	Nut,
	Piton,
	Bolt,
	Sling,
	FixedLine
};

// Gameplay locomotion state. Not a sim enum: it decides which sim Activity is fed to the Climber and
// which CharacterMovement mode (walking vs. MOVE_Custom sub-mode) is active.
UENUM(BlueprintType)
enum class ESummitClimbMode : uint8
{
	Walking,       // upright on the ground (trail, glacier, snow slopes below the walkable limit)
	Scrambling,    // hands down on steep ground, still using walking physics but slower
	RockClimbing,  // on the wall, hands and feet on holds
	IceClimbing,   // on the wall with axes and crampons
	Jumaring,      // ascending a fixed rope with an ascender
	Rappelling,    // descending a fixed rope
	Sliding,       // uncontrolled slide on steep snow/ice; self-arrest window
	Fallen,        // lost balance, on the ground; must recover
	Resting        // sitting/standing still to recover (in camp, on a ledge)
};

// Mode groupings shared by the character, movement and climbing code (non-reflected helpers).
namespace SummitClimbModes
{
	inline bool IsOnWall(ESummitClimbMode Mode)
	{
		return Mode == ESummitClimbMode::RockClimbing || Mode == ESummitClimbMode::IceClimbing;
	}

	inline bool IsOnRope(ESummitClimbMode Mode)
	{
		return Mode == ESummitClimbMode::Jumaring || Mode == ESummitClimbMode::Rappelling;
	}

	inline bool IsUpright(ESummitClimbMode Mode)
	{
		return Mode == ESummitClimbMode::Walking || Mode == ESummitClimbMode::Scrambling;
	}
}

// One active medical condition and its severity, for UI and events.
USTRUCT(BlueprintType)
struct SUMMIT_API FSummitConditionEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Vitals")
	ESummitCondition Condition = ESummitCondition::Hypoxia;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Vitals", meta = (ClampMin = "0", ClampMax = "1"))
	float Severity01 = 0.f;
};

// Snapshot of the climber's physiology for UI, animation and gameplay checks.
USTRUCT(BlueprintType)
struct SUMMIT_API FSummitVitals
{
	GENERATED_BODY()

	// Arterial oxygen saturation, 0..1 (0.98 at sea level, ~0.55-0.70 on Everest without O2).
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float SpO2 = 0.98f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float CoreTempC = 37.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float HandsTempC = 30.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float FeetTempC = 30.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float FaceTempC = 32.f;

	// Permanent tissue damage 0..1 per extremity.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float HandsFrostbite01 = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float FeetFrostbite01 = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float FaceFrostbite01 = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float Shivering01 = 0.f;

	// Lake Louise score 0..12; >= 3 with headache is AMS.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float AMSScore = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float HAPESeverity01 = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float HACESeverity01 = 0.f;

	// Litres of body water missing (> 0 means dehydrated).
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float HydrationDeficitL = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float GlycogenKcal = 2000.f;

	// Glycogen as a fraction of capacity, convenient for UI bars.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float Glycogen01 = 1.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float Fatigue01 = 0.f;

	// Short-term anaerobic reserve (grip, hard moves); recovers in minutes.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float Stamina01 = 1.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float CognitiveImpairment01 = 0.f;

	// Photokeratitis from UV without goggles; blurs and whites out vision.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float SnowBlindness01 = 0.f;

	// Kcal the climber could still eat right now; appetite collapses at altitude.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float RemainingAppetiteKcal = 0.f;

	// Altitude the climber "feels" given supplemental O2 (pressure-equivalent).
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float EquivalentAltitudeM = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float CurrentAltitudeM = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float AcclimatizedAltitudeM = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float MetabolicRateW = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float Exertion01 = 0.f;

	// Highest along-ground speed the climber can hold for hours in the current situation (m/s).
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	float MaxSustainableSpeedMS = 1.5f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	bool bIncapacitated = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	bool bDead = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	ESummitCauseOfDeath CauseOfDeath = ESummitCauseOfDeath::None;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Vitals")
	TArray<FSummitConditionEntry> Conditions;
};

// A piece of cargo on the climber's pack (or in hand). Mirrors summit::sim::CargoItem plus inventory data.
USTRUCT(BlueprintType)
struct SUMMIT_API FSummitCargoItem
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Cargo")
	FName Id;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Cargo")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Cargo", meta = (ClampMin = "0", Units = "Kilograms"))
	float MassKg = 1.f;

	// Offset from the pack attachment point (upper back) in actor-local Unreal space, centimetres
	// (X forward, Y right, Z up). Packs sit at X < 0. Converted to the sim's (x right, y forward) metres.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Cargo")
	FVector AttachOffsetCm = FVector(-20.0, 0.0, 0.0);

	// Unstrapped cargo sways and amplifies balance disturbances.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Cargo")
	bool bStrapped = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Cargo", meta = (ClampMin = "0"))
	float VolumeL = 1.f;

	// Optional consumable payload: food energy and water, consumed by Eat/Drink from the inventory.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Cargo", meta = (ClampMin = "0"))
	float FoodKcal = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Cargo", meta = (ClampMin = "0"))
	float WaterL = 0.f;
};

// Clothing layer system. Mirrors summit::sim::ClothingSystem.
USTRUCT(BlueprintType)
struct SUMMIT_API FSummitClothing
{
	GENERATED_BODY()

	// 1 clo = 0.155 m²K/W. Softshell ~1.5, down suit ~6-8.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Clothing", meta = (ClampMin = "0"))
	float TorsoClo = 4.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Clothing", meta = (ClampMin = "0"))
	float HandsClo = 2.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Clothing", meta = (ClampMin = "0"))
	float FeetClo = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Clothing", meta = (ClampMin = "0", ClampMax = "1"))
	float FaceCoverage01 = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Clothing", meta = (ClampMin = "0", ClampMax = "1"))
	float Windproof01 = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Clothing", meta = (ClampMin = "0", ClampMax = "1"))
	float Wetness01 = 0.f;
};

// Weather at a point. Mirrors summit::sim::WeatherSample plus derived values for UI.
USTRUCT(BlueprintType)
struct SUMMIT_API FSummitWeatherSnapshot
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float AltitudeM = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float TemperatureC = 0.f;

	// Wind chill (JAG/TI) for the sampled wind; what bare skin feels.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float WindChillC = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float PressureKPa = 101.325f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float WindSpeedMS = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float WindGustMS = 0.f;

	// Meteorological convention: direction the wind blows FROM, degrees clockwise from north.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float WindDirectionDeg = 270.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float VisibilityM = 20000.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float PrecipitationMmH = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float SnowfallCmH = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float CloudCover01 = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float RelativeHumidity01 = 0.5f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float SolarWm2 = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float StormIntensity01 = 0.f;
};

// One line of the radio forecast.
USTRUCT(BlueprintType)
struct SUMMIT_API FSummitForecastEntry
{
	GENERATED_BODY()

	// Hours after the forecast was issued.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float HoursAhead = 0.f;

	// Absolute sim time of this entry, hours since 1 January 00:00.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float SimTimeHours = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	FSummitWeatherSnapshot Expected;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Summit|Weather")
	float Confidence01 = 1.f;
};

// A fixed-rope or protection anchor. Mirrors summit::sim::Anchor plus its position on the rope.
USTRUCT(BlueprintType)
struct SUMMIT_API FSummitAnchor
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope")
	ESummitAnchorType AnchorType = ESummitAnchorType::FixedLine;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope", meta = (ClampMin = "0"))
	float RatedStrengthKN = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope", meta = (ClampMin = "0", ClampMax = "1"))
	float PlacementQuality01 = 0.8f;

	// Days since placement. Old fixed lines melt out and UV-degrade.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope", meta = (ClampMin = "0"))
	float AgeDays = 0.f;

	// Where along the rope spline the anchor sits (cm from the spline start).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Rope", meta = (ClampMin = "0"))
	float DistanceAlongRopeCm = 0.f;
};
