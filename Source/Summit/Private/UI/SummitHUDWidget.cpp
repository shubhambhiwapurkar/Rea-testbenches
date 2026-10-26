#include "UI/SummitHUDWidget.h"

#include "Character/SummitCharacter.h"
#include "Simulation/SummitBalanceComponent.h"
#include "Simulation/SummitCargoComponent.h"
#include "Simulation/SummitClimberComponent.h"
#include "World/SummitWeatherSubsystem.h"
#include "World/SummitWorldClockSubsystem.h"

#include "Engine/World.h"

void USummitHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();
	BindToPawn();
	RefreshNow();
}

void USummitHUDWidget::NativeDestruct()
{
	UnbindFromPawn();
	Super::NativeDestruct();
}

void USummitHUDWidget::BindToPawn()
{
	ASummitCharacter* Character = Cast<ASummitCharacter>(GetOwningPlayerPawn());
	if (Character == BoundCharacter.Get())
	{
		return;
	}
	UnbindFromPawn();
	BoundCharacter = Character;
	if (Character && Character->Climber)
	{
		Character->Climber->OnConditionChanged.AddDynamic(this, &USummitHUDWidget::HandleConditionChanged);
		Character->Climber->OnDeath.AddDynamic(this, &USummitHUDWidget::HandleDeath);
	}
}

void USummitHUDWidget::UnbindFromPawn()
{
	if (ASummitCharacter* Character = BoundCharacter.Get())
	{
		if (Character->Climber)
		{
			Character->Climber->OnConditionChanged.RemoveDynamic(this, &USummitHUDWidget::HandleConditionChanged);
			Character->Climber->OnDeath.RemoveDynamic(this, &USummitHUDWidget::HandleDeath);
		}
	}
	BoundCharacter.Reset();
}

void USummitHUDWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	RefreshCooldownS -= InDeltaTime;
	if (RefreshCooldownS > 0.f)
	{
		return;
	}
	RefreshCooldownS = RefreshIntervalS;

	// The pawn can change (respawn, possession); rebind lazily.
	BindToPawn();
	RefreshNow();
}

void USummitHUDWidget::RefreshNow()
{
	if (const ASummitCharacter* Character = BoundCharacter.Get())
	{
		ClimbMode = Character->GetClimbMode();
		LeanInput = Character->GetLeanInput();
		if (const USummitClimberComponent* Climber = Character->Climber)
		{
			Vitals = Climber->GetVitals();
			Weather = Climber->GetLocalWeather();
			bOxygenMaskOn = Climber->IsMaskOn();
			OxygenFlowLMin = Climber->GetOxygenFlow();
			OxygenHoursRemaining = Climber->GetOxygenHoursRemaining();
		}
		if (const USummitBalanceComponent* Balance = Character->Balance)
		{
			BalanceStability01 = Balance->GetStability01();
			SlipRisk01 = Balance->GetSlipRisk01();
			CargoLateralBiasDeg = Balance->GetCargoLateralBiasDeg();
		}
		if (const USummitCargoComponent* Cargo = Character->Cargo)
		{
			CargoMassKg = Cargo->GetTotalMassKg();
		}
	}

	if (const UWorld* World = GetWorld())
	{
		if (const USummitWeatherSubsystem* WeatherSubsystem = World->GetSubsystem<USummitWeatherSubsystem>())
		{
			AvalancheDanger = WeatherSubsystem->GetAvalancheDanger();
		}
		if (const USummitWorldClockSubsystem* Clock = World->GetSubsystem<USummitWorldClockSubsystem>())
		{
			DayOfYear = Clock->GetDayOfYear();
			HourOfDay = Clock->GetHourOfDay();
		}
	}

	OnSummitDataUpdated();
}

void USummitHUDWidget::HandleConditionChanged(ESummitCondition Condition, float Severity01, bool bActive)
{
	OnConditionAlert(Condition, Severity01, bActive);
}

void USummitHUDWidget::HandleDeath(ESummitCauseOfDeath Cause)
{
	OnPlayerDied(Cause);
}
