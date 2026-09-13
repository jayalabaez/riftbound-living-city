param([switch]$Render, [switch]$Packaged)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$editor = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if ($Render) { $editor = $editor.Replace('UnrealEditor-Cmd.exe', 'UnrealEditor.exe') }
$logPath = Join-Path $projectRoot 'Saved\Logs\VoyagerLifeAudit.log'
$reportPath = Join-Path $projectRoot 'Saved\VoyagerLifeAuditReport.json'
if ($Packaged) { $editor = Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Binaries\Win64\Riftbound.exe'; $reportPath = Join-Path $projectRoot 'Saved\VoyagerPackagedReport.json'; $logPath = Join-Path $projectRoot 'Saved\Logs\VoyagerPackagedLife.log' }
$arguments = @(('"' + (Join-Path $projectRoot 'Riftbound.uproject') + '"'),
    '/Game/Maps/Forest', '-game', '-nosound', '-unattended', '-NoSplash',
    '-NoScreenMessages', '-VoyagerProbe', '-VoyagerLifeAudit', ('-abslog="' + $logPath + '"'))
if ($Packaged) { $arguments = $arguments[1..($arguments.Length-1)] }
if ($Render) { $arguments += @('-windowed', '-ResX=1280', '-ResY=720', '-VoyagerCapture') }
else { $arguments += '-NullRHI' }
if (Test-Path -LiteralPath $logPath) { Copy-Item -LiteralPath $logPath -Destination ($logPath + '.previous') }
[IO.File]::WriteAllText($logPath, '')
$ownedProcess = $null
$failure = $null
$logText = ''
$clock = [Diagnostics.Stopwatch]::StartNew()
$startedAt = [DateTime]::UtcNow
$captures = @()
try {
    $ownedProcess = Start-Process -FilePath $editor -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "Voyager world life audit: hidden PID $($ownedProcess.Id), five biomes, procedural settlements and roaming wildlife."
    while ($clock.Elapsed.TotalSeconds -lt 100) {
        $stream = [IO.File]::Open($logPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader = [IO.StreamReader]::new($stream)
        try { $logText = $reader.ReadToEnd() } finally { $reader.Dispose() }
        if ($logText -match 'VOYAGER LIFE AUDIT FAIL|Fatal error|Assertion failed|Ensure condition failed|Failed to compile Material') { throw 'World life audit failed; inspect retained log.' }
        if ($logText -match 'VOYAGER LIFE AUDIT COMPLETE PASS') { break }
        if ($ownedProcess.HasExited) { throw 'Audit process exited before completion.' }
        Start-Sleep -Milliseconds 500
    }
    if ($logText -notmatch 'VOYAGER LIFE AUDIT COMPLETE PASS') { throw 'Surface audit timed out.' }
    if ($Render) {
        Add-Type -AssemblyName System.Drawing
        $screenshotDirectory = Join-Path $projectRoot 'Saved\Screenshots\WindowsEditor'
        if ($Packaged) { $screenshotDirectory = Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Saved\Screenshots\Windows' }
        $captures = @(Get-ChildItem -LiteralPath $screenshotDirectory -Filter '*.png' | Where-Object { $_.LastWriteTimeUtc -ge $startedAt } | Select-Object -ExpandProperty FullName)
        if ($captures.Count -lt 6) { throw 'Rendered audit did not produce all five biome views.' }
        foreach ($capture in $captures) {
            $bitmap = [Drawing.Bitmap]::new($capture)
            try {
                $dark=0; $samples=0
                for ($x=[int]($bitmap.Width*.3); $x -lt $bitmap.Width*.7; $x+=16) {
                    for ($y=[int]($bitmap.Height*.30); $y -lt $bitmap.Height*.70; $y+=16) {
                        $pixel=$bitmap.GetPixel($x,$y);$samples++
                        if ([int]$pixel.R+[int]$pixel.G+[int]$pixel.B -lt 8) { $dark++ }
                    }
                }
                if ($dark/[double]$samples -gt .97) { throw "Scene rendered black in $capture" }
            } finally { $bitmap.Dispose() }
        }
    }
}
catch { $failure = $_.Exception.Message }
finally {
    if ($ownedProcess -and -not $ownedProcess.HasExited) { Stop-Process -InputObject $ownedProcess -Force }
    $clock.Stop()
    [ordered]@{
        status = $(if ($failure) { 'failed' } else { 'passed' })
        elapsed_seconds = [Math]::Round($clock.Elapsed.TotalSeconds,2)
        rendered = [bool]$Render
        packaged = [bool]$Packaged
        screenshots = $captures
        evidence = @([regex]::Matches($logText,'VOYAGER LIFE AUDIT PASS[^\r\n]*') | ForEach-Object { $_.Value })
        failure = $failure
        log = $logPath
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath
}
if ($failure) { throw $failure }
Write-Host "Voyager spherical world life audit PASSED. Report: $reportPath"
