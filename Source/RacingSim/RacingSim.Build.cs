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

		// racing wheels and pedals (DirectInput; XInput pads go through Unreal's own input)
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.AddRange(new string[] { "dinput8.lib", "dxguid.lib" });
		}
	}
}
