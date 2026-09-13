<#
.SYNOPSIS
Runs real doorway/stair/roof movement against an already built Voyager game.
.DESCRIPTION
Fixture relocation occurs only between four building cases. Each occupied floor
is reached and left using ordinary character movement. Static collision checks
cover all sixty buildings at each of two spherical settlements. Does not build,
stop another game, download assets, or modify normal expedition save files.
#>
[CmdletBinding()]
param([switch]$Render, [switch]$Packaged, [switch]$VisualOnly, [ValidateRange(180,900)][int]$TimeoutSeconds = 720)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$variant = $(if ($Packaged) { 'Packaged' } else { 'Editor' }) + $(if ($Render) { 'Rendered' } else { 'Headless' })
if($VisualOnly){$variant += 'VisualOnly'}
$caseIndices = $(if($VisualOnly){@(0)}else{@(0..3)})
$runId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$runDirectory = Join-Path $projectRoot ('Saved\VoyagerBuildingAudit\' + $variant + '-' + $runId)
[IO.Directory]::CreateDirectory($runDirectory) | Out-Null
$logPath = Join-Path $runDirectory 'VoyagerBuildingAudit.log'
$reportPath = Join-Path $projectRoot ('Saved\VoyagerBuildings' + $variant + 'Report.json')
$executable = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if ($Render) { $executable = $executable.Replace('UnrealEditor-Cmd.exe','UnrealEditor.exe') }
if ($Packaged) { $executable = Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Binaries\Win64\Riftbound.exe' }

function Get-ExpeditionHashes {
    foreach ($relative in @('Saved\SaveGames\Voyager-Expedition.sav',
        'Packaged\Riftbound\Windows\Riftbound\Saved\SaveGames\Voyager-Expedition.sav')) {
        $path = Join-Path $projectRoot $relative
        $exists = Test-Path -LiteralPath $path -PathType Leaf
        [pscustomobject]@{ relative_path = $relative; exists = [bool]$exists
            sha256 = $(if ($exists) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { $null }) }
    }
}
function Read-SharedAuditLog {
    if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) { return '' }
    $stream = [IO.File]::Open($logPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}

$arguments = @(('"' + (Join-Path $projectRoot 'Riftbound.uproject') + '"'),
    '/Game/Maps/Forest?game=/Script/Riftbound.VoyagerGameMode','-game','-nosound','-unattended',
    '-NoSplash','-NoScreenMessages','-NoVoyagerLocalAI','-VoyagerBuildingAudit',
    '-ExecCmds="t.MaxFPS 60"',('-abslog="' + $logPath + '"'),('-VoyagerBuildingOutput="' + $runDirectory + '"'))
if ($Packaged) { $arguments = $arguments[1..($arguments.Length-1)] }
if ($VisualOnly) { $arguments += '-VoyagerBuildingVisualAudit' }
if ($Render) { $arguments += @('-windowed','-ResX=1280','-ResY=720','-VoyagerBuildingCapture') }
else { $arguments += '-NullRHI' }
$before = @(Get-ExpeditionHashes)
$after = @()
$started = [DateTime]::UtcNow
$clock = [Diagnostics.Stopwatch]::StartNew()
$ownedProcess = $null
$failure = $null
$logText = ''
$checks = [ordered]@{}
$captures = @()
$failurePattern = 'VOYAGER BUILDING AUDIT FAIL\b|Fatal error\b|LowLevelFatalError|Assertion failed\b|Ensure condition failed|Failed to compile Material|Failed to compile global shader'
try {
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing executable: $executable" }
    $ownedProcess = Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "Building audit PID $($ownedProcess.Id), $variant; evidence $runDirectory"
    while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $logText = Read-SharedAuditLog
        if ($logText -match $failurePattern) { throw 'Game or building audit reported an error; inspect retained log.' }
        if ($ownedProcess.HasExited) { break }
        Start-Sleep -Milliseconds 500
    }
    if (-not $ownedProcess.HasExited) { throw 'Building audit timed out.' }
    if ($ownedProcess.ExitCode -ne 0) { throw "Game exited with code $($ownedProcess.ExitCode)." }
}
catch { $failure = $_.Exception.Message }
finally {
    # This exact process handle is the only process owned by the audit.
    if ($ownedProcess -and -not $ownedProcess.HasExited) {
        Stop-Process -InputObject $ownedProcess -Force
        $ownedProcess.WaitForExit(10000) | Out-Null
    }
    $clock.Stop()
    try {
        $logText = Read-SharedAuditLog
        $checks['COMPLETE'] = $logText -match 'VOYAGER BUILDING AUDIT COMPLETE PASS\b'
        $checks['NO_ENGINE_ERRORS'] = $logText -notmatch $failurePattern
        $checks['ALL_CITY_BUILDINGS_GEOMETRY'] = [regex]::Matches($logText,'VOYAGER BUILDING AUDIT PASS ALL_BUILDING_GEOMETRY\b').Count -eq $(if($VisualOnly){1}else{2})
        foreach ($case in $caseIndices) {
            foreach ($name in @('DOOR_ENTRY','STAIR_ASCENT','ROOF_REACHED','STAIR_DESCENT','DOOR_EXIT')) {
                $checks["${name}_CASE$case"] = $logText -match ("VOYAGER BUILDING AUDIT PASS $name case=$case\b")
            }
        }
        $after = @(Get-ExpeditionHashes)
        $preserved = $before.Count -eq $after.Count
        for ($i=0; $i -lt $before.Count; $i++) {
            if ($before[$i].exists -ne $after[$i].exists -or $before[$i].sha256 -ne $after[$i].sha256) { $preserved = $false }
        }
        $checks['NORMAL_EXPEDITION_SAVES_UNCHANGED'] = $preserved
        if ($Render) {
            Add-Type -AssemblyName System.Drawing
            foreach ($case in $caseIndices) {
                $kinds=@('Door','Roof');if($case -eq 0){$kinds += 'Stairs'}
                foreach ($kind in $kinds) {
                    $path = Join-Path $runDirectory "Building$kind$case.png"
                    $valid = Test-Path -LiteralPath $path -PathType Leaf
                    $width = 0; $height = 0; $lit = 0
                    if ($valid) {
                        $info = Get-Item -LiteralPath $path
                        $bitmap = [Drawing.Bitmap]::new($path)
                        try {
                            $width=$bitmap.Width; $height=$bitmap.Height
                            for ($y=3; $y -le 7; $y++) { for ($x=3; $x -le 7; $x++) {
                                $pixel=$bitmap.GetPixel([int]($width*$x/10),[int]($height*$y/10))
                                if (($pixel.R+$pixel.G+$pixel.B) -gt 18) { $lit++ }
                            } }
                            $valid = $width -ge 640 -and $height -ge 360 -and $info.Length -gt 10000 -and
                                $info.LastWriteTimeUtc -ge $started.AddSeconds(-2) -and $lit -ge 5
                        } finally { $bitmap.Dispose() }
                    }
                    $checks["CAPTURE_$kind$case"] = [bool]$valid
                    $captures += [pscustomobject]@{path=$path;width=$width;height=$height;lit_center_samples=$lit;passed=[bool]$valid}
                }
            }
        }
        if (@($checks.Values | Where-Object { -not $_ }).Count -gt 0 -and -not $failure) {
            $failure = 'Required checks failed: ' + (($checks.Keys | Where-Object { -not $checks[$_] }) -join ', ')
        }
    } catch { if ($failure) { $failure += ' ' + $_.Exception.Message } else { $failure = $_.Exception.Message } }
    $report = [ordered]@{
        status=$(if($failure){'failed'}else{'passed'}); variant=$variant; started_utc=$started.ToString('o')
        elapsed_seconds=[Math]::Round($clock.Elapsed.TotalSeconds,3); rendered=[bool]$Render; packaged=[bool]$Packaged; visual_only=[bool]$VisualOnly
        process_id=$(if($ownedProcess){$ownedProcess.Id}else{$null}); checks=$checks; screenshots=$captures
        save_hashes_before=$before; save_hashes_after=$after; log=$logPath; failure=$failure
        performance_lines=@([regex]::Matches($logText,'(?m)^.*VOYAGER BUILDING PERF.*$') | ForEach-Object { $_.Value.Trim() })
        performance_caveat='End-to-end audit FPS includes fixture setup, streaming, and screenshot readback; it is not steady gameplay FPS.'
        coverage=$(if($VisualOnly){'Short visual regression: cafe, stairs and roof capture, full four-floor walk in both directions, and complete60-building local geometry sweep.'}else{'Four building types/locations use real walking through all floors and roof, in both directions. Fixture teleports only between buildings. Every landing, pane band and door is collision checked across two complete cities.'})
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding UTF8
}
Write-Host "Building audit report: $reportPath"
if ($failure) { throw $failure }
Write-Host 'Building audit passed; both normal expedition saves are unchanged.'
