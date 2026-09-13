param(
    [switch]$Network,
    [switch]$Render,
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [ValidateRange(1024, 65535)][int]$Port = 7789,
    [ValidateRange(30, 1200)][int]$TimeoutSeconds = 600,
    [ValidateRange(5, 120)][int]$StartupTimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$projectPath = Join-Path $projectRoot 'Riftbound.uproject'
$editorName = if ($Render) { 'UnrealEditor.exe' } else { 'UnrealEditor-Cmd.exe' }
$editorPath = Join-Path $EngineRoot ('Engine\Binaries\Win64\' + $editorName)
$logDirectory = Join-Path $projectRoot 'Saved\Logs'
$soloLogPath = Join-Path $logDirectory 'VoyagerSolo.log'
$hostLogPath = Join-Path $logDirectory 'VoyagerHost.log'
$clientLogPath = Join-Path $logDirectory 'VoyagerClient.log'
$reportName = if ($Network) { 'VoyagerNetworkReport.json' } else { 'VoyagerTestReport.json' }
$reportPath = Join-Path $projectRoot ('Saved\' + $reportName)
$ownedProcesses = [Collections.Generic.List[Diagnostics.Process]]::new()
$testClock = [Diagnostics.Stopwatch]::StartNew()
$checks = [ordered]@{}
$failure = $null
$hostReady = $false
$soloProcess = $null
$hostProcess = $null
$clientProcess = $null
$lastStatusSecond = -20
$hasUdpInspection = $null -ne (Get-Command Get-NetUDPEndpoint -ErrorAction SilentlyContinue)
$failurePattern = 'VOYAGER TEST FAIL\b|Fatal error\b|LowLevelFatalError|Assertion failed\b|could not resolve the new relative movement base'
$runLogPaths = if ($Network) { @($hostLogPath, $clientLogPath) } else { @($soloLogPath) }

function Read-TestLog([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    # Poll without denying Unreal access to its active log file.
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

function Get-TestChecks([string]$TestText, [string]$HostText) {
    $result = [ordered]@{}
    foreach ($marker in @('WALK_SCAN', 'MINING', 'BOARD', 'FLIGHT_ORBIT',
            'SEAMLESS_ASCENT', 'CONTINUOUS_CRUISE', 'SEAMLESS_DESCENT', 'SECOND_PLANET_WALK',
            'PULSE_JUMP', 'PLANET_LANDING', 'DISEMBARK', 'SECOND_DISCOVERY', 'NEXT_SYSTEM')) {
        $result[$marker.ToLowerInvariant()] = $TestText -match ('VOYAGER TEST PASS\s+' + $marker + '\b')
    }
    $result['flight_continuity'] = $TestText -match 'VOYAGER CONTINUITY PASS\s+max_step_cm='
    if ($Network) {
        $result['host_is_listen_server'] = $HostText -match 'VOYAGER PROBE net=2\b'
        $result['client_is_network_client'] = $TestText -match 'VOYAGER PROBE net=3\b'
        $result['host_sees_two_players'] = $HostText -match 'VOYAGER PROBE net=2 players=(?:[2-9]|[1-9]\d+)\b'
        $result['client_sees_two_players'] = $TestText -match 'VOYAGER PROBE net=3 players=(?:[2-9]|[1-9]\d+)\b'
        $result['host_reaches_next_system'] = $HostText -match 'VOYAGER PROBE net=2[^\r\n]*\bsystem=2\b'
        $result['client_replicates_next_system'] = $TestText -match 'VOYAGER PROBE net=3[^\r\n]*\bsystem=2\b'
    }
    else {
        $result['solo_is_standalone'] = $TestText -match 'VOYAGER PROBE net=0\b'
        $result['space_combat'] = $TestText -match 'VOYAGER TEST PASS\s+SPACE_COMBAT\b'
        $result['save_readback'] = $TestText -match 'VOYAGER TEST PASS\s+SAVE_READBACK\b'
    }
    $allText = $HostText + "`n" + $TestText
    $result['complete'] = $TestText -match 'VOYAGER TEST COMPLETE PASS\b'
    $result['no_test_failures'] = $allText -notmatch 'VOYAGER TEST FAIL\b'
    $result['no_movement_base_errors'] = $allText -notmatch 'could not resolve the new relative movement base'
    $result['no_fatal_errors'] = $allText -notmatch 'Fatal error\b|LowLevelFatalError|Assertion failed\b'
    return $result
}

function Write-PendingStatus([string]$Phase, [string[]]$PendingChecks) {
    if ($testClock.Elapsed.TotalSeconds - $script:lastStatusSecond -ge 20) {
        $script:lastStatusSecond = [int]$testClock.Elapsed.TotalSeconds
        Write-Host "Voyager $Phase ($($script:lastStatusSecond) sec): pending $($PendingChecks -join ', ')."
    }
}

function Get-LastProbe([string]$Text) {
    return @([regex]::Matches($Text, 'VOYAGER PROBE[^\r\n]*') |
        ForEach-Object { $_.Value } | Select-Object -Last 1)
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
    if ($Network -and $hasUdpInspection) {
        $existingEndpoints = @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue)
        if ($existingEndpoints.Count -gt 0) {
            $ownerIds = ($existingEndpoints.OwningProcess | Sort-Object -Unique) -join ', '
            throw "UDP port $Port is owned by process(es) $ownerIds. Choose another -Port; existing programs will not be stopped."
        }
    }

    $archiveStamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    foreach ($logPath in $runLogPaths) {
        if (Test-Path -LiteralPath $logPath) {
            Copy-Item -LiteralPath $logPath -Destination ($logPath + '.' + $archiveStamp + '.previous')
        }
        [IO.File]::WriteAllText($logPath, '')
    }

    $commonArguments = @('-game', '-nosound', '-unattended', '-NoSplash', '-NoScreenMessages', '-VoyagerProbe')
    if ($Render) {
        $commonArguments += @('-windowed', '-ResX=1280', '-ResY=720', '-VoyagerCapture')
    }
    else { $commonArguments += '-NullRHI' }
    $quotedProject = '"' + $projectPath + '"'
    $mapUrl = '/Game/Maps/Forest?game=/Script/Riftbound.VoyagerGameMode'

    if ($Network) {
        $hostArguments = @($quotedProject, ($mapUrl + '?listen')) + $commonArguments +
            @(("-port=$Port"), '-VoyagerNetTest', ('-abslog="' + $hostLogPath + '"'))
        $hostProcess = Start-Process -FilePath $editorPath -ArgumentList $hostArguments `
            -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
        $ownedProcesses.Add($hostProcess)
        Write-Host "Voyager network: hidden listen host PID $($hostProcess.Id), UDP $Port."

        $startupDeadline = [Math]::Min($StartupTimeoutSeconds, $TimeoutSeconds)
        while ($testClock.Elapsed.TotalSeconds -lt $startupDeadline) {
            $hostText = Read-TestLog $hostLogPath
            if ($hostText -match ($failurePattern + '|Failed to init listen|Failed to bind')) {
                throw "Host reported a startup failure. Inspect $hostLogPath."
            }
            if (-not (Test-ProcessRunning $hostProcess)) {
                throw "Host exited during startup. Inspect $hostLogPath."
            }
            $portReady = $hostText -match ("IpNetDriver listening on port $Port\b")
            if (-not $portReady -and $hasUdpInspection) {
                $portReady = @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue |
                    Where-Object { $_.OwningProcess -eq $hostProcess.Id }).Count -gt 0
            }
            if ($hostText -match 'VOYAGER READY\b' -and $portReady) {
                $hostReady = $true
                break
            }
            Write-PendingStatus 'host startup' @('VOYAGER READY and listening port')
            Start-Sleep -Milliseconds 400
        }
        if (-not $hostReady) {
            throw "Host did not reach VOYAGER READY and bind UDP $Port within $startupDeadline seconds. Inspect $hostLogPath."
        }

        $clientArguments = @($quotedProject, ("127.0.0.1:$Port")) + $commonArguments +
            @('-VoyagerNetTest', ('-abslog="' + $clientLogPath + '"'))
        $clientProcess = Start-Process -FilePath $editorPath -ArgumentList $clientArguments `
            -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
        $ownedProcesses.Add($clientProcess)
        Write-Host "Voyager network: hidden client PID $($clientProcess.Id); the client drives the exploration test."
    }
    else {
        $soloArguments = @($quotedProject, $mapUrl) + $commonArguments +
            @('-VoyagerTest', ('-abslog="' + $soloLogPath + '"'))
        $soloProcess = Start-Process -FilePath $editorPath -ArgumentList $soloArguments `
            -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
        $ownedProcesses.Add($soloProcess)
        Write-Host "Voyager solo: hidden PID $($soloProcess.Id); using the separate Voyager-Automation save slot."
    }

    while ($testClock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        $hostText = if ($Network) { Read-TestLog $hostLogPath } else { '' }
        $testText = if ($Network) { Read-TestLog $clientLogPath } else { Read-TestLog $soloLogPath }
        $checks = Get-TestChecks $testText $hostText
        if (($hostText + "`n" + $testText) -match $failurePattern) {
            throw 'Unreal or the Voyager probe reported a test failure, assertion, fatal error, or unresolved movement base. Inspect the retained logs.'
        }
        foreach ($ownedProcess in $ownedProcesses) {
            if (-not (Test-ProcessRunning $ownedProcess)) {
                throw "Test process PID $($ownedProcess.Id) exited unexpectedly. Inspect the retained logs."
            }
        }
        $pending = @($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key })
        if ($pending.Count -eq 0) {
            # Give rendered captures a moment to finish writing before cleanup.
            if ($Render) { Start-Sleep -Seconds 2 }
            break
        }
        if ($checks.complete) {
            # A final network probe can arrive after the completion marker.
            $missingGameplay = @($pending | Where-Object {
                $_ -notin @('host_reaches_next_system', 'client_replicates_next_system')
            })
            if ($missingGameplay.Count -gt 0) {
                throw "Voyager completed without required checks: $($missingGameplay -join ', '). Inspect the retained logs."
            }
        }
        $phase = if ($Network) { 'network' } else { 'solo' }
        Write-PendingStatus $phase $pending
        Start-Sleep -Milliseconds 500
    }

    $hostText = if ($Network) { Read-TestLog $hostLogPath } else { '' }
    $testText = if ($Network) { Read-TestLog $clientLogPath } else { Read-TestLog $soloLogPath }
    $checks = Get-TestChecks $testText $hostText
    $missingChecks = @($checks.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key })
    if ($missingChecks.Count -gt 0) {
        throw "Voyager test failed or timed out after $TimeoutSeconds seconds. Missing checks: $($missingChecks -join ', '). Inspect the retained logs."
    }
}
catch {
    $failure = $_.Exception.Message
}
finally {
    # Only process objects returned by this run's Start-Process calls are owned.
    # Never stop the user's existing editor, game, or unrelated server.
    foreach ($ownedProcess in $ownedProcesses) {
        if (Test-ProcessRunning $ownedProcess) {
            Stop-Process -InputObject $ownedProcess -Force -ErrorAction SilentlyContinue
            try { $null = $ownedProcess.WaitForExit(1500) } catch { }
        }
    }
    $testClock.Stop()
    $hostText = if ($Network) { Read-TestLog $hostLogPath } else { '' }
    $testText = if ($Network) { Read-TestLog $clientLogPath } else { Read-TestLog $soloLogPath }
    $checks = Get-TestChecks $testText $hostText
    $report = [ordered]@{
        schema_version = 2
        flight_model = 'continuous_spherical_v1'
        status = $(if ($null -eq $failure) { 'passed' } else { 'failed' })
        mode = $(if ($Network) { 'network' } else { 'solo' })
        rendered = [bool]$Render
        completed_at = [DateTimeOffset]::Now.ToString('o')
        elapsed_seconds = [Math]::Round($testClock.Elapsed.TotalSeconds, 2)
        timeout_seconds = $TimeoutSeconds
        executable = $editorPath
        port = $(if ($Network) { $Port } else { $null })
        host_ready = $(if ($Network) { $hostReady } else { $null })
        solo_pid = $(if ($soloProcess) { $soloProcess.Id } else { $null })
        host_pid = $(if ($hostProcess) { $hostProcess.Id } else { $null })
        client_pid = $(if ($clientProcess) { $clientProcess.Id } else { $null })
        checks = $checks
        optional_observations = @{
            space_combat = $testText -match 'VOYAGER TEST PASS\s+SPACE_COMBAT\b'
        }
        failure = $failure
        logs = $runLogPaths
        host_last_probe = Get-LastProbe $hostText
        test_last_probe = Get-LastProbe $testText
        continuity_evidence = @([regex]::Matches($testText, 'VOYAGER CONTINUITY PASS[^\r\n]*') |
            ForEach-Object { $_.Value } | Select-Object -Last 1)
        failure_lines = @([regex]::Matches(($hostText + "`n" + $testText), ('[^\r\n]*(?:' + $failurePattern + ')[^\r\n]*')) |
            ForEach-Object { $_.Value } | Select-Object -Last 10)
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $reportPath) -Force | Out-Null
    $report | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $reportPath -Encoding UTF8
}

if ($null -ne $failure) {
    Write-Host "Voyager test FAILED: $failure"
    Write-Host "Report: $reportPath"
    throw $failure
}
Write-Host "Voyager $($report.mode) PASSED: $($checks.Count) checks in $($report.elapsed_seconds) sec."
Write-Host "Report: $reportPath"
