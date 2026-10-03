$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$profileTool = Join-Path $projectRoot 'x64/Release/NvPowerProfile.exe'
foreach ($executable in @('圣人双机服务端.exe','InferenceBench.exe')) {
    & $profileTool remove $executable
    if ($LASTEXITCODE -ne 0) { throw "Profile restore failed: $executable" }
}
Write-Host 'Only the Saga test-created application profiles were removed. Restart the application to apply.'
