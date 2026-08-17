using UnrealBuildTool;

public class AlgonaRTSTarget : TargetRules
{
	public AlgonaRTSTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;

		ExtraModuleNames.AddRange(new[]
		{
			"AlgonaRTS",
			"AlgonaSimulation"
		});
	}
}