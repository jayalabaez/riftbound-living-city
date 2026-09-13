param(
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [ValidateRange(1024, 65535)][int]$Port = 7787,
    [ValidateRange(30, 300)][int]$TimeoutSeconds = 125,
    [ValidateRange(5, 60)][int]$StartupTimeoutSeconds = 45
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$projectPath = Join-Path $projectRoot 'Riftbound.uproject'
$editorPath = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$logDirectory = Join-Path $projectRoot 'Saved\Logs'
$hostLogPath = Join-Path $logDirectory 'NetworkHost.log'
$clientLogPath = Join-Path $logDirectory 'NetworkClient.log'
$reportPath = Join-Path $projectRoot 'Saved\NetworkTestReport.json'
$ownedProcesses = [Collections.Generic.List[Diagnostics.Process]]::new()
$testClock = [Diagnostics.Stopwatch]::StartNew()
$checks = [ordered]@{}
$failure = $null
$hostReady = $false
$hostProcess = $null
$clientProcess = $null
$hasUdpInspection = $null -ne (Get-Command Get-NetUDPEndpoint -ErrorAction SilentlyContinue)

function Read-TestLog([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    # Unreal appends while the test reads. Share the file explicitly so polling
    # never blocks its writer or mistakes a sharing violation for a crash.
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() }
    finally { $reader.Dispose() }
}

function Test-ProcessRunning([Diagnostics.Process]$Process) {
    if ($null -eq $Process) { return $false }
    try {
        $Process.Refresh()
        return -not $Process.HasExited
    }
    catch { return $false }
}

function Get-TestChecks([string]$HostText, [string]$ClientText) {
    return [ordered]@{
        host_is_listen_server = $HostText -match 'RIFT PROBE net=2\b'
        client_is_network_client = $ClientText -match 'RIFT PROBE net=3\b'
        host_sees_two_players = $HostText -match 'RIFT PROBE net=2 players=(?:[2-9]|[1-9]\d+)\b'
        client_sees_two_players = $ClientText -match 'RIFT PROBE net=3 players=(?:[2-9]|[1-9]\d+)\b'
        host_reaches_victory = $HostText -match 'RIFTBOUND: victory' -and $HostText -match 'RIFT PROBE net=2[^\r\n]*phase=4\b'
        client_replicates_victory = $ClientText -match 'RIFT PROBE net=3[^\r\n]*phase=4\b'
        client_fire_rpc = $ClientText -match 'RIFT CLIENT TEST PASS[:\s]+FIRE\b'
        client_reload_rpc = $ClientText -match 'RIFT CLIENT TEST PASS[:\s]+RELOAD\b'
        client_grenade_rpc = $ClientText -match 'RIFT CLIENT TEST PASS[:\s]+GRENADE\b'
        client_movement = $ClientText -match 'RIFT CLIENT TEST PASS[:\s]+MOVEMENT\b'
        client_world_replication = $ClientText -match 'RIFT CLIENT TEST PASS[:\s]+REPLICATION\b'
        host_restart = $HostText -match 'RIFT HOST TEST PASS[:\s]+RESTART\b'
        no_client_failure_marker = $ClientText -notmatch 'RIFT CLIENT TEST FAIL\b'
        no_movement_base_errors = ($HostText + "`n" + $ClientText) -notmatch 'could not resolve the new relative movement base'
        no_fatal_errors = ($HostText + "`n" + $ClientText) -notmatch 'Fatal error:|LowLevelFatalError|Assertion failed:'
    }
}

try {
    foreach ($requiredPath in @($editorPath, $projectPath,
            (Join-Path $projectRoot 'Binaries\Win64\UnrealEditor-Riftbound.dll'),
            (Join-Path $projectRoot 'Content\Maps\Forest.umap'))) {
        if (-not (Test-Path -LiteralPath $requiredPath)) {
            throw "Missing prerequisite: $requiredPath. Run Scripts\Build.ps1 first."
        }
    }
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    if ($hasUdpInspection) {
        $existingEndpoints = @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue)
        if ($existingEndpoints.Count -gt 0) {
            $ownerIds = ($existingEndpoints.OwningProcess | Sort-Object -Unique) -join ', '
            throw "UDP port $Port is already owned by process(es) $ownerIds. Choose another -Port; this test will not stop existing programs."
        }
    }
    # These two paths are owned test logs. Save each previous run beside them.
    $archiveStamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    foreach ($logPath in @($hostLogPath, $clientLogPath)) {
        if (Test-Path -LiteralPath $logPath) {
            Copy-Item -LiteralPath $logPath -Destination ($logPath + '.' + $archiveStamp + '.previous')
        }
        [IO.File]::WriteAllText($logPath, '')
    }

    $commonArguments = @(('"' + $projectPath + '"'), '-game', '-NullRHI', '-unattended',
        '-nosound', '-NoSplash', '-NoScreenMessages', '-RiftProbe')
    $hostArguments = @($commonArguments[0], '/Game/Maps/Forest?game=/Script/Riftbound.RiftGameMode?listen') +
        $commonArguments[1..($commonArguments.Count - 1)] +
        @("-port=$Port", '-RiftAutoTest', ('-abslog="' + $hostLogPath + '"'))
    $hostProcess = Start-Process -FilePath $editorPath -ArgumentList $hostArguments `
        -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    $ownedProcesses.Add($hostProcess)
    Write-Host "Network test: hidden listen host PID $($hostProcess.Id), UDP $Port."

    $startupDeadline = [Math]::Min($StartupTimeoutSeconds, $TimeoutSeconds)
    while ($testClock.Elapsed.TotalSeconds -lt $startupDeadline) {
        $hostText = Read-TestLog $hostLogPath
        if (-not (Test-ProcessRunning $hostProcess)) {
            throw "Listen host exited during startup. Inspect $hostLogPath."
        }
        if ($hostText -match 'Fatal error:|LowLevelFatalError|Assertion failed:|Failed to init listen|Failed to bind') {
            throw "Listen host reported a startup failure. Inspect $hostLogPath."
        }
        $portReady = $hostText -match "IpNetDriver listening on port $Port\b"
        if (-not $portReady -and $hasUdpInspection) {
            $portReady = @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue |
                Where-Object { $_.OwningProcess -eq $hostProcess.Id }).Count -gt 0
        }
        if ($hostText -match 'dynamic world ready' -and $portReady) {
            $hostReady = $true
            break
        }
        Start-Sleep -Milliseconds 400
    }
    if (-not $hostReady) {
        throw "Host did not finish world initialization and bind UDP $Port within $startupDeadline seconds. Inspect $hostLogPath."
    }

    $clientArguments = @($commonArguments[0], "127.0.0.1:$Port") +
        $commonArguments[1..($commonArguments.Count - 1)] +
        @('-RiftClientTest', ('-abslog="' + $clientLogPath + '"'))
    $clientProcess = Start-Process -FilePath $editorPath -ArgumentList $clientArguments `
        -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    $ownedProcesses.Add($clientProcess)
    Write-Host "Network test: hidden client PID $($clientProcess.Id); checking replication, RPCs and victory."

    $lastStatusSecond = -20
    while ($testClock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $hostText = Read-TestLog $hostLogPath
        $clientText = Read-TestLog $clientLogPath
        $checks = Get-TestChecks $hostText $clientText
        if (-not $checks.no_movement_base_errors) {
            throw 'Character movement base replication failed: Unreal could not resolve the new relative movement base. Inspect the retained host/client logs.'
        }
        if (-not $checks.no_fatal_errors -or -not $checks.no_client_failure_marker) {
            throw 'Unreal or the client probe reported a failure. See the retained host/client logs.'
        }
        if (-not (Test-ProcessRunning $hostProcess)) { throw "Listen host exited unexpectedly. Inspect $hostLogPath." }
        if (-not (Test-ProcessRunning $clientProcess)) { throw "Network client exited unexpectedly. Inspect $clientLogPath." }
        $pending = @($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key })
        if ($pending.Count -eq 0) { break }
        if ($testClock.Elapsed.TotalSeconds - $lastStatusSecond -ge 20) {
            $lastStatusSecond = [int]$testClock.Elapsed.TotalSeconds
            Write-Host "Network test: $lastStatusSecond sec; $($pending.Count) checks pending."
        }
        Start-Sleep -Milliseconds 500
    }

    $checks = Get-TestChecks (Read-TestLog $hostLogPath) (Read-TestLog $clientLogPath)
    $missingChecks = @($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key })
    if ($missingChecks.Count -gt 0) {
        throw "Network test timed out after $TimeoutSeconds seconds. Missing checks: $($missingChecks -join ', '). Inspect $hostLogPath and $clientLogPath."
    }
}
catch {
    $failure = $_.Exception.Message
}
finally {
    # Only processes returned by the two Start-Process calls belong to this run.
    # Never search by executable name or terminate the user's open editor/game.
    foreach ($ownedProcess in $ownedProcesses) {
        if (Test-ProcessRunning $ownedProcess) {
            Stop-Process -InputObject $ownedProcess -Force -ErrorAction SilentlyContinue
            try { $null = $ownedProcess.WaitForExit(1500) } catch { }
        }
    }
    $testClock.Stop()
    $report = [ordered]@{
        status = $(if ($null -eq $failure) { 'passed' } else { 'failed' })
        completed_at = [DateTimeOffset]::Now.ToString('o')
        elapsed_seconds = [Math]::Round($testClock.Elapsed.TotalSeconds, 2)
        port = $Port
        host_ready = $hostReady
        host_pid = $(if ($hostProcess) { $hostProcess.Id } else { $null })
        client_pid = $(if ($clientProcess) { $clientProcess.Id } else { $null })
        checks = $checks
        failure = $failure
        host_log = $hostLogPath
        client_log = $clientLogPath
        host_last_probe = @([regex]::Matches((Read-TestLog $hostLogPath), 'RIFT PROBE[^\r\n]*') | ForEach-Object { $_.Value } | Select-Object -Last 1)
        client_last_probe = @([regex]::Matches((Read-TestLog $clientLogPath), 'RIFT PROBE[^\r\n]*') | ForEach-Object { $_.Value } | Select-Object -Last 1)
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $reportPath) -Force | Out-Null
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding UTF8
}

if ($null -ne $failure) {
    Write-Host "Network test FAILED: $failure"
    Write-Host "Report: $reportPath"
    throw $failure
}
Write-Host "Network test PASSED: two players, movement, fire/reload/grenade RPCs, world replication, synchronized victory and restart; no movement base errors ($($report.elapsed_seconds) sec)."
Write-Host "Report: $reportPath"
