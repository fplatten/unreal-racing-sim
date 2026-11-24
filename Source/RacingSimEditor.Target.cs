using UnrealBuildTool;

public class RacingSimEditorTarget : TargetRules
{
	public RacingSimEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("RacingSim");
	}
}
