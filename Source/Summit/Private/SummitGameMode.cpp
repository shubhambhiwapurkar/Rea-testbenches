#include "SummitGameMode.h"

#include "Character/SummitCharacter.h"
#include "UI/SummitHUD.h"

ASummitGameMode::ASummitGameMode()
{
	DefaultPawnClass = ASummitCharacter::StaticClass();
	HUDClass = ASummitHUD::StaticClass();
}
