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
                "MassCore"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                "MassCommon",
                "MassEntity",
                "MassMovement",
                "MassNavigation",
                "NavigationSystem"
            }
        );
    }
}
