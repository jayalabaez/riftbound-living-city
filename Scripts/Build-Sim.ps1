<#
.SYNOPSIS
    Builds the LIVING CITY pure-C++ simulation core with no Unreal involvement.

.DESCRIPTION
    This is the fast iteration loop (architecture rule R11). It compiles Sim/ directly with
    cl.exe from Visual Studio Build Tools - no CMake, no Ninja, no package manager, no engine.
    Produces two binaries in Sim/build/<Config>/:

        livingcity_headless.exe   the headless simulation host
        livingcity_tests.exe      the invariant test suite

    LC_TRACK_ALLOCATIONS is defined here and ONLY here. It replaces global operator new so
    invariant I5 (zero steady-state allocation) is measurable. It must never be defined for
    an Unreal build - see DECISIONS.md D-005.

.EXAMPLE
    .\Scripts\Build-Sim.ps1
    .\Scripts\Build-Sim.ps1 -Config Debug
    .\Scripts\Build-Sim.ps1 -Clean
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Release',

    [switch] $Clean,

    # Optional full path to vcvars64.bat. LC_VCVARS64 is the environment equivalent.
    [string] $VcVarsPath = $env:LC_VCVARS64,

    # Escape hatch while integrating; CI always builds with warnings as errors.
    [switch] $NoWarningsAsErrors
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$SimRoot  = Join-Path $RepoRoot 'Sim'
$BuildDir = Join-Path $SimRoot "build\$Config"
$ObjDir   = Join-Path $BuildDir 'obj'

# ---------------------------------------------------------------- locate the toolchain
function Resolve-VcVars([string] $ExplicitPath) {
    if ($ExplicitPath) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "The requested vcvars64.bat does not exist: $ExplicitPath"
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    # Respect the developer shell / CI toolchain already selected by the caller.
    if ($env:VSINSTALLDIR) {
        $selected = Join-Path $env:VSINSTALLDIR 'VC\Auxiliary\Build\vcvars64.bat'
        if (Test-Path -LiteralPath $selected -PathType Leaf) { return $selected }
    }

    # vswhere discovers custom install locations and future VS versions on hosted runners.
    $vswhere = if (${env:ProgramFiles(x86)}) {
        Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    if ($vswhere -and (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        $install = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -ne 0) { throw "vswhere failed (exit $LASTEXITCODE)." }
        foreach ($path in $install) {
            $candidate = Join-Path $path 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        }
    }

    $localFallback = 'C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path -LiteralPath $localFallback -PathType Leaf) { return $localFallback }
    throw 'MSVC x64 tools not found. Install the Visual Studio C++ workload, or set LC_VCVARS64 to vcvars64.bat.'
}
$VcVars = Resolve-VcVars $VcVarsPath

if ($Clean -and (Test-Path -LiteralPath (Join-Path $SimRoot 'build'))) {
    $cleanTarget = (Resolve-Path -LiteralPath (Join-Path $SimRoot 'build')).Path
    $expectedTarget = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot 'Sim\build'))
    if ($cleanTarget -ne $expectedTarget -or
        ((Get-Item -LiteralPath $cleanTarget).Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to clean an unexpected build directory: $cleanTarget"
    }
    Write-Host "Cleaning $cleanTarget"
    Remove-Item -LiteralPath $cleanTarget -Recurse -Force
}

New-Item -ItemType Directory -Force $ObjDir | Out-Null

# ---------------------------------------------------------------- gather sources
function Get-Sources([string] $Dir) {
    if (-not (Test-Path $Dir)) { return @() }
    Get-ChildItem -Path $Dir -Filter '*.cpp' -Recurse -File |
        Sort-Object FullName |          # deterministic link order
        ForEach-Object { $_.FullName }
}

$CoreSrc  = @(Get-Sources (Join-Path $SimRoot 'src'))
$AppSrc   = @(Get-Sources (Join-Path $SimRoot 'apps\headless'))
$TestSrc  = @(Get-Sources (Join-Path $SimRoot 'tests'))

if ($CoreSrc.Count -eq 0) { throw "No sources found under $SimRoot\src" }

# ---------------------------------------------------------------- UBT aggregate
# UBT compiles the sim through ONE generated translation unit under Source/LivingCitySim.
# LivingCitySim.Build.cs writes it too - but only when UBT re-evaluates module rules, which
# it does not do just because a new .cpp appeared under Sim/src. So a new sim source built
# fine here and then vanished from the Unreal link (D-025). This build runs first and globs
# the same sources, so it keeps the aggregate current. Ordinal sort and CRLF match what the
# C# side writes, byte for byte, so the two generators never fight over the file.
$UnityPath = Join-Path $RepoRoot 'Source\LivingCitySim\Private\LivingCitySimUnity.cpp'
$Ordered   = [string[]]$CoreSrc
[Array]::Sort($Ordered, [System.StringComparer]::Ordinal)
$UnityText = "// GENERATED - do not edit. Written by LivingCitySim.Build.cs AND Scripts/Build-Sim.ps1,`r`n" +
             "// byte-identically, so whichever build runs first leaves the file current.`r`n" +
             "// Aggregates the engine-agnostic sim sources from <repo>/Sim/src so that UBT`r`n" +
             "// compiles them without those files having to live under Source/.`r`n`r`n"
$RepoPrefix = [System.IO.Path]::GetFullPath($RepoRoot).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
foreach ($src in $Ordered) {
    $relative = '../../../' + $src.Substring($RepoPrefix.Length).Replace('\', '/')
    $UnityText += ('#include "' + $relative + '"' + "`r`n")
}
New-Item -ItemType Directory -Force (Split-Path $UnityPath) | Out-Null
$Existing = if (Test-Path $UnityPath) { [System.IO.File]::ReadAllText($UnityPath) } else { '' }
if ($Existing -ne $UnityText) {
    [System.IO.File]::WriteAllText($UnityPath, $UnityText, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "  aggregate : regenerated ($($Ordered.Count) sources) -> Source\LivingCitySim\Private\LivingCitySimUnity.cpp"
}

Write-Host "LIVING CITY sim build  [$Config]" -ForegroundColor Cyan
Write-Host "  toolchain : $VcVars"
Write-Host "  core      : $($CoreSrc.Count) file(s)"
Write-Host "  headless  : $($AppSrc.Count) file(s)"
Write-Host "  tests     : $($TestSrc.Count) file(s)"

# ---------------------------------------------------------------- compiler flags
$Common = @(
    '/nologo'
    '/std:c++20'
    '/permissive-'
    '/W4'
    '/EHsc'         # unwinding semantics for the STL; our own code never throws (R3/D-005)
    '/utf-8'
    '/Zi'
    '/FS'
    '/DLC_TRACK_ALLOCATIONS'
    "/I`"$(Join-Path $SimRoot 'include')`""
    "/I`"$(Join-Path $SimRoot 'tests')`""
)
if (-not $NoWarningsAsErrors) { $Common += '/WX' }

if ($Config -eq 'Release') {
    $Common += @('/O2', '/DNDEBUG')
} else {
    $Common += @('/Od', '/RTC1', '/MDd', '/D_DEBUG')
}
if ($Config -eq 'Release') { $Common += '/MD' }

# ---------------------------------------------------------------- emit build batch
function New-BuildBatch {
    param([string] $Name, [string[]] $Sources, [string] $OutExe)

    $objSub = Join-Path $ObjDir $Name
    New-Item -ItemType Directory -Force $objSub | Out-Null

    $lines = @(
        '@echo off'
        "call `"$VcVars`" >nul 2>&1"
        'if errorlevel 1 (echo VCVARS FAILED & exit /b 90)'
    )

    $objs = @()
    foreach ($src in $Sources) {
        $obj = Join-Path $objSub ((Split-Path $src -Leaf) -replace '\.cpp$', '.obj')
        # Distinct .obj names even if two directories hold the same filename.
        if ($objs -contains $obj) {
            $obj = Join-Path $objSub ((Split-Path (Split-Path $src -Parent) -Leaf) + '_' + ((Split-Path $src -Leaf) -replace '\.cpp$', '.obj'))
        }
        $objs += $obj
        $flags = $Common -join ' '
        $lines += "cl $flags /c `"$src`" /Fo`"$obj`" /Fd`"$objSub\vc.pdb`""
        $lines += "if errorlevel 1 (echo COMPILE FAILED: $src & exit /b 91)"
    }

    $objList = ($objs | ForEach-Object { "`"$_`"" }) -join ' '
    $lines += "link /nologo /DEBUG /OUT:`"$OutExe`" $objList"
    $lines += "if errorlevel 1 (echo LINK FAILED & exit /b 92)"
    $lines += 'exit /b 0'

    $bat = Join-Path $BuildDir "_build_$Name.bat"
    $lines | Out-File -Encoding ascii $bat
    return $bat
}

function Invoke-BuildBatch([string] $Bat, [string] $What) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $output = & cmd.exe /c $Bat 2>&1
    $sw.Stop()
    $exit = $LASTEXITCODE

    $output | ForEach-Object {
        if ($_ -match 'error|FAILED') { Write-Host $_ -ForegroundColor Red }
        elseif ($_ -match 'warning')  { Write-Host $_ -ForegroundColor Yellow }
        elseif ($_ -match '\.cpp$')   { }   # cl echoes each filename; too noisy
        else { Write-Host $_ }
    }

    if ($exit -ne 0) {
        Write-Host "$What FAILED in $([math]::Round($sw.Elapsed.TotalSeconds,1))s (exit $exit)" -ForegroundColor Red
        return $false
    }
    Write-Host "$What built in $([math]::Round($sw.Elapsed.TotalSeconds,1))s" -ForegroundColor Green
    return $true
}

# ---------------------------------------------------------------- build
$headlessExe = Join-Path $BuildDir 'livingcity_headless.exe'
$testsExe    = Join-Path $BuildDir 'livingcity_tests.exe'

$okHeadless = $true
$okTests    = $true

if ($AppSrc.Count -gt 0) {
    $bat = New-BuildBatch -Name 'headless' -Sources ($CoreSrc + $AppSrc) -OutExe $headlessExe
    $okHeadless = Invoke-BuildBatch $bat 'livingcity_headless'
}

if ($TestSrc.Count -gt 0) {
    $bat = New-BuildBatch -Name 'tests' -Sources ($CoreSrc + $TestSrc) -OutExe $testsExe
    $okTests = Invoke-BuildBatch $bat 'livingcity_tests'
}

if (-not ($okHeadless -and $okTests)) {
    Write-Host "`nBUILD FAILED" -ForegroundColor Red
    exit 1
}

Write-Host "`nOutputs:" -ForegroundColor Cyan
if (Test-Path $headlessExe) { Write-Host "  $headlessExe" }
if (Test-Path $testsExe)    { Write-Host "  $testsExe" }
exit 0
