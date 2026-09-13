<# Runs isolated real-fire / pursuit / hiding tests; -Network adds an innocent peer. #>
[CmdletBinding()]
param([switch]$Render,[switch]$Packaged,[switch]$Network,[ValidateRange(180,360)][int]$TimeoutSeconds=240)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$variant=$(if($Packaged){'Packaged'}else{'Editor'})+$(if($Network){'Network'}else{'Solo'})+$(if($Render){'Rendered'}else{'Headless'})
$runDirectory=Join-Path $projectRoot ('Saved\VoyagerPoliceAudit\'+$variant+'-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
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
$common=@('-game','-nosound','-unattended','-NoSplash','-NoScreenMessages','-NoVoyagerLocalAI','-VoyagerPoliceAudit','-VoyagerCityVisit','-ExecCmds="t.MaxFPS 60"')
$arguments=@(('"'+(Join-Path $projectRoot 'Riftbound.uproject')+'"'),$map)+$common+@('-port=7797',('-abslog="'+$logPath+'"'),('-VoyagerPoliceOutput="'+$runDirectory+'"'))
if($Packaged){$arguments=$arguments[1..($arguments.Length-1)]}
if($Render){$arguments+=@('-windowed','-ResX=1280','-ResY=720','-VoyagerPoliceCapture')}else{$arguments+='-NullRHI'}
$before=@(Save-Hashes)
$owned=$null;$peer=$null;$failure=$null;$checks=[ordered]@{};$log='';$peerText=''
$clock=[Diagnostics.Stopwatch]::StartNew()
$pattern='VOYAGER POLICE AUDIT FAIL\b|Fatal error\b|LowLevelFatalError|Assertion failed\b|Ensure condition failed|Failed to compile Material|missing usage flag|VOYAGER CHARACTER MISSING|VOYAGER SECURITY MISSING'
try{
    $owned=Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "Police audit PID $($owned.Id), $variant; evidence $runDirectory"
    while($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds){
        $log=Read-Shared $logPath
        if($log -match $pattern){throw 'Police audit or engine reported an error.'}
        if($Network -and -not $peer -and $log -match 'VOYAGER READY'){
            $peerArguments=@(('"'+(Join-Path $projectRoot 'Riftbound.uproject')+'"'),'127.0.0.1:7797')+$common+@('-NullRHI',('-abslog="'+$peerLog+'"'))
            if($Packaged){$peerArguments=$peerArguments[1..($peerArguments.Length-1)]}
            $peer=Start-Process -FilePath $executable -ArgumentList $peerArguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
        }
        if($peer){$peerText=Read-Shared $peerLog;if($peerText -match $pattern){throw 'Police audit peer reported an error.'}}
        if($owned.HasExited){break}
        Start-Sleep -Milliseconds 450
    }
    if(-not $owned.HasExited){throw 'Police audit timed out.'}
    if($owned.ExitCode -ne 0){throw "Game exited with code $($owned.ExitCode)."}
}catch{$failure=$_.Exception.Message}finally{
    foreach($process in @($owned,$peer)){if($process -and -not $process.HasExited){Stop-Process -InputObject $process -Force;$process.WaitForExit(10000)|Out-Null}}
    $clock.Stop();$log=Read-Shared $logPath;$peerText=Read-Shared $peerLog
    foreach($name in @('JAIL_REMOVED_AND_LEGACY_SAVE_CLEARED','WITNESSED_CRIME_WANTED','WARNING_GRACE','OFFICER_REAL_WEAPON_DAMAGE','WANTED_LEVEL_DISPATCHES_DRONE','DRONE_FLIES_AND_DAMAGES_CRIMINAL','HOVER_CAR_DISPATCHED','SHAPED_MODELS_THREE_LODS','SOLID_COVER_BLOCKS_DRONE','DRONE_REACQUIRES_SIGHT','DRONE_CAN_BE_SHOT_DOWN','PURSUIT_END_CLEARS_DRONES','LETHAL_DAMAGE_RECOVERS_WITHOUT_JAIL')){
        $checks[$name]=$log -match ("VOYAGER POLICE AUDIT PASS $name\b")
    }
    $checks['COMPLETE']=$log -match 'VOYAGER POLICE AUDIT COMPLETE PASS'
    $checks['NO_ENGINE_ERRORS']=$log -notmatch $pattern
    if($Network){foreach($name in @('REMOTE_DRONE_AND_MOTION','REMOTE_AUTHORITY_GUARD','REMOTE_DRONE_DEATH','INNOCENT_PEER_FREE')){$checks[$name]=$peerText -match ("VOYAGER POLICE AUDIT PASS $name\b")};$checks['PEER_COMPLETE']=$peerText -match 'VOYAGER POLICE AUDIT CLIENT COMPLETE PASS'}
    $after=@(Save-Hashes);$preserved=$true
    for($i=0;$i -lt $before.Count;$i++){if($before[$i].hash -ne $after[$i].hash){$preserved=$false}}
    $checks['NORMAL_SAVES_UNCHANGED']=$preserved
    if($Render){
        foreach($name in @('DroneAndHoverCar')){$path=Join-Path $runDirectory ($name+'.png');$checks["CAPTURE_$name"]=(Test-Path -LiteralPath $path) -and (Get-Item -LiteralPath $path).Length -gt 10000}
    }
    if(-not $failure -and @($checks.Values|Where-Object{-not $_}).Count){$failure='Required checks failed: '+(($checks.Keys|Where-Object{-not $checks[$_]}) -join ', ')}
    $report=[ordered]@{status=$(if($failure){'failed'}else{'passed'});variant=$variant;elapsed_seconds=[Math]::Round($clock.Elapsed.TotalSeconds,2);checks=$checks;failure=$failure;log=$logPath;peer_log=$peerLog;normal_saves_before=$before;normal_saves_after=$after;evidence=@([regex]::Matches($log,'VOYAGER POLICE AUDIT PASS[^\r\n]*')|ForEach-Object{$_.Value})}
    $json=$report|ConvertTo-Json -Depth 7
    [IO.File]::WriteAllText((Join-Path $runDirectory 'Report.json'),$json)
    [IO.File]::WriteAllText((Join-Path $projectRoot ('Saved\VoyagerPolice'+$variant+'Report.json')),$json)
}
if($failure){throw $failure}
Write-Host "Voyager police audit PASSED: $runDirectory"
