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
	}
}