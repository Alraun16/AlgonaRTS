using UnrealBuildTool;

public class AlgonaSimulation : ModuleRules
{
	public AlgonaSimulation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Используются public headers модуля.
		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"MassEntity",
			"MassSpawner"
		});

		// Нужны только текущей private implementation.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"MassCore",
			"MassRepresentation",
			"MassLOD",
			"MassActors"
		});
	}
}