<# Isolated survival items, crafting and field care integration audit. #>
[CmdletBinding()]
param([switch]$Render,[switch]$Packaged,[ValidateRange(100,220)][int]$TimeoutSeconds=150)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$variant=$(if($Packaged){'Packaged'}else{'Editor'})+$(if($Render){'Rendered'}else{'Headless'})
$runDirectory=Join-Path $projectRoot ('Saved\VoyagerSurvivalAudit\'+$variant+'-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
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
    '-game','-nosound','-unattended','-NoSplash','-NoScreenMessages','-NoVoyagerLocalAI','-VoyagerSurvivalAudit',
    '-ExecCmds="t.MaxFPS 60"',('-abslog="'+$logPath+'"'),('-VoyagerSurvivalOutput="'+$runDirectory+'"'))
if($Packaged){$arguments=$arguments[1..($arguments.Length-1)]}
if($Render){$arguments+=@('-windowed','-ResX=1280','-ResY=720','-VoyagerSurvivalCapture')}else{$arguments+='-NullRHI'}
$before=@(Save-Hashes);$owned=$null;$failure=$null;$checks=[ordered]@{};$log=''
$clock=[Diagnostics.Stopwatch]::StartNew()
$pattern='VOYAGER SURVIVAL AUDIT FAIL\b|VOYAGER SURVIVAL MISSING|Fatal error\b|LowLevelFatalError|Assertion failed\b|Ensure condition failed|Failed to compile Material|missing usage flag'
try{
    $owned=Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
    Write-Host "Survival audit PID $($owned.Id), $variant; evidence $runDirectory"
    while($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds){
        $log=Read-Shared $logPath
        if($log -match $pattern){throw 'Survival audit or engine reported an error.'}
        if($owned.HasExited){break}
        Start-Sleep -Milliseconds 450
    }
    if(-not $owned.HasExited){throw 'Survival audit timed out.'}
    if($owned.ExitCode -ne 0){throw "Game exited with code $($owned.ExitCode)."}
}catch{$failure=$_.Exception.Message}finally{
    if($owned -and -not $owned.HasExited){Stop-Process -InputObject $owned -Force;$owned.WaitForExit(10000)|Out-Null}
    $clock.Stop();$log=Read-Shared $logPath
    foreach($name in @('HUNT_LOOT_ITEMS','BACKPACK_MODAL_INPUT','PHONE_SWITCH_AND_INPUT_RESTORE','COOKING_LOCATION_REJECTS_WITHOUT_LOSS',
        'COOKS_BESIDE_OWN_SHIP','REPEATED_RPC_RATE_LIMIT','MEDKIT_RECIPE_EXACT_COST','RAW_FOOD_CONSUMED_WITH_HEALTH_COST',
        'FIELD_MEDICINE_UPDATES_REAL_HEALTH','COOKED_MEAL_CONSUMED','AMMUNITION_RECIPE_EXACT_COST','FULL_STACK_REJECTS_WITHOUT_LOSS',
        'DEMOLITION_RECIPE_EXACT_COST','INVALID_CHARGE_SURFACE_PRESERVES_ITEM','INVENTORY_SAVE_COMPLETES',
        'SAME_FRAME_DAMAGE_MEDKIT_RESTORES_PAWN','LATER_DAMAGE_AFTER_HEAL_STILL_APPLIES',
        'SAME_FRAME_DAMAGE_MEDKIT_DAMAGE_PRESERVES_ORDER','SAME_FRAME_DAMAGE_MEAL_DAMAGE_PRESERVES_ORDER',
        'HEAL_BEFORE_LETHAL_HIT_PREVENTS_FALSE_RECOVERY','DEAD_PLAYER_CARE_REJECTS_WITHOUT_ITEM_LOSS',
        'JAILED_CARE_REJECTS_WITHOUT_ITEM_LOSS')){
        $checks[$name]=$log -match ("VOYAGER SURVIVAL AUDIT PASS $name\b")
    }
    $checks['COMPLETE']=$log -match 'VOYAGER SURVIVAL AUDIT COMPLETE PASS'
    $checks['NO_ENGINE_ERRORS']=$log -notmatch $pattern
    $after=@(Save-Hashes);$preserved=$true
    for($i=0;$i -lt $before.Count;$i++){if($before[$i].hash -ne $after[$i].hash){$preserved=$false}}
    $checks['NORMAL_SAVES_UNCHANGED']=$preserved
    if($Render){foreach($name in @('Backpack','CraftedSupplies')){
        $path=Join-Path $runDirectory ($name+'.png');$checks["CAPTURE_$name"]=(Test-Path -LiteralPath $path) -and (Get-Item -LiteralPath $path).Length -gt 10000
    }}
    if(-not $failure -and @($checks.Values|Where-Object{-not $_}).Count){$failure='Required checks failed: '+(($checks.Keys|Where-Object{-not $checks[$_]}) -join ', ')}
    $report=[ordered]@{status=$(if($failure){'failed'}else{'passed'});variant=$variant;elapsed_seconds=[Math]::Round($clock.Elapsed.TotalSeconds,2);checks=$checks;failure=$failure;log=$logPath;normal_saves_before=$before;normal_saves_after=$after;evidence=@([regex]::Matches($log,'VOYAGER SURVIVAL AUDIT PASS[^\r\n]*')|ForEach-Object{$_.Value})}
    $json=$report|ConvertTo-Json -Depth 7
    [IO.File]::WriteAllText((Join-Path $runDirectory 'Report.json'),$json)
    [IO.File]::WriteAllText((Join-Path $projectRoot ('Saved\VoyagerSurvival'+$variant+'Report.json')),$json)
}
if($failure){throw $failure}
Write-Host "Voyager survival audit PASSED: $runDirectory"
