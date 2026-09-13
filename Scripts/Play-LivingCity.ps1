<#
.SYNOPSIS
    Launch Living City using the packaged game when available.
.DESCRIPTION
    -Rebuild builds and launches through Unreal Editor. -Editor opens for editing.
    Set UE_ROOT for a custom Unreal Engine 5.8 installation. Keep pure ASCII (D-011).
#>
[CmdletBinding()]
param(
    [switch] $Rebuild,
    [switch] $Editor,
    [ValidateRange(640, 7680)][int] $ResX = 1600,
    [ValidateRange(480, 4320)][int] $ResY = 900
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Project = Join-Path $ProjectRoot 'Riftbound.uproject'
$EngineRoot = if ($env:UE_ROOT) { $env:UE_ROOT } else { 'C:\Program Files\Epic Games\UE_5.8' }
$EditorExe = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.exe'
$BuildBat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat'
$ModuleDll = Join-Path $ProjectRoot 'Binaries\Win64\UnrealEditor-LivingCityGame.dll'
$Standalone = Join-Path $ProjectRoot 'Packaged\Riftbound\Windows\Riftbound.exe'
$UseStandalone = (Test-Path -LiteralPath $Standalone) -and -not ($Rebuild -or $Editor)
$Destination = '/Engine/Maps/Entry?game=/Script/LivingCityGame.LivingCityGameMode'
if (-not (Test-Path -LiteralPath $Project)) { throw "Project missing: $Project" }
if (-not $UseStandalone) {
    if (-not (Test-Path -LiteralPath $EditorExe)) {
        throw 'Install Unreal Engine 5.8 or set UE_ROOT to its installation. See BUILDING.md.'
    }
    if ($Rebuild -or -not (Test-Path -LiteralPath $ModuleDll)) {
        if (Get-Process -Name UnrealEditor -ErrorAction SilentlyContinue) {
            throw 'Close Unreal Editor before rebuilding, or compile using Ctrl+Alt+F11 in the editor.'
        }
        & $BuildBat RiftboundEditor Win64 Development "-Project=$Project" -WaitMutex
        if ($LASTEXITCODE -ne 0) { throw "Unreal build failed (exit $LASTEXITCODE)." }
    }
}
Write-Host 'LIVING CITY - 1,500 citizens, jobs, shopping and editable terrain' -ForegroundColor Cyan
Write-Host 'WASD + mouse: walk/look   Shift: sprint   Space: jump   F: camera'
Write-Host 'Left mouse: dig   Right mouse: pile   P: pause   O: single step'
Write-Host '1 / 2 / 3 / 4: time x1 / x10 / x60 / x300   Escape: quit'
if ($UseStandalone) {
    $LaunchExe = $Standalone
    $LaunchArgs = @($Destination)
    $WorkingDir = Split-Path -Parent $Standalone
} else {
    $LaunchExe = $EditorExe
    $LaunchArgs = @(('"' + $Project + '"'), $Destination)
    $WorkingDir = $ProjectRoot
    if (-not $Editor) { $LaunchArgs += '-game' }
}
if (-not $Editor) {
    $LaunchArgs += @('-windowed', "-ResX=$ResX", "-ResY=$ResY", '-NoSplash', '-NoScreenMessages')
}
Start-Process -FilePath $LaunchExe -ArgumentList $LaunchArgs -WorkingDirectory $WorkingDir -WindowStyle Normal