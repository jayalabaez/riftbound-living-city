<# Runs the already built hunting integration audit. Uses Voyager-Automation-Hunt;
   fixture relocation, capacity and opaque-wall checks never touch normal saves. #>
[CmdletBinding()]
param([switch]$Render,[switch]$Packaged,[switch]$Network,[ValidateRange(150,300)][int]$TimeoutSeconds=180)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$variant=$(if($Packaged){'Packaged'}else{'Editor'})+$(if($Network){'Network'}else{'Solo'})+$(if($Render){'Rendered'}else{'Headless'})
$runId=[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
$directory=Join-Path $projectRoot ('Saved\VoyagerHuntAudit\'+$variant+'-'+$runId)
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
$common=@('-game','-nosound','-unattended','-NoSplash','-NoScreenMessages','-NoVoyagerLocalAI','-VoyagerHuntAudit','-VoyagerCityVisit','-ExecCmds="t.MaxFPS 60"')
$arguments=@(('"'+(Join-Path $projectRoot 'Riftbound.uproject')+'"'),$map)+$common+@('-port=7796',('-abslog="'+$logPath+'"'),('-VoyagerHuntOutput="'+$directory+'"'))
if($Packaged){$arguments=$arguments[1..($arguments.Length-1)]}
if($Render){$arguments+=@('-windowed','-ResX=1280','-ResY=720','-VoyagerHuntCapture')}else{$arguments+='-NullRHI'}
$before=@(Save-Hashes);$after=@();$hostProcess=$null;$peer=$null;$failure=$null;$log='';$peerText=''
$checks=[ordered]@{};$captures=@();$started=[DateTime]::UtcNow;$clock=[Diagnostics.Stopwatch]::StartNew()
$errorsPattern='VOYAGER HUNT AUDIT FAIL\b|Fatal error\b|LowLevelFatalError|Assertion failed\b|Ensure condition failed|Failed to compile Material|VOYAGER CHARACTER MISSING'
try{
    if(-not(Test-Path -LiteralPath $executable -PathType Leaf)){throw "Missing executable: $executable"}
    $hostProcess=Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "Hunting audit PID $($hostProcess.Id); evidence $directory"
    while($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds){
        $log=Read-Shared $logPath;if($log -match $errorsPattern){throw 'Hunting audit or engine reported an error.'}
        if($Network -and -not $peer -and $log -match 'VOYAGER READY'){
            $peerArguments=@(('"'+(Join-Path $projectRoot 'Riftbound.uproject')+'"'),'127.0.0.1:7796')+$common+@('-NullRHI',('-abslog="'+$peerLog+'"'))
            if($Packaged){$peerArguments=$peerArguments[1..($peerArguments.Length-1)]}
            $peer=Start-Process -FilePath $executable -ArgumentList $peerArguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
        }
        if($peer){$peerText=Read-Shared $peerLog;if($peerText -match $errorsPattern){throw 'Network hunting peer reported an error.'}}
        if($hostProcess.HasExited){break};Start-Sleep -Milliseconds 500
    }
    if(-not $hostProcess.HasExited){throw 'Hunting audit timed out.'}
    if($hostProcess.ExitCode -ne 0){throw "Game exited with code $($hostProcess.ExitCode)."}
}catch{$failure=$_.Exception.Message}finally{
    foreach($process in @($hostProcess,$peer)){
        if($process -and -not $process.HasExited){Stop-Process -InputObject $process -Force;$process.WaitForExit(10000)|Out-Null}
    }
    $clock.Stop();$log=Read-Shared $logPath;$peerText=Read-Shared $peerLog
    $required=@('THREE_HUMANOIDS_AND_AIM_ASSETS','HUMANOID_FORWARD_AXIS','INVALID_DAMAGE_AND_LIVE_HARVEST_REJECTED',
        'NORMAL_GUN_DAMAGE_CAUSES_FLEE','WILDLIFE_DEATH_STOPS_AND_LEAVES_CARCASS','OUT_OF_RANGE_HARVEST_REJECTED',
        'OBSTRUCTED_HARVEST_REJECTED','FULL_INVENTORY_PRESERVES_CARCASS','E_HARVEST_GRANTS_EXACT_LOOT',
        'REPEATED_HARVEST_REJECTED','HARVESTED_CARCASS_EXPIRES','HUNT_INVENTORY_SAVED')
    foreach($name in $required){$checks[$name]=$log -match ("VOYAGER HUNT AUDIT PASS $name\b")}
    $checks['COMPLETE']=$log -match 'VOYAGER HUNT AUDIT COMPLETE PASS';$checks['NO_ENGINE_ERRORS']=$log -notmatch $errorsPattern
    if($Network){
        $checks['PEER_OBSERVED_LIVE_SUBJECT']=$log -match 'VOYAGER HUNT AUDIT PASS PEER_OBSERVED_LIVE_SUBJECT\b'
        foreach($name in @('REMOTE_AUTHORITY_GUARD','REMOTE_DEATH_REPLICATED','REMOTE_HARVEST_REPLICATED','REMOTE_LOOT_ISOLATED')){
            $checks[$name]=$peerText -match ("VOYAGER HUNT AUDIT PASS $name\b")
        }
        $checks['PEER_COMPLETE']=$peerText -match 'VOYAGER HUNT AUDIT CLIENT COMPLETE PASS';$checks['PEER_NO_ENGINE_ERRORS']=$peerText -notmatch $errorsPattern
    }
    $after=@(Save-Hashes);$preserved=$before.Count -eq $after.Count
    for($i=0;$i -lt $before.Count;$i++){if($before[$i].exists -ne $after[$i].exists -or $before[$i].sha256 -ne $after[$i].sha256){$preserved=$false}}
    $checks['NORMAL_SAVES_UNCHANGED']=$preserved
    if($Render){
        Add-Type -AssemblyName System.Drawing
        foreach($name in @('CitizenEyes','Carcass')){
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
        evidence=@([regex]::Matches($log,'VOYAGER HUNT AUDIT PASS[^\r\n]*')|ForEach-Object{$_.Value});
        coverage='Normal gun and interact RPC paths, server validation, inventory capacity atomicity, corpse lifecycle and disk persistence. Optional peer observes replicated state and rejects local authority mutation. Geometry/stance checks run separately with check_voyager_character_geometry.py; screenshots require visual inspection.'}
    $json=$report|ConvertTo-Json -Depth 8;[IO.File]::WriteAllText((Join-Path $directory 'Report.json'),$json)
    [IO.File]::WriteAllText((Join-Path $projectRoot ('Saved\VoyagerHunt'+$variant+'Report.json')),$json)
}
Write-Host "Hunting audit evidence: $directory";if($failure){throw $failure}
Write-Host 'Hunting audit passed; normal expedition saves are unchanged.'
