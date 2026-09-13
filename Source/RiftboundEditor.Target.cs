using UnrealBuildTool;
public class RiftboundEditorTarget : TargetRules
{
    public RiftboundEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("Riftbound");

        // LIVING CITY modules. Riftbound and LivingCity share this .uproject and nothing
        // else - neither references the other (docs/ARCHITECTURE.md section 2).
        ExtraModuleNames.Add("LivingCitySim");
        ExtraModuleNames.Add("LivingCityGame");
        ExtraModuleNames.Add("LivingCityEditor");
    }
}
