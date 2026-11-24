using UnrealBuildTool;

public class RacingSim : ModuleRules
{
	public RacingSim(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// includes are written relative to the module folder ("Sim/...", "Track/...")
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "InputCore",
			"PhysicsCore", "Chaos", "ProceduralMeshComponent", "Slate", "SlateCore", "RenderCore", "RHI", "AudioExtensions"
		});
	}
}
