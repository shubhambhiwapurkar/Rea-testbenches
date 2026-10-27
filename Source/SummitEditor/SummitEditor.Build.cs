// Editor-only tooling: the SummitSetup commandlet that builds maps and content from Tools/ outputs.

using UnrealBuildTool;

public class SummitEditor : ModuleRules
{
	public SummitEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "UnrealEd", "Landscape", "MaterialEditor", "Json", "Summit"
		});
	}
}
