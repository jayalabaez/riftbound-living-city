<# Isolated atmosphere, orbital, anomaly and authority hazard integration audit. #>
[CmdletBinding()]
param([switch]$Render,[switch]$Packaged,[switch]$Diagnostic,[ValidateRange(60,300)][int]$TimeoutSeconds=200)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$variant=$(if($Packaged){'Packaged'}else{'Editor'})+$(if($Render){'Rendered'}else{'Headless'})
if($Diagnostic){$variant+='Diagnostic'}
$runDirectory=Join-Path $projectRoot ('Saved\VoyagerCosmosAudit\'+$variant+'-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
[IO.Directory]::CreateDirectory($runDirectory)|Out-Null
$logPath=Join-Path $runDirectory 'Host.log'
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
$arguments=@(('"'+(Join-Path $projectRoot 'Riftbound.uproject')+'"'),'/Game/Maps/Forest?game=/Script/Riftbound.VoyagerGameMode',
    '-game','-nosound','-unattended','-NoSplash','-NoScreenMessages','-NoVoyagerLocalAI','-VoyagerCosmosAudit',
    '-ExecCmds="t.MaxFPS 60"',('-abslog="'+$logPath+'"'),('-VoyagerCosmosOutput="'+$runDirectory+'"'))
if($Packaged){$arguments=$arguments[1..($arguments.Length-1)]}
if($Diagnostic){$arguments+='-VoyagerCloudDiagnostic'}
if($Render){$arguments+=@('-windowed','-ResX=1280','-ResY=720','-VoyagerCosmosCapture')}else{$arguments+='-NullRHI'}
$before=@(Save-Hashes);$owned=$null;$failure=$null;$checks=[ordered]@{};$log=''
$clock=[Diagnostics.Stopwatch]::StartNew()
$pattern='VOYAGER COSMOS AUDIT FAIL\b|VOYAGER COSMOS MISSING|Fatal error\b|LowLevelFatalError|Assertion failed\b|Ensure condition failed|Failed to compile Material|missing usage flag'
try{
    $owned=Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "Cosmos audit PID $($owned.Id), $variant; evidence $runDirectory"
    while($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds){
        $log=Read-Shared $logPath
        if($log -match $pattern){throw 'Cosmos audit or engine reported an error.'}
        if($owned.HasExited){break}
        Start-Sleep -Milliseconds 450
    }
    if(-not $owned.HasExited){throw 'Cosmos audit timed out.'}
    if($owned.ExitCode -ne 0){throw "Game exited with code $($owned.ExitCode)."}
}catch{$failure=$_.Exception.Message}finally{
    if($owned -and -not $owned.HasExited){Stop-Process -InputObject $owned -Force;$owned.WaitForExit(10000)|Out-Null}
    $clock.Stop();$log=Read-Shared $logPath
    if(-not $Diagnostic){foreach($name in @('NATIVE_ATMOSPHERE_AND_VOLUMETRIC_CLOUDS','SEEDED_BLACK_HOLE_VISUALS','MATCHED_PLANET_SKY_CENTER_AND_RADIUS',
        'ORBIT_USES_SAME_PLANET_TERRAIN','ATMOSPHERE_DESCENT_RETAINS_COORDINATES','SAFE_SURVEY_AND_BOUNDED_INCREASING_TIDES',
        'BLACK_HOLE_OBSERVATION_ORBIT','SURVEY_CRUISE_STARTS_WITHOUT_TELEPORT','SURVEY_CONTINUOUS_CRUISE_ARRIVAL','AUTHORITY_TIDAL_DAMAGE','OTHER_STAR_SYSTEM_REBUILDS_SEEDED_COSMOS')){
        $checks[$name]=$log -match ("VOYAGER COSMOS AUDIT PASS $name\b")
    }
    $checks['COMPLETE']=$log -match 'VOYAGER COSMOS AUDIT COMPLETE PASS'
    }else{$checks['COMPLETE']=$log -match 'VOYAGER CLOUD DIAGNOSTIC COMPLETE'}
    $checks['NO_ENGINE_ERRORS']=$log -notmatch $pattern
    $after=@(Save-Hashes);$preserved=$true
    for($i=0;$i -lt $before.Count;$i++){if($before[$i].hash -ne $after[$i].hash){$preserved=$false}}
    $checks['NORMAL_SAVES_UNCHANGED']=$preserved
    $captureNames=if($Diagnostic){@('DenseWeather','DirectVolume','EngineCloud')}else{@('NaturalSky','PlanetOrbit','CloudFlight','BlackHole')}
    if($Render){foreach($name in $captureNames){
        $path=Join-Path $runDirectory ($name+'.png');$checks["CAPTURE_$name"]=(Test-Path -LiteralPath $path) -and (Get-Item -LiteralPath $path).Length -gt 10000
    }}
    if(-not $failure -and @($checks.Values|Where-Object{-not $_}).Count){$failure='Required checks failed: '+(($checks.Keys|Where-Object{-not $checks[$_]}) -join ', ')}
    $report=[ordered]@{status=$(if($failure){'failed'}else{'passed'});variant=$variant;elapsed_seconds=[Math]::Round($clock.Elapsed.TotalSeconds,2);checks=$checks;failure=$failure;log=$logPath;normal_saves_before=$before;normal_saves_after=$after;evidence=@([regex]::Matches($log,'VOYAGER COSMOS AUDIT PASS[^\r\n]*')|ForEach-Object{$_.Value})}
    $json=$report|ConvertTo-Json -Depth 7
    [IO.File]::WriteAllText((Join-Path $runDirectory 'Report.json'),$json)
    [IO.File]::WriteAllText((Join-Path $projectRoot ('Saved\VoyagerCosmos'+$variant+'Report.json')),$json)
}
if($failure){throw $failure}
Write-Host "Voyager cosmos audit PASSED: $runDirectory"
