<#
.SYNOPSIS
Exercises the integrated city phone, economy, work, evidence and saves in Riftbound.
.DESCRIPTION
Runs an already built game with its real fixed-step city worker. A shift takes a
full simulated hour (60 real seconds with the shipped economy settings). Fixtures
relocate the character to actual buildings; all transactions use normal commands.
The test uses Voyager-Automation-CityLife and hashes both normal expedition saves.
Only processes started by this script are stopped. No build or asset changes occur.
#>
[CmdletBinding()]
param([switch]$Render,[switch]$Packaged,[switch]$Network,[ValidateRange(240,480)][int]$TimeoutSeconds=300)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$variant=$(if($Packaged){'Packaged'}else{'Editor'})+$(if($Network){'Network'}else{'Solo'})+$(if($Render){'Rendered'}else{'Headless'})
$runId=[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
$runDirectory=Join-Path $projectRoot ('Saved\VoyagerCityLifeAudit\'+$variant+'-'+$runId)
[IO.Directory]::CreateDirectory($runDirectory)|Out-Null
$logPath=Join-Path $runDirectory 'Host.log'
$peerLog=Join-Path $runDirectory 'Peer.log'
$reportPath=Join-Path $projectRoot ('Saved\VoyagerCityLife'+$variant+'Report.json')
$executable='C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if($Render){$executable=$executable.Replace('UnrealEditor-Cmd.exe','UnrealEditor.exe')}
if($Packaged){$executable=Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Binaries\Win64\Riftbound.exe'}
function Read-Shared([string]$Path){
    if(-not(Test-Path -LiteralPath $Path -PathType Leaf)){return ''}
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=[IO.StreamReader]::new($stream)
    try{return $reader.ReadToEnd()}finally{$reader.Dispose()}
}
function Get-ExpeditionHashes {
    foreach($relative in @('Saved\SaveGames\Voyager-Expedition.sav','Packaged\Riftbound\Windows\Riftbound\Saved\SaveGames\Voyager-Expedition.sav')){
        $path=Join-Path $projectRoot $relative
        $exists=Test-Path -LiteralPath $path -PathType Leaf
        [pscustomobject]@{path=$relative;exists=[bool]$exists;sha256=$(if($exists){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}else{$null})}
    }
}
$map='/Game/Maps/Forest?game=/Script/Riftbound.VoyagerGameMode'
if($Network){$map+='?listen'}
$common=@('-game','-nosound','-unattended','-NoSplash','-NoScreenMessages','-NoVoyagerLocalAI','-VoyagerCityLifeAudit','-VoyagerCityVisit','-ExecCmds="t.MaxFPS 60"')
$arguments=@(('"'+(Join-Path $projectRoot 'Riftbound.uproject')+'"'),$map)+$common+@('-port=7795',('-abslog="'+$logPath+'"'),('-VoyagerCityLifeOutput="'+$runDirectory+'"'))
if($Packaged){$arguments=$arguments[1..($arguments.Length-1)]}
if($Render){$arguments+=@('-windowed','-ResX=1280','-ResY=720','-VoyagerCityLifeCapture')}else{$arguments+='-NullRHI'}
$before=@(Get-ExpeditionHashes)
$after=@()
$started=[DateTime]::UtcNow
$owned=$null;$peer=$null;$failure=$null;$checks=[ordered]@{};$captures=@();$log='';$peerText=''
$clock=[Diagnostics.Stopwatch]::StartNew()
$pattern='VOYAGER CITY LIFE AUDIT FAIL\b|Fatal error\b|LowLevelFatalError|Assertion failed\b|Ensure condition failed|Failed to compile Material|Failed to compile global shader|missing usage flag|VOYAGER CHARACTER MISSING|VOYAGER CITY LIFE rejected incompatible/corrupt'
try{
    if(-not(Test-Path -LiteralPath $executable -PathType Leaf)){throw "Missing executable: $executable"}
    $owned=Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "City Life audit PID $($owned.Id), $variant; evidence $runDirectory"
    while($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds){
        $log=Read-Shared $logPath
        if($log -match $pattern){throw 'City Life audit or engine reported an error; inspect the retained log.'}
        if($Network -and -not $peer -and $log -match 'VOYAGER READY'){
            $peerArguments=@(('"'+(Join-Path $projectRoot 'Riftbound.uproject')+'"'),'127.0.0.1:7795')+$common+@('-NullRHI',('-abslog="'+$peerLog+'"'))
            if($Packaged){$peerArguments=$peerArguments[1..($peerArguments.Length-1)]}
            $peer=Start-Process -FilePath $executable -ArgumentList $peerArguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
        }
        if($peer){$peerText=Read-Shared $peerLog;if($peerText -match $pattern){throw 'City Life peer reported an error.'}}
        if($owned.HasExited){break}
        Start-Sleep -Milliseconds 500
    }
    if(-not $owned.HasExited){throw 'City Life audit timed out.'}
    if($owned.ExitCode -ne 0){throw "Game exited with code $($owned.ExitCode)."}
    if($Network -and (-not $peer -or $peerText -notmatch 'VOYAGER CITY LIFE AUDIT CLIENT COMPLETE PASS')){throw 'The network peer did not complete its checks before the host exited.'}
}catch{$failure=$_.Exception.Message}finally{
    foreach($process in @($owned,$peer)){
        if($process -and -not $process.HasExited){Stop-Process -InputObject $process -Force;$process.WaitForExit(10000)|Out-Null}
    }
    $clock.Stop()
    try{
        $log=Read-Shared $logPath;$peerText=Read-Shared $peerLog
        $required=@('INITIAL_MONEY_CONSERVED','FIFTY_FOUR_THOUSAND_RESIDENTS','NINE_NEEDS_EIGHT_GOODS','OWNER_SNAPSHOT_DELIVERED',
            'PHONE_MODAL_INPUT','PHONE_BLOCKS_WEAPON_ACTION','N_KEY_CYCLES_ALL_DESTINATIONS','PHONE_EIGHT_CYCLES_NAVIGATION','FIVE_PHONE_PAGES','PHONE_CLOSE_RESTORES_INPUT',
            'OUTSIDE_PURCHASE_REJECTED','REJECTED_TRADE_PRESERVES_CARGO','MARKET_PURCHASE_TRANSFERS_CREDITS',
            'FOOD_CONSUMED_AND_NEED_SATISFIED','MINERAL_TRADE_DEDUCTS_EXACTLY_TEN','JOB_AT_REAL_WORKPLACE',
            'WORK_SHIFT_STARTED','REAL_HOUR_EARNS_WAGES','WORK_REQUIRES_REAL_WORKPLACE','LOW_CONFIDENCE_EVIDENCE_REJECTED',
            'ACTUAL_WITNESS_STARTS_PURSUIT','OBSERVED_OFFENSE_HAS_FINE_AND_RECORD','FINE_PAYMENT_REQUIRES_CIVIC_BUILDING',
            'FINE_PAYMENT_EXACT_RECORD_RETAINED','SETTLED_FINE_NO_PURSUIT','HOME_WASH_SATISFIES_HYGIENE','HOME_REST_REDUCES_PRESSURE',
            'RENT_CHANGES_ADDRESS_AND_TRANSFERS_CREDITS','HOME_BILL_ACTION_ACCEPTED','VISIBLE_CITIZEN_HOME_WORK_MATCH_CORE','CITIZEN_FATAL_DAMAGE_REACHES_CORE',
            'FIFTEEN_CITY_ARCHIVES_ON_DISK','FIFTEEN_ARCHIVES_RELOAD_AND_CONSERVE','PLAYER_WALLET_HOME_FINE_ROUND_TRIP','NPC_DEATH_HOME_WORK_ROUND_TRIP','CORRUPT_PACKED_ARCHIVE_REJECTED',
            'ALL_CITY_MONEY_CONSERVED','CONTROLLER_TEARDOWN_PRESERVES_CARGO')
        foreach($name in $required){$checks[$name]=$log -match ("VOYAGER CITY LIFE AUDIT PASS $name\b")}
        $checks['COMPLETE']=$log -match 'VOYAGER CITY LIFE AUDIT COMPLETE PASS'
        $checks['NO_ENGINE_ERRORS']=$log -notmatch $pattern
        if($Network){
            foreach($name in @('DISTINCT_COOP_RESIDENTS','HOST_TRADE_ISOLATED_FROM_PEER')){$checks[$name]=$log -match ("VOYAGER CITY LIFE AUDIT PASS $name\b")}
            foreach($name in @('REMOTE_CITY_SNAPSHOT','REMOTE_PHONE_OPENS','REMOTE_LIVE_CLOCK','REMOTE_ACCOUNT_UNTOUCHED')){$checks[$name]=$peerText -match ("VOYAGER CITY LIFE AUDIT PASS $name\b")}
            $checks['PEER_COMPLETE']=$peerText -match 'VOYAGER CITY LIFE AUDIT CLIENT COMPLETE PASS'
            $checks['PEER_NO_ENGINE_ERRORS']=$peerText -notmatch $pattern
        }
        $after=@(Get-ExpeditionHashes);$preserved=$before.Count -eq $after.Count
        for($i=0;$i -lt $before.Count;$i++){
            if($before[$i].exists -ne $after[$i].exists -or $before[$i].sha256 -ne $after[$i].sha256){$preserved=$false}
        }
        $checks['NORMAL_SAVES_UNCHANGED']=$preserved
        if($Render){
            Add-Type -AssemblyName System.Drawing
            $seenHashes=[Collections.Generic.HashSet[string]]::new()
            foreach($name in @('Overview','Market','WorkHome','Justice','News','Navigation')){
                $path=Join-Path $runDirectory ($name+'.png')
                $valid=Test-Path -LiteralPath $path -PathType Leaf
                $width=0;$height=0;$imageHash=$null
                if($valid){
                    $info=Get-Item -LiteralPath $path
                    $bitmap=[Drawing.Bitmap]::new($path)
                    try{$width=$bitmap.Width;$height=$bitmap.Height}finally{$bitmap.Dispose()}
                    $imageHash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
                    $valid=$width -eq 1280 -and $height -eq 720 -and $info.Length -gt 10000 -and $info.LastWriteTimeUtc -ge $started.AddSeconds(-2)
                    if(-not $seenHashes.Add($imageHash)){$valid=$false}
                }
                $checks["CAPTURE_$name"]=[bool]$valid
                $captures+=[pscustomobject]@{page=$name;path=$path;width=$width;height=$height;sha256=$imageHash;passed=[bool]$valid}
            }
        }
        if(-not $failure -and @($checks.Values|Where-Object{-not $_}).Count){$failure='Required checks failed: '+(($checks.Keys|Where-Object{-not $checks[$_]}) -join ', ')}
    }catch{if($failure){$failure+=' '+$_.Exception.Message}else{$failure=$_.Exception.Message}}
    $report=[ordered]@{
        status=$(if($failure){'failed'}else{'passed'});variant=$variant;started_utc=$started.ToString('o')
        elapsed_seconds=[Math]::Round($clock.Elapsed.TotalSeconds,3);checks=$checks;failure=$failure
        process_id=$(if($owned){$owned.Id}else{$null});peer_process_id=$(if($peer){$peer.Id}else{$null})
        log=$logPath;peer_log=$peerLog;normal_saves_before=$before;normal_saves_after=$after;screenshots=$captures
        evidence=@([regex]::Matches($log,'VOYAGER CITY LIFE AUDIT (PASS|TRANSACTION|WAGES|METRICS)[^\r\n]*')|ForEach-Object{$_.Value})
        coverage='Real 20 Hz worker, phone/navigation bindings, location-validated purchases/work/citations, mineral escrow, an hour of wages, home washing/rest/rental, bill-action acknowledgement, NPC identity/death synchronization, reload of all fifteen saved city cores, money conservation, player/NPC state round trips and controller-teardown cargo preservation. This short run does not cross a billing day. Fixture teleports only place the test character at actual buildings.'
        performance_caveat='Wall-clock average FPS and worst frame include screenshot, relocation, streaming and save overhead after the first eight startup seconds. Peak worker time is sampled from snapshots. These audit measurements are not steady gameplay performance guarantees.'
    }
    $json=$report|ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText((Join-Path $runDirectory 'Report.json'),$json)
    [IO.File]::WriteAllText($reportPath,$json)
}
Write-Host "City Life audit report: $reportPath"
if($failure){throw $failure}
Write-Host 'City Life audit passed; both normal expedition saves are unchanged.'
