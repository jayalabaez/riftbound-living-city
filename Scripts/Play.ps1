param(
    [ValidateSet('Solo','Host','Join')][string]$Mode = 'Solo',
    [string]$Address = '127.0.0.1',
    [ValidateSet('Voyager','Survival')][string]$Game = 'Voyager',
    [switch]$CityVisit,
    [switch]$NatureVisit,
    [ValidateSet('Saved','Low','Medium','High')][string]$Quality='Saved'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$engineRoot = 'C:\Program Files\Epic Games\UE_5.8'
$editor = Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
$project = Join-Path $projectRoot 'Riftbound.uproject'
$standalone = Join-Path $projectRoot 'Packaged\Riftbound\Windows\Riftbound.exe'
$useStandalone = Test-Path -LiteralPath $standalone
if (-not $useStandalone -and -not (Test-Path -LiteralPath $editor)) { throw 'Unreal Engine 5.8 was not found. Update engineRoot in Scripts/Play.ps1.' }
if (-not $useStandalone -and -not (Test-Path -LiteralPath (Join-Path $projectRoot 'Binaries\Win64\UnrealEditor-Riftbound.dll'))) {
    & (Join-Path $PSScriptRoot 'Build.ps1')
}
$destination = '/Game/Maps/Forest'
if ($Game -eq 'Survival') { $destination += '?game=/Script/Riftbound.RiftGameMode' }
if ($Mode -eq 'Host') { $destination += '?listen' }
if ($Mode -eq 'Join') {
    if ($Address -notmatch '^[a-zA-Z0-9.:-]+$') { throw 'Enter a valid host name or IP address.' }
    $destination = $Address
}
$launchArgs = @(('"' + $project + '"'), $destination, '-game', '-windowed', '-ResX=1600', '-ResY=900', '-NoSplash', '-NoScreenMessages')
if ($useStandalone) {
    $launchArgs = @($destination, '-windowed', '-ResX=1600', '-ResY=900', '-NoSplash', '-NoScreenMessages')
    if($Quality -ne 'Saved'){$launchArgs += '-VoyagerQuality='+(@('Low','Medium','High').IndexOf($Quality))}
    if ($CityVisit -and $Game -eq 'Voyager' -and $Mode -ne 'Join') { $launchArgs += '-VoyagerCityVisit' }
    if ($NatureVisit -and $Game -eq 'Voyager' -and $Mode -ne 'Join') { $launchArgs += '-VoyagerNatureVisit' }
    Start-Process -FilePath $standalone -ArgumentList $launchArgs -WorkingDirectory (Split-Path -Parent $standalone) -WindowStyle Normal
} else {
    if($Quality -ne 'Saved'){$launchArgs += '-VoyagerQuality='+(@('Low','Medium','High').IndexOf($Quality))}
    if ($CityVisit -and $Game -eq 'Voyager' -and $Mode -ne 'Join') { $launchArgs += '-VoyagerCityVisit' }
    if ($NatureVisit -and $Game -eq 'Voyager' -and $Mode -ne 'Join') { $launchArgs += '-VoyagerNatureVisit' }
    Start-Process -FilePath $editor -ArgumentList $launchArgs -WorkingDirectory $projectRoot -WindowStyle Normal
}
