<#
.SYNOPSIS
    Opt-in Unreal integration audit for Living City, or its absence in Voyager.
.DESCRIPTION
    Uses the already compiled editor/game. This script never builds or closes an
    existing game/editor session. Reports and fresh screenshots live under Saved.
    -Render exercises real rendering; default NullRHI tests gameplay integration.
    -Isolation verifies that Voyager starts no Living City simulation thread.
#>
[CmdletBinding()]
param([switch]$Render, [switch]$Packaged, [switch]$Isolation)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$variant = $(if ($Isolation) { 'Isolation' } else { 'Runtime' }) + $(if ($Packaged) { 'Packaged' } else { 'Editor' })
$runId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$runDirectory = Join-Path $projectRoot ('Saved\LivingCityAudit\' + $variant + '-' + $runId)
[IO.Directory]::CreateDirectory($runDirectory) | Out-Null
$logPath = Join-Path $runDirectory 'LivingCityAudit.log'
$reportPath = Join-Path $projectRoot ('Saved\LivingCity' + $variant + 'Report.json')
$executable = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if ($Render) { $executable = $executable.Replace('UnrealEditor-Cmd.exe', 'UnrealEditor.exe') }
if ($Packaged) { $executable = Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Binaries\Win64\Riftbound.exe' }
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing executable: $executable" }

function Read-LivingCityAuditLog {
    if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) { return '' }
    $stream = [IO.File]::Open($logPath, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() }
    finally { $reader.Dispose() }
}

function Get-ExpeditionSaveHashes {
    foreach ($relative in @('Saved\SaveGames\Voyager-Expedition.sav',
        'Packaged\Riftbound\Windows\Riftbound\Saved\SaveGames\Voyager-Expedition.sav')) {
        $path = Join-Path $projectRoot $relative
        $exists = Test-Path -LiteralPath $path -PathType Leaf
        [pscustomobject]@{
            relative_path = $relative
            exists = [bool]$exists
            sha256 = $(if ($exists) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { $null })
        }
    }
}

$map = if ($Isolation) { '/Game/Maps/Forest' } else { '/Engine/Maps/Entry?game=/Script/LivingCityGame.LivingCityGameMode' }
$arguments = @(
    ('"' + (Join-Path $projectRoot 'Riftbound.uproject') + '"'), $map,
    '-game', '-nosound', '-unattended', '-NoSplash', '-NoScreenMessages',
    ('-abslog="' + $logPath + '"'), ('-LivingCityAuditOutput="' + $runDirectory + '"')
)
if ($Packaged) { $arguments = $arguments[1..($arguments.Length - 1)] }
if ($Isolation) { $arguments += '-LivingCityIsolationAudit' }
else { $arguments += '-LivingCityAudit' }
if ($Render) { $arguments += @('-windowed', '-ResX=1280', '-ResY=720', '-LivingCityCapture') }
else { $arguments += '-NullRHI' }

$ownedProcess = $null
$failure = $null
$logText = ''
$screenshotChecks = @()
$savePreserved = $false
$beforeSaves = @(Get-ExpeditionSaveHashes)
$afterSaves = @()
$startedAt = [DateTime]::UtcNow
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
try {
    $ownedProcess = Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "Living City $variant audit running in owned hidden PID $($ownedProcess.Id)."
    while ($stopwatch.Elapsed.TotalSeconds -lt 150) {
        $logText = Read-LivingCityAuditLog
        if ($logText -match 'LIVINGCITY AUDIT FAIL|Fatal error|Assertion failed|Ensure condition failed|Failed to compile Material|Failed to compile global shader') {
            throw 'Runtime or renderer audit failed; inspect retained log.'
        }
        if ($ownedProcess.HasExited) {
            if ($logText -notmatch 'LIVINGCITY AUDIT COMPLETE PASS') { throw 'Game exited before audit completion.' }
            if ($ownedProcess.ExitCode -ne 0) { throw "Game exited with code $($ownedProcess.ExitCode)." }
            break
        }
        Start-Sleep -Milliseconds 350
    }
    if ($logText -notmatch 'LIVINGCITY AUDIT COMPLETE PASS') { throw 'Runtime audit timed out.' }
    if (-not $ownedProcess.HasExited) { throw 'Runtime reported completion but did not shut down gracefully.' }
}
catch { $failure = $_.Exception.Message }
finally {
    # Only this exact process handle belongs to the audit. Never enumerate/kill games.
    if ($ownedProcess -and -not $ownedProcess.HasExited) {
        Stop-Process -InputObject $ownedProcess -Force
        $ownedProcess.WaitForExit(10000) | Out-Null
    }
    $stopwatch.Stop()
    try {
        $logText = Read-LivingCityAuditLog
        if ($logText -match 'LIVINGCITY AUDIT FAIL|Fatal error|Assertion failed|Ensure condition failed|Failed to compile Material|Failed to compile global shader') {
            if (-not $failure) { $failure = 'Runtime or renderer audit failed; inspect retained log.' }
        }
        $afterSaves = @(Get-ExpeditionSaveHashes)
        $savePreserved = $true
        for ($index = 0; $index -lt $beforeSaves.Count; $index++) {
            if ($beforeSaves[$index].exists -ne $afterSaves[$index].exists -or $beforeSaves[$index].sha256 -ne $afterSaves[$index].sha256) {
                $savePreserved = $false
                throw 'Normal Expedition save changed during audit.'
            }
        }
        if ($Render -and -not $Isolation) {
            Add-Type -AssemblyName System.Drawing
            foreach ($name in @('LivingCityStreet.png', 'LivingCityTerrainEdit.png')) {
                $imagePath = Join-Path $runDirectory $name
                if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) { throw "Missing rendered capture: $name" }
                $bitmap = [Drawing.Bitmap]::new($imagePath)
                try {
                    $samples = 0
                    $dark = 0
                    for ($y = [int]($bitmap.Height * .2); $y -lt [int]($bitmap.Height * .9); $y += 8) {
                        for ($x = [int]($bitmap.Width * .25); $x -lt [int]($bitmap.Width * .9); $x += 8) {
                            $pixel = $bitmap.GetPixel($x, $y)
                            $samples++
                            if ($pixel.R -lt 5 -and $pixel.G -lt 5 -and $pixel.B -lt 5) { $dark++ }
                        }
                    }
                    $fraction = $dark / [double]$samples
                    $screenshotChecks += [pscustomobject]@{
                        path = $imagePath
                        width = $bitmap.Width
                        height = $bitmap.Height
                        black_center_fraction = [Math]::Round($fraction, 4)
                        passed = $fraction -lt .97
                    }
                    if ($fraction -ge .97) { throw "Capture rendered black: $name" }
                }
                finally { $bitmap.Dispose() }
            }
        }
    }
    catch {
        if ($failure) { $failure += ' Additional verification failure: ' + $_.Exception.Message }
        else { $failure = $_.Exception.Message }
    }
    $checks = @([regex]::Matches($logText, 'LIVINGCITY AUDIT PASS[^\r\n]*') | ForEach-Object { $_.Value })
    $requiredChecks = if ($Isolation) { 1 } else { 10 }
    if ($checks.Count -lt $requiredChecks -and -not $failure) { $failure = 'Audit omitted required checks.' }
    [ordered]@{
        status = $(if ($failure) { 'failed' } else { 'passed' })
        mode = $variant
        rendered = [bool]$Render
        packaged = [bool]$Packaged
        started_utc = $startedAt.ToString('o')
        elapsed_seconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 2)
        process_id = $(if ($ownedProcess) { $ownedProcess.Id } else { $null })
        process_exit_code = $(if ($ownedProcess -and $ownedProcess.HasExited) { $ownedProcess.ExitCode } else { $null })
        evidence = $checks
        screenshots = $screenshotChecks
        performance_interpretation = $(if ($Render) { 'Rendered hidden-window integration run, including screenshot readback. Not a steady-state gameplay benchmark.' } else { 'NullRHI: frame timings do not measure rendering performance.' })
        performance_log = @([regex]::Matches($logText, 'LIVINGCITY AUDIT PERFORMANCE[^\r\n]*') | ForEach-Object { $_.Value })
        expedition_save_preserved = $savePreserved
        expedition_saves_before = $beforeSaves
        expedition_saves_after = $afterSaves
        failure = $failure
        log = $logPath
    } | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $reportPath -Encoding utf8
}
if ($failure) { throw $failure }
Write-Host "Living City audit PASSED. Report: $reportPath"
