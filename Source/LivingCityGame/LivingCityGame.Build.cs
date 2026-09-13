// LIVING CITY — Unreal runtime module.
//
// This is the ONLY LivingCity module allowed to touch Unreal. It reads simulation state
// and draws it; it pushes player input into the sim as commands. Nothing more.
// If gameplay logic ends up in an AActor here, it is in the wrong place (rule R2).
//
// Dependency direction (docs/ARCHITECTURE.md section 2):
//     LivingCityGame  ->  LivingCitySim        (allowed, one way only)
//     LivingCityGame  ->  Riftbound            (FORBIDDEN — enforced by Check-SimPurity.ps1)

using UnrealBuildTool;

public class LivingCityGame : ModuleRules
{
    public LivingCityGame(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "LivingCitySim",
            // ALivingCityTerrainView draws the heightfield through UProceduralMeshComponent.
            // The plugin is already enabled in Riftbound.uproject.
            "ProceduralMeshComponent",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate",
            "SlateCore",
            "RenderCore",
        });
    }
}
