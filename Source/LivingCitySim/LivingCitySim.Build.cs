// LIVING CITY — UBT wrapper around the engine-agnostic simulation core.
//
// The real sources live in <repo>/Sim, deliberately OUTSIDE Source/, so that architecture
// rule R1 is visible in the directory tree itself: nothing under Sim/ knows Unreal exists.
// See DECISIONS.md D-005 for why the same sources are compiled twice (once by cl.exe for
// the headless/test build, once by UBT here) rather than linked as a prebuilt static library.
//
// NOTE ON R1's LETTER VS ITS SPIRIT: this file lists "Core" as a dependency because UBT
// requires every module to have one. No source file under Sim/ includes any Unreal header —
// that is the invariant that matters (I2), and Scripts/Check-SimPurity.ps1 enforces it by
// grepping the .cpp/.h files, not this .Build.cs.

using UnrealBuildTool;
using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Collections.Generic;

public class LivingCitySim : ModuleRules
{
    public LivingCitySim(ReadOnlyTargetRules Target) : base(Target)
    {
        // The sim core is plain C++20. No PCH, no unity from UBT — we build our own
        // aggregate below so UBT compiles sources that live outside the module directory.
        PCHUsage = PCHUsageMode.NoPCHs;
        bUseUnity = false;
        CppStandard = CppStandardVersion.Cpp20;

        // Required by UBT. No sim source includes an Unreal header.
        PublicDependencyModuleNames.Add("Core");

        string RepoRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
        string SimInclude = Path.Combine(RepoRoot, "Sim", "include");
        string SimSrc = Path.Combine(RepoRoot, "Sim", "src");

        PublicIncludePaths.Add(SimInclude);

        // In an Unreal EDITOR build every module is a separate DLL, and nothing crosses a
        // DLL boundary unless it is explicitly exported. Without these two defines the
        // sim compiles perfectly, produces a DLL that exports nothing at all, and every
        // out-of-line sim function becomes an unresolved external in LivingCityGame.
        //
        //   LC_SHARED_BUILD is PUBLIC  -> consumers see LC_API as __declspec(dllimport)
        //   LC_EXPORTS      is PRIVATE -> this module sees LC_API as __declspec(dllexport)
        //
        // Neither is defined by Scripts/Build-Sim.ps1, so LC_API vanishes in the standalone
        // build. See Sim/include/livingcity/core/Core.h and DECISIONS.md D-015.
        PublicDefinitions.Add("LC_SHARED_BUILD=1");
        PrivateDefinitions.Add("LC_EXPORTS=1");

        // C4251: "class X needs to have dll-interface to be used by clients of class Y",
        // raised for every std::vector/std::string member of an exported class. It is
        // advisory here rather than dangerous: UBT compiles every module in this build with
        // one identical toolchain and CRT, so there is no allocator mismatch to warn about.
        // Suppressed in code, because ModuleRules has no portable per-warning switch.

        // LC_TRACK_ALLOCATIONS is deliberately NOT defined here. It replaces global
        // operator new, which must never happen inside an Unreal module — UE owns the
        // allocator. Only Scripts/Build-Sim.ps1 defines it.

        GenerateUnitySource(SimSrc);
    }

    // UBT compiles .cpp files found under the module directory. The sim sources are not
    // there, so we generate one aggregate translation unit inside Private/ that #includes
    // them. Regenerated when UBT evaluates these rules; Build-Sim.ps1 also refreshes it
    // before UBT so newly added sources cannot be lost to cached module rules (D-025).
    private void GenerateUnitySource(string SimSrc)
    {
        string PrivateDir = Path.Combine(ModuleDirectory, "Private");
        Directory.CreateDirectory(PrivateDir);
        string OutPath = Path.Combine(PrivateDir, "LivingCitySimUnity.cpp");

        var Builder = new StringBuilder();
        Builder.AppendLine("// GENERATED - do not edit. Written by LivingCitySim.Build.cs AND Scripts/Build-Sim.ps1,");
        Builder.AppendLine("// byte-identically, so whichever build runs first leaves the file current.");
        Builder.AppendLine("// Aggregates the engine-agnostic sim sources from <repo>/Sim/src so that UBT");
        Builder.AppendLine("// compiles them without those files having to live under Source/.");
        Builder.AppendLine();

        if (Directory.Exists(SimSrc))
        {
            // Sorted for a deterministic translation unit — the same discipline we apply
            // to the simulation applies to the build that produces it.
            var Files = Directory.GetFiles(SimSrc, "*.cpp", SearchOption.AllDirectories)
                                 .OrderBy(f => f, StringComparer.Ordinal)
                                 .ToList();

            foreach (string File in Files)
            {
                string Relative = Path.GetRelativePath(PrivateDir, File).Replace('\\', '/');
                Builder.AppendLine("#include \"" + Relative + "\"");
            }

            if (Files.Count == 0)
            {
                Builder.AppendLine("// (no sim sources found yet)");
            }
        }
        else
        {
            Builder.AppendLine("// (Sim/src not found: " + SimSrc.Replace('\\', '/') + ")");
        }

        // Both generators use CRLF and UTF-8 without a BOM, independent of the host.
        string Contents = Builder.ToString().Replace("\r\n", "\n").Replace("\n", "\r\n");

        // Only rewrite when the content actually changes, so an unchanged build does not
        // dirty the file and force a needless recompile.
        if (!File.Exists(OutPath) || File.ReadAllText(OutPath) != Contents)
        {
            File.WriteAllText(OutPath, Contents, new UTF8Encoding(false));
        }
    }
}
