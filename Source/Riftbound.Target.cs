using UnrealBuildTool;
public class RiftboundTarget : TargetRules
{
    public RiftboundTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("Riftbound");

        // LIVING CITY runtime modules. Riftbound and LivingCity share this .uproject and
        // nothing else - neither references the other (docs/ARCHITECTURE.md section 2).
        ExtraModuleNames.Add("LivingCitySim");
        ExtraModuleNames.Add("LivingCityGame");
    }
}
