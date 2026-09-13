<#
.SYNOPSIS
    Enforces the LIVING CITY architecture rules that are easy to write down and easy to violate.

.DESCRIPTION
    Boundaries that are only written in a document get violated. This script is the
    enforcement half of docs/ARCHITECTURE.md section 5. It runs in CI on every commit.

    Checks:
      I2   no Unreal type appears anywhere in the sim core
      I8   no bIsPlayer / isPlayer branch in any sim system (the player IS an NPC)
      I9   no file IO or HTTP in the sim core (the headless app owns all IO)
      I12  no Chaos or Mass symbol reaches sim state
      R3   no float/double, no wall-clock, and no unordered-container iteration in the sim
      A3   the internal layer DAG holds - no cycles, no upward dependencies
      A2   Riftbound and LivingCity* modules never reference each other

    Exit code 0 = clean, 1 = violations found.

    NOTE ON ENCODING: this file must stay PURE ASCII. Windows PowerShell 5.1 decodes a
    .ps1 without a BOM as Windows-1252, so a UTF-8 em-dash (E2 80 94) becomes the three
    characters a-hat, euro, right-curly-quote - and 5.1 accepts a curly quote as a string
    delimiter, which silently closes a string early and produces baffling parse errors.
#>
[CmdletBinding()]
param(
    [switch] $Quiet
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$SimRoot  = Join-Path $RepoRoot 'Sim'

$script:Violations = @()

function Add-Violation {
    param([string] $Rule, [string] $File, [int] $Line, [string] $Detail)
    $rel = $File.Replace($RepoRoot, '').TrimStart('\')
    $script:Violations += [pscustomobject]@{
        Rule = $Rule; File = $rel; Line = $Line; Detail = $Detail
    }
}

function Get-SimFiles {
    param([string[]] $SubDirs)
    $files = @()
    foreach ($sd in $SubDirs) {
        $p = Join-Path $SimRoot $sd
        if (Test-Path $p) {
            $files += Get-ChildItem -Path $p -Include '*.cpp', '*.h', '*.hpp', '*.inl' -Recurse -File
        }
    }
    return $files
}

# Strip block comments, line comments and string literals, so a rule name mentioned in a
# comment is not reported as a violation of itself.
function Get-CodeLines {
    param([string] $Path)
    $raw = @(Get-Content -Path $Path -ErrorAction SilentlyContinue)
    if ($raw.Count -eq 0) { return @() }

    $inBlock = $false
    $out = @()
    for ($i = 0; $i -lt $raw.Count; $i++) {
        $line = $raw[$i]
        if ($inBlock) {
            $end = $line.IndexOf('*/')
            if ($end -ge 0) { $line = $line.Substring($end + 2); $inBlock = $false }
            else { $out += ''; continue }
        }
        $bs = $line.IndexOf('/*')
        while ($bs -ge 0) {
            $be = $line.IndexOf('*/', $bs + 2)
            if ($be -ge 0) { $line = $line.Substring(0, $bs) + $line.Substring($be + 2) }
            else { $line = $line.Substring(0, $bs); $inBlock = $true; break }
            $bs = $line.IndexOf('/*')
        }
        $sl = $line.IndexOf('//')
        if ($sl -ge 0) { $line = $line.Substring(0, $sl) }
        $line = [regex]::Replace($line, '"(\\.|[^"\\])*"', '""')
        $out += $line
    }
    return $out
}

function Test-Pattern {
    param(
        [object[]] $Files,
        [string]   $Pattern,
        [string]   $Rule,
        [string]   $Detail,
        [string[]] $AllowFiles = @()
    )
    foreach ($f in $Files) {
        if ($AllowFiles -contains $f.Name) { continue }
        $lines = @(Get-CodeLines $f.FullName)
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match $Pattern) {
                Add-Violation -Rule $Rule -File $f.FullName -Line ($i + 1) -Detail "$Detail  ->  $($lines[$i].Trim())"
            }
        }
    }
}

# ============================================================ the sim core proper
# Sim/apps is the headless application: it owns file IO and may use floats for reporting.
$CoreFiles = @(Get-SimFiles @('src', 'include'))
$TestFiles = @(Get-SimFiles @('tests'))

if ($CoreFiles.Count -eq 0) {
    Write-Host "Check-SimPurity: no sim sources found yet - nothing to check." -ForegroundColor Yellow
    exit 0
}

$AllSim = $CoreFiles + $TestFiles

# ---- I2: zero Unreal -------------------------------------------------------
$unreal = '\b(UObject|AActor|UActorComponent|FVector|FRotator|FTransform|FString|FName|TArray|TMap|TSet|TSharedPtr|TWeakObjectPtr|UPROPERTY|UFUNCTION|UCLASS|USTRUCT|GENERATED_BODY|UE_LOG|FMath|UWorld)\b'
Test-Pattern -Files $AllSim -Pattern $unreal -Rule 'I2' -Detail 'Unreal type in the engine-agnostic sim core'
Test-Pattern -Files $AllSim -Pattern '#\s*include\s*[<"](Engine|CoreMinimal|Runtime/|GameFramework)' -Rule 'I2' -Detail 'Unreal header included by the sim core'

# ---- I8: the player is an NPC ---------------------------------------------
Test-Pattern -Files $AllSim -Pattern '\b(bIsPlayer|isPlayer)\b' -Rule 'I8' -Detail 'player special-case inside a simulation system'

# ---- I9: no IO in the sim core --------------------------------------------
Test-Pattern -Files $CoreFiles -Pattern '\b(fopen|fopen_s|ifstream|ofstream|fstream|CreateFileA|CreateFileW|curl_|WSAStartup)\b' -Rule 'I9' -Detail 'file or network IO in the sim core (the headless app owns IO)'
Test-Pattern -Files $CoreFiles -Pattern '#\s*include\s*<(fstream|filesystem)>' -Rule 'I9' -Detail 'IO header in the sim core'

# ---- I12: Chaos and Mass never touch sim state ----------------------------
Test-Pattern -Files $AllSim -Pattern '\b(Chaos::|FGeometryCollection|FMassEntity|UMass|FMassArchetype|ZoneGraph)\b' -Rule 'I12' -Detail 'physics or Mass symbol in the sim core'

# ---- R3: determinism hazards ----------------------------------------------
# Floats are banned in the sim core. Sim/apps may use them for reporting only.
Test-Pattern -Files $CoreFiles -Pattern '(^|[^\w.])(float|double)\s+[A-Za-z_]' -Rule 'R3' -Detail 'floating point in the sim core - use integers or Fixed'
Test-Pattern -Files $TestFiles -Pattern '(^|[^\w.])(float|double)\s+[A-Za-z_]' -Rule 'R3' -Detail 'floating point in a sim test'

# Wall-clock is legitimate in exactly one place: the thread host that SCHEDULES ticks.
Test-Pattern -Files $CoreFiles -Pattern '(#\s*include\s*<chrono>|std::chrono)' -Rule 'R3' -Detail 'wall-clock read in the sim core' -AllowFiles @('Sim.cpp')

Test-Pattern -Files $AllSim -Pattern '\bstd::unordered_(map|set|multimap|multiset)\b' -Rule 'R3' -Detail 'unordered container - iteration order is not deterministic'

# Exceptions and RTTI are disabled in the Unreal build; the sim must not need them.
Test-Pattern -Files $CoreFiles -Pattern '(^|[^\w])throw\s' -Rule 'D005' -Detail 'exceptions in the sim core (disabled under UBT)'
Test-Pattern -Files $CoreFiles -Pattern '\b(dynamic_cast|typeid)\b' -Rule 'D005' -Detail 'RTTI in the sim core (disabled under UBT)'

# A test file must not define main(); the runner supplies it.
foreach ($f in $TestFiles) {
    if ($f.Name -eq 'TestMain.cpp') { continue }
    $lines = @(Get-CodeLines $f.FullName)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*int\s+main\s*\(') {
            Add-Violation -Rule 'TEST' -File $f.FullName -Line ($i + 1) -Detail 'test file defines main(); the runner supplies it'
        }
    }
}

# ============================================================ layer DAG (ARCHITECTURE section 3)
$Allowed = @{
    'core'      = @('core')
    'world'     = @('core', 'world')
    'agents'    = @('core', 'world', 'agents')
    'economy'   = @('core', 'world', 'agents', 'economy')
    'transport' = @('core', 'world', 'agents', 'transport')
    'law'       = @('core', 'world', 'agents', 'economy', 'transport', 'law')
    'destruct'  = @('core', 'world', 'economy', 'destruct')
    'director'  = @('core', 'director')
    'sim'       = @('core', 'world', 'agents', 'economy', 'transport', 'law', 'destruct', 'director', 'sim')
}

foreach ($f in $CoreFiles) {
    $rel = $f.FullName.Replace($SimRoot, '').TrimStart('\')
    $layer = $null
    if ($rel -match '^src\\([a-z]+)\\') { $layer = $Matches[1] }
    elseif ($rel -match '^include\\livingcity\\([a-z]+)\\') { $layer = $Matches[1] }
    if (-not $layer -or -not $Allowed.ContainsKey($layer)) { continue }

    $lines = @(Get-CodeLines $f.FullName)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '#\s*include\s*"livingcity/([a-z]+)/') {
            $dep = $Matches[1]
            if ($Allowed.ContainsKey($dep) -and ($Allowed[$layer] -notcontains $dep)) {
                Add-Violation -Rule 'ARCH3' -File $f.FullName -Line ($i + 1) `
                    -Detail "layer '$layer' may not depend on '$dep' (docs/ARCHITECTURE.md section 3)"
            }
        }
    }
}

# ============================================================ module isolation (section 2)
$SourceDir = Join-Path $RepoRoot 'Source'
if (Test-Path $SourceDir) {
    $BuildCs = @(Get-ChildItem -Path $SourceDir -Filter '*.Build.cs' -Recurse -File -ErrorAction SilentlyContinue)
    foreach ($f in $BuildCs) {
        $text = Get-Content -Raw $f.FullName -ErrorAction SilentlyContinue
        if (-not $text) { continue }
        if ($f.Name -like 'Riftbound*' -and $text -match 'LivingCity') {
            Add-Violation -Rule 'ARCH2' -File $f.FullName -Line 0 -Detail 'Riftbound module references a LivingCity module'
        }
        if ($f.Name -like 'LivingCity*' -and $text -match '"Riftbound"') {
            Add-Violation -Rule 'ARCH2' -File $f.FullName -Line 0 -Detail 'LivingCity module references the Riftbound module'
        }
    }
}

# ============================================================ report
$n = $script:Violations.Count
if ($n -eq 0) {
    if (-not $Quiet) {
        Write-Host "Check-SimPurity: clean" -ForegroundColor Green
        Write-Host "  $($CoreFiles.Count) core file(s), $($TestFiles.Count) test file(s) checked"
    }
    exit 0
}

Write-Host ""
Write-Host "Check-SimPurity: $n violation(s)" -ForegroundColor Red
Write-Host ""
$script:Violations |
    Sort-Object Rule, File, Line |
    ForEach-Object { Write-Host ("  [{0}] {1}:{2}`n        {3}" -f $_.Rule, $_.File, $_.Line, $_.Detail) -ForegroundColor Red }
Write-Host ""
exit 1
