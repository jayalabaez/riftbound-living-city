<# Runs the already built destruction integration audit. Uses Voyager-Automation-Destruction;
   actual instance traces, structural thresholds and Chaos debris never touch normal saves. #>
[CmdletBinding()]
param([switch]$Render,[switch]$Packaged,[switch]$Network,[ValidateRange(150,300)][int]$TimeoutSeconds=180)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$variant=$(if($Packaged){'Packaged'}else{'Editor'})+$(if($Network){'Network'}else{'Solo'})+$(if($Render){'Rendered'}else{'Headless'})
$runId=[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
$directory=Join-Path $projectRoot ('Saved\VoyagerDestructionAudit\'+$variant+'-'+$runId)
[IO.Directory]::CreateDirectory($directory)|Out-Null
$logPath=Join-Path $directory 'Host.log';$peerLog=Join-Path $directory 'Peer.log'
$executable='C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if($Render){$executable=$executable.Replace('UnrealEditor-Cmd.exe','UnrealEditor.exe')}
if($Packaged){$executable=Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound\Binaries\Win64\Riftbound.exe'}
function Read-Shared([string]$Path){
    if(-not(Test-Path -LiteralPath $Path -PathType Leaf)){return ''}
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=[IO.StreamReader]::new($stream);try{return $reader.ReadToEnd()}finally{$reader.Dispose()}
}
function Save-Hashes {
    foreach($relative in @('Saved\SaveGames\Voyager-Expedition.sav','Packaged\Riftbound\Windows\Riftbound\Saved\SaveGames\Voyager-Expedition.sav')){
        $path=Join-Path $projectRoot $relative;$exists=Test-Path -LiteralPath $path -PathType Leaf
        [pscustomobject]@{path=$relative;exists=[bool]$exists;sha256=$(if($exists){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}else{$null})}
    }
}
$map='/Game/Maps/Forest?game=/Script/Riftbound.VoyagerGameMode';if($Network){$map+='?listen'}
$common=@('-game','-nosound','-unattended','-NoSplash','-NoScreenMessages','-NoVoyagerLocalAI','-VoyagerDestructionAudit','-VoyagerCityVisit','-ExecCmds="t.MaxFPS 60"')
$arguments=@(('"'+(Join-Path $projectRoot 'Riftbound.uproject')+'"'),$map)+$common+@('-port=7797',('-abslog="'+$logPath+'"'),('-VoyagerDestructionOutput="'+$directory+'"'))
if($Packaged){$arguments=$arguments[1..($arguments.Length-1)]}
if($Render){$arguments+=@('-windowed','-ResX=1280','-ResY=720','-VoyagerDestructionCapture')}else{$arguments+='-NullRHI'}
$before=@(Save-Hashes);$after=@();$hostProcess=$null;$peer=$null;$failure=$null;$log='';$peerText=''
$checks=[ordered]@{};$captures=@();$started=[DateTime]::UtcNow;$clock=[Diagnostics.Stopwatch]::StartNew()
$errorsPattern='VOYAGER DESTRUCTION AUDIT FAIL\b|Fatal error\b|LowLevelFatalError|Assertion failed\b|Ensure condition failed|Failed to compile Material|VOYAGER CHARACTER MISSING|Gamethread hitch waiting for resource cleanup|overwrite took'
try{
    if(-not(Test-Path -LiteralPath $executable -PathType Leaf)){throw "Missing executable: $executable"}
    $hostProcess=Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "Destruction audit PID $($hostProcess.Id); evidence $directory"
    while($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds){
        $log=Read-Shared $logPath;if($log -match $errorsPattern){throw 'Destruction audit or engine reported an error.'}
        if($Network -and -not $peer -and $log -match 'VOYAGER READY'){
            $peerArguments=@(('"'+(Join-Path $projectRoot 'Riftbound.uproject')+'"'),'127.0.0.1:7797')+$common+@('-NullRHI',('-abslog="'+$peerLog+'"'))
            if($Packaged){$peerArguments=$peerArguments[1..($peerArguments.Length-1)]}
            $peer=Start-Process -FilePath $executable -ArgumentList $peerArguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
        }
        if($peer){$peerText=Read-Shared $peerLog;if($peerText -match $errorsPattern){throw 'Network destruction peer reported an error.'}}
        if($hostProcess.HasExited){break};Start-Sleep -Milliseconds 500
    }
    if(-not $hostProcess.HasExited){throw 'Destruction audit timed out.'}
    if($hostProcess.ExitCode -ne 0){throw "Game exited with code $($hostProcess.ExitCode)."}
}catch{$failure=$_.Exception.Message}finally{
    foreach($process in @($hostProcess,$peer)){
        if($process -and -not $process.HasExited){Stop-Process -InputObject $process -Force;$process.WaitForExit(10000)|Out-Null}
    }
    $clock.Stop();$log=Read-Shared $logPath;$peerText=Read-Shared $peerLog
    $required=@('INTACT_BUILDINGS_READY','INVALID_BUILDING_DAMAGE_REJECTED','REAL_TRACE_RESOLVES_WINDOW','REAL_TRACE_RESOLVES_UPPER_WALL',
        'NATIVE_CHAOS_BODIES_SIMULATE','GLASS_BREAK_REMOVES_PIECES_AND_COLLISION','REPEATED_GLASS_BREAK_REJECTED',
        'CHAOS_DEBRIS_ACTUALLY_MOVES','STRUCTURAL_THRESHOLD_REMOVES_UPPER_FLOORS','COLLAPSED_UPPER_STOREY_HAS_NO_COLLISION',
        'OTHER_BUILDING_REMAINS_INTACT','SECOND_STRUCTURAL_THRESHOLD','FULL_COLLAPSE_PERSISTS_FOUNDATION','DEBRIS_HARD_LIMIT_96',
        'BUILDING_DAMAGE_WRITTEN_TO_DISK','CLEAN_RESTORE_REBUILDS_INTACT_COLLISION','DISK_RESTORE_REAPPLIES_DAMAGE_AND_COLLISION',
        'BACKPACK_ACTION_PLACES_AND_CONSUMES_CHARGE','ARMED_CHARGE_OBSERVES_FUSE','TIMED_CHARGE_EXPLODES_AND_DAMAGES_REAL_BUILDING',
        'GLASS_NATIVE_MASS_FRICTION_AND_DAMPING','MATERIAL_AWARE_CHAOS_RESPONSE_AND_MASS',
        'CHAOS_POOL_REUSES_BODIES_WITHOUT_ACTOR_GROWTH','ONE_FAILED_SUPPORT_RETAINS_STOREYS',
        'LOCAL_SUPPORT_LOSS_COLLAPSES_ONLY_UPPER_STOREYS','SUPPORT_COLLAPSE_UPDATES_REAL_COLLISION',
        'SUPPORT_DELTAS_SAVED_TO_DISK','MALFORMED_SUPPORT_DELTA_REJECTED','LEGACY_INTEGRITY_AND_GLASS_SAVE_PRESERVED',
        'SUPPORT_DISK_RESTORE_RETAINS_COLLAPSE_AND_GLASS','GROUND_SUPPORT_LOSS_COLLAPSES_WITH_POSITIVE_INTEGRITY',
        'POSITIVE_INTEGRITY_RUINS_REFUSE_CITY_SERVICES','BLAST_ABOVE_RUIN_DAMAGES_REMAINING_SUPPORT')
    foreach($name in $required){$checks[$name]=$log -match ("VOYAGER DESTRUCTION AUDIT PASS $name\b")}
    $checks['COMPLETE']=$log -match 'VOYAGER DESTRUCTION AUDIT COMPLETE PASS';$checks['NO_ENGINE_ERRORS']=$log -notmatch $errorsPattern
    if($Network){
        foreach($name in @('REMOTE_INTACT_BUILDING','REMOTE_BROKEN_GLASS','REMOTE_PARTIAL_COLLAPSE','REMOTE_FULL_COLLAPSE','REMOTE_DESTRUCTION_AUTHORITY_GUARD','REMOTE_DEBRIS_PRESENT','REMOTE_DEBRIS_IS_PRESENTATION_ONLY',
            'REMOTE_RECYCLED_FRAGMENTS_SNAP_TO_REPLICATED_POSES','REMOTE_SUPPORT_DELTAS_AND_PARTIAL_COLLAPSE',
            'REMOTE_SUPPORT_LOSS_REMOVES_GROUND_STOREY','REMOTE_LOCAL_SUPPORT_AUTHORITY_GUARD')){
            $checks[$name]=$peerText -match ("VOYAGER DESTRUCTION AUDIT PASS $name\b")
        }
        $checks['PEER_COMPLETE']=$peerText -match 'VOYAGER DESTRUCTION AUDIT CLIENT COMPLETE PASS';$checks['PEER_NO_ENGINE_ERRORS']=$peerText -notmatch $errorsPattern
    }
    $after=@(Save-Hashes);$preserved=$before.Count -eq $after.Count
    for($i=0;$i -lt $before.Count;$i++){if($before[$i].exists -ne $after[$i].exists -or $before[$i].sha256 -ne $after[$i].sha256){$preserved=$false}}
    $checks['NORMAL_SAVES_UNCHANGED']=$preserved
    if($Render){
        Add-Type -AssemblyName System.Drawing
        foreach($name in @('Intact','GlassBroken','PartialCollapse','Rubble')){
            $path=Join-Path $directory ($name+'.png');$valid=Test-Path -LiteralPath $path -PathType Leaf
            $width=0;$height=0;$hash=$null
            if($valid){
                $info=Get-Item -LiteralPath $path;$bitmap=[Drawing.Bitmap]::new($path)
                try{$width=$bitmap.Width;$height=$bitmap.Height}finally{$bitmap.Dispose()}
                $hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
                $valid=$width -eq 1280 -and $height -eq 720 -and $info.Length -gt 10000 -and $info.LastWriteTimeUtc -ge $started.AddSeconds(-2)
            }
            $checks["CAPTURE_$name"]=[bool]$valid;$captures+=[pscustomobject]@{name=$name;path=$path;width=$width;height=$height;sha256=$hash;passed=[bool]$valid}
        }
    }
    if(-not $failure -and @($checks.Values|Where-Object{-not $_}).Count){$failure='Required checks failed: '+(($checks.Keys|Where-Object{-not $checks[$_]}) -join ', ')}
    $report=[ordered]@{status=$(if($failure){'failed'}else{'passed'});variant=$variant;elapsed_seconds=[Math]::Round($clock.Elapsed.TotalSeconds,3);
        failure=$failure;checks=$checks;log=$logPath;peer_log=$peerLog;normal_saves_before=$before;normal_saves_after=$after;screenshots=$captures;
        evidence=@([regex]::Matches($log,'VOYAGER DESTRUCTION AUDIT PASS[^\r\n]*')|ForEach-Object{$_.Value});
        coverage='Real instanced wall/window traces, aggregate and regional support collapse, changed collision, native glass/concrete mass and contact settings, moving Chaos bodies, recycling within 96 actors, intact/legacy/malformed/support disk saves, charge placement/fuse/explosion and service refusal at positive-integrity ruins. Network mode also observes actual replicated support deltas, partial/ground-storey removal, authority rejection and recycled fragment poses on the first client frame receiving a new serial; it never invokes OnRep manually. Resource-cleanup overwrite warnings fail the run. Screenshots require visual inspection.'}
    $json=$report|ConvertTo-Json -Depth 8;[IO.File]::WriteAllText((Join-Path $directory 'Report.json'),$json)
    [IO.File]::WriteAllText((Join-Path $projectRoot ('Saved\VoyagerDestruction'+$variant+'Report.json')),$json)
}
Write-Host "Destruction audit evidence: $directory";if($failure){throw $failure}
Write-Host 'Destruction audit passed; normal expedition saves are unchanged.'
