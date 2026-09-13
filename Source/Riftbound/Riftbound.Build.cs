using UnrealBuildTool;
public class Riftbound : ModuleRules
{
    public Riftbound(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "UMG", "Slate", "SlateCore", "ProceduralMeshComponent" });
        PrivateDependencyModuleNames.AddRange(new string[] { "HTTP", "Json" });
    }
}
