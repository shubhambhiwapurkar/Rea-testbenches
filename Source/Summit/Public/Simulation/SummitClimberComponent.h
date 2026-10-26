// The climber's body: physiology, thermal balance and energy, via summit::sim::Climber.
//
// Every tick this component assembles the sim inputs from the world (altitude from Z, weather from the
// weather subsystem, terrain from the physical material underfoot, speed and grade from the owner's
// velocity, activity from the climb mode the character pushes in, gear from its own properties and the
// cargo component), steps the sim in game seconds, and publishes an FSummitVitals snapshot.
// It also samples the floor once for everyone (balance and climbing components read it from here).

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Data/SummitTypes.h"
#include "Simulation/SummitSimHandles.h"
#include "SummitClimberComponent.generated.h"

class USummitCargoComponent;
class USummitWeatherSubsystem;
class USummitWorldClockSubsystem;

namespace summit::sim
{
	struct ClimberEnvironment;
	struct ClimberAction;
	struct ClimberGear;
}

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FSummitConditionChangedSignature, ESummitCondition, Condition, float, Severity01, bool, bActive);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSummitDeathSignature, ESummitCauseOfDeath, Cause);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSummitOxygenEmptySignature);

UCLASS(ClassGroup = (Summit), meta = (BlueprintSpawnableComponent))
class SUMMIT_API USummitClimberComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USummitClimberComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// ---- Profile (applied at BeginPlay) -------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Profile")
	FString ClimberName = TEXT("Climber");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Profile", meta = (ClampMin = "40", ClampMax = "140", Units = "Kilograms"))
	float BodyMassKg = 75.f;

	// Sea-level VO2max (mL/kg/min). Trained mountaineers ~50-65.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Profile", meta = (ClampMin = "25", ClampMax = "90"))
	float VO2MaxSeaLevel = 55.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Profile", meta = (ClampMin = "0.5", ClampMax = "2"))
	float AMSSusceptibility = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Profile", meta = (ClampMin = "0", ClampMax = "1"))
	float SkillTechnical01 = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Profile", meta = (ClampMin = "0", ClampMax = "1"))
	float SkillIce01 = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Profile", meta = (ClampMin = "0", ClampMax = "1"))
	float SkillRope01 = 0.5f;

	// Starting acclimatization (e.g. 5364 m and 1.0 for a climber who has done rotations from EBC).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Profile", meta = (ClampMin = "0", Units = "Meters"))
	float InitialAcclimatizedAltitudeM = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Profile", meta = (ClampMin = "0", ClampMax = "1"))
	float InitialAcclimatization01 = 0.f;

	// ---- Gear -----------------------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Gear")
	FSummitClothing Clothing;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Gear")
	bool bCrampons = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Gear")
	bool bIceAxes = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Gear")
	bool bGlacierGoggles = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Gear")
	bool bAcetazolamide = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Gear")
	bool bDexamethasone = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Gear")
	bool bNifedipine = false;

	// Litres of O2 gas left across carried bottles. A 4 L bottle at 300 bar holds ~1200 L.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Gear|Oxygen", meta = (ClampMin = "0"))
	float OxygenRemainingL = 0.f;

	// ---- Situation ------------------------------------------------------------------------------------

	// 0 = sheltered (lee of a serac, tent), 1 = exposed ridge. Trigger volumes can set this.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Situation", meta = (ClampMin = "0", ClampMax = "1"))
	float WindExposure01 = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Situation")
	float SleepingBagComfortC = -20.f;

	// Fallback terrain when the floor has no Summit physical material or surface type.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Situation")
	ESummitTerrainType DefaultTerrain = ESummitTerrainType::Trail;

	// Seconds between floor traces (terrain/normal under the feet).
	UPROPERTY(EditAnywhere, Category = "Summit|Situation", meta = (ClampMin = "0"))
	float FloorSampleIntervalS = 0.1f;

	// ---- Events ---------------------------------------------------------------------------------------

	// Fires when a condition appears, clears, or its severity crosses a 25% band.
	UPROPERTY(BlueprintAssignable, Category = "Summit|Vitals")
	FSummitConditionChangedSignature OnConditionChanged;

	UPROPERTY(BlueprintAssignable, Category = "Summit|Vitals")
	FSummitDeathSignature OnDeath;

	UPROPERTY(BlueprintAssignable, Category = "Summit|Gear|Oxygen")
	FSummitOxygenEmptySignature OnOxygenEmpty;

	// ---- Queries --------------------------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Summit|Vitals")
	FSummitVitals GetVitals() const { return Vitals; }

	// Sustainable along-ground speed for current conditions, in Unreal units (cm/s), for movement clamping.
	UFUNCTION(BlueprintPure, Category = "Summit|Vitals")
	float GetMaxSustainableSpeedCmS() const;

	UFUNCTION(BlueprintPure, Category = "Summit|Vitals")
	float GetStamina01() const;

	UFUNCTION(BlueprintPure, Category = "Summit|Vitals")
	float GetFatigue01() const { return Vitals.Fatigue01; }

	UFUNCTION(BlueprintPure, Category = "Summit|Vitals")
	float GetCognitiveImpairment01() const { return Vitals.CognitiveImpairment01; }

	UFUNCTION(BlueprintPure, Category = "Summit|Vitals")
	bool IsDead() const { return Vitals.bDead; }

	UFUNCTION(BlueprintPure, Category = "Summit|Situation")
	float GetAltitudeM() const { return Vitals.CurrentAltitudeM; }

	UFUNCTION(BlueprintPure, Category = "Summit|Situation")
	FSummitWeatherSnapshot GetLocalWeather() const { return LocalWeather; }

	// Floor under the feet, from the last trace.
	UFUNCTION(BlueprintPure, Category = "Summit|Situation")
	ESummitTerrainType GetFloorTerrain() const { return FloorTerrain; }

	UFUNCTION(BlueprintPure, Category = "Summit|Situation")
	FVector GetFloorNormal() const { return FloorNormal; }

	UFUNCTION(BlueprintPure, Category = "Summit|Situation")
	float GetFloorRoughness01() const { return FloorRoughness01; }

	UFUNCTION(BlueprintPure, Category = "Summit|Situation")
	float GetFloorSnowDepthM() const { return FloorSnowDepthM; }

	UFUNCTION(BlueprintPure, Category = "Summit|Situation")
	bool HasFloor() const { return bHasFloor; }

	// Boot/crampon friction coefficient on the current floor (summit::sim::SurfaceFriction).
	UFUNCTION(BlueprintPure, Category = "Summit|Situation")
	float GetFloorFriction() const;

	// Steepest slope walkable upright on the current floor with current crampons (degrees).
	UFUNCTION(BlueprintPure, Category = "Summit|Situation")
	float GetMaxWalkableSlopeDeg() const;

	float GetSkillTechnical01() const { return SkillTechnical01; }
	float GetSkillIce01() const { return SkillIce01; }
	float GetSkillRope01() const { return SkillRope01; }

	// ---- Actions --------------------------------------------------------------------------------------

	// The character pushes its locomotion state here; it selects the sim Activity.
	UFUNCTION(BlueprintCallable, Category = "Summit|Situation")
	void SetClimbMode(ESummitClimbMode NewMode) { ClimbMode = NewMode; }

	UFUNCTION(BlueprintCallable, Category = "Summit|Actions")
	void Eat(float Kcal);

	UFUNCTION(BlueprintCallable, Category = "Summit|Actions")
	void Drink(float Liters);

	// Eats/drinks from the cargo component's consumables. Returns the amount actually consumed.
	UFUNCTION(BlueprintCallable, Category = "Summit|Actions")
	float EatFromPack(float Kcal);

	UFUNCTION(BlueprintCallable, Category = "Summit|Actions")
	float DrinkFromPack(float Liters);

	// Regulator flow (L/min): 0.5 sleeping, 2-3 climbing, 4 emergency.
	UFUNCTION(BlueprintCallable, Category = "Summit|Gear|Oxygen")
	void SetOxygenFlow(float LitersPerMinute);

	UFUNCTION(BlueprintPure, Category = "Summit|Gear|Oxygen")
	float GetOxygenFlow() const { return OxygenFlowLMin; }

	UFUNCTION(BlueprintCallable, Category = "Summit|Gear|Oxygen")
	void ToggleMask();

	UFUNCTION(BlueprintCallable, Category = "Summit|Gear|Oxygen")
	void SetMaskOn(bool bOn) { bMaskOn = bOn; }

	UFUNCTION(BlueprintPure, Category = "Summit|Gear|Oxygen")
	bool IsMaskOn() const { return bMaskOn; }

	// Adds a bottle's worth of gas (water volume in litres, fill pressure in bar).
	UFUNCTION(BlueprintCallable, Category = "Summit|Gear|Oxygen")
	void AddOxygenBottle(float WaterVolumeL = 4.f, float FillPressureBar = 300.f);

	// Hours left at the current flow.
	UFUNCTION(BlueprintPure, Category = "Summit|Gear|Oxygen")
	float GetOxygenHoursRemaining() const;

	UFUNCTION(BlueprintCallable, Category = "Summit|Actions")
	void SetSleeping(bool bInSleeping) { bSleeping = bInSleeping; }

	UFUNCTION(BlueprintCallable, Category = "Summit|Actions")
	void SetInTent(bool bInTentNow) { bInTent = bInTentNow; }

	UFUNCTION(BlueprintCallable, Category = "Summit|Actions")
	void SetInSleepingBag(bool bInBag) { bInSleepingBag = bInBag; }

	UFUNCTION(BlueprintPure, Category = "Summit|Actions")
	bool IsSleeping() const { return bSleeping; }

	// Spends short-term (anaerobic) stamina for grip and hard moves. Returns false if there was not enough;
	// in that case stamina is drained to zero and the caller should make the climber let go.
	UFUNCTION(BlueprintCallable, Category = "Summit|Actions")
	bool ConsumeStamina(float Amount01);

	// Kills the climber from an external cause (fall, avalanche). Idempotent.
	UFUNCTION(BlueprintCallable, Category = "Summit|Actions")
	void KillClimber(ESummitCauseOfDeath Cause);

	// Steps the sim over an interval without real time passing (sleep/fast travel). Called from the clock's
	// jump event; public for expedition scripting.
	void IntegrateSkippedTime(double Seconds);

private:
	void SampleFloor();
	void BuildEnvironment(summit::sim::ClimberEnvironment& OutEnv) const;
	void BuildAction(summit::sim::ClimberAction& OutAction, bool bForMovementQuery) const;
	void BuildGear(summit::sim::ClimberGear& OutGear) const;
	void StepSim(double Seconds);
	void ConsumeOxygen(double Seconds);
	void RefreshVitals();
	void DispatchConditionEvents();
	void HandleClockJump(double FromSimSeconds, double JumpSeconds);
	double GetPressureKPa() const;

	TSimPtr<summit::sim::Climber> Climber;

	UPROPERTY(Transient)
	TObjectPtr<USummitCargoComponent> Cargo;

	UPROPERTY(Transient)
	TObjectPtr<USummitWeatherSubsystem> WeatherSubsystem;

	UPROPERTY(Transient)
	TObjectPtr<USummitWorldClockSubsystem> Clock;

	FSummitVitals Vitals;
	FSummitWeatherSnapshot LocalWeather;
	ESummitClimbMode ClimbMode = ESummitClimbMode::Walking;

	float OxygenFlowLMin = 0.f;
	bool bMaskOn = false;
	bool bSleeping = false;
	bool bInTent = false;
	bool bInSleepingBag = false;
	bool bDeathBroadcast = false;

	// Floor sample
	ESummitTerrainType FloorTerrain = ESummitTerrainType::Trail;
	FVector FloorNormal = FVector::UpVector;
	float FloorRoughness01 = 0.f;
	float FloorSnowDepthM = 0.f;
	bool bHasFloor = false;
	float FloorSampleCooldownS = 0.f;

	// Last reported severity band per condition (-1 = inactive), for change events.
	TArray<int32> ConditionBands;
};
