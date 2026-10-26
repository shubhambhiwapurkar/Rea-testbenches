// Engine-independent simulation core. Sources under Public/Sim and Private/Sim are pure C++17 and are
// also built standalone by the root CMakeLists.txt for fast unit testing.

using UnrealBuildTool;

public class SummitSim : ModuleRules
{
	public SummitSim(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.NoPCHs;
		bEnableExceptions = false;
		bUseRTTI = false;

		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
	}
}
