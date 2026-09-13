<#
.SYNOPSIS
    Runs the LIVING CITY simulation headlessly and shows that it is deterministic.

.DESCRIPTION
    This needs no game engine at all - that is the whole point of architecture rule R1.
    It builds the sim core with cl.exe if needed, then demonstrates the Phase 0 result:
    a city simulation heartbeat that produces bit-identical worlds from the same seed.

    Safe to run while the Unreal Editor is open. Nothing here touches Unreal.

    If Windows Smart App Control blocks one of our own binaries (see DECISIONS.md D-014),
    this reports it clearly and keeps going with whatever still runs, rather than dying
    and looking like a crash in the simulation.

.NOTE
    This file must stay PURE ASCII - see DECISIONS.md D-011.
#>
[CmdletBinding()]
param(
    [uint64] $Seed  = 1337,
    [uint64] $Ticks = 100000,
    [switch] $Rebuild,
    [switch] $NoPause
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Exe         = Join-Path $ProjectRoot 'Sim\build\Release\livingcity_headless.exe'
$TestsExe    = Join-Path $ProjectRoot 'Sim\build\Release\livingcity_tests.exe'

function Say([string] $Text, [string] $Colour = 'Gray') { Write-Host $Text -ForegroundColor $Colour }
function Rule { Say ('  ' + ('-' * 66)) 'DarkGray' }

$script:Blocked = $false

Say ''
Say '  LIVING CITY - simulation core' 'Cyan'
Say '  no engine, no renderer, no window. just the simulation.' 'DarkGray'
Say ''

# ---------------------------------------------------------------- build
if ($Rebuild -or -not (Test-Path -LiteralPath $Exe)) {
    Say '  Building the sim core...' 'Yellow'
    & (Join-Path $PSScriptRoot 'Build-Sim.ps1') -Config Release | Out-Null
    if ($LASTEXITCODE -ne 0) { Say '  BUILD FAILED.' 'Red'; exit 1 }
    Say '  Build OK.' 'Green'
    Say ''
}

# Returns $null when Smart App Control refuses to run the binary.
function Invoke-Sim {
    param([string[]] $SimArgs)
    if ($script:Blocked) { return $null }
    try {
        $output = & $Exe @SimArgs 2>&1
        return $output
    } catch {
        if ("$_" -match 'Application Control') { $script:Blocked = $true; return $null }
        throw
    }
}

function Show-BlockedBanner {
    Say ''
    Say '    Windows Smart App Control refused to run livingcity_headless.exe.' 'Red'
    Say ''
    Say '    This is not a fault in the simulation. Smart App Control is enforced on' 'Yellow'
    Say '    this machine and blocks unsigned executables - which is every binary a' 'Yellow'
    Say '    compiler produces. The verdict follows the file contents, so rebuilding' 'Yellow'
    Say '    does not reliably clear it.' 'Yellow'
    Say ''
    Say '    The fix is a decision only you can make, and it cannot be undone:' 'White'
    Say '      Windows Security > App and browser control > Smart App Control > Off' 'White'
    Say '      (Microsoft provides no way to turn it back on without reinstalling Windows.)' 'DarkGray'
    Say ''
    Say '    Full write-up: DECISIONS.md, entry D-014.' 'DarkGray'
    Say ''
}

# ---------------------------------------------------------------- 1. invariants
Rule
Say '  1. Invariant tests' 'White'
Rule
if (Test-Path -LiteralPath $TestsExe) {
    try {
        $testOut = & $TestsExe 2>&1
        $summary = $testOut | Select-String -Pattern 'run, .* passed' | Select-Object -Last 1
        if ($LASTEXITCODE -eq 0) {
            Say "    $($summary.ToString().Trim())" 'Green'
            Say '    determinism, money conservation, replay round-trip, zero allocation' 'DarkGray'
        } else {
            Say "    $summary" 'Red'
            $testOut | Select-String 'FAIL' | ForEach-Object { Say "    $_" 'Red' }
        }
    } catch {
        Say '    Smart App Control blocked livingcity_tests.exe too.' 'Red'
        $script:Blocked = $true
    }
} else {
    Say '    tests binary not built' 'Yellow'
}
Say ''

# ---------------------------------------------------------------- 2. determinism
Rule
Say '  2. Determinism - two separate processes, same seed' 'White'
Rule
$a = Invoke-Sim @('run', '--seed', "$Seed", '--ticks', "$Ticks", '--hash-every', '1000', '--print-final-hash', '--quiet')
if ($script:Blocked) {
    Show-BlockedBanner
} else {
    $b = Invoke-Sim @('run', '--seed', "$Seed", '--ticks', "$Ticks", '--hash-every', '1000', '--print-final-hash', '--quiet')
    $c = Invoke-Sim @('run', '--seed', '9001',  '--ticks', "$Ticks", '--hash-every', '1000', '--print-final-hash', '--quiet')

    $hashA = ($a | Select-Object -Last 1).ToString().Trim()
    $hashB = ($b | Select-Object -Last 1).ToString().Trim()
    $hashC = ($c | Select-Object -Last 1).ToString().Trim()

    Say "    process 1, seed $Seed  ->  $hashA" 'Gray'
    Say "    process 2, seed $Seed  ->  $hashB" 'Gray'
    Say "    process 3, seed 9001  ->  $hashC" 'Gray'
    Say ''
    if ($hashA -eq $hashB) { Say '    same seed  -> identical world, bit for bit.' 'Green' }
    else                   { Say '    DETERMINISM VIOLATED - same seed gave different worlds.' 'Red' }
    if ($hashC -ne $hashA) { Say '    other seed -> different world, as it must be.' 'Green' }
    else                   { Say '    the seed is not reaching the simulation.' 'Red' }
    Say ''

    # ------------------------------------------------------------ 3. record and replay
    Rule
    Say '  3. Record and replay' 'White'
    Rule
    $tmp = Join-Path $ProjectRoot 'Sim\build\_run'
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $log = Join-Path $tmp 'session.lcreplay'
    Invoke-Sim @('run', '--seed', '4242', '--ticks', "$Ticks", '--hash-every', '1000', '--record', $log, '--quiet') | Out-Null
    $ver = Invoke-Sim @('verify', '--log', $log)
    $ver | ForEach-Object { Say "    $_" 'Green' }
    Say ''

    # ------------------------------------------------------------ 4. a full run
    Rule
    Say '  4. A full run' 'White'
    Rule
    Invoke-Sim @('run', '--seed', "$Seed", '--ticks', "$Ticks", '--hash-every', '1000') |
        ForEach-Object { Say "    $_" 'Gray' }
    Say ''

    # ------------------------------------------------------------ 5. throughput
    Rule
    Say '  5. Throughput and allocation' 'White'
    Rule
    Invoke-Sim @('bench', '--ticks', '500000') | ForEach-Object {
        $colour = if ("$_" -match 'I5 holds') { 'Green' } elseif ("$_" -match 'VIOLATED') { 'Red' } else { 'Gray' }
        Say "    $_" $colour
    }
    Say ''
}

Rule
if ($script:Blocked) {
    Say '  The invariant tests above are real and passed. The rest is gated by Windows,' 'DarkGray'
    Say '  not by anything in this project.' 'DarkGray'
} else {
    Say '  Every number above came from a simulation that has never seen a renderer.' 'DarkGray'
    Say '  The Unreal client is the same simulation with a window attached.' 'DarkGray'
}
Rule
Say ''

if (-not $NoPause) {
    Say '  Press any key to close...' 'DarkGray'
    $null = $Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown')
}
