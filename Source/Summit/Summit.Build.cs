// Gameplay module. Wraps the engine-independent SummitSim core (pure C++17) in components and subsystems.

using UnrealBuildTool;

public class Summit : ModuleRules
{
	public Summit(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bEnableExceptions = false;

		// Lets "Character/SummitCharacter.h" style includes resolve from both Public and Private.
		PublicIncludePaths.Add(ModuleDirectory + "/Public");
		PrivateIncludePaths.Add(ModuleDirectory + "/Private");

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
