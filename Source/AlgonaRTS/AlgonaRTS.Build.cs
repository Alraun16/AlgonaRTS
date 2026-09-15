using UnrealBuildTool;

public class AlgonaRTS : ModuleRules
{
	public AlgonaRTS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AlgonaSimulation"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"InputCore"
		});

		// GPU interpolation materials are authored ahead of time by the
		// AlgonaRTSEditor module. The game module only loads ready assets.
	}
}
