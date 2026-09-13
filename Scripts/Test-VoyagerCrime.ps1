<# Runs isolated real-fire / pursuit / hiding tests; -Network adds an innocent peer. #>
[CmdletBinding()]
param([switch]$Render,[switch]$Packaged,[switch]$Network,[ValidateRange(180,360)][int]$TimeoutSeconds=240)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$variant=$(if($Packaged){'Packaged'}else{'Editor'})+$(if($Network){'Network'}else{'Solo'})+$(if($Render){'Rendered'}else{'Headless'})
$runDirectory=Join-Path $projectRoot ('Saved\VoyagerCrimeAudit\'+$variant+'-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
[IO.Directory]::CreateDirectory($runDirectory)|Out-Null
$logPath=Join-Path $runDirectory 'Host.log'
$peerLog=Join-Path $runDirectory 'Peer.log'
$executable='C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if($Render){$executable=$executable.Replace('UnrealEditor-Cmd.exe','UnrealEditor.exe')}
if($Packaged){$executable=Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Binaries\Win64\Riftbound.exe'}
function Read-Shared([string]$Path){
    if(-not(Test-Path -LiteralPath $Path)){return ''}
    $stream=[IO.File]::Open($Path,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=[IO.StreamReader]::new($stream)
    try{return $reader.ReadToEnd()}finally{$reader.Dispose()}
}
function Save-Hashes {
    foreach($relative in @('Saved\SaveGames\Voyager-Expedition.sav','Packaged\Riftbound\Windows\Riftbound\Saved\SaveGames\Voyager-Expedition.sav')){
        $path=Join-Path $projectRoot $relative
        [pscustomobject]@{path=$relative;hash=$(if(Test-Path -LiteralPath $path){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}else{''})}
    }
}
$map='/Game/Maps/Forest?game=/Script/Riftbound.VoyagerGameMode'
if($Network){$map+='?listen'}
$common=@('-game','-nosound','-unattended','-NoSplash','-NoScreenMessages','-NoVoyagerLocalAI','-VoyagerCrimeAudit','-VoyagerCityVisit','-ExecCmds="t.MaxFPS 60"')
$arguments=@(('"'+(Join-Path $projectRoot 'Riftbound.uproject')+'"'),$map)+$common+@('-port=7794',('-abslog="'+$logPath+'"'),('-VoyagerCrimeOutput="'+$runDirectory+'"'))
if($Packaged){$arguments=$arguments[1..($arguments.Length-1)]}
if($Render){$arguments+=@('-windowed','-ResX=1280','-ResY=720','-VoyagerCrimeCapture')}else{$arguments+='-NullRHI'}
$before=@(Save-Hashes)
$owned=$null;$peer=$null;$failure=$null;$checks=[ordered]@{};$log='';$peerText=''
$clock=[Diagnostics.Stopwatch]::StartNew()
$pattern='VOYAGER CRIME AUDIT FAIL\b|Fatal error\b|LowLevelFatalError|Assertion failed\b|Ensure condition failed|Failed to compile Material|missing usage flag|VOYAGER CHARACTER MISSING'
try{
    $owned=Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "Crime audit PID $($owned.Id), $variant; evidence $runDirectory"
    while($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds){
        $log=Read-Shared $logPath
        if($log -match $pattern){throw 'Crime audit or engine reported an error.'}
        if($Network -and -not $peer -and $log -match 'VOYAGER READY'){
            $peerArguments=@(('"'+(Join-Path $projectRoot 'Riftbound.uproject')+'"'),'127.0.0.1:7794')+$common+@('-NullRHI',('-abslog="'+$peerLog+'"'))
            if($Packaged){$peerArguments=$peerArguments[1..($peerArguments.Length-1)]}
            $peer=Start-Process -FilePath $executable -ArgumentList $peerArguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
        }
        if($peer){$peerText=Read-Shared $peerLog;if($peerText -match $pattern){throw 'Crime audit peer reported an error.'}}
        if($owned.HasExited){break}
        Start-Sleep -Milliseconds 450
    }
    if(-not $owned.HasExited){throw 'Crime audit timed out.'}
    if($owned.ExitCode -ne 0){throw "Game exited with code $($owned.ExitCode)."}
}catch{$failure=$_.Exception.Message}finally{
    foreach($process in @($owned,$peer)){if($process -and -not $process.HasExited){Stop-Process -InputObject $process -Force;$process.WaitForExit(10000)|Out-Null}}
    $clock.Stop();$log=Read-Shared $logPath;$peerText=Read-Shared $peerLog
    foreach($name in @('SIDEARM_EQUIPPED','CITIZEN_PRESENT','SIDEARM_REAL_HIT','ASSAULT_WANTED','CITIZEN_KILLABLE','HOMICIDE_ESCALATION','DEAD_CITIZEN_INACTIVE','PATROL_DISPATCH_AND_SENSOR','PATROL_WEAPON_DAMAGE','BUILDING_BREAKS_LINE_OF_SIGHT','SEARCH_LAST_KNOWN_POSITION','ESCAPED_SEARCH_COOLDOWN','DEATH_SURVIVES_CORPSE_RETIREMENT','FIVE_STAR_LIMIT','PLAYER_RECOVERY_CARGO_PRESERVED','PATROL_DESTROYED_AND_ESCALATED')){
        $checks[$name]=$log -match ("VOYAGER CRIME AUDIT PASS $name\b")
    }
    $checks['COMPLETE']=$log -match 'VOYAGER CRIME AUDIT COMPLETE PASS'
    $checks['NO_ENGINE_ERRORS']=$log -notmatch $pattern
    if($Network){foreach($name in @('REMOTE_WANTED_REPLICATED','REMOTE_CITIZEN_DEATH_REPLICATED','REMOTE_PATROL_REPLICATED','INNOCENT_PEER_UNWANTED')){$checks[$name]=$peerText -match ("VOYAGER CRIME AUDIT PASS $name\b")};$checks['PEER_COMPLETE']=$peerText -match 'VOYAGER CRIME AUDIT CLIENT COMPLETE PASS'}
    $after=@(Save-Hashes);$preserved=$true
    for($i=0;$i -lt $before.Count;$i++){if($before[$i].hash -ne $after[$i].hash){$preserved=$false}}
    $checks['NORMAL_SAVES_UNCHANGED']=$preserved
    if($Render){foreach($name in @('CitizenBefore','CrimeScene','PatrolResponse','Searching','KestrelShip')){$path=Join-Path $runDirectory ($name+'.png');$checks["CAPTURE_$name"]=(Test-Path -LiteralPath $path) -and (Get-Item -LiteralPath $path).Length -gt 10000}}
    if(-not $failure -and @($checks.Values|Where-Object{-not $_}).Count){$failure='Required checks failed: '+(($checks.Keys|Where-Object{-not $checks[$_]}) -join ', ')}
    $report=[ordered]@{status=$(if($failure){'failed'}else{'passed'});variant=$variant;elapsed_seconds=[Math]::Round($clock.Elapsed.TotalSeconds,2);checks=$checks;failure=$failure;log=$logPath;peer_log=$peerLog;normal_saves_before=$before;normal_saves_after=$after;evidence=@([regex]::Matches($log,'VOYAGER CRIME AUDIT PASS[^\r\n]*')|ForEach-Object{$_.Value})}
    $json=$report|ConvertTo-Json -Depth 7
    [IO.File]::WriteAllText((Join-Path $runDirectory 'Report.json'),$json)
    [IO.File]::WriteAllText((Join-Path $projectRoot ('Saved\VoyagerCrime'+$variant+'Report.json')),$json)
}
if($failure){throw $failure}
Write-Host "Voyager crime audit PASSED: $runDirectory"
