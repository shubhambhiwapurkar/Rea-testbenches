#include "Data/SummitPhysicalMaterial.h"

USummitPhysicalMaterial::USummitPhysicalMaterial()
{
	// Friction here only affects rigid bodies (dropped gear, ragdolls); the character's footing comes from
	// summit::sim::SurfaceFriction, which knows about crampons and temperature.
	Friction = 0.7f;
}
