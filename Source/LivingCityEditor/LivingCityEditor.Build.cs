// LIVING CITY — editor-only tools module.
//
// Commandlets, validators, and generators that must not ship in the game.
// Depends on LivingCityGame and LivingCitySim; never on Riftbound.

using UnrealBuildTool;

public class LivingCityEditor : ModuleRules
{
    public LivingCityEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "UnrealEd",
            "LivingCitySim",
            "LivingCityGame",
        });
    }
}
