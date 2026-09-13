<#
.SYNOPSIS
    Runs isolated production landing regressions or four rendered reference scenes.
.DESCRIPTION
    Safety uses real possession, flight velocity, obstacle queries and save/load.
    Benchmark measures 8 s settling + 10 s sampling in each scene at 1080p/60 cap.
    Normal expedition saves are hashed before and after. Only owned processes stop.
#>
[CmdletBinding()]
param(
    [switch]$Packaged,
    [switch]$Benchmark,
    [ValidateRange(0,2)][int]$Quality=2,
    [int[]]$Seeds = @(1,42,9001),
    [ValidateRange(90,300)][int]$TimeoutSeconds = 180
)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$kind=if($Benchmark){'Benchmark'}else{'Safety'}
$variant=if($Packaged){'Packaged'}else{'Editor'}
$stamp=[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
$folder=Join-Path $root "Saved\VoyagerProduction\$kind-$variant-$stamp"
[IO.Directory]::CreateDirectory($folder)|Out-Null
$exe='C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if($Benchmark){$exe=$exe.Replace('UnrealEditor-Cmd.exe','UnrealEditor.exe')}
if($Packaged){$exe=Join-Path $root 'Packaged\Riftbound\Windows\Riftbound\Binaries\Win64\Riftbound.exe'}
function SaveHashes {
    foreach($relative in @('Saved\SaveGames\Voyager-Expedition.sav','Packaged\Riftbound\Windows\Riftbound\Saved\SaveGames\Voyager-Expedition.sav')) {
        $path=Join-Path $root $relative
        [pscustomobject]@{path=$relative;sha256=$(if(Test-Path -LiteralPath $path){(Get-FileHash -LiteralPath $path).Hash}else{'absent'})}
    }
}
function ReadLog([string]$path) {
    if(!(Test-Path -LiteralPath $path)){return ''}
    $stream=[IO.File]::Open($path,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=[IO.StreamReader]::new($stream)
    try{return $reader.ReadToEnd()}finally{$reader.Dispose()}
}
$before=@(SaveHashes);$failure=$null;$runs=@();$owned=$null
if($Benchmark -and !$PSBoundParameters.ContainsKey('Seeds')){$Seeds=@(42)}
try {
    foreach($seed in $Seeds) {
        if($seed -lt 1){throw 'Seeds must be positive.'}
        $sceneFolder=Join-Path $folder "Seed-$seed";[IO.Directory]::CreateDirectory($sceneFolder)|Out-Null
        $log=Join-Path $sceneFolder 'Game.log'
        $arguments=@(('"'+(Join-Path $root 'Riftbound.uproject')+'"'),'/Game/Maps/Forest?game=/Script/Riftbound.VoyagerGameMode',
            '-game','-nosound','-unattended','-NoSplash','-NoScreenMessages','-NoVoyagerLocalAI','-VoyagerNatureVisit',
            "-VoyagerAuditSeed=$seed",('-abslog="'+$log+'"'),'-ExecCmds="t.MaxFPS 60"')
        if($Packaged){$arguments=$arguments[1..($arguments.Length-1)]}
        if($Benchmark){$arguments+=@('-VoyagerBenchmark',"-VoyagerQuality=$Quality",'-windowed','-ResX=1920','-ResY=1080',('-VoyagerBenchmarkOutput="'+$sceneFolder+'"'))}
        else{$arguments+=@('-VoyagerSafetyAudit','-NullRHI')}
        $owned=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru
        Write-Host "$kind $variant seed=$seed PID=$($owned.Id): $sceneFolder"
        $timer=[Diagnostics.Stopwatch]::StartNew()
        $errors='VOYAGER SAFETY FAIL|Fatal error|LowLevelFatalError|Assertion failed|Ensure condition failed|Failed to compile Material|Failed to compile global shader'
        while(!$owned.HasExited -and $timer.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
            if((ReadLog $log) -match $errors){throw "Engine/audit failure: $log"}
            Start-Sleep -Milliseconds 450
        }
        if(!$owned.HasExited){throw "Timed out: $log"}
        $text=ReadLog $log
        if($owned.ExitCode -ne 0 -or $text -match $errors){throw "Game failed: $log"}
        if($Benchmark) {
            if($text -notmatch 'VOYAGER BENCHMARK COMPLETE saved=1'){throw "Benchmark incomplete: $log"}
            $measurements=Get-Content -LiteralPath (Join-Path $sceneFolder 'Measurements.json') -Raw|ConvertFrom-Json
            if($measurements.scenes.Count -ne 4 -or $measurements.width -ne 1920 -or $measurements.height -ne 1080){throw 'Incorrect scene count or resolution.'}
            foreach($scene in $measurements.scenes){
                if($scene.frames -lt 100 -or $scene.view_alignment -lt .995 -or !(Test-Path -LiteralPath (Join-Path $sceneFolder ($scene.scene+'.png')))){throw 'Insufficient measured frames, moved camera, or missing image.'}
            }
        } elseif($text -notmatch "VOYAGER SAFETY COMPLETE seed=$seed checks=9") {throw "Safety sequence incomplete: $log"}
        $runs += [pscustomobject]@{seed=$seed;seconds=[Math]::Round($timer.Elapsed.TotalSeconds,2);log=$log;evidence=@([regex]::Matches($text,'VOYAGER (?:SAFETY|BENCHMARK) (?:PASS|SCENE|COMPLETE)[^\r\n]*')|ForEach-Object{$_.Value})}
    }
} catch {$failure=$_.Exception.Message}
finally {
    if($owned -and !$owned.HasExited){Stop-Process -InputObject $owned -Force;$owned.WaitForExit(10000)|Out-Null}
    $after=@(SaveHashes)
    $preserved=($before|ConvertTo-Json -Compress) -eq ($after|ConvertTo-Json -Compress)
    if(!$preserved){$failure='Normal expedition save changed. '+$failure}
    $report=[ordered]@{status=$(if($failure){'failed'}else{'passed'});kind=$kind;variant=$variant;head=(git -C $root rev-parse HEAD);working_tree=(git -C $root status --short);runs=$runs;normal_saves_preserved=$preserved;before=$before;after=$after;failure=$failure}
    $json=$report|ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText((Join-Path $folder 'Report.json'),$json)
    [IO.File]::WriteAllText((Join-Path $root "Saved\Voyager$kind${variant}Report.json"),$json)
}
if($failure){throw $failure}
Write-Host "$kind PASSED: $folder"
