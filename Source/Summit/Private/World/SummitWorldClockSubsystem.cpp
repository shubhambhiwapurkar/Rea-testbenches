#include "World/SummitWorldClockSubsystem.h"

#include "Data/SummitSettings.h"
#include "Summit.h"

#include "Sim/Constants.h"
#include "Sim/Weather.h"

void USummitWorldClockSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const USummitSettings* Settings = USummitSettings::Get();
	TimeScale = Settings->TimeScale;
	StartSimTimeSeconds = static_cast<double>(Settings->StartDayOfYear) * summit::sim::kSecondsPerDay
		+ static_cast<double>(Settings->StartHour) * summit::sim::kSecondsPerHour;
	SimTimeSeconds = StartSimTimeSeconds;
}

void USummitWorldClockSubsystem::Deinitialize()
{
	OnTimeJumpedNative.Clear();
	Super::Deinitialize();
}

bool USummitWorldClockSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId USummitWorldClockSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USummitWorldClockSubsystem, STATGROUP_Tickables);
}

void USummitWorldClockSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (bClockPaused || TimeScale <= 0.f)
	{
		return;
	}

	const double OldTime = SimTimeSeconds;
	SimTimeSeconds += static_cast<double>(DeltaTime) * static_cast<double>(TimeScale);
	BroadcastBoundaryCrossings(OldTime);
}

int32 USummitWorldClockSubsystem::GetDayOfYear() const
{
	return summit::sim::DayOfYear(SimTimeSeconds);
}

float USummitWorldClockSubsystem::GetHourOfDay() const
{
	return static_cast<float>(summit::sim::HourOfDay(SimTimeSeconds));
}

int32 USummitWorldClockSubsystem::GetExpeditionDay() const
{
	return FMath::FloorToInt32((SimTimeSeconds - StartSimTimeSeconds) / summit::sim::kSecondsPerDay);
}

void USummitWorldClockSubsystem::SetTimeScale(float NewScale)
{
	TimeScale = FMath::Clamp(NewScale, 0.f, 3600.f);
}

void USummitWorldClockSubsystem::AdvanceBy(float Hours)
{
	if (Hours <= 0.f)
	{
		return;
	}

	const double OldTime = SimTimeSeconds;
	const double JumpSeconds = static_cast<double>(Hours) * summit::sim::kSecondsPerHour;
	SimTimeSeconds += JumpSeconds;

	UE_LOG(LogSummit, Log, TEXT("Clock: advanced %.2f h (day %d, %.2f h)"), Hours, GetDayOfYear(), GetHourOfDay());

	// Native listeners first: they integrate the skipped interval (climber physiology, weather), so that
	// Blueprint listeners see a consistent, already-updated world.
	OnTimeJumpedNative.Broadcast(OldTime, JumpSeconds);
	OnTimeJumped.Broadcast(Hours);
	BroadcastBoundaryCrossings(OldTime);
}

void USummitWorldClockSubsystem::SetSimTime(double NewSimTimeSeconds)
{
	SimTimeSeconds = FMath::Max(0.0, NewSimTimeSeconds);
	StartSimTimeSeconds = FMath::Min(StartSimTimeSeconds, SimTimeSeconds);
}

void USummitWorldClockSubsystem::BroadcastBoundaryCrossings(double OldTime)
{
	// Only the latest boundary is reported; after a long jump listeners should re-query state anyway.
	const int64 OldHourIndex = static_cast<int64>(FMath::FloorToDouble(OldTime / summit::sim::kSecondsPerHour));
	const int64 NewHourIndex = static_cast<int64>(FMath::FloorToDouble(SimTimeSeconds / summit::sim::kSecondsPerHour));
	if (NewHourIndex != OldHourIndex)
	{
		OnHourChanged.Broadcast(FMath::FloorToInt32(GetHourOfDay()));
	}

	const int64 OldDayIndex = static_cast<int64>(FMath::FloorToDouble(OldTime / summit::sim::kSecondsPerDay));
	const int64 NewDayIndex = static_cast<int64>(FMath::FloorToDouble(SimTimeSeconds / summit::sim::kSecondsPerDay));
	if (NewDayIndex != OldDayIndex)
	{
		OnDayChanged.Broadcast(GetDayOfYear());
	}
}
