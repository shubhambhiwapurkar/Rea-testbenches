// Simulation clock for the current world.
//
// Sim time is seconds since 00:00 local solar time on 1 January of the campaign year (the convention of
// summit::sim::SimTimeSeconds). Everything time-dependent (weather, physiology, sun) reads from here, so
// sleeping or fast-travelling is a single AdvanceBy() call that the other systems catch up on.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SummitWorldClockSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSummitClockHourSignature, int32, Hour);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSummitClockDaySignature, int32, DayOfYear);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSummitClockJumpSignature, float, Hours);

// C++ listeners (components that must integrate the skipped interval) bind to this one.
DECLARE_MULTICAST_DELEGATE_TwoParams(FSummitClockJumpNative, double /*FromSimSeconds*/, double /*JumpSeconds*/);

UCLASS()
class SUMMIT_API USummitWorldClockSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableGameObject (via UTickableWorldSubsystem)
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// Seconds since 1 Jan 00:00 local solar time.
	UFUNCTION(BlueprintPure, Category = "Summit|Time")
	double GetSimTimeSeconds() const { return SimTimeSeconds; }

	UFUNCTION(BlueprintPure, Category = "Summit|Time")
	double GetSimTimeHours() const { return SimTimeSeconds / 3600.0; }

	// 0..364
	UFUNCTION(BlueprintPure, Category = "Summit|Time")
	int32 GetDayOfYear() const;

	// 0..24, fractional.
	UFUNCTION(BlueprintPure, Category = "Summit|Time")
	float GetHourOfDay() const;

	// Whole days elapsed since the expedition clock started (day 0 = arrival).
	UFUNCTION(BlueprintPure, Category = "Summit|Time")
	int32 GetExpeditionDay() const;

	UFUNCTION(BlueprintPure, Category = "Summit|Time")
	float GetTimeScale() const { return TimeScale; }

	// Game seconds per real second. Kept modest; use AdvanceBy for long skips so systems integrate cleanly.
	UFUNCTION(BlueprintCallable, Category = "Summit|Time")
	void SetTimeScale(float NewScale);

	UFUNCTION(BlueprintCallable, Category = "Summit|Time")
	void SetClockPaused(bool bPaused) { bClockPaused = bPaused; }

	UFUNCTION(BlueprintPure, Category = "Summit|Time")
	bool IsClockPaused() const { return bClockPaused; }

	// Skip forward (sleeping, waiting out a storm, fast travel between camps). Listeners on OnTimeJumped
	// integrate the interval in one go (the sim sub-steps internally).
	UFUNCTION(BlueprintCallable, Category = "Summit|Time")
	void AdvanceBy(float Hours);

	// Hard reset (new expedition / loading a save). Does not fire OnTimeJumped.
	UFUNCTION(BlueprintCallable, Category = "Summit|Time")
	void SetSimTime(double NewSimTimeSeconds);

	UPROPERTY(BlueprintAssignable, Category = "Summit|Time")
	FSummitClockHourSignature OnHourChanged;

	UPROPERTY(BlueprintAssignable, Category = "Summit|Time")
	FSummitClockDaySignature OnDayChanged;

	UPROPERTY(BlueprintAssignable, Category = "Summit|Time")
	FSummitClockJumpSignature OnTimeJumped;

	FSummitClockJumpNative OnTimeJumpedNative;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void BroadcastBoundaryCrossings(double OldTime);

	double SimTimeSeconds = 0.0;
	double StartSimTimeSeconds = 0.0;
	float TimeScale = 1.f;
	bool bClockPaused = false;
};
