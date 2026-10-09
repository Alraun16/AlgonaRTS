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

		// Пути центров Squad по навигационной карте мира (P2, шаг 16).
		PrivateDependencyModuleNames.Add("NavigationSystem");
	}
}
