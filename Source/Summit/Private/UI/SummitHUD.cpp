#include "UI/SummitHUD.h"

#include "Character/SummitCharacter.h"
#include "Simulation/SummitBalanceComponent.h"
#include "Simulation/SummitClimberComponent.h"
#include "Summit.h"
#include "UI/SummitHUDWidget.h"
#include "World/SummitWorldClockSubsystem.h"

#include "Blueprint/UserWidget.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"

void ASummitHUD::BeginPlay()
{
	Super::BeginPlay();

	APlayerController* PC = GetOwningPlayerController();
	if (!PC || !PC->IsLocalController())
	{
		return;
	}
	if (!HUDWidgetClass)
	{
		return;  // DrawHUD shows the text readout instead.
	}

	HUDWidget = CreateWidget<USummitHUDWidget>(PC, HUDWidgetClass);
	if (HUDWidget)
	{
		HUDWidget->AddToViewport();
	}
}

void ASummitHUD::DrawHUD()
{
	Super::DrawHUD();
	const ASummitCharacter* Climber = Cast<ASummitCharacter>(GetOwningPawn());
	if (HUDWidget || !Canvas || !Climber || !Climber->Climber)
	{
		return;
	}

	const FSummitVitals V = Climber->Climber->GetVitals();
	const FSummitWeatherSnapshot W = Climber->Climber->GetLocalWeather();
	const float Stability = Climber->Balance ? Climber->Balance->GetStability01() : 1.f;
	const USummitWorldClockSubsystem* Clock = GetWorld()->GetSubsystem<USummitWorldClockSubsystem>();
	const float Hour = Clock ? Clock->GetHourOfDay() : 0.f;

	const TArray<FString> Lines = {
		FString::Printf(TEXT("Altitude %.0f m   (feels like %.0f m)   %02d:%02d"), V.CurrentAltitudeM, V.EquivalentAltitudeM,
			FMath::FloorToInt(Hour), FMath::FloorToInt(FMath::Fmod(Hour, 1.f) * 60.f)),
		FString::Printf(TEXT("SpO2 %.0f%%   Core %.1f C   Hands %.1f C   AMS %.1f"), V.SpO2 * 100.f, V.CoreTempC, V.HandsTempC, V.AMSScore),
		FString::Printf(TEXT("Air %.0f C   Wind %.0f km/h   Chill %.0f C   Visibility %.0f m"), W.TemperatureC, W.WindSpeedMS * 3.6f,
			W.WindChillC, W.VisibilityM),
		FString::Printf(TEXT("Stamina %.0f%%   Fatigue %.0f%%   Stability %.0f%%   O2 %.1f L/min"), V.Stamina01 * 100.f,
			V.Fatigue01 * 100.f, Stability * 100.f, Climber->Climber->GetOxygenFlow()),
		TEXT("WASD move  Mouse look  Q/E lean  Ctrl brace  Space jump  LMB climb  O oxygen"),
	};

	const UFont* Font = GEngine->GetMediumFont();
	float Y = 24.f;
	for (const FString& Line : Lines)
	{
		DrawText(Line, FLinearColor(0.f, 0.f, 0.f, 0.6f), 26.f, Y + 2.f, const_cast<UFont*>(Font));
		DrawText(Line, FLinearColor::White, 24.f, Y, const_cast<UFont*>(Font));
		Y += 22.f;
	}
}

void ASummitHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (HUDWidget)
	{
		HUDWidget->RemoveFromParent();
		HUDWidget = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}
