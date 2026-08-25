using UnrealBuildTool;

public class AlgonaSimulation : ModuleRules
{
	public AlgonaSimulation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"MassCore",
			"MassEntity",
			"MassSpawner"
		});
	}
}
