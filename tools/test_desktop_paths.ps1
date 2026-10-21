param(
    [string]$VisualStudio = 'C:/Program Files/Microsoft Visual Studio/18/Community',
    [string]$OutputDirectory = '.workbuddy/desktop-validation',
    [string]$ModelReleaseDirectory = 'x64/Release'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path $root $OutputDirectory
& (Join-Path $PSScriptRoot 'test_tensorrt.ps1') -VisualStudio $VisualStudio -OutputDirectory $OutputDirectory
& (Join-Path $PSScriptRoot 'build_performance.ps1') -Source tools/test_model_loading_paths.cpp `
    -AdditionalSources src/ONNX.cpp,src/NCNN.cpp,src/Mouse.cpp,src/TensorRT.cpp,src/TrtRuntime.cpp `
    -Name test_model_loading_paths -OutputDirectory $OutputDirectory -VisualStudio $VisualStudio
$modelDirectory = if ([IO.Path]::IsPathRooted($ModelReleaseDirectory)) { $ModelReleaseDirectory } else { Join-Path $root $ModelReleaseDirectory }
& (Join-Path $output 'test_model_loading_paths.exe') $modelDirectory
if ($LASTEXITCODE) { throw 'Native model path regression failed' }

# Repeat with a UTF-8 process ACP, without changing the computer's locale.
$manifest = Join-Path $output 'utf8-fixture.manifest'
@'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="Saga.PathTest" version="1.0.0.0" processorArchitecture="amd64" />
  <application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>
    <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
  </windowsSettings></application>
</assembly>
'@ | Set-Content -LiteralPath $manifest -Encoding UTF8
$sdk = Get-ChildItem 'C:/Program Files (x86)/Windows Kits/10/bin' -Directory |
    Where-Object { $_.Name -match '^10\.0\.\d+\.0$' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$mt = Join-Path $sdk.FullName 'x64/mt.exe'
foreach ($name in @('test_model_http','test_model_loading_paths')) {
    $exe = Join-Path $output "$name.exe"
    & $mt -nologo -manifest $manifest "-outputresource:$exe;#1"
    if ($LASTEXITCODE) { throw "Test manifest failed: $name" }
    if ($name -eq 'test_model_loading_paths') { & $exe $modelDirectory } else { & $exe }
    if ($LASTEXITCODE) { throw "UTF-8 ACP regression failed: $name" }
}
& node (Join-Path $PSScriptRoot 'test_model_ui.cjs')
if ($LASTEXITCODE) { throw 'Model UI regression failed' }
Write-Output 'Desktop path regressions passed in system and UTF-8 process locales. No driver/capture/mouse service was started.'
