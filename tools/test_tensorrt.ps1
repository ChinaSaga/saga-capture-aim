param(
    [string]$VisualStudio = 'C:/Program Files/Microsoft Visual Studio/18/Community',
    [string]$OutputDirectory = '.workbuddy/tensorrt-cpu-validation'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path $root $OutputDirectory
# These fixtures validate file discovery, production HTTP routing, and invalid
# inference inputs. They never start the application, drivers or GPU inference.
foreach ($name in @('test_model_files', 'test_model_http', 'test_tensorrt_api', 'test_onnx_fp16')) {
    & (Join-Path $PSScriptRoot 'build_performance.ps1') -Source "tools/$name.cpp" -Name $name `
        -VisualStudio $VisualStudio -OutputDirectory $OutputDirectory
    if ($LASTEXITCODE -ne 0) { throw "CPU test compilation failed: $name" }
    & (Join-Path $output "$name.exe")
    if ($LASTEXITCODE -ne 0) { throw "CPU test failed: $name ($LASTEXITCODE)" }
}
Write-Output 'TensorRT CPU-only validation passed. No GPU inference, driver or mouse service was started.'
