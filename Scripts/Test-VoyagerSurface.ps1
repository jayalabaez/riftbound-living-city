param([switch]$Render)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$editor = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if ($Render) { $editor = $editor.Replace('UnrealEditor-Cmd.exe', 'UnrealEditor.exe') }
$logPath = Join-Path $projectRoot 'Saved\Logs\VoyagerSurfaceAudit.log'
$reportPath = Join-Path $projectRoot 'Saved\VoyagerSurfaceAuditReport.json'
$arguments = @(('"' + (Join-Path $projectRoot 'Riftbound.uproject') + '"'),
    '/Game/Maps/Forest', '-game', '-nosound', '-unattended', '-NoSplash',
    '-NoScreenMessages', '-VoyagerProbe', '-VoyagerSurfaceAudit', ('-abslog="' + $logPath + '"'))
if ($Render) { $arguments += @('-windowed', '-ResX=1280', '-ResY=720', '-VoyagerCapture') }
else { $arguments += '-NullRHI' }
if (Test-Path -LiteralPath $logPath) { Copy-Item -LiteralPath $logPath -Destination ($logPath + '.previous') }
[IO.File]::WriteAllText($logPath, '')
$ownedProcess = $null
$failure = $null
$logText = ''
$clock = [Diagnostics.Stopwatch]::StartNew()
try {
    $ownedProcess = Start-Process -FilePath $editor -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "Voyager surface audit: hidden PID $($ownedProcess.Id), three distant spherical locations."
    while ($clock.Elapsed.TotalSeconds -lt 100) {
        $stream = [IO.File]::Open($logPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader = [IO.StreamReader]::new($stream)
        try { $logText = $reader.ReadToEnd() } finally { $reader.Dispose() }
        if ($logText -match 'VOYAGER SURFACE AUDIT FAIL|Fatal error|Assertion failed') { throw 'Surface audit failed; inspect retained log.' }
        if ($logText -match 'VOYAGER SURFACE AUDIT COMPLETE PASS') { break }
        if ($ownedProcess.HasExited) { throw 'Audit process exited before completion.' }
        Start-Sleep -Milliseconds 500
    }
    if ($logText -notmatch 'VOYAGER SURFACE AUDIT COMPLETE PASS') { throw 'Surface audit timed out.' }
}
catch { $failure = $_.Exception.Message }
finally {
    if ($ownedProcess -and -not $ownedProcess.HasExited) { Stop-Process -InputObject $ownedProcess -Force }
    $clock.Stop()
    [ordered]@{
        status = $(if ($failure) { 'failed' } else { 'passed' })
        elapsed_seconds = [Math]::Round($clock.Elapsed.TotalSeconds,2)
        rendered = [bool]$Render
        evidence = @([regex]::Matches($logText,'VOYAGER SURFACE AUDIT PASS[^\r\n]*') | ForEach-Object { $_.Value })
        failure = $failure
        log = $logPath
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath
}
if ($failure) { throw $failure }
Write-Host "Voyager spherical surface audit PASSED. Report: $reportPath"
