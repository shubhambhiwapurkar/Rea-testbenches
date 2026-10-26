#include "Simulation/SummitCargoComponent.h"

namespace
{
	// Typical food energy density of expedition rations (~4.5 kcal/g incl. packaging overhead).
	constexpr float kFoodKcalPerKg = 4500.f;
	constexpr float kWaterKgPerL = 1.f;
}

USummitCargoComponent::USummitCargoComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

int32 USummitCargoComponent::FindItemIndex(FName ItemId) const
{
	return Items.IndexOfByPredicate([ItemId](const FSummitCargoItem& Item) { return Item.Id == ItemId; });
}

bool USummitCargoComponent::AddItem(const FSummitCargoItem& Item)
{
	if (GetUsedVolumeL() + Item.VolumeL > MaxVolumeL)
	{
		return false;
	}
	Items.Add(Item);
	NotifyCargoChanged();
	return true;
}

bool USummitCargoComponent::RemoveItem(FName ItemId, FSummitCargoItem& OutItem)
{
	const int32 Index = FindItemIndex(ItemId);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	OutItem = Items[Index];
	Items.RemoveAt(Index);
	NotifyCargoChanged();
	return true;
}

bool USummitCargoComponent::SetItemStrapped(FName ItemId, bool bStrapped)
{
	const int32 Index = FindItemIndex(ItemId);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	Items[Index].bStrapped = bStrapped;
	NotifyCargoChanged();
	return true;
}

void USummitCargoComponent::Rebalance()
{
	// Heaviest first so they claim the slots nearest the spine.
	Items.Sort([](const FSummitCargoItem& A, const FSummitCargoItem& B) { return A.MassKg > B.MassKg; });

	// Offsets are from the pack attachment point at the upper back (cm, actor-local: X fwd, Y right, Z up).
	// Heavy items: close to the back (X ~ -15) and slightly below the attachment point (shoulder blades).
	// Light items: further out and higher/lower where they matter less for CoM.
	double LateralMoment = 0.0;
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		FSummitCargoItem& Item = Items[Index];
		const float Rank01 = Items.Num() > 1 ? static_cast<float>(Index) / static_cast<float>(Items.Num() - 1) : 0.f;

		const float Depth = FMath::Lerp(-15.f, -32.f, Rank01);
		const float Height = FMath::Lerp(-10.f, 15.f, Rank01);
		const float SideMagnitude = FMath::Lerp(3.f, 12.f, Rank01);

		// Greedy: put each item on the side that drives the running lateral moment toward zero.
		const float Side = LateralMoment > 0.0 ? -1.f : 1.f;
		Item.AttachOffsetCm = FVector(Depth, Side * SideMagnitude, Height);
		Item.bStrapped = true;
		LateralMoment += static_cast<double>(Item.MassKg) * Item.AttachOffsetCm.Y;
	}
	NotifyCargoChanged();
}

float USummitCargoComponent::GetTotalMassKg() const
{
	float Total = EmptyPackMassKg;
	for (const FSummitCargoItem& Item : Items)
	{
		Total += FMath::Max(0.f, Item.MassKg);
	}
	return Total;
}

float USummitCargoComponent::GetUsedVolumeL() const
{
	float Total = 0.f;
	for (const FSummitCargoItem& Item : Items)
	{
		Total += FMath::Max(0.f, Item.VolumeL);
	}
	return Total;
}

float USummitCargoComponent::GetLateralMomentKgM() const
{
	float Moment = 0.f;
	for (const FSummitCargoItem& Item : Items)
	{
		Moment += Item.MassKg * static_cast<float>(Item.AttachOffsetCm.Y) / 100.f;
	}
	return Moment;
}

float USummitCargoComponent::ConsumeFood(float RequestedKcal)
{
	float Obtained = 0.f;
	for (int32 Index = Items.Num() - 1; Index >= 0 && Obtained < RequestedKcal; --Index)
	{
		FSummitCargoItem& Item = Items[Index];
		if (Item.FoodKcal <= 0.f)
		{
			continue;
		}
		const float Take = FMath::Min(Item.FoodKcal, RequestedKcal - Obtained);
		Item.FoodKcal -= Take;
		Item.MassKg = FMath::Max(0.f, Item.MassKg - Take / kFoodKcalPerKg);
		Obtained += Take;
		if (Item.FoodKcal <= KINDA_SMALL_NUMBER && Item.WaterL <= KINDA_SMALL_NUMBER && Item.MassKg < 0.05f)
		{
			Items.RemoveAt(Index);
		}
	}
	if (Obtained > 0.f)
	{
		NotifyCargoChanged();
	}
	return Obtained;
}

float USummitCargoComponent::ConsumeWater(float RequestedL)
{
	float Obtained = 0.f;
	for (int32 Index = Items.Num() - 1; Index >= 0 && Obtained < RequestedL; --Index)
	{
		FSummitCargoItem& Item = Items[Index];
		if (Item.WaterL <= 0.f)
		{
			continue;
		}
		const float Take = FMath::Min(Item.WaterL, RequestedL - Obtained);
		Item.WaterL -= Take;
		Item.MassKg = FMath::Max(0.f, Item.MassKg - Take * kWaterKgPerL);
		Obtained += Take;
		// Empty bottles stay in the pack (you melt snow into them at camp).
	}
	if (Obtained > 0.f)
	{
		NotifyCargoChanged();
	}
	return Obtained;
}

void USummitCargoComponent::NotifyCargoChanged()
{
	OnCargoChanged.Broadcast();
}
