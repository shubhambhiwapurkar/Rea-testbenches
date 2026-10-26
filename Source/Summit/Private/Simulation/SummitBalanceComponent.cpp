#include "Simulation/SummitBalanceComponent.h"

#include "Data/SummitSimConversions.h"
#include "Simulation/SummitCargoComponent.h"
#include "Simulation/SummitClimberComponent.h"
#include "World/SummitWeatherSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include "Sim/Atmosphere.h"
#include "Sim/Balance.h"

#include <vector>

USummitBalanceComponent::USummitBalanceComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	// Every frame: the pendulum has sub-second dynamics and lean input must feel immediate.
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void USummitBalanceComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	if (Owner)
	{
		Cargo = Owner->FindComponentByClass<USummitCargoComponent>();
		ClimberComp = Owner->FindComponentByClass<USummitClimberComponent>();
	}
	if (UWorld* World = GetWorld())
	{
		WeatherSubsystem = World->GetSubsystem<USummitWeatherSubsystem>();
	}

	summit::sim::BodyGeometry Body;
	Body.bodyMassKg = ClimberComp ? ClimberComp->BodyMassKg : 75.0;
	Body.heightM = HeightM;
	// Scale CoM and pack attachment with height (anthropometric ratios ~0.56 and ~0.70 of stature).
	Body.comHeightM = HeightM * 0.56;
	Body.packAttachHeightM = HeightM * 0.70;
	Body.stanceHalfWidthM = StanceHalfWidthM;
	Balance.Reset(new summit::sim::BalanceModel(Body, static_cast<std::uint64_t>(static_cast<uint32>(Seed))));

	if (Cargo)
	{
		Cargo->OnCargoChanged.AddDynamic(this, &USummitBalanceComponent::HandleCargoChanged);
	}
	PushCargo();
}

void USummitBalanceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Cargo)
	{
		Cargo->OnCargoChanged.RemoveDynamic(this, &USummitBalanceComponent::HandleCargoChanged);
	}
	Super::EndPlay(EndPlayReason);
}

void USummitBalanceComponent::HandleCargoChanged()
{
	PushCargo();
}

void USummitBalanceComponent::PushCargo()
{
	if (!Balance)
	{
		return;
	}

	std::vector<summit::sim::CargoItem> SimCargo;
	if (Cargo)
	{
		SimCargo.reserve(static_cast<size_t>(Cargo->Items.Num()) + 1);

		// The empty pack itself sits on the back at the attachment point.
		summit::sim::CargoItem Pack;
		Pack.id = "pack";
		Pack.massKg = Cargo->EmptyPackMassKg;
		Pack.offsetM = SummitConv::LocalCmToSimM(FVector(-15.0, 0.0, 0.0));
		SimCargo.push_back(Pack);

		for (const FSummitCargoItem& Item : Cargo->Items)
		{
			SimCargo.push_back(SummitConv::ToSim(Item));
		}
	}
	Balance->SetCargo(SimCargo);

	CargoLateralBiasDeg = static_cast<float>(FMath::RadiansToDegrees(Balance->CargoLateralBiasRad()));
	CombinedCoMCm = SummitConv::SimMToLocalCm(Balance->CombinedCoM());
}

void USummitBalanceComponent::ComputeSlope(const FVector& TravelDir2D, const FVector& FloorNormal)
{
	// Decompose the ground plane into pitch along travel and roll across it.
	//  along:  + uphill ahead (normal leans back toward the climber, so N·F < 0)
	//  across: + ground falls away to the right (normal leans right, N·R > 0)
	const FVector Right = FVector::CrossProduct(FVector::UpVector, TravelDir2D).GetSafeNormal();
	const double Nz = FMath::Max(FloorNormal.Z, KINDA_SMALL_NUMBER);
	SlopeAlongDeg = static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(-FVector::DotProduct(FloorNormal, TravelDir2D), Nz)));
	SlopeAcrossDeg = static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(FVector::DotProduct(FloorNormal, Right), Nz)));
}

void USummitBalanceComponent::Recover()
{
	if (Balance)
	{
		Balance->Recover();
	}
	bFallen = false;
	bStumbling = false;
	Stability01 = 1.f;
}

void USummitBalanceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const AActor* Owner = GetOwner();
	if (!Balance || !Owner || DeltaTime <= 0.f)
	{
		return;
	}

	const bool bUpright = SummitClimbModes::IsUpright(ClimbMode);
	if (!bUpright || bFallen)
	{
		return;
	}

	// Direction of travel: velocity when moving, facing otherwise.
	const FVector Velocity = Owner->GetVelocity();
	FVector TravelDir = Velocity.GetSafeNormal2D();
	if (TravelDir.IsNearlyZero() || Velocity.Size2D() < 10.0)
	{
		TravelDir = Owner->GetActorForwardVector().GetSafeNormal2D();
	}
	const FVector Right = FVector::CrossProduct(FVector::UpVector, TravelDir).GetSafeNormal();

	const FVector FloorNormal = (ClimberComp && ClimberComp->HasFloor()) ? ClimberComp->GetFloorNormal() : FVector::UpVector;
	ComputeSlope(TravelDir, FloorNormal);

	summit::sim::BalanceInputs In;
	In.slopeAlongDeg = SlopeAlongDeg;
	In.slopeAcrossDeg = SlopeAcrossDeg;
	In.speedMS = SummitConv::CmsToMs(Velocity.Size2D());
	In.usingPoles = bUsingPoles;
	In.leanInput = LeanInput;
	In.bracing = bBracing;

	if (ClimberComp)
	{
		const FSummitWeatherSnapshot Weather = ClimberComp->GetLocalWeather();
		In.terrain = SummitConv::ToSim(ClimberComp->GetFloorTerrain());
		In.crampons = ClimberComp->bCrampons;
		In.temperatureC = Weather.TemperatureC;
		In.roughness01 = ClimberComp->GetFloorRoughness01();
		In.fatigue01 = ClimberComp->GetFatigue01();
		In.cognitiveImpairment01 = ClimberComp->GetCognitiveImpairment01();

		// Crosswind: project the wind (blowing TOWARD from+180°) onto the climber's right vector.
		const float NorthYaw = WeatherSubsystem ? WeatherSubsystem->NorthYawDeg : -90.f;
		const float ToYawRad = FMath::DegreesToRadians(NorthYaw + Weather.WindDirectionDeg + 180.f);
		const FVector WindDir(FMath::Cos(ToYawRad), FMath::Sin(ToYawRad), 0.0);
		In.lateralWindMS = Weather.WindSpeedMS * FVector::DotProduct(WindDir, Right);
		// Thin air pushes less: wind force scales with density (~0.5 kg/m³ near 8000 m vs 1.2 at sea level).
		In.airDensityKgM3 = summit::sim::AirDensityKgM3(Weather.PressureKPa, Weather.TemperatureC);
		In.gustiness01 = Weather.WindSpeedMS > 0.5f
			? FMath::Clamp((Weather.WindGustMS - Weather.WindSpeedMS) / Weather.WindSpeedMS, 0.f, 1.f)
			: 0.f;
	}

	Balance->Step(DeltaTime, In);

	const summit::sim::BalanceState& S = Balance->State();
	Stability01 = static_cast<float>(S.stability01);
	SlipRisk01 = static_cast<float>(S.slipRisk01);
	LateralTiltDeg = static_cast<float>(FMath::RadiansToDegrees(S.lateralTiltRad));
	ForwardTiltDeg = static_cast<float>(FMath::RadiansToDegrees(S.forwardTiltRad));
	SafeSpeedCmS = static_cast<float>(SummitConv::MsToCms(Balance->SafeSpeedMS(In)));

	const bool bWasStumbling = bStumbling;
	bStumbling = S.stumbling;
	if (bStumbling && !bWasStumbling)
	{
		OnStumbleStarted.Broadcast();
	}
	else if (!bStumbling && bWasStumbling && !S.fallen)
	{
		OnStumbleRecovered.Broadcast();
	}

	if (S.fallen && !bFallen)
	{
		bFallen = true;
		OnFallen.Broadcast();
	}
}
