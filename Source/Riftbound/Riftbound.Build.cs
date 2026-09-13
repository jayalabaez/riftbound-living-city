using UnrealBuildTool;
public class Riftbound : ModuleRules
{
    public Riftbound(ReadOnlyTargetRules Target) : base(Target)
    {
        // Procedural generators intentionally keep local helpers in separate translation units.
        bUseUnity = false;
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "UMG", "Slate", "SlateCore", "ProceduralMeshComponent" });
        PrivateDependencyModuleNames.AddRange(new string[] { "HTTP", "Json", "LivingCitySim", "RHI", "RenderCore", "PhysicsCore" });
    }
}
