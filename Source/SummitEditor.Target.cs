using UnrealBuildTool;
using System.Collections.Generic;

public class SummitEditorTarget : TargetRules
{
	public SummitEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "Summit", "SummitSim", "SummitEditor" });
	}
}
