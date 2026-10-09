using UnrealBuildTool;

public class SpeechGenRadio : ModuleRules
{
    public SpeechGenRadio(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "AudioMixer",
            "SpeechGen",
            "Synthesis"
        });
    }
}
