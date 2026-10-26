// Gameplay module. Wraps the engine-independent SummitSim core (pure C++17) in components and subsystems.

using UnrealBuildTool;

public class Summit : ModuleRules
{
	public Summit(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bEnableExceptions = false;

		// Includes are written relative to Public/ and Private/ (e.g. "Character/SummitCharacter.h",
		// "Data/SummitSimConversions.h"); UBT adds both folders by default. SummitSim's Public/ makes the
		// simulation core available as "Sim/Climber.h" etc. Sim headers are only included from .cpp files
		// (and the private Data/SummitSimConversions.h) so they never reach reflected headers.

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"UMG",
			"PhysicsCore",
			"DeveloperSettings",
			"CableComponent",
			"SummitSim"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"Json",
			"JsonUtilities",
			"Niagara",
			"MotionWarping",
			"Projects"
		});
	}
}
