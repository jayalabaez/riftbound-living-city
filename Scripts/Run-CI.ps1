<#
.SYNOPSIS
    The LIVING CITY commit gate. Everything that must be true before code lands.

.DESCRIPTION
    This is the shared local and GitHub Actions simulation gate. It builds without Unreal;
    the Unreal targets and rendered checks are validated separately on the development PC.
    Wire this gate as a pre-commit hook with:

        .\Scripts\Run-CI.ps1 -InstallHook

    Steps, in order (purity and build fail fast; runtime failures are collected):
      1. Architecture purity      (I2, I8, I9, I12, R3, layer DAG, module isolation)
      2. Build the sim core       (headless + tests, warnings as errors)
      3. Invariant test suite     (I1, I3, I5, I7 and friends)
      4. Fast selfcheck           (same seed converges, different seed diverges)
      5. Cross-PROCESS determinism (two separate launches, hashes compared)
      6. Record and replay        (first divergent tick reported, if any)

    Step 5 deliberately uses two separate process launches rather than two in-process runs.
    An in-process comparison would miss any determinism bug caused by address-space layout,
    static initialisation order, or environment - exactly the bugs that are hardest to find
    later.

.EXAMPLE
    .\Scripts\Run-CI.ps1
    .\Scripts\Run-CI.ps1 -Quick
    .\Scripts\Run-CI.ps1 -InstallHook
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Release',

    # Shorter determinism runs, for a fast inner loop.
    [switch] $Quick,

    [switch] $InstallHook
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot

if ($InstallHook) {
    $hookDir = Join-Path $RepoRoot '.git\hooks'
    if (-not (Test-Path $hookDir)) { throw "No .git\hooks directory - is this a git repository?" }
    $hook = Join-Path $hookDir 'pre-commit'
    @'
#!/bin/sh
# LIVING CITY commit gate - see Scripts/Run-CI.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(git rev-parse --show-toplevel)/Scripts/Run-CI.ps1" -Quick
exit $?
'@ | Out-File -Encoding ascii $hook
    Write-Host "Installed pre-commit hook at $hook" -ForegroundColor Green
    exit 0
}

$Ticks     = if ($Quick) { 20000 } else { 100000 }
$HashEvery = if ($Quick) {   500 } else {   1000 }

$BuildDir    = Join-Path $RepoRoot "Sim\build\$Config"
$HeadlessExe = Join-Path $BuildDir 'livingcity_headless.exe'
$TestsExe    = Join-Path $BuildDir 'livingcity_tests.exe'
$TmpDir      = Join-Path $RepoRoot 'Sim\build\_ci'

$script:Step = 0
$script:Failed = @()

function Start-Step([string] $Name) {
    $script:Step++
    Write-Host ""
    Write-Host ("[{0}] {1}" -f $script:Step, $Name) -ForegroundColor Cyan
    Write-Host ("-" * 60)
}

function Fail([string] $Name, [string] $Detail) {
    $script:Failed += "$Name : $Detail"
    Write-Host "  FAILED - $Detail" -ForegroundColor Red
}

$overall = [System.Diagnostics.Stopwatch]::StartNew()

# ---------------------------------------------------------------- 1. purity
Start-Step 'Architecture purity'
& (Join-Path $PSScriptRoot 'Check-SimPurity.ps1')
if ($LASTEXITCODE -ne 0) {
    Fail 'purity' 'architecture violations found'
    Write-Host "`nCI FAILED at step 1 - fix the violations above before building.`n" -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------- 2. build
Start-Step "Build sim core [$Config]"
& (Join-Path $PSScriptRoot 'Build-Sim.ps1') -Config $Config
if ($LASTEXITCODE -ne 0) {
    Fail 'build' 'compilation failed'
    Write-Host "`nCI FAILED at step 2.`n" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $HeadlessExe)) { Fail 'build' "missing $HeadlessExe"; exit 1 }
if (-not (Test-Path $TestsExe))    { Fail 'build' "missing $TestsExe";    exit 1 }

$script:EnvBlocked = $false

# Windows Smart App Control refuses to execute unsigned binaries - which is every binary a
# compiler produces (DECISIONS.md D-014). That is an ENVIRONMENT failure, not a code failure,
# and the two must never be confused: one means "fix your simulation", the other means
# "Windows would not let us look". This wrapper keeps them apart.
function Invoke-Binary {
    param([string] $Path, [string[]] $Arguments = @())
    if ($script:EnvBlocked) { return $null }
    try {
        # No 2>&1 here. Windows PowerShell 5.1 wraps each stderr line from a NATIVE exe in an
        # ErrorRecord (NativeCommandError) and sets $? to false even when the process exited 0,
        # so redirecting turns a perfectly good run into a phantom CI failure.
        $out = & $Path @Arguments
        return [pscustomobject]@{ Output = $out; Code = $LASTEXITCODE }
    } catch {
        if ("$_" -match 'Application Control') {
            $script:EnvBlocked = $true
            Write-Host "  BLOCKED by Windows Smart App Control: $(Split-Path $Path -Leaf)" -ForegroundColor Magenta
            return $null
        }
        throw
    }
}

# ---------------------------------------------------------------- 3. tests
Start-Step 'Invariant test suite'
$r = Invoke-Binary $TestsExe
if ($r) {
    $r.Output | ForEach-Object { Write-Host $_ }
    if ($r.Code -ne 0) { Fail 'tests' 'one or more invariant tests failed' }
}

# ---------------------------------------------------------------- 4. selfcheck
Start-Step 'Simulation selfcheck'
$r = Invoke-Binary $HeadlessExe @('selfcheck')
if ($r) {
    $r.Output | ForEach-Object { Write-Host $_ }
    if ($r.Code -ne 0) { Fail 'selfcheck' 'seed handling or conservation is broken' }
}

# ---------------------------------------------------------------- 5. cross-process determinism
Start-Step "Cross-process determinism ($Ticks ticks, two separate launches)"
$ra = Invoke-Binary $HeadlessExe @('run','--seed','1337','--ticks',"$Ticks",'--hash-every',"$HashEvery",'--print-final-hash','--quiet')
$rb = Invoke-Binary $HeadlessExe @('run','--seed','1337','--ticks',"$Ticks",'--hash-every',"$HashEvery",'--print-final-hash','--quiet')
$rc = Invoke-Binary $HeadlessExe @('run','--seed','9001','--ticks',"$Ticks",'--hash-every',"$HashEvery",'--print-final-hash','--quiet')

if ($ra -and $rb -and $rc) {
    $hashA = "$($ra.Output | Select-Object -Last 1)".Trim()
    $hashB = "$($rb.Output | Select-Object -Last 1)".Trim()
    $hashC = "$($rc.Output | Select-Object -Last 1)".Trim()

    if ($ra.Code -ne 0 -or $rb.Code -ne 0 -or $rc.Code -ne 0) {
        Fail 'determinism' "a run exited non-zero (A=$($ra.Code) B=$($rb.Code) C=$($rc.Code))"
    } elseif ($hashA -notmatch '^[0-9a-fA-F]{16}$' -or
              $hashB -notmatch '^[0-9a-fA-F]{16}$' -or
              $hashC -notmatch '^[0-9a-fA-F]{16}$') {
        Fail 'determinism' 'a run did not emit a complete 64-bit world hash'
    } elseif ($hashA -ne $hashB) {
        Fail 'determinism' "INVARIANT I1 VIOLATED - same seed produced different worlds`n           run A: $hashA`n           run B: $hashB"
    } elseif ($hashC -eq $hashA) {
        Fail 'determinism' 'a different seed produced an identical world - the seed is not reaching the simulation'
    } else {
        Write-Host "  both launches -> $hashA" -ForegroundColor Green
        Write-Host "  seed 9001     -> $hashC (differs, as it must)" -ForegroundColor Green
    }
}

# ---------------------------------------------------------------- 6. record + replay
Start-Step "Record and replay ($Ticks ticks)"
New-Item -ItemType Directory -Force $TmpDir | Out-Null
$log = Join-Path $TmpDir 'ci-run.lcreplay'

$rr = Invoke-Binary $HeadlessExe @('run','--seed','4242','--ticks',"$Ticks",'--hash-every',"$HashEvery",'--record',$log,'--quiet')
if ($rr) {
    if ($rr.Code -ne 0) {
        Fail 'replay' 'recording run failed'
    } else {
        $rv = Invoke-Binary $HeadlessExe @('verify','--log',$log)
        if ($rv) {
            $rv.Output | ForEach-Object { Write-Host $_ }
            if ($rv.Code -ne 0) { Fail 'replay' 'replay diverged from the recorded run' }
        }
    }
}

# ---------------------------------------------------------------- report
$overall.Stop()
Write-Host ""
Write-Host ("=" * 60)

if ($script:EnvBlocked) {
    Write-Host "CI BLOCKED - environment, not code" -ForegroundColor Magenta
    Write-Host ("=" * 60)
    Write-Host "Windows Smart App Control refused to run one of our own binaries, so the" -ForegroundColor Yellow
    Write-Host "determinism and replay checks could not be performed. This is NOT a" -ForegroundColor Yellow
    Write-Host "determinism failure - nothing was verified either way." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  Keep Windows security enabled. Use the GitHub Actions gate or a trusted build environment." -ForegroundColor White
    Write-Host "  Local validation is incomplete until these binaries can run. See DECISIONS.md D-014." -ForegroundColor DarkGray
    if ($script:Failed.Count -gt 0) {
        Write-Host ""
        Write-Host "Real failures also recorded:" -ForegroundColor Red
        $script:Failed | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    }
    exit 2
}

if ($script:Failed.Count -eq 0) {
    Write-Host "CI GREEN  ($([math]::Round($overall.Elapsed.TotalSeconds,1))s)" -ForegroundColor Green
    Write-Host ("=" * 60)
    exit 0
}

Write-Host "CI FAILED  ($($script:Failed.Count) step(s))" -ForegroundColor Red
$script:Failed | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
Write-Host ("=" * 60)
exit 1
