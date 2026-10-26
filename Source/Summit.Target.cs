using UnrealBuildTool;
using System.Collections.Generic;

public class SummitTarget : TargetRules
{
	public SummitTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "Summit", "SummitSim" });
	}
}
