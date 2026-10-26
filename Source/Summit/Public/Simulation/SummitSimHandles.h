// Owning handles for summit::sim objects held by UObjects.
//
// Gameplay headers forward-declare the sim classes instead of including them, so the C++17 sim headers
// (and <vector>/<string>) stay out of reflected headers. A plain TUniquePtr<Incomplete> is not enough:
// UHT generates constructors (e.g. the vtable-helper constructor) in .gen.cpp files where the type is
// still incomplete, and a constructor potentially invokes member destructors. The deleter below is a
// complete type whose call operators are defined out of line in SummitSimHandles.cpp, so the delete
// expression is only ever compiled where the sim type is complete.

#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

namespace summit::sim
{
	class Climber;
	class WeatherSystem;
	class SnowpackModel;
	class BalanceModel;
	class Pcg32;
}

struct SUMMIT_API FSummitSimDeleter
{
	void operator()(summit::sim::Climber* Ptr) const;
	void operator()(summit::sim::WeatherSystem* Ptr) const;
	void operator()(summit::sim::SnowpackModel* Ptr) const;
	void operator()(summit::sim::BalanceModel* Ptr) const;
	void operator()(summit::sim::Pcg32* Ptr) const;
};

// Usage: TSimPtr<summit::sim::Climber> Climber; Climber.Reset(new summit::sim::Climber(Profile));
template <typename T>
using TSimPtr = TUniquePtr<T, FSummitSimDeleter>;
