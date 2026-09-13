<#
.SYNOPSIS
    Runs the opt-in nature/bodycam/streaming audit against an already compiled game.
.DESCRIPTION
    -Render captures actual terrain and vegetation in a hidden owned game window.
    -AI also exercises a real citizen interaction, asynchronous local Ollama reply,
    cache replay, and exact authored fallback. No model is downloaded or started.
    -LightingCompare runs a short rendered sequence of cumulative lighting changes
    at one fixed forest viewpoint, capturing seven images for visual diagnosis.
    Altitude images use explicitly labelled teleport fixtures; this is not a
    substitute for Test-VoyagerAtmosphere.ps1's real controlled-flight regression.
#>
[CmdletBinding()]
param(
    [switch]$Render,
    [switch]$AI,
    [switch]$Packaged,
    [switch]$LightingCompare,
    [switch]$ClearWeather,
    [ValidateRange(140, 300)][int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
if ($LightingCompare) {
    if ($AI) { throw 'LightingCompare is a separate short diagnostic; run the AI audit separately.' }
    $Render = $true
}
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$variant = $(if ($Packaged) { 'Packaged' } else { 'Editor' }) + $(if ($LightingCompare) { 'LightingCompare' } elseif ($AI) { 'AI' } else { 'Nature' }) + $(if ($Render) { 'Rendered' } else { 'Headless' })
if($ClearWeather){$variant += 'ClearWeather'}
$runId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$runDirectory = Join-Path $projectRoot ('Saved\VoyagerRealismAudit\' + $variant + '-' + $runId)
[IO.Directory]::CreateDirectory($runDirectory) | Out-Null
$logPath = Join-Path $runDirectory 'VoyagerRealismAudit.log'
$reportPath = Join-Path $projectRoot ('Saved\VoyagerRealism' + $variant + 'Report.json')
$executable = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if ($Render) { $executable = $executable.Replace('UnrealEditor-Cmd.exe', 'UnrealEditor.exe') }
if ($Packaged) { $executable = Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Binaries\Win64\Riftbound.exe' }

function Read-SharedAuditLog {
    if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) { return '' }
    $stream = [IO.File]::Open($logPath, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() }
    finally { $reader.Dispose() }
}

function Get-ExpeditionHashes {
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

$arguments = @(
    ('"' + (Join-Path $projectRoot 'Riftbound.uproject') + '"'), '/Game/Maps/Forest?game=/Script/Riftbound.VoyagerGameMode',
    '-game', '-nosound', '-unattended', '-NoSplash', '-NoScreenMessages',
    '-VoyagerRealismAudit', '-VoyagerNatureVisit',
    ('-abslog="' + $logPath + '"'), ('-VoyagerRealismOutput="' + $runDirectory + '"')
)
if ($Packaged) { $arguments = $arguments[1..($arguments.Length - 1)] }
if ($LightingCompare) { $arguments += '-VoyagerLightingCompare' }
if ($Render) { $arguments += @('-windowed', '-ResX=1280', '-ResY=720', '-VoyagerRealismCapture') }
else { $arguments += '-NullRHI' }
if ($AI) {
    $arguments += @('-VoyagerAITest', '-ExecCmds="t.MaxFPS 60,Automation RunTests Voyager.LocalAI.ValidatedReply"')
}
elseif($ClearWeather){ $arguments += '-ExecCmds="t.MaxFPS 60,r.VolumetricCloud 0"' }
else { $arguments += '-ExecCmds="t.MaxFPS 60"' }

$required = @('IMPORTED_ASSETS', 'GROUND_VEGETATION', 'NATURAL_OBSTACLE_COLLISION', 'NATURAL_TERRAIN_MATERIAL',
    'PRIMARY_ATMOSPHERE_SUN', 'GROUNDED_WALK', 'BODYCAM_TOGGLE', 'ALTITUDE_UNLOAD', 'EXOSPHERE_FIXTURE',
    'REMOTE_RADIAL_STREAMING', 'DETERMINISTIC_RELOAD')
if ($AI) { $required += @('AI_IMMEDIATE_AUTHORED_REPLY', 'AI_ASYNC_REPLY', 'AI_CACHE_REPLAY', 'AI_AUTHORED_FALLBACK') }
if ($LightingCompare) { $required = @('IMPORTED_ASSETS', 'GROUND_VEGETATION', 'NATURAL_OBSTACLE_COLLISION',
    'NATURAL_TERRAIN_MATERIAL', 'PRIMARY_ATMOSPHERE_SUN', 'LIGHTING_COMPARE') }
$failurePattern = 'VOYAGER REALISM AUDIT FAIL\b|Fatal error\b|LowLevelFatalError|Assertion failed\b|Ensure condition failed|Failed to compile Material|Failed to compile global shader'
$ownedProcess = $null
$failure = $null
$logText = ''
$checks = [ordered]@{}
$screenshots = @()
$beforeSaves = @(Get-ExpeditionHashes)
$afterSaves = @()
$savePreserved = $false
$started = [DateTime]::UtcNow
$clock = [Diagnostics.Stopwatch]::StartNew()

try {
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing executable: $executable" }
    if ($AI) {
        $installed = Invoke-RestMethod -Uri 'http://127.0.0.1:11434/api/tags' -TimeoutSec 4
        if (@($installed.models | Where-Object { $_.name -eq 'llama3.2:3b' }).Count -eq 0) {
            throw 'Optional AI audit needs the already-installed llama3.2:3b model. No download was attempted.'
        }
    }
    $ownedProcess = Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "Voyager realism audit PID $($ownedProcess.Id), variant $variant."
    Write-Host "Evidence directory: $runDirectory"
    while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $logText = Read-SharedAuditLog
        if ($logText -match $failurePattern) { throw 'Engine or realism audit failed; inspect the retained log.' }
        if ($ownedProcess.HasExited) { break }
        Start-Sleep -Milliseconds 450
    }
    if (-not $ownedProcess.HasExited) { throw 'Realism audit timed out.' }
    if ($ownedProcess.ExitCode -ne 0) { throw "Game exited with code $($ownedProcess.ExitCode)." }
}
catch { $failure = $_.Exception.Message }
finally {
    # Only this exact process handle belongs to this audit. Other games/editors stay open.
    if ($ownedProcess -and -not $ownedProcess.HasExited) {
        Stop-Process -InputObject $ownedProcess -Force
        $ownedProcess.WaitForExit(10000) | Out-Null
    }
    $clock.Stop()
    try {
        $logText = Read-SharedAuditLog
        foreach ($name in $required) { $checks[$name] = $logText -match ("VOYAGER REALISM AUDIT PASS " + $name + '\b') }
        $checks['COMPLETE'] = $logText -match 'VOYAGER REALISM AUDIT COMPLETE PASS\b'
        $checks['NO_ENGINE_ERRORS'] = $logText -notmatch $failurePattern
        if ($AI) {
            $checks['ACTUAL_OLLAMA_REPLY_APPLIED'] = $logText -match 'VOYAGER LOCAL AI APPLIED chars=\d+ seconds='
            $checks['ACTUAL_CACHED_REPLY_APPLIED'] = $logText -match 'VOYAGER LOCAL AI CACHE_APPLIED chars='
            $checks['CPP_VALIDATION_TESTS'] = $logText -match 'Test Completed\. Result=\{Success\} Name=\{ValidatedReply\}'
        }
        $afterSaves = @(Get-ExpeditionHashes)
        $savePreserved = $true
        for ($i = 0; $i -lt $beforeSaves.Count; $i++) {
            if ($beforeSaves[$i].exists -ne $afterSaves[$i].exists -or $beforeSaves[$i].sha256 -ne $afterSaves[$i].sha256) { $savePreserved = $false }
        }
        $checks['NORMAL_EXPEDITION_SAVE_PRESERVED'] = $savePreserved
        if ($Render) {
            Add-Type -AssemblyName System.Drawing
            $captureNames = @('NatureGround.png', 'Nature15kmFixture.png', 'Nature65kmFixture.png', 'NatureRemoteGround.png')
            if ($LightingCompare) { $captureNames = @('Lighting00Baseline.png', 'Lighting01SkylightAtEye.png',
                'Lighting02NoCloudShadows.png', 'Lighting03NoClouds.png', 'Lighting04NoSunShadows.png', 'Lighting05Sun100Exposure1.png',
                'Lighting06NoPerPixelTransmittance.png') }
            foreach ($name in $captureNames) {
                $path = Join-Path $runDirectory $name
                $exists = Test-Path -LiteralPath $path -PathType Leaf
                $width = 0; $height = 0; $bytes = 0
                if ($exists) {
                    $bitmap = [Drawing.Bitmap]::new($path)
                    try { $width = $bitmap.Width; $height = $bitmap.Height }
                    finally { $bitmap.Dispose() }
                    $bytes = (Get-Item -LiteralPath $path).Length
                }
                $passed = $exists -and $width -ge 640 -and $height -ge 360 -and $bytes -gt 10000
                $checks["CAPTURE_$name"] = $passed
                $screenshots += [pscustomobject]@{ path = $path; width = $width; height = $height; bytes = $bytes; passed = $passed }
            }
        }
        if (@($checks.Values | Where-Object { -not $_ }).Count -gt 0 -and -not $failure) {
            $failure = 'Required checks failed: ' + (($checks.Keys | Where-Object { -not $checks[$_] }) -join ', ')
        }
    }
    catch { if ($failure) { $failure += ' ' + $_.Exception.Message } else { $failure = $_.Exception.Message } }
    $report = [ordered]@{
        status = $(if ($failure) { 'failed' } else { 'passed' })
        variant = $variant
        started_utc = $started.ToString('o')
        elapsed_seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 3)
        process_id = $(if ($ownedProcess) { $ownedProcess.Id } else { $null })
        rendered = [bool]$Render
        optional_ai = [bool]$AI
        packaged = [bool]$Packaged
        lighting_comparison = [bool]$LightingCompare
        clear_weather_diagnostic = [bool]$ClearWeather
        lighting_comparison_interpretation = $(if ($LightingCompare) { 'Seven fixed-view images: baseline, skylight at eye, cloud shadows off, clouds hidden, sun shadows off, sun intensity 100 with exposure bias 1, per-pixel atmosphere transmittance off. Changes are cumulative; completion validates captures, not visual quality.' } else { $null })
        checks = $checks
        evidence = @([regex]::Matches($logText, 'VOYAGER REALISM AUDIT PASS[^\r\n]*') | ForEach-Object { $_.Value })
        model_evidence = @([regex]::Matches($logText, 'VOYAGER LOCAL AI (?:REQUEST|APPLIED|CACHE_APPLIED|AUTHORED_FALLBACK)[^\r\n]*') | ForEach-Object { $_.Value })
        screenshots = $screenshots
        altitude_interpretation = $(if ($LightingCompare) { 'No altitude fixtures are run in this short lighting diagnostic.' } else { '15 km and 65 km are teleport fixtures for streaming/render inspection, not proof of flight. Run the existing controlled-flight audit separately.' })
        performance_interpretation = $(if ($Render) { 'Hidden rendered integration run with screenshots; not a steady-state graphics benchmark.' } else { 'NullRHI validates game objects and interaction, not visual appearance or rendering performance.' })
        normal_expedition_save_preserved = $savePreserved
        normal_saves_before = $beforeSaves
        normal_saves_after = $afterSaves
        failure = $failure
        log = $logPath
    }
    $json = $report | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText($reportPath, $json, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $runDirectory 'Report.json'), $json, [Text.UTF8Encoding]::new($false))
}
if ($failure) { throw $failure }
Write-Host "Voyager realism audit PASSED. Report: $reportPath"
