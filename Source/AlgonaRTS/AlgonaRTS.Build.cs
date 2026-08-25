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

		// P1 currently injects the proven GPU interpolation WPO into transient
		// materials in Editor builds. A cooked production material path can
		// replace this bridge later without changing Simulation or visibility.
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("MaterialEditor");
		}
	}
}
