param(
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [string]$VisualStudio = 'C:\Program Files\Microsoft Visual Studio\18\Community',
    [switch]$InstallDependencies,
    [string]$Python = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if ($InstallDependencies) {
    $arguments = @{ VisualStudio = $VisualStudio }
    if ($Python) { $arguments.Python = $Python }
    & (Join-Path $PSScriptRoot 'install_dependencies.ps1') @arguments
    & (Join-Path $PSScriptRoot 'install_tensorrt.ps1')
}
$msbuild = Join-Path $VisualStudio 'MSBuild\Current\Bin\MSBuild.exe'
if (!(Test-Path -LiteralPath $msbuild)) { throw "MSBuild is missing: $msbuild" }
& $msbuild (Join-Path $repo 'Saga.sln') "/p:Configuration=$Configuration" '/p:Platform=x64' '/m' '/v:minimal' '/nologo'
if ($LASTEXITCODE) { throw "Application build failed: $LASTEXITCODE" }
