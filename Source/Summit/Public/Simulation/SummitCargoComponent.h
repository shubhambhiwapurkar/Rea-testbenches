// The climber's load: pack contents and lashed-on gear.
//
// Pure gameplay data (no sim object of its own). The balance component turns the item list into
// summit::sim::CargoItem for the CoM model, and the climber component reads the total mass for Pandolf
// load-carriage cost. Mass and placement both matter, Death Stranding style: a heavy item hung off one
// side biases the lean the player must hold.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Data/SummitTypes.h"
#include "SummitCargoComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSummitCargoChangedSignature);

UCLASS(ClassGroup = (Summit), meta = (BlueprintSpawnableComponent))
class SUMMIT_API USummitCargoComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USummitCargoComponent();

	// Items on the pack. Edit in the character Blueprint to define a starting loadout.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Summit|Cargo")
	TArray<FSummitCargoItem> Items;

	// Mass of the empty pack itself (frame, lid, straps).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Cargo", meta = (ClampMin = "0", Units = "Kilograms"))
	float EmptyPackMassKg = 2.5f;

	// Pack capacity. Expedition packs are 65-90 L; anything beyond is lashed outside (unstrapped-ish).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Summit|Cargo", meta = (ClampMin = "0"))
	float MaxVolumeL = 75.f;

	UPROPERTY(BlueprintAssignable, Category = "Summit|Cargo")
	FSummitCargoChangedSignature OnCargoChanged;

	// Adds an item. Fails (returns false) if it does not fit in the remaining volume.
	UFUNCTION(BlueprintCallable, Category = "Summit|Cargo")
	bool AddItem(const FSummitCargoItem& Item);

	// Removes the first item with this id. Returns false if not carried.
	UFUNCTION(BlueprintCallable, Category = "Summit|Cargo")
	bool RemoveItem(FName ItemId, FSummitCargoItem& OutItem);

	UFUNCTION(BlueprintCallable, Category = "Summit|Cargo")
	bool SetItemStrapped(FName ItemId, bool bStrapped);

	// Repacks for best carry: heavy items close to the spine at shoulder-blade height, alternating sides
	// to cancel lateral moment. Standard load-carriage advice (and what porters do instinctively).
	UFUNCTION(BlueprintCallable, Category = "Summit|Cargo")
	void Rebalance();

	// Pack contents plus the empty pack.
	UFUNCTION(BlueprintPure, Category = "Summit|Cargo")
	float GetTotalMassKg() const;

	UFUNCTION(BlueprintPure, Category = "Summit|Cargo")
	float GetUsedVolumeL() const;

	// Sum of mass x lateral offset (kg·m, + = right). Zero means a laterally balanced pack.
	UFUNCTION(BlueprintPure, Category = "Summit|Cargo")
	float GetLateralMomentKgM() const;

	// Takes up to RequestedKcal of food from the pack, reducing item mass. Returns kcal actually obtained.
	UFUNCTION(BlueprintCallable, Category = "Summit|Cargo")
	float ConsumeFood(float RequestedKcal);

	// Takes up to RequestedL of water (bottles/thermos), reducing item mass. Returns litres obtained.
	UFUNCTION(BlueprintCallable, Category = "Summit|Cargo")
	float ConsumeWater(float RequestedL);

	// Call after editing Items directly from Blueprint.
	UFUNCTION(BlueprintCallable, Category = "Summit|Cargo")
	void NotifyCargoChanged();

private:
	int32 FindItemIndex(FName ItemId) const;
};
