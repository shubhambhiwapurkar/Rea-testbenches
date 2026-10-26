#include "Character/SummitCharacter.h"

#include "Character/SummitCharacterMovementComponent.h"
#include "Climbing/SummitClimbingComponent.h"
#include "Simulation/SummitBalanceComponent.h"
#include "Simulation/SummitCargoComponent.h"
#include "Simulation/SummitClimberComponent.h"
#include "Summit.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/HitResult.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"

namespace
{
	using FSummitInputHandler = void (ASummitCharacter::*)(const FInputActionValue&);

	// Loads a soft input action and binds it; missing assets are skipped so a half-configured BP still runs.
	void BindSoftAction(UEnhancedInputComponent* Input, const TSoftObjectPtr<UInputAction>& SoftAction,
		ETriggerEvent Event, ASummitCharacter* Character, FSummitInputHandler Handler)
	{
		if (SoftAction.IsNull())
		{
			return;
		}
		if (const UInputAction* Action = SoftAction.LoadSynchronous())
		{
			Input->BindAction(Action, Event, Character, Handler);
		}
	}
}

ASummitCharacter::ASummitCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<USummitCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;

	GetCapsuleComponent()->InitCapsuleSize(35.f, 90.f);

	// Third-person: the camera follows the controller, the body turns toward movement.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 380.f;
	CameraBoom->SocketOffset = FVector(0.f, 45.f, 55.f);
	CameraBoom->bUsePawnControlRotation = true;
	CameraBoom->bEnableCameraLag = true;
	CameraBoom->CameraLagSpeed = 8.f;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	Climber = CreateDefaultSubobject<USummitClimberComponent>(TEXT("Climber"));
	Cargo = CreateDefaultSubobject<USummitCargoComponent>(TEXT("Cargo"));
	Balance = CreateDefaultSubobject<USummitBalanceComponent>(TEXT("Balance"));
	Climbing = CreateDefaultSubobject<USummitClimbingComponent>(TEXT("Climbing"));
}

USummitCharacterMovementComponent* ASummitCharacter::GetSummitMovement() const
{
	return Cast<USummitCharacterMovementComponent>(GetCharacterMovement());
}

void ASummitCharacter::BeginPlay()
{
	Super::BeginPlay();

	Balance->OnFallen.AddDynamic(this, &ASummitCharacter::HandleBalanceFallen);
	Climber->OnDeath.AddDynamic(this, &ASummitCharacter::HandleClimberDeath);

	Climber->SetClimbMode(ClimbMode);
	Balance->SetClimbMode(ClimbMode);
}

void ASummitCharacter::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	const APlayerController* PC = Cast<APlayerController>(Controller);
	if (!PC || DefaultMappingContext.IsNull())
	{
		return;
	}
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
	{
		if (const UInputMappingContext* Context = DefaultMappingContext.LoadSynchronous())
		{
			Subsystem->AddMappingContext(Context, 0);
		}
	}
}

void ASummitCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		UE_LOG(LogSummit, Error, TEXT("SummitCharacter needs EnhancedInputComponent as the default input component (Config/DefaultInput.ini)."));
		return;
	}

	BindSoftAction(Input, MoveAction, ETriggerEvent::Triggered, this, &ASummitCharacter::HandleMove);
	BindSoftAction(Input, MoveAction, ETriggerEvent::Completed, this, &ASummitCharacter::HandleMoveCompleted);
	BindSoftAction(Input, LookAction, ETriggerEvent::Triggered, this, &ASummitCharacter::HandleLook);
	BindSoftAction(Input, LeanLeftAction, ETriggerEvent::Triggered, this, &ASummitCharacter::HandleLeanLeft);
	BindSoftAction(Input, LeanLeftAction, ETriggerEvent::Completed, this, &ASummitCharacter::HandleLeanLeftCompleted);
	BindSoftAction(Input, LeanRightAction, ETriggerEvent::Triggered, this, &ASummitCharacter::HandleLeanRight);
	BindSoftAction(Input, LeanRightAction, ETriggerEvent::Completed, this, &ASummitCharacter::HandleLeanRightCompleted);
	BindSoftAction(Input, BraceAction, ETriggerEvent::Started, this, &ASummitCharacter::HandleBraceStarted);
	BindSoftAction(Input, BraceAction, ETriggerEvent::Completed, this, &ASummitCharacter::HandleBraceCompleted);
	BindSoftAction(Input, InteractAction, ETriggerEvent::Started, this, &ASummitCharacter::HandleInteract);
	BindSoftAction(Input, ClimbAction, ETriggerEvent::Started, this, &ASummitCharacter::HandleClimb);
	BindSoftAction(Input, UseAxeAction, ETriggerEvent::Started, this, &ASummitCharacter::HandleUseAxeStarted);
	BindSoftAction(Input, UseAxeAction, ETriggerEvent::Completed, this, &ASummitCharacter::HandleUseAxeCompleted);
	BindSoftAction(Input, JumpAction, ETriggerEvent::Started, this, &ASummitCharacter::HandleJump);
	BindSoftAction(Input, ToggleOxygenAction, ETriggerEvent::Started, this, &ASummitCharacter::HandleToggleOxygen);
	BindSoftAction(Input, OpenInventoryAction, ETriggerEvent::Started, this, &ASummitCharacter::HandleOpenInventory);
}

// ---- Climb mode ----------------------------------------------------------------------------------------

void ASummitCharacter::SetClimbMode(ESummitClimbMode NewMode)
{
	// Re-entrancy guard: changing the CharacterMovement mode calls back into OnMovementModeChanged.
	if (NewMode == ClimbMode || bChangingMode)
	{
		return;
	}
	if (bDead && NewMode != ESummitClimbMode::Fallen)
	{
		return;
	}

	bChangingMode = true;
	const ESummitClimbMode OldMode = ClimbMode;
	ClimbMode = NewMode;

	Climber->SetClimbMode(NewMode);
	Balance->SetClimbMode(NewMode);

	if (USummitCharacterMovementComponent* Move = GetSummitMovement())
	{
		switch (NewMode)
		{
		case ESummitClimbMode::RockClimbing:
		case ESummitClimbMode::IceClimbing:
			Move->EnterSummitMode(ESummitCustomMovement::Climb);
			break;
		case ESummitClimbMode::Jumaring:
		case ESummitClimbMode::Rappelling:
			Move->EnterSummitMode(ESummitCustomMovement::Rope);
			break;
		case ESummitClimbMode::Sliding:
			Move->EnterSummitMode(ESummitCustomMovement::Slide);
			break;
		case ESummitClimbMode::Fallen:
			Move->EnterSummitMode(ESummitCustomMovement::None);
			Move->StopMovementImmediately();
			break;
		default:
			// Walking/Scrambling/Resting: leave any custom mode via falling; with ground below it lands at once.
			Move->EnterSummitMode(ESummitCustomMovement::None);
			break;
		}
	}

	Climbing->HandleClimbModeChanged(OldMode, NewMode);
	if (NewMode == ESummitClimbMode::Fallen)
	{
		FallenTimeS = 0.f;
	}
	bChangingMode = false;

	UE_LOG(LogSummit, Verbose, TEXT("Climb mode %d -> %d"), static_cast<int32>(OldMode), static_cast<int32>(NewMode));
	OnClimbModeChanged(OldMode, NewMode);
}

void ASummitCharacter::OnMovementModeChanged(EMovementMode PrevMovementMode, uint8 PreviousCustomMode)
{
	Super::OnMovementModeChanged(PrevMovementMode, PreviousCustomMode);

	if (bChangingMode)
	{
		return;
	}
	// Something outside SetClimbMode (teleport, root motion, engine fallback) knocked us out of a custom mode
	// while on a wall or rope: reconcile. Sliding is left alone; a slide that goes airborne is resolved in Landed.
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	if (Move && Move->MovementMode != MOVE_Custom && (SummitClimbModes::IsOnWall(ClimbMode) || SummitClimbModes::IsOnRope(ClimbMode)))
	{
		SetClimbMode(ESummitClimbMode::Walking);
	}
}

void ASummitCharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	if (bDead)
	{
		return;
	}

	// Injury depends on the velocity into the surface, not the vertical speed: landing on a steep snow slope
	// glances off, landing on a ledge stops dead.
	const FVector V = GetCharacterMovement()->Velocity;
	const float ImpactMS = static_cast<float>(FMath::Max(0.0, -FVector::DotProduct(V, Hit.ImpactNormal)) / 100.0);

	if (ImpactMS > FatalLandingSpeedMS)
	{
		Climber->KillClimber(ESummitCauseOfDeath::Fall);
		return;
	}

	if (ClimbMode == ESummitClimbMode::Sliding)
	{
		if (Climbing->ShouldFallBecomeSlide() && ImpactMS <= HardLandingSpeedMS)
		{
			// Back onto the slope: keep sliding. Switching mode here stops CMC from applying post-landed walking.
			if (USummitCharacterMovementComponent* Move = GetSummitMovement())
			{
				Move->EnterSummitMode(ESummitCustomMovement::Slide);
			}
			return;
		}
		EnterFallen(ImpactMS > HardLandingSpeedMS);
		return;
	}

	if (ImpactMS > HardLandingSpeedMS)
	{
		EnterFallen(true);
	}
}

void ASummitCharacter::EnterFallen(bool bHardImpact)
{
	SetClimbMode(ESummitClimbMode::Fallen);
	GetCharacterMovement()->StopMovementImmediately();
	OnFallenDown(bHardImpact);
}

void ASummitCharacter::HandleBalanceFallen()
{
	if (bDead)
	{
		return;
	}
	if (Climbing->ShouldFallBecomeSlide())
	{
		SetClimbMode(ESummitClimbMode::Sliding);
		OnFallenDown(false);
		return;
	}
	EnterFallen(false);
}

void ASummitCharacter::HandleClimberDeath(ESummitCauseOfDeath Cause)
{
	SetClimbMode(ESummitClimbMode::Fallen);
	bDead = true;
	GetCharacterMovement()->StopMovementImmediately();
	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		DisableInput(PC);
	}
	OnClimberDied(Cause);
}

bool ASummitCharacter::TryRecoverFromFall()
{
	if (ClimbMode != ESummitClimbMode::Fallen || bDead || FallenTimeS < MinFallenTimeS)
	{
		return false;
	}
	// Severe HACE, hypothermia or exhaustion: the climber cannot get up unaided.
	if (Climber->GetVitals().bIncapacitated)
	{
		return false;
	}
	Balance->Recover();
	SetClimbMode(ESummitClimbMode::Walking);
	OnRecoveredFromFall();
	return true;
}

// ---- Per-frame -----------------------------------------------------------------------------------------

void ASummitCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (ClimbMode == ESummitClimbMode::Fallen)
	{
		FallenTimeS += DeltaSeconds;
	}

	Balance->SetLeanInput(GetLeanInput());
	Balance->SetBracing(IsBracing());

	UpdateMovementLimits();
}

void ASummitCharacter::UpdateMovementLimits()
{
	USummitCharacterMovementComponent* Move = GetSummitMovement();
	if (!Move)
	{
		return;
	}

	// The sim decides how steep a surface can be walked with the current boots/crampons. A small margin lets
	// the climbing component's slide check fire before CharacterMovement rejects the floor outright.
	Move->SetWalkableFloorAngle(FMath::Clamp(Climber->GetMaxWalkableSlopeDeg() + 8.f, 20.f, 75.f));

	if (bDead || ClimbMode == ESummitClimbMode::Fallen || ClimbMode == ESummitClimbMode::Resting)
	{
		Move->MaxWalkSpeed = 0.f;
		return;
	}

	float Limit = MaxWalkSpeedCeilingCmS;

	// Body: the sim's sustainable along-ground speed, with a burst allowance while anaerobic stamina lasts.
	const float Sustainable = Climber->GetMaxSustainableSpeedCmS();
	if (Sustainable > 0.f)
	{
		const float Burst = Climber->GetStamina01() > 0.15f ? BurstSpeedMultiplier : 1.f;
		Limit = FMath::Min(Limit, Sustainable * Burst);
	}

	// Footing: the balance model's safe speed for slope, load and surface.
	const float Safe = Balance->GetSafeSpeedCmS();
	if (Safe > 0.f)
	{
		Limit = FMath::Min(Limit, Safe);
	}

	if (IsBracing())
	{
		Limit *= BraceSpeedMultiplier;
	}
	if (ClimbMode == ESummitClimbMode::Scrambling)
	{
		Limit *= ScrambleSpeedMultiplier;
	}

	// Always allow a shuffle so an exhausted climber can still crawl to a tent.
	Move->MaxWalkSpeed = FMath::Max(Limit, 15.f);
}

// ---- Input ---------------------------------------------------------------------------------------------

void ASummitCharacter::HandleMove(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	switch (ClimbMode)
	{
	case ESummitClimbMode::Walking:
	case ESummitClimbMode::Scrambling:
		if (Controller)
		{
			const FRotator Yaw(0.f, Controller->GetControlRotation().Yaw, 0.f);
			const FRotationMatrix YawMatrix(Yaw);
			AddMovementInput(YawMatrix.GetUnitAxis(EAxis::X), static_cast<float>(Axis.Y));
			AddMovementInput(YawMatrix.GetUnitAxis(EAxis::Y), static_cast<float>(Axis.X));
		}
		break;
	case ESummitClimbMode::RockClimbing:
	case ESummitClimbMode::IceClimbing:
	case ESummitClimbMode::Jumaring:
	case ESummitClimbMode::Rappelling:
	case ESummitClimbMode::Sliding:
		Climbing->SetClimbInput(Axis);
		break;
	default:
		break;
	}
}

void ASummitCharacter::HandleMoveCompleted(const FInputActionValue& /*Value*/)
{
	Climbing->SetClimbInput(FVector2D::ZeroVector);
}

void ASummitCharacter::HandleLook(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(static_cast<float>(Axis.X));
	AddControllerPitchInput(static_cast<float>(Axis.Y));
}

void ASummitCharacter::HandleLeanLeft(const FInputActionValue& Value)
{
	LeanLeftValue = FMath::Clamp(Value.Get<float>(), 0.f, 1.f);
}

void ASummitCharacter::HandleLeanRight(const FInputActionValue& Value)
{
	LeanRightValue = FMath::Clamp(Value.Get<float>(), 0.f, 1.f);
}

void ASummitCharacter::HandleLeanLeftCompleted(const FInputActionValue& /*Value*/)
{
	LeanLeftValue = 0.f;
}

void ASummitCharacter::HandleLeanRightCompleted(const FInputActionValue& /*Value*/)
{
	LeanRightValue = 0.f;
}

void ASummitCharacter::HandleBraceStarted(const FInputActionValue& /*Value*/)
{
	bBraceHeld = true;
}

void ASummitCharacter::HandleBraceCompleted(const FInputActionValue& /*Value*/)
{
	bBraceHeld = false;
}

void ASummitCharacter::HandleInteract(const FInputActionValue& /*Value*/)
{
	if (ClimbMode == ESummitClimbMode::Fallen)
	{
		TryRecoverFromFall();
		return;
	}
	if (SummitClimbModes::IsOnRope(ClimbMode))
	{
		Climbing->DetachFromRope();
		return;
	}
	if (Climbing->TryAttachToRope())
	{
		return;
	}
	OnInteract();
}

void ASummitCharacter::HandleClimb(const FInputActionValue& /*Value*/)
{
	if (SummitClimbModes::IsOnWall(ClimbMode))
	{
		Climbing->StopClimbing(/*bDrop*/ true);
		return;
	}
	Climbing->TryStartClimb();
}

void ASummitCharacter::HandleUseAxeStarted(const FInputActionValue& /*Value*/)
{
	if (ClimbMode == ESummitClimbMode::Sliding)
	{
		Climbing->TrySelfArrest();
		return;
	}
	// Planting the axe shaft on the uphill side gives a third point of contact on steep snow.
	if ((ClimbMode == ESummitClimbMode::Walking || ClimbMode == ESummitClimbMode::Scrambling) && Climber->bIceAxes)
	{
		bAxePlanted = true;
		Balance->bUsingPoles = true;
	}
}

void ASummitCharacter::HandleUseAxeCompleted(const FInputActionValue& /*Value*/)
{
	if (bAxePlanted)
	{
		bAxePlanted = false;
		Balance->bUsingPoles = false;
	}
}

void ASummitCharacter::HandleJump(const FInputActionValue& /*Value*/)
{
	switch (ClimbMode)
	{
	case ESummitClimbMode::Fallen:
		TryRecoverFromFall();
		break;
	case ESummitClimbMode::RockClimbing:
	case ESummitClimbMode::IceClimbing:
		Climbing->StopClimbing(/*bDrop*/ true);
		break;
	case ESummitClimbMode::Walking:
		Jump();
		break;
	default:
		break;
	}
}

void ASummitCharacter::HandleToggleOxygen(const FInputActionValue& /*Value*/)
{
	Climber->ToggleMask();
	// First time on: a typical climbing flow of 2 L/min.
	if (Climber->IsMaskOn() && Climber->GetOxygenFlow() <= 0.f)
	{
		Climber->SetOxygenFlow(2.f);
	}
}

void ASummitCharacter::HandleOpenInventory(const FInputActionValue& /*Value*/)
{
	OnOpenInventory();
}
