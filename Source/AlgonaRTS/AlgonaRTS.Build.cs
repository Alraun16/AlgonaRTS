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

		// Backend 3 builds a transient test material graph in Editor PIE so the
		// interpolation can execute in the vertex shader without requiring a
		// hand-authored .uasset just for this experiment.
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("MaterialEditor");
		}
	}
}
