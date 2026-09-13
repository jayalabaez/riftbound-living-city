param([switch]$Packaged)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$exe='C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$log=Join-Path $root 'Saved\Logs\VoyagerAtmosphereCheck.log'
$report=Join-Path $root 'Saved\VoyagerAtmosphereReport.json'
$arguments=@(('"'+(Join-Path $root 'Riftbound.uproject')+'"'),'/Game/Maps/Forest','-game','-windowed','-ResX=1280','-ResY=720','-unattended','-NoSplash','-nosound','-NoScreenMessages','-VoyagerTest','-VoyagerProbe','-VoyagerCapture',('-abslog="'+$log+'"'))
if($Packaged){$exe=Join-Path $root 'Packaged\Riftbound\Windows\Riftbound\Binaries\Win64\Riftbound.exe';$arguments=$arguments[1..($arguments.Length-1)]}
[IO.File]::WriteAllText($log,'')
$owned=$null;$failure=$null;$text='';$crossed=-1
$timer=[Diagnostics.Stopwatch]::StartNew()
try {
    $owned=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru
    Write-Host "Atmosphere rendering check PID $($owned.Id)."
    while($timer.Elapsed.TotalSeconds -lt 80){
        $stream=[IO.File]::Open($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $reader=[IO.StreamReader]::new($stream)
        try{$text=$reader.ReadToEnd()}finally{$reader.Dispose()}
        if($text -match 'Fatal error|Assertion failed|Ensure condition failed|Failed to compile Material|VOYAGER TEST FAIL'){throw 'Atmosphere check failed; inspect log.'}
        if($text -match 'VOYAGER TEST PASS SEAMLESS_ASCENT' -and $crossed -lt 0){$crossed=$timer.Elapsed.TotalSeconds}
        if($crossed -ge 0 -and $timer.Elapsed.TotalSeconds-$crossed -gt 3){break}
        if($owned.HasExited){throw 'Game exited before atmospheric crossing.'}
        Start-Sleep -Milliseconds 400
    }
    if($crossed -lt 0){throw 'Atmosphere check timed out'}
}catch{$failure=$_.Exception.Message}
finally{
    if($owned -and -not $owned.HasExited){Stop-Process -InputObject $owned -Force}
    $timer.Stop()
    [ordered]@{status=$(if($failure){'failed'}else{'passed'});packaged=[bool]$Packaged;elapsed_seconds=$timer.Elapsed.TotalSeconds;log=$log;failure=$failure;check='Rendered surface,15km atmosphere and65km exosphere with clouds'} | ConvertTo-Json | Set-Content -LiteralPath $report
}
if($failure){throw $failure}
Write-Host 'Atmosphere rendering check PASSED.'
