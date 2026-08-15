using UnrealBuildTool;

public class AlgonaSimulation : ModuleRules
{
    public AlgonaSimulation(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "MassCore",
                "MassEntity",
                "MassRepresentation",
                "MassLOD",
                "MassSpawner"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "MassActors"
            }
        );
    }
}