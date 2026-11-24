using UnrealBuildTool;

public class RacingSimTarget : TargetRules
{
	public RacingSimTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("RacingSim");
	}
}
