using UnrealBuildTool;

public class AlgonaRTSEditorTarget : TargetRules
{
	public AlgonaRTSEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;

		ExtraModuleNames.AddRange(new[]
		{
			"AlgonaRTS",
			"AlgonaSimulation"
		});
	}
}
