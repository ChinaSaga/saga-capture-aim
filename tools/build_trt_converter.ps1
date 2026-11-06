param(
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [string]$VisualStudio = 'C:\Program Files\Microsoft Visual Studio\18\Community',
    [string]$CudaDirectory = '',
    [switch]$InstallTensorRt
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Get-ProjectDependencies.ps1')
$dependencies = Get-ProjectDependencies -ProjectRoot $repo
if ($InstallTensorRt) {
    & (Join-Path $PSScriptRoot 'install_tensorrt.ps1')
}
$headers = Join-Path $dependencies.SagaTensorRtDir 'include'
foreach ($header in @('NvInfer.h','NvOnnxParser.h')) {
    if (!(Test-Path -LiteralPath (Join-Path $headers $header))) {
        throw "TensorRT $($dependencies.SagaTensorRtVersion) SDK is missing. Run tools/install_tensorrt.ps1 or use -InstallTensorRt. Expected: $headers"
    }
}
foreach ($dll in @('nvinfer_11.dll','nvonnxparser_11.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $dependencies.SagaTensorRtDir "bin/$dll"))) {
        throw "TensorRT SDK is incomplete ($dll missing). Run tools/install_tensorrt.ps1."
    }
}
if (!$CudaDirectory) { $CudaDirectory = $dependencies.SagaCudaDir }
if (!$CudaDirectory -or !(Test-Path -LiteralPath (Join-Path $CudaDirectory 'include/cuda_runtime_api.h'))) {
    throw "CUDA $($dependencies.SagaCudaVersion) headers are missing. Install the NVIDIA CUDA Toolkit and set CUDA_PATH, or pass -CudaDirectory with the toolkit folder."
}
$runtimeHeader = Get-Content -LiteralPath (Join-Path $CudaDirectory 'include/cuda_runtime_api.h') -Raw
if ($runtimeHeader -notmatch '#define\s+CUDART_VERSION\s+13040\b') {
    throw "This TensorRT SDK targets CUDA 13.4. Point -CudaDirectory to the CUDA 13.4 toolkit."
}
$cudaRuntime = @('bin/x64/cudart64_13.dll','bin/cudart64_13.dll') |
    ForEach-Object { Join-Path $CudaDirectory $_ } | Where-Object { Test-Path -LiteralPath $_ }
if (!$cudaRuntime) { throw "CUDA runtime cudart64_13.dll is missing from the selected toolkit." }
$msbuild = Join-Path $VisualStudio 'MSBuild/Current/Bin/MSBuild.exe'
if (!(Test-Path -LiteralPath $msbuild)) { throw "MSBuild is missing: $msbuild. Select a Visual Studio installation with the C++ desktop workload." }
& $msbuild (Join-Path $repo 'TrtConverter.vcxproj') "/p:Configuration=$Configuration" '/p:Platform=x64' "/p:SagaCudaDir=$CudaDirectory" '/m' '/v:minimal' '/nologo'
if ($LASTEXITCODE) { throw "TensorRT converter build failed: $LASTEXITCODE" }
Write-Output "Converter ready: $(Join-Path $repo "x64/$Configuration/ONNX转TRT工具.exe")"
