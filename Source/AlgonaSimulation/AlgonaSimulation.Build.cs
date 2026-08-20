using UnrealBuildTool;

public class AlgonaSimulation : ModuleRules
{
	public AlgonaSimulation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		/*
		 * AlgonaSoldierFragments.h — public header и напрямую использует
		 * Mass/EntityElementTypes.h, поэтому MassCore тоже public dependency.
		 */
		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"MassCore",
			"MassEntity",
			"MassSpawner"
		});

		// Нужны только пока существует старый Mass Presentation.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"MassRepresentation",
			"MassLOD",
			"MassActors"
		});
	}
}