param(
    [ValidateRange(1024, 65535)][int]$Port = 7793,
    [ValidateRange(60, 300)][int]$TimeoutSeconds = 150,
    [switch]$Packaged
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$projectPath = Join-Path $projectRoot 'Riftbound.uproject'
$executable = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if ($Packaged) {
    $executable = Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Binaries\Win64\Riftbound.exe'
}
$suffix = if ($Packaged) { 'Packaged' } else { '' }
$hostLogPath = Join-Path $projectRoot ("Saved\Logs\VoyagerCityNetwork${suffix}Host.log")
$clientLogPath = Join-Path $projectRoot ("Saved\Logs\VoyagerCityNetwork${suffix}Client.log")
$reportPath = Join-Path $projectRoot ("Saved\VoyagerCityNetwork${suffix}Report.json")
$ownedProcesses = [Collections.Generic.List[Diagnostics.Process]]::new()
$hostProcess = $null
$clientProcess = $null
$hostText = ''
$clientText = ''
$failure = $null
$savePreserved = $false
$afterSaves = @()
$checks = [ordered]@{}
$startedAt = [DateTime]::UtcNow
$clock = [Diagnostics.Stopwatch]::StartNew()
$failurePattern = 'VOYAGER CITY NETWORK AUDIT FAIL\b|Fatal error\b|LowLevelFatalError|Assertion failed\b|Ensure condition failed|could not resolve the new relative movement base'

function Read-SharedLog([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() }
    finally { $reader.Dispose() }
}

function Get-ExpeditionHashes {
    foreach ($path in @(
        (Join-Path $projectRoot 'Saved\SaveGames\Voyager-Expedition.sav'),
        (Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Saved\SaveGames\Voyager-Expedition.sav')
    )) {
        $exists = Test-Path -LiteralPath $path -PathType Leaf
        [pscustomobject]@{
            path = $path
            exists = [bool]$exists
            sha256 = $(if ($exists) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { $null })
        }
    }
}

function Assert-OwnedProcessesAlive {
    foreach ($process in $ownedProcesses) {
        $process.Refresh()
        if ($process.HasExited) { throw "Owned test process $($process.Id) exited unexpectedly." }
    }
}

function Get-NetworkChecks([string]$HostText, [string]$ClientText) {
    $result = [ordered]@{}
    foreach ($marker in @('TWO_PLANET_FIXTURE', 'AUTHORITATIVE_POPULATIONS', 'AUTHORITATIVE_MOTION', 'DIALOGUE_FIXTURE', 'CLIENT_ALARM_AUTHORITY')) {
        $result["host_$($marker.ToLowerInvariant())"] = $HostText -match ("VOYAGER CITY NETWORK AUDIT PASS $marker net=2\b")
    }
    foreach ($marker in @('REMOTE_IDENTITIES_AND_POSES', 'REPLICATED_POPULATIONS', 'REPLICATED_MOTION', 'INTERACT_RPC_DIALOGUE', 'TOPIC_RPC_DIALOGUE', 'END_CONVERSATION_RPC', 'REPLICATED_ALARM')) {
        $result["client_$($marker.ToLowerInvariant())"] = $ClientText -match ("VOYAGER CITY NETWORK AUDIT PASS $marker net=3\b")
    }
    $result['host_complete'] = $HostText -match 'VOYAGER CITY NETWORK AUDIT COMPLETE PASS net=2\b'
    $result['client_complete'] = $ClientText -match 'VOYAGER CITY NETWORK AUDIT COMPLETE PASS net=3\b'
    $result['no_engine_or_audit_errors'] = ($HostText + "`n" + $ClientText) -notmatch $failurePattern
    return $result
}

$beforeSaves = @(Get-ExpeditionHashes)
try {
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Missing executable: $executable" }
    if (-not (Test-Path -LiteralPath $projectPath -PathType Leaf)) { throw "Missing project: $projectPath" }
    if (Get-Command Get-NetUDPEndpoint -ErrorAction SilentlyContinue) {
        $existingEndpoints = @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue)
        if ($existingEndpoints.Count -gt 0) { throw "UDP $Port is already in use. Choose another -Port." }
    }
    [IO.Directory]::CreateDirectory((Split-Path -Parent $hostLogPath)) | Out-Null
    $archiveStamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    foreach ($path in @($hostLogPath, $clientLogPath)) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Copy-Item -LiteralPath $path -Destination ($path + '.' + $archiveStamp + '.previous')
        }
        [IO.File]::WriteAllText($path, '')
    }
    $prefix = @()
    if (-not $Packaged) { $prefix = @(('"' + $projectPath + '"')) }
    $common = @('-game', '-NullRHI', '-nosound', '-unattended', '-NoSplash', '-NoScreenMessages', '-VoyagerProbe', '-VoyagerCityNetAudit')
    $hostArguments = $prefix + @('/Game/Maps/Forest?game=/Script/Riftbound.VoyagerGameMode?listen') + $common + @("-port=$Port", ('-abslog="' + $hostLogPath + '"'))
    $hostProcess = Start-Process -FilePath $executable -ArgumentList $hostArguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    $ownedProcesses.Add($hostProcess)
    Write-Host "Voyager city network audit: hidden listen host PID $($hostProcess.Id), UDP $Port."
    $ready = $false
    while ($clock.Elapsed.TotalSeconds -lt 50) {
        $hostText = Read-SharedLog $hostLogPath
        Assert-OwnedProcessesAlive
        if ($hostText -match ($failurePattern + '|Failed to init listen|Failed to bind')) { throw 'Listen host failed during startup.' }
        if ($hostText -match 'VOYAGER READY\b' -and $hostText -match ("IpNetDriver listening on port $Port\b")) { $ready = $true; break }
        Start-Sleep -Milliseconds 400
    }
    if (-not $ready) { throw 'Listen host did not become ready within 50 seconds.' }
    $clientArguments = $prefix + @("127.0.0.1:$Port") + $common + @(('-abslog="' + $clientLogPath + '"'))
    $clientProcess = Start-Process -FilePath $executable -ArgumentList $clientArguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    $ownedProcesses.Add($clientProcess)
    Write-Host "Voyager city network audit: hidden client PID $($clientProcess.Id); remote planet crowd and dialogue RPC checks."
    $lastStatus = 0
    while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $hostText = Read-SharedLog $hostLogPath
        $clientText = Read-SharedLog $clientLogPath
        Assert-OwnedProcessesAlive
        $checks = Get-NetworkChecks $hostText $clientText
        if (($hostText + "`n" + $clientText) -match $failurePattern) { throw 'City network audit or engine reported a failure; inspect retained logs.' }
        $pending = @($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key })
        if ($pending.Count -eq 0) { break }
        if ($clock.Elapsed.TotalSeconds - $lastStatus -gt 20) {
            $lastStatus = [int]$clock.Elapsed.TotalSeconds
            Write-Host "City network audit at $lastStatus seconds: $($pending.Count) checks pending."
        }
        Start-Sleep -Milliseconds 400
    }
    $checks = Get-NetworkChecks $hostText $clientText
    $missing = @($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key })
    if ($missing.Count -gt 0) { throw "City network audit incomplete: $($missing -join ', ')." }
}
catch { $failure = $_.Exception.Message }
finally {
    # Only handles returned by this script's Start-Process calls are owned.
    foreach ($process in $ownedProcesses) {
        if (-not $process.HasExited) {
            Stop-Process -InputObject $process -Force
            $process.WaitForExit(10000) | Out-Null
        }
    }
    $clock.Stop()
    try {
        $hostText = Read-SharedLog $hostLogPath
        $clientText = Read-SharedLog $clientLogPath
        $checks = Get-NetworkChecks $hostText $clientText
        $afterSaves = @(Get-ExpeditionHashes)
        $savePreserved = $true
        for ($index = 0; $index -lt $beforeSaves.Count; $index++) {
            if ($beforeSaves[$index].exists -ne $afterSaves[$index].exists -or $beforeSaves[$index].sha256 -ne $afterSaves[$index].sha256) {
                $savePreserved = $false
                $failure = 'Normal Expedition save changed during the audit: ' + $beforeSaves[$index].path
            }
        }
        if (($hostText + "`n" + $clientText) -match $failurePattern -and -not $failure) { $failure = 'Engine or city network audit failure detected during cleanup.' }
    }
    catch { if (-not $failure) { $failure = $_.Exception.Message } }
    [ordered]@{
        status = $(if ($failure) { 'failed' } else { 'passed' })
        started_utc = $startedAt.ToString('o')
        elapsed_seconds = [Math]::Round($clock.Elapsed.TotalSeconds, 2)
        packaged = [bool]$Packaged
        rendered = $false
        interpretation = 'Two real Unreal processes with a listen server and remote client. NullRHI verifies replication and gameplay RPCs, not graphics performance.'
        port = $Port
        process_ids = @($ownedProcesses | ForEach-Object { $_.Id })
        checks = $checks
        host_evidence = @([regex]::Matches($hostText, 'VOYAGER CITY NETWORK AUDIT PASS[^\r\n]*') | ForEach-Object { $_.Value })
        client_evidence = @([regex]::Matches($clientText, 'VOYAGER CITY NETWORK AUDIT PASS[^\r\n]*') | ForEach-Object { $_.Value })
        expedition_save_preserved = $savePreserved
        expedition_saves_before = $beforeSaves
        expedition_saves_after = $afterSaves
        failure = $failure
        logs = @($hostLogPath, $clientLogPath)
    } | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $reportPath -Encoding utf8
}

if ($failure) { throw $failure }
Write-Host "Voyager city network audit PASSED. Report: $reportPath"
