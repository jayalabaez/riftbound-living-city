$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$engineRoot = 'C:\Program Files\Epic Games\UE_5.8'
$project = Join-Path $projectRoot 'Riftbound.uproject'
& (Join-Path $engineRoot 'Engine\Build\BatchFiles\Build.bat') RiftboundEditor Win64 Development $project -WaitMutex -NoHotReloadFromIDE
if ($LASTEXITCODE -ne 0) { throw 'C++ build failed.' }
$bootstrap = Join-Path $PSScriptRoot 'bootstrap_unreal.py'
& (Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') $project -unattended '-NullRHI' "-ExecutePythonScript=$bootstrap" -NoSplash
if ($LASTEXITCODE -ne 0) { throw 'Content bootstrap failed.' }
$planetBootstrap = Join-Path $PSScriptRoot 'bootstrap_voyager_materials.py'
& (Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') $project -unattended '-NullRHI' "-ExecutePythonScript=$planetBootstrap" -NoSplash
if ($LASTEXITCODE -ne 0) { throw 'Planet material generation failed.' }
$materialReport = Get-Content -LiteralPath (Join-Path $projectRoot 'Saved\VoyagerMaterialsReport.json') -Raw | ConvertFrom-Json
if ($materialReport.status -ne 'success') { throw ('Planet material generation failed: ' + $materialReport.error) }
$cloudBootstrap = Join-Path $PSScriptRoot 'bootstrap_voyager_clouds.py'
& (Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') $project -unattended '-NullRHI' "-ExecutePythonScript=$cloudBootstrap" -NoSplash
if ($LASTEXITCODE -ne 0) { throw 'Cloud material generation failed.' }
$cloudReport = Get-Content -LiteralPath (Join-Path $projectRoot 'Saved\VoyagerCloudsReport.json') -Raw | ConvertFrom-Json
if ($cloudReport.status -ne 'success') { throw ('Cloud material generation failed: ' + $cloudReport.error) }
$natureBootstrap = Join-Path $PSScriptRoot 'bootstrap_voyager_nature.py'
& (Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') $project -unattended '-NullRHI' "-ExecutePythonScript=$natureBootstrap" -NoSplash
if ($LASTEXITCODE -ne 0) { throw 'Nature asset generation failed.' }
$natureReport = Get-Content -LiteralPath (Join-Path $projectRoot 'Saved\VoyagerNatureReport.json') -Raw | ConvertFrom-Json
if ($natureReport.status -ne 'success') { throw 'Nature asset generation failed; inspect the Unreal log.' }
$cityBootstrap = Join-Path $PSScriptRoot 'bootstrap_voyager_city_materials.py'
& (Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') $project -unattended '-NullRHI' "-ExecutePythonScript=$cityBootstrap" -NoSplash
if ($LASTEXITCODE -ne 0) { throw 'City material generation failed.' }
$cityReport = Get-Content -LiteralPath (Join-Path $projectRoot 'Saved\VoyagerCityMaterialsReport.json') -Raw | ConvertFrom-Json
if ($cityReport.status -ne 'success') { throw ('City material generation failed: ' + $cityReport.error) }
foreach ($assetPass in @('ships','characters','building_materials','cosmos','security')) {
    $assetScript = Join-Path $PSScriptRoot ('bootstrap_voyager_' + $assetPass + '.py')
    & (Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe') $project -unattended '-NullRHI' "-ExecutePythonScript=$assetScript" -NoSplash
    if ($LASTEXITCODE -ne 0) { throw "Asset generation failed: $assetPass" }
    $reportName = @{ships='VoyagerShipsReport.json';characters='VoyagerCharactersReport.json';building_materials='VoyagerBuildingMaterialsReport.json';cosmos='VoyagerCosmosMaterialsReport.json';security='VoyagerSecurityAssetsReport.json'}[$assetPass]
    $assetReport = Get-Content -LiteralPath (Join-Path $projectRoot ('Saved\' + $reportName)) -Raw | ConvertFrom-Json
    if ($assetReport.status -ne 'success') { throw "Asset report failed: $assetPass" }
}
Write-Host 'Riftbound is ready. Double-click Play Riftbound.cmd.'
