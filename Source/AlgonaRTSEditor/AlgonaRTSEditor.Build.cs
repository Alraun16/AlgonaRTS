using UnrealBuildTool;

public class AlgonaRTSEditor : ModuleRules
{
	public AlgonaRTSEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Редакторские инструменты проекта. Модуль не входит в Game target,
		// поэтому игровой код никогда не зависит от MaterialEditor/UnrealEd.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"MaterialEditor",
			"AssetRegistry",
			"AlgonaRTS"
		});
	}
}
