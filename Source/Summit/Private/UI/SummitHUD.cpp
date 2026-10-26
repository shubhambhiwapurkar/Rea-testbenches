#include "UI/SummitHUD.h"

#include "Summit.h"
#include "UI/SummitHUDWidget.h"

#include "Blueprint/UserWidget.h"
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
		UE_LOG(LogSummit, Warning, TEXT("SummitHUD: HUDWidgetClass not set; no HUD will be shown."));
		return;
	}

	HUDWidget = CreateWidget<USummitHUDWidget>(PC, HUDWidgetClass);
	if (HUDWidget)
	{
		HUDWidget->AddToViewport();
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
