#include "Simulation/SummitClimberComponent.h"

#include "Data/SummitPhysicalMaterial.h"
#include "Data/SummitSettings.h"
#include "Data/SummitSimConversions.h"
#include "Simulation/SummitCargoComponent.h"
#include "Summit.h"
#include "World/SummitWeatherSubsystem.h"
#include "World/SummitWorldClockSubsystem.h"

#include "CollisionQueryParams.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include "Sim/Atmosphere.h"
#include "Sim/Climber.h"
#include "Sim/Expedition.h"
#include "Sim/Terrain.h"

namespace
{
	// Below this horizontal speed the climber counts as standing still (resting metabolism, recovery).
	constexpr double kRestSpeedMS = 0.15;

	// Pandolf is validated to roughly +/-25% grade and stays plausible to ~45°; steeper ground is scrambling
	// or climbing. Clamp so a frame of vertical velocity (stepping up a ledge) does not spike the cost.
	constexpr double kMaxGradePercent = 100.0;

	constexpr int32 kConditionCount = static_cast<int32>(summit::sim::Condition::Count);

	float DefaultRoughness(ESummitTerrainType Terrain)
	{
		switch (Terrain)
		{
		case ESummitTerrainType::Boulder: return 0.6f;
		case ESummitTerrainType::Scree: return 0.45f;
		case ESummitTerrainType::DeepSnow: return 0.3f;
		case ESummitTerrainType::HardSnow: return 0.2f;   // sastrugi
		case ESummitTerrainType::GlacierIce: return 0.2f;
		default: return 0.1f;
		}
	}
}

USummitClimberComponent::USummitClimberComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	// Physiology changes over seconds to days; 20 Hz is plenty and keeps stamina responsive for grip.
	PrimaryComponentTick.TickInterval = 0.05f;
}

void USummitClimberComponent::BeginPlay()
{
	Super::BeginPlay();

	summit::sim::ClimberProfile Profile;
	Profile.name = SummitConv::ToStd(ClimberName);
	Profile.physiology.bodyMassKg = BodyMassKg;
	Profile.physiology.vo2MaxSeaLevelMlKgMin = VO2MaxSeaLevel;
	Profile.physiology.amsSusceptibility = AMSSusceptibility;
	Profile.energy.bodyMassKg = BodyMassKg;
	Profile.skillTechnical01 = SkillTechnical01;
	Profile.skillIce01 = SkillIce01;
	Profile.skillRope01 = SkillRope01;
	Climber.Reset(new summit::sim::Climber(Profile));

	summit::sim::PhysiologyState& Physio = Climber->Physio().MutableState();
	Physio.acclimatizedAltitudeM = InitialAcclimatizedAltitudeM;
	Physio.ventilatoryAcclimatization = InitialAcclimatization01;
	Physio.hematologicAcclimatization = InitialAcclimatization01;

	if (AActor* Owner = GetOwner())
	{
		Cargo = Owner->FindComponentByClass<USummitCargoComponent>();
	}

	if (UWorld* World = GetWorld())
	{
		WeatherSubsystem = World->GetSubsystem<USummitWeatherSubsystem>();
		Clock = World->GetSubsystem<USummitWorldClockSubsystem>();
	}
	if (Clock)
	{
		Clock->OnTimeJumpedNative.AddUObject(this, &USummitClimberComponent::HandleClockJump);
	}

	ConditionBands.Init(-1, kConditionCount);

	SampleFloor();
	RefreshVitals();
}

void USummitClimberComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Clock)
	{
		Clock->OnTimeJumpedNative.RemoveAll(this);
	}
	Super::EndPlay(EndPlayReason);
}

void USummitClimberComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!Climber)
	{
		return;
	}

	FloorSampleCooldownS -= DeltaTime;
	if (FloorSampleCooldownS <= 0.f)
	{
		SampleFloor();
		FloorSampleCooldownS = FloorSampleIntervalS;
	}

	// The body lives on game time, not real time.
	double GameSeconds = DeltaTime;
	if (Clock)
	{
		GameSeconds = Clock->IsClockPaused() ? 0.0 : static_cast<double>(DeltaTime) * Clock->GetTimeScale();
	}
	StepSim(GameSeconds);
}

// ---- Sim input assembly --------------------------------------------------------------------------------

void USummitClimberComponent::SampleFloor()
{
	const AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World)
	{
		return;
	}

	const FVector Start = Owner->GetActorLocation();
	const FVector End = Start - FVector(0.0, 0.0, Owner->GetSimpleCollisionHalfHeight() + 60.0);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(SummitFloorSample), /*bTraceComplex*/ false, Owner);
	Params.bReturnPhysicalMaterial = true;

	FHitResult Hit;
	bHasFloor = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);
	if (!bHasFloor)
	{
		return;
	}

	FloorNormal = Hit.ImpactNormal;

	const UPhysicalMaterial* PhysMat = Hit.PhysMaterial.Get();
	if (const USummitPhysicalMaterial* SummitMat = Cast<USummitPhysicalMaterial>(PhysMat))
	{
		FloorTerrain = SummitMat->Terrain;
		FloorSnowDepthM = SummitMat->SnowDepthM;
		FloorRoughness01 = SummitMat->Roughness01;
		return;
	}

	ESummitTerrainType FromSurface = DefaultTerrain;
	if (PhysMat)
	{
		SummitConv::TerrainFromSurfaceType(PhysMat->SurfaceType.GetValue(), FromSurface);
	}
	FloorTerrain = FromSurface;
	FloorRoughness01 = DefaultRoughness(FloorTerrain);
	FloorSnowDepthM = FloorTerrain == ESummitTerrainType::DeepSnow ? 0.5f
		: (FloorTerrain == ESummitTerrainType::SoftSnow ? 0.15f : 0.f);
}

void USummitClimberComponent::BuildEnvironment(summit::sim::ClimberEnvironment& OutEnv) const
{
	const USummitSettings* Settings = USummitSettings::Get();
	const AActor* Owner = GetOwner();

	OutEnv.altitudeM = Owner ? Settings->WorldZToAltitudeM(Owner->GetActorLocation().Z) : 0.0;
	OutEnv.latitudeDeg = Settings->LatitudeDeg;
	OutEnv.dayOfYear = Clock ? Clock->GetDayOfYear() : 140;
	OutEnv.exposure01 = WindExposure01;
	if (WeatherSubsystem)
	{
		WeatherSubsystem->SampleSim(OutEnv.altitudeM, OutEnv.exposure01, OutEnv.weather);
	}
	OutEnv.terrain = SummitConv::ToSim(FloorTerrain);
	OutEnv.snowDepthM = FloorSnowDepthM;
	OutEnv.inTent = bInTent;
	OutEnv.inSleepingBag = bInSleepingBag;
	OutEnv.sleepingBagComfortC = SleepingBagComfortC;
}

void USummitClimberComponent::BuildAction(summit::sim::ClimberAction& OutAction, bool bForMovementQuery) const
{
	using summit::sim::Activity;

	const AActor* Owner = GetOwner();
	const FVector Velocity = Owner ? Owner->GetVelocity() : FVector::ZeroVector;
	const double HorizontalMS = SummitConv::CmsToMs(Velocity.Size2D());
	const double VerticalMS = SummitConv::CmsToMs(Velocity.Z);

	// Grade from motion when moving; from the floor slope along the facing direction when standing (so the
	// speed limit shown before the first step is already right for the hill ahead).
	double GradePercent = 0.0;
	if (HorizontalMS > 0.2)
	{
		GradePercent = VerticalMS / HorizontalMS * 100.0;
	}
	else if (Owner && bHasFloor && FloorNormal.Z > KINDA_SMALL_NUMBER)
	{
		const FVector Forward = Owner->GetActorForwardVector().GetSafeNormal2D();
		GradePercent = -FVector::DotProduct(FloorNormal, Forward) / FloorNormal.Z * 100.0;
	}
	GradePercent = FMath::Clamp(GradePercent, -kMaxGradePercent, kMaxGradePercent);

	OutAction.speedMS = HorizontalMS;
	OutAction.gradePercent = GradePercent;

	switch (ClimbMode)
	{
	case ESummitClimbMode::Walking:
	case ESummitClimbMode::Scrambling:
		if (bSleeping && !bForMovementQuery)
		{
			OutAction.activity = Activity::Sleeping;
		}
		else if (HorizontalMS < kRestSpeedMS && !bForMovementQuery)
		{
			OutAction.activity = Activity::Resting;
		}
		else if (FloorTerrain == ESummitTerrainType::DeepSnow)
		{
			OutAction.activity = Activity::BreakingTrail;
		}
		else
		{
			OutAction.activity = Activity::Walking;
		}
		break;

	case ESummitClimbMode::RockClimbing:
	case ESummitClimbMode::IceClimbing:
		OutAction.activity = Activity::Climbing;
		OutAction.speedMS = SummitConv::CmsToMs(Velocity.Size());
		OutAction.gradePercent = 0.0;   // vertical work is costed by the Climbing activity itself
		break;

	case ESummitClimbMode::Jumaring:
		OutAction.activity = Activity::Jumaring;
		OutAction.speedMS = SummitConv::CmsToMs(Velocity.Size());
		break;

	case ESummitClimbMode::Rappelling:
		OutAction.activity = Activity::Rappelling;
		OutAction.speedMS = SummitConv::CmsToMs(Velocity.Size());
		break;

	case ESummitClimbMode::Resting:
		OutAction.activity = bSleeping ? Activity::Sleeping : Activity::Resting;
		OutAction.speedMS = 0.0;
		break;

	case ESummitClimbMode::Sliding:
	case ESummitClimbMode::Fallen:
	default:
		// Not locomotion the sim models; treat as resting metabolism (the fall itself is resolved elsewhere).
		OutAction.activity = bForMovementQuery ? Activity::Walking : Activity::Resting;
		OutAction.speedMS = 0.0;
		break;
	}
}

void USummitClimberComponent::BuildGear(summit::sim::ClimberGear& OutGear) const
{
	OutGear.clothing = SummitConv::ToSim(Clothing);
	OutGear.oxygen.maskOn = bMaskOn;
	OutGear.oxygen.flowLitersPerMin = (bMaskOn && OxygenRemainingL > 0.f) ? OxygenFlowLMin : 0.0;
	OutGear.meds.acetazolamide = bAcetazolamide;
	OutGear.meds.dexamethasone = bDexamethasone;
	OutGear.meds.nifedipine = bNifedipine;
	OutGear.crampons = bCrampons;
	OutGear.glacierGoggles = bGlacierGoggles;
	OutGear.packMassKg = Cargo ? Cargo->GetTotalMassKg() : 0.0;
}

double USummitClimberComponent::GetPressureKPa() const
{
	const USummitSettings* Settings = USummitSettings::Get();
	const int32 Day = Clock ? Clock->GetDayOfYear() : 140;
	return summit::sim::BarometricPressureKPa(Vitals.CurrentAltitudeM, Settings->LatitudeDeg, Day);
}

// ---- Stepping ------------------------------------------------------------------------------------------

void USummitClimberComponent::StepSim(double Seconds)
{
	if (!Climber)
	{
		return;
	}

	if (Seconds > 0.0 && !Climber->IsDead())
	{
		summit::sim::ClimberEnvironment Env;
		summit::sim::ClimberAction Action;
		summit::sim::ClimberGear Gear;
		BuildEnvironment(Env);
		BuildAction(Action, /*bForMovementQuery*/ false);
		BuildGear(Gear);

		Climber->Step(Seconds, Env, Action, Gear);
		ConsumeOxygen(Seconds);
	}

	RefreshVitals();
	DispatchConditionEvents();

	if (Climber->IsDead() && !bDeathBroadcast)
	{
		bDeathBroadcast = true;
		UE_LOG(LogSummit, Log, TEXT("%s died (cause %d) at %.0f m"), *ClimberName,
			static_cast<int32>(Climber->Cause()), Vitals.CurrentAltitudeM);
		OnDeath.Broadcast(SummitConv::FromSim(Climber->Cause()));
	}
}

void USummitClimberComponent::IntegrateSkippedTime(double Seconds)
{
	// Climber::Step sub-steps internally (<= 60 s), so a whole night is one call. Movement is zero during
	// a skip; the action builder sees zero velocity and picks Resting/Sleeping.
	StepSim(Seconds);
}

void USummitClimberComponent::HandleClockJump(double /*FromSimSeconds*/, double JumpSeconds)
{
	SampleFloor();
	IntegrateSkippedTime(JumpSeconds);
}

void USummitClimberComponent::ConsumeOxygen(double Seconds)
{
	if (!bMaskOn || OxygenFlowLMin <= 0.f || OxygenRemainingL <= 0.f)
	{
		return;
	}
	OxygenRemainingL = FMath::Max(0.f, OxygenRemainingL - static_cast<float>(OxygenFlowLMin * Seconds / 60.0));
	if (OxygenRemainingL <= 0.f)
	{
		OnOxygenEmpty.Broadcast();
	}
}

void USummitClimberComponent::RefreshVitals()
{
	if (!Climber)
	{
		return;
	}

	summit::sim::ClimberEnvironment Env;
	summit::sim::ClimberAction MoveAction;
	summit::sim::ClimberGear Gear;
	BuildEnvironment(Env);
	BuildAction(MoveAction, /*bForMovementQuery*/ true);
	BuildGear(Gear);

	const summit::sim::PhysiologyState& P = Climber->Physio().State();
	const summit::sim::ThermalState& T = Climber->Thermal().State();
	const summit::sim::EnergyState& E = Climber->Energy().State();

	constexpr int32 Hands = static_cast<int32>(summit::sim::BodyPart::Hands);
	constexpr int32 Feet = static_cast<int32>(summit::sim::BodyPart::Feet);
	constexpr int32 Face = static_cast<int32>(summit::sim::BodyPart::Face);

	Vitals.CurrentAltitudeM = static_cast<float>(Env.altitudeM);
	const double PressureKPa = GetPressureKPa();

	Vitals.SpO2 = static_cast<float>(P.spo2);
	Vitals.CoreTempC = static_cast<float>(T.coreTempC);
	Vitals.HandsTempC = static_cast<float>(T.extremityTempC[Hands]);
	Vitals.FeetTempC = static_cast<float>(T.extremityTempC[Feet]);
	Vitals.FaceTempC = static_cast<float>(T.extremityTempC[Face]);
	Vitals.HandsFrostbite01 = static_cast<float>(T.frostbiteDamage01[Hands]);
	Vitals.FeetFrostbite01 = static_cast<float>(T.frostbiteDamage01[Feet]);
	Vitals.FaceFrostbite01 = static_cast<float>(T.frostbiteDamage01[Face]);
	Vitals.Shivering01 = static_cast<float>(T.shivering01);
	Vitals.AMSScore = static_cast<float>(P.amsScore);
	Vitals.HAPESeverity01 = static_cast<float>(P.hapeSeverity01);
	Vitals.HACESeverity01 = static_cast<float>(P.haceSeverity01);
	Vitals.HydrationDeficitL = static_cast<float>(E.hydrationDeficitL);
	Vitals.GlycogenKcal = static_cast<float>(E.glycogenKcal);
	const double GlycogenCapacity = Climber->Profile().energy.glycogenCapacityKcal;
	Vitals.Glycogen01 = GlycogenCapacity > 0.0 ? static_cast<float>(E.glycogenKcal / GlycogenCapacity) : 0.f;
	Vitals.Fatigue01 = static_cast<float>(E.fatigue01);
	Vitals.Stamina01 = static_cast<float>(E.shortTermStamina01);
	Vitals.CognitiveImpairment01 = static_cast<float>(Climber->Physio().CognitiveImpairment01());
	Vitals.EquivalentAltitudeM = static_cast<float>(Climber->Physio().EquivalentAltitudeM(PressureKPa));
	Vitals.AcclimatizedAltitudeM = static_cast<float>(P.acclimatizedAltitudeM);
	Vitals.MetabolicRateW = static_cast<float>(Climber->LastMetabolicRateW());
	Vitals.Exertion01 = static_cast<float>(Climber->LastExertion01());
	Vitals.MaxSustainableSpeedMS = static_cast<float>(Climber->MaxSustainableSpeedMS(Env, MoveAction, Gear));
	Vitals.bIncapacitated = Climber->IsIncapacitated();
	Vitals.bDead = Climber->IsDead();
	Vitals.CauseOfDeath = SummitConv::FromSim(Climber->Cause());

	LocalWeather = SummitConv::FromSim(Env.weather, Env.altitudeM, PressureKPa);
}

void USummitClimberComponent::DispatchConditionEvents()
{
	if (!Climber)
	{
		return;
	}

	TArray<float> Severity;
	Severity.Init(-1.f, kConditionCount);

	Vitals.Conditions.Reset();
	const std::vector<summit::sim::ConditionReport> Reports = Climber->ActiveConditions();
	for (const summit::sim::ConditionReport& Report : Reports)
	{
		const int32 Index = static_cast<int32>(Report.condition);
		if (Index < 0 || Index >= kConditionCount)
		{
			continue;
		}
		Severity[Index] = static_cast<float>(FMath::Clamp(Report.severity01, 0.0, 1.0));

		FSummitConditionEntry& Entry = Vitals.Conditions.AddDefaulted_GetRef();
		Entry.Condition = SummitConv::FromSim(Report.condition);
		Entry.Severity01 = Severity[Index];
	}

	if (ConditionBands.Num() != kConditionCount)
	{
		ConditionBands.Init(-1, kConditionCount);
	}

	// Quantise to 25% bands so UI/audio cues fire on meaningful changes, not every tick.
	for (int32 Index = 0; Index < kConditionCount; ++Index)
	{
		const int32 NewBand = Severity[Index] < 0.f ? -1 : FMath::Min(3, FMath::FloorToInt32(Severity[Index] * 4.f));
		if (NewBand != ConditionBands[Index])
		{
			ConditionBands[Index] = NewBand;
			OnConditionChanged.Broadcast(static_cast<ESummitCondition>(Index), FMath::Max(0.f, Severity[Index]), NewBand >= 0);
		}
	}
}

// ---- Queries -------------------------------------------------------------------------------------------

float USummitClimberComponent::GetMaxSustainableSpeedCmS() const
{
	return static_cast<float>(SummitConv::MsToCms(Vitals.MaxSustainableSpeedMS));
}

float USummitClimberComponent::GetStamina01() const
{
	return Climber ? static_cast<float>(Climber->Energy().State().shortTermStamina01) : 1.f;
}

float USummitClimberComponent::GetFloorFriction() const
{
	return static_cast<float>(summit::sim::SurfaceFriction(SummitConv::ToSim(FloorTerrain), bCrampons, LocalWeather.TemperatureC));
}

float USummitClimberComponent::GetMaxWalkableSlopeDeg() const
{
	return static_cast<float>(summit::sim::MaxWalkableSlopeDeg(SummitConv::ToSim(FloorTerrain), bCrampons));
}

float USummitClimberComponent::GetOxygenHoursRemaining() const
{
	const float Flow = FMath::Max(OxygenFlowLMin, 0.1f);
	return OxygenRemainingL / (Flow * 60.f);
}

// ---- Actions -------------------------------------------------------------------------------------------

void USummitClimberComponent::Eat(float Kcal)
{
	if (Climber && Kcal > 0.f)
	{
		Climber->Energy().Eat(Kcal);
	}
}

void USummitClimberComponent::Drink(float Liters)
{
	if (Climber && Liters > 0.f)
	{
		Climber->Energy().Drink(Liters);
	}
}

float USummitClimberComponent::EatFromPack(float Kcal)
{
	const float Got = Cargo ? Cargo->ConsumeFood(Kcal) : 0.f;
	Eat(Got);
	return Got;
}

float USummitClimberComponent::DrinkFromPack(float Liters)
{
	const float Got = Cargo ? Cargo->ConsumeWater(Liters) : 0.f;
	Drink(Got);
	return Got;
}

void USummitClimberComponent::SetOxygenFlow(float LitersPerMinute)
{
	// Regulators on Himalayan sets go to ~4 L/min (some to 6-8 for emergencies).
	OxygenFlowLMin = FMath::Clamp(LitersPerMinute, 0.f, 8.f);
}

void USummitClimberComponent::ToggleMask()
{
	bMaskOn = !bMaskOn;
}

void USummitClimberComponent::AddOxygenBottle(float WaterVolumeL, float FillPressureBar)
{
	OxygenRemainingL += static_cast<float>(summit::sim::OxygenBottleLiters(WaterVolumeL, FillPressureBar));
}

bool USummitClimberComponent::ConsumeStamina(float Amount01)
{
	if (!Climber)
	{
		return true;
	}
	double& Stamina = Climber->Energy().MutableState().shortTermStamina01;
	if (Stamina >= Amount01)
	{
		Stamina -= Amount01;
		return true;
	}
	Stamina = 0.0;
	return false;
}

void USummitClimberComponent::KillClimber(ESummitCauseOfDeath Cause)
{
	if (!Climber || Cause == ESummitCauseOfDeath::None || Climber->IsDead())
	{
		return;
	}
	Climber->Kill(static_cast<summit::sim::CauseOfDeath>(static_cast<uint8>(Cause)));
	StepSim(0.0);   // refresh vitals and broadcast OnDeath
}
