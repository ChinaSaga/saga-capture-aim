param(
    [string]$Source = 'tools/benchmark_onnx.cpp',
    [string]$Headers = 'src',
    [string]$Name = 'benchmark_onnx',
    [string]$OutputDirectory = '.workbuddy/performance/bin',
    [string]$VisualStudio = 'C:/Program Files/Microsoft Visual Studio/18/Community'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Get-ProjectDependencies.ps1')
$deps = Get-ProjectDependencies -ProjectRoot $root
$vs = $VisualStudio
$vc = (Get-ChildItem -LiteralPath "$vs/VC/Tools/MSVC" -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1).FullName
$sdk = 'C:/Program Files (x86)/Windows Kits/10'
$sdkVersion = (Get-ChildItem -LiteralPath "$sdk/Include" -Directory | Where-Object { $_.Name -match '^10\.0\.\d+\.0$' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1).Name
$out = Join-Path $root $OutputDirectory
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:INCLUDE = "$vc/include;$sdk/Include/$sdkVersion/ucrt;$sdk/Include/$sdkVersion/um;$sdk/Include/$sdkVersion/shared;$sdk/Include/$sdkVersion/winrt"
$env:LIB = "$vc/lib/x64;$sdk/Lib/$sdkVersion/ucrt/x64;$sdk/Lib/$sdkVersion/um/x64"
$compilerArgs = @('/nologo','/O2','/Ob3','/Gy','/MD','/EHsc','/std:c++20','/arch:AVX2','/fp:fast','/source-charset:utf-8','/execution-charset:.936', '/DNDEBUG',
    "/I$(Join-Path $root $Headers)","/I$($deps.SagaOpenCvDir)/include","/I$($deps.SagaOrtDir)/include",
    "/I$($deps.SagaNcnnDir)/include","/I$($deps.SagaNcnnDir)/include/ncnn",
    "/I$($deps.SagaTensorRtDir)/include","/I$($deps.SagaCudaDir)/include",
    "/Fo$out/$Name.obj", "/Fe$out/$Name.exe", (Join-Path $root $Source), '/link', '/OPT:REF',
    "/LIBPATH:$($deps.SagaOpenCvDir)/x64/vc16/lib","/LIBPATH:$($deps.SagaOrtDir)/lib",
    "/LIBPATH:$($deps.SagaNcnnDir)/lib",'ncnn.lib',$deps.SagaOpenCvLibrary,'onnxruntime.lib','dxgi.lib')
& "$vc/bin/Hostx64/x64/cl.exe" @compilerArgs
if ($LASTEXITCODE -ne 0) { throw "Benchmark build failed: $LASTEXITCODE" }
# Keep native dependencies beside the EXE so unrelated PATH DLLs cannot win.
foreach ($dll in @("$($deps.SagaOpenCvDir)/x64/vc16/bin/$($deps.SagaOpenCvRuntime)",
    "$($deps.SagaOrtDir)/lib/onnxruntime.dll",
    "$($deps.SagaDirectMLDir)/DirectML.dll",
    "$($deps.SagaNcnnDir)/bin/ncnn.dll")) {
    Copy-Item -LiteralPath $dll -Destination $out -Force
}
# This SDK links CPU and DirectML into onnxruntime.dll.
$unusedProviderDll = Join-Path $out 'onnxruntime_providers_shared.dll'
if (Test-Path -LiteralPath $unusedProviderDll) { Remove-Item -LiteralPath $unusedProviderDll }
