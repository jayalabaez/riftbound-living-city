param([switch]$Render, [switch]$Packaged, [ValidateRange(1,2147483647)][int]$Seed=1)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$executable = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if ($Render) { $executable = $executable.Replace('UnrealEditor-Cmd.exe', 'UnrealEditor.exe') }
$logPath = Join-Path $projectRoot 'Saved\Logs\VoyagerCityAudit.log'
$reportPath = Join-Path $projectRoot 'Saved\VoyagerCityAuditReport.json'
$screenshotDirectory = Join-Path $projectRoot 'Saved\Screenshots\WindowsEditor'
if ($Packaged) {
    $executable = Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Binaries\Win64\Riftbound.exe'
    $logPath = Join-Path $projectRoot 'Saved\Logs\VoyagerCityPackagedAudit.log'
    $reportPath = Join-Path $projectRoot 'Saved\VoyagerCityPackagedReport.json'
    $screenshotDirectory = Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Saved\Screenshots\Windows'
}

function Get-ExpeditionHashes {
    $savePaths = @(
        (Join-Path $projectRoot 'Saved\SaveGames\Voyager-Expedition.sav'),
        (Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Saved\SaveGames\Voyager-Expedition.sav')
    )
    foreach ($path in $savePaths) {
        $exists = Test-Path -LiteralPath $path -PathType Leaf
        [pscustomobject]@{
            path = $path
            exists = [bool]$exists
            sha256 = $(if ($exists) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { $null })
        }
    }
}

function Read-AuditLog {
    $stream = [IO.File]::Open($logPath, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() }
    finally { $reader.Dispose() }
}

if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing game executable: $executable" }
$arguments = @(
    ('"' + (Join-Path $projectRoot 'Riftbound.uproject') + '"'),
    '/Game/Maps/Forest', '-game', '-nosound', '-unattended', '-NoSplash',
    '-NoScreenMessages', '-NoVoyagerLocalAI', '-VoyagerProbe', '-VoyagerCityAudit', '-VoyagerFurnitureAudit',
    "-VoyagerAuditSeed=$Seed", ('-abslog="' + $logPath + '"')
)
if ($Packaged) { $arguments = $arguments[1..($arguments.Length - 1)] }
if ($Render) { $arguments += @('-windowed', '-ResX=1280', '-ResY=720', '-VoyagerCapture') }
else { $arguments += '-NullRHI' }

[IO.Directory]::CreateDirectory((Split-Path -Parent $logPath)) | Out-Null
if (Test-Path -LiteralPath $logPath -PathType Leaf) {
    Copy-Item -LiteralPath $logPath -Destination ($logPath + '.previous') -Force
}
[IO.File]::WriteAllText($logPath, '')
$beforeSaves = @(Get-ExpeditionHashes)
$afterSaves = @()
$ownedProcess = $null
$failure = $null
$logText = ''
$captures = @()
$captureChecks = @()
$savePreserved = $false
$completedAt = $null
$clock = [Diagnostics.Stopwatch]::StartNew()
$startedAt = [DateTime]::UtcNow

try {
    $ownedProcess = Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "Voyager city audit: hidden PID $($ownedProcess.Id), walk-in interiors, citizens and a remote settlement."
    while ($clock.Elapsed.TotalSeconds -lt 180) {
        $logText = Read-AuditLog
        if ($logText -match 'VOYAGER (CITY|FURNITURE) AUDIT FAIL|Fatal error|Assertion failed|Ensure condition failed|Failed to compile Material|Failed to compile global shader') {
            throw 'City audit or renderer failed; inspect the retained log.'
        }
        if ($logText -match 'VOYAGER CITY AUDIT COMPLETE PASS') {
            if ($null -eq $completedAt) { $completedAt = $clock.Elapsed.TotalSeconds }
            # A screenshot request is fulfilled on a later render frame. Allow
            # its file to finish writing before this owned test process exits.
            if ($ownedProcess.HasExited -or ($clock.Elapsed.TotalSeconds - $completedAt) -ge 1.5) { break }
        }
        elseif ($ownedProcess.HasExited) { throw 'City audit process exited before completion.' }
        Start-Sleep -Milliseconds 400
    }
    if ($logText -notmatch 'VOYAGER CITY AUDIT COMPLETE PASS') { throw 'City audit timed out after 180 seconds.' }
    if ($logText -notmatch 'VOYAGER CITY AUDIT PASS') { throw 'City audit produced no individual passing checks.' }
    if ($logText -notmatch "VOYAGER FURNITURE AUDIT PASS seed=$Seed\b[^\r\n]*assemblies=[1-9][0-9]*") {
        throw 'No generated furniture assemblies passed orientation, support and radial transform checks.'
    }
}
catch { $failure = $_.Exception.Message }
finally {
    # Never enumerate or stop other editor/game processes. This handle is only
    # the process launched above, so an existing player session stays untouched.
    if ($ownedProcess -and -not $ownedProcess.HasExited) {
        Stop-Process -InputObject $ownedProcess -Force
        $ownedProcess.WaitForExit(10000) | Out-Null
    }
    $clock.Stop()
    try {
        $logText = Read-AuditLog
        if ($logText -match 'VOYAGER (CITY|FURNITURE) AUDIT FAIL|Fatal error|Assertion failed|Ensure condition failed|Failed to compile Material|Failed to compile global shader') {
            if (-not $failure) { $failure = 'City audit or renderer failed; inspect the retained log.' }
        }
        $afterSaves = @(Get-ExpeditionHashes)
        $savePreserved = $true
        for ($index = 0; $index -lt $beforeSaves.Count; $index++) {
            if ($beforeSaves[$index].exists -ne $afterSaves[$index].exists -or
                $beforeSaves[$index].sha256 -ne $afterSaves[$index].sha256) {
                $savePreserved = $false
                $failure = 'Normal Expedition save changed during the audit: ' + $beforeSaves[$index].path
            }
        }
        if ($Render) {
            Add-Type -AssemblyName System.Drawing
            if (Test-Path -LiteralPath $screenshotDirectory -PathType Container) {
                $captures = @(Get-ChildItem -LiteralPath $screenshotDirectory -Filter '*.png' -File |
                    Where-Object { $_.LastWriteTimeUtc -ge $startedAt } |
                    Sort-Object LastWriteTimeUtc, Name | Select-Object -ExpandProperty FullName)
            }
            foreach ($capture in $captures) {
                $bitmap = [Drawing.Bitmap]::new($capture)
                try {
                    $dark = 0
                    $samples = 0
                    for ($x = [int]($bitmap.Width * .3); $x -lt $bitmap.Width * .7; $x += 16) {
                        for ($y = [int]($bitmap.Height * .3); $y -lt $bitmap.Height * .7; $y += 16) {
                            $pixel = $bitmap.GetPixel($x, $y)
                            $samples++
                            if ([int]$pixel.R + [int]$pixel.G + [int]$pixel.B -lt 8) { $dark++ }
                        }
                    }
                    if ($samples -eq 0) { throw "Empty screenshot image: $capture" }
                    $blackFraction = $dark / [double]$samples
                    $captureChecks += [pscustomobject]@{
                        path = $capture
                        width = $bitmap.Width
                        height = $bitmap.Height
                        black_center_fraction = [Math]::Round($blackFraction, 4)
                        passed = $blackFraction -le .97
                    }
                    if ($blackFraction -gt .97) { throw "Scene rendered black in $capture" }
                }
                finally { $bitmap.Dispose() }
            }
            if ($captures.Count -lt 4) { throw 'Rendered city audit produced fewer than four fresh screenshots.' }
        }
    }
    catch {
        if ($failure) { $failure += ' Additional verification failure: ' + $_.Exception.Message }
        else { $failure = $_.Exception.Message }
    }
    $performanceLines = @([regex]::Matches($logText, 'VOYAGER PERFORMANCE[^\r\n]*') | ForEach-Object { $_.Value })
    $performanceSamples = @(
        foreach ($line in $performanceLines) {
            if ($line -match 'fps=([\d.]+) worst_frame_ms=([\d.]+) process_ram_mb=([\d.]+) peak_ram_mb=([\d.]+)') {
                [pscustomobject]@{
                    fps = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
                    worst_frame_ms = [double]::Parse($Matches[2], [Globalization.CultureInfo]::InvariantCulture)
                    process_ram_mb = [double]::Parse($Matches[3], [Globalization.CultureInfo]::InvariantCulture)
                    peak_ram_mb = [double]::Parse($Matches[4], [Globalization.CultureInfo]::InvariantCulture)
                }
            }
        }
    )
    [ordered]@{
        status = $(if ($failure) { 'failed' } else { 'passed' })
        elapsed_seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 2)
        rendered = [bool]$Render
        packaged = [bool]$Packaged
        seed = $Seed
        started_utc = $startedAt.ToString('o')
        process_id = $(if ($ownedProcess) { $ownedProcess.Id } else { $null })
        screenshots = $captures
        screenshot_checks = $captureChecks
        evidence = @([regex]::Matches($logText, 'VOYAGER (CITY|FURNITURE) AUDIT PASS[^\r\n]*') | ForEach-Object { $_.Value })
        performance = [ordered]@{
            interpretation = $(if ($Render) { 'Rendered hidden-window audit; includes first-use streaming and screenshot readback stalls. These samples are not a steady-state gameplay or GPU benchmark.' } else { 'NullRHI headless engine tick rate; FPS values do not measure rendering performance.' })
            samples = $performanceSamples
            log_lines = $performanceLines
        }
        expedition_save_preserved = $savePreserved
        expedition_saves_before = $beforeSaves
        expedition_saves_after = $afterSaves
        failure = $failure
        log = $logPath
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding utf8
}

if ($failure) { throw $failure }
Write-Host "Voyager city audit PASSED. Report: $reportPath"
