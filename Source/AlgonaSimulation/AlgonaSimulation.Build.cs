using UnrealBuildTool;

public class AlgonaSimulation : ModuleRules
{
	public AlgonaSimulation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Stable Simulation API: Mass state remains the public module boundary.
		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"MassCore",
			"MassEntity",
			"MassSpawner"
		});

		// UE navigation is an implementation detail of squad command processing.
		PrivateDependencyModuleNames.Add("NavigationSystem");
	}
}
