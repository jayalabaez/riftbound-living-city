$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$projectPath = Join-Path $projectRoot 'Riftbound.uproject'
$engineRoot = 'C:\Program Files\Epic Games\UE_5.8'
$archivePath = Join-Path $projectRoot 'Packaged\Riftbound'
& (Join-Path $engineRoot 'Engine\Build\BatchFiles\RunUAT.bat') BuildCookRun "-project=$projectPath" -noP4 -platform=Win64 -clientconfig=Development -build -cook -stage -pak '-map=/Game/Maps/Forest+/Engine/Maps/Entry' -unattended -utf8output
if ($LASTEXITCODE -ne 0) { throw 'Standalone build failed. See Saved/Logs and the Unreal AutomationTool log.' }
# Copy changed payloads with native PowerShell. An unchanged launcher can be
# held by Explorer/icon inspection; comparing contents avoids rewriting it.
$stagedRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'Saved\StagedBuilds\Windows'))
$targetRoot = [IO.Path]::GetFullPath((Join-Path $archivePath 'Windows'))
if (-not (Test-Path -LiteralPath (Join-Path $stagedRoot 'Riftbound\Binaries\Win64\Riftbound.exe'))) { throw 'Staged game executable is missing.' }
$copied=0; $unchanged=0
foreach ($source in Get-ChildItem -LiteralPath $stagedRoot -Recurse -File) {
    $relative=[IO.Path]::GetRelativePath($stagedRoot,$source.FullName)
    $destination=[IO.Path]::GetFullPath((Join-Path $targetRoot $relative))
    if (-not $destination.StartsWith($targetRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Archive destination escaped the game directory.' }
    if (Test-Path -LiteralPath $destination) {
        $existing=Get-Item -LiteralPath $destination
        if ($source.Length -eq $existing.Length -and (Get-FileHash -LiteralPath $source.FullName).Hash -eq (Get-FileHash -LiteralPath $destination).Hash) { $unchanged++;continue }
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source.FullName -Destination $destination -Force
    $copied++
}
Write-Host "Archived $copied updated files; $unchanged identical files retained."
Write-Host "Standalone build ready: $archivePath\Windows\Riftbound.exe"
