param(
    [string]$Python = 'C:\Users\Administrator\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe',
    [string]$VisualStudio = 'C:\Program Files\Microsoft Visual Studio\18\Community',
    [int]$Jobs = 8,
    [string]$DirectMLPackage = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$version = '1.30.0'
$commit = 'f2c39fe2f838cf35ce7da92824f5a5e3ee6e88a7'
$work = Join-Path $repo '.workbuddy\dependency-updates\ort-1.30.0'
$source = Join-Path $work 'source'
$build = Join-Path $work 'build'
$sdk = Join-Path $repo '.deps\onnxruntime-dml-1.30.0'
$dml = Join-Path $work 'directml-sdk'
$cmakeBin = Join-Path $VisualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$cmake = Join-Path $cmakeBin 'cmake.exe'
$ctest = Join-Path $cmakeBin 'ctest.exe'
foreach ($path in @($Python, $cmake, $ctest)) { if (!(Test-Path -LiteralPath $path)) { throw "Missing build tool: $path" } }
if ($Jobs -lt 1) { throw 'Jobs must be positive.' }
New-Item -ItemType Directory -Path $work -Force | Out-Null
if (!(Test-Path -LiteralPath (Join-Path $source '.git'))) {
    & git clone --depth 1 --branch "v$version" --recursive --shallow-submodules https://github.com/microsoft/onnxruntime.git $source
    if ($LASTEXITCODE) { throw 'Official ONNX Runtime source clone failed.' }
}
$actual = (& git -C $source rev-parse HEAD).Trim()
if ($LASTEXITCODE -or $actual -ne $commit) { throw "Expected official v$version commit $commit; found $actual. Preserve the existing checkout and use a separate build directory." }
& git -C $source submodule update --init --recursive --depth 1
if ($LASTEXITCODE) { throw 'Source submodule initialization failed.' }
if (!$DirectMLPackage) {
    $DirectMLPackage = Join-Path $work 'microsoft.ai.directml.1.15.4.nupkg'
    if (!(Test-Path -LiteralPath $DirectMLPackage)) {
        Invoke-WebRequest 'https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg' -OutFile $DirectMLPackage
    }
}
$dmlSha = (Get-FileHash -LiteralPath $DirectMLPackage -Algorithm SHA256).Hash.ToLowerInvariant()
if ($dmlSha -ne '4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9') { throw "DirectML 1.15.4 package hash mismatch: $dmlSha" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
New-Item -ItemType Directory -Path "$dml\include", "$dml\bin", "$dml\lib" -Force | Out-Null
$archive = [IO.Compression.ZipFile]::OpenRead($DirectMLPackage)
try {
    $extract = @{'include/DirectML.h'='include/DirectML.h'; 'bin/x64-win/DirectML.dll'='bin/DirectML.dll'; 'bin/x64-win/DirectML.lib'='lib/DirectML.lib'}
    foreach ($name in $extract.Keys) {
        $entry = $archive.GetEntry($name)
        if (!$entry) { throw "DirectML package missing $name" }
        $destination = Join-Path $dml $extract[$name]
        $entryStream = $entry.Open()
        $hasher = [Security.Cryptography.SHA256]::Create()
        try { $entrySha = ([BitConverter]::ToString($hasher.ComputeHash($entryStream))).Replace('-', '').ToLowerInvariant() }
        finally { $entryStream.Dispose(); $hasher.Dispose() }
        # Preserve header timestamps so an incremental rerun does not rebuild
        # every DirectML operator just because the same package was extracted.
        if (!(Test-Path -LiteralPath $destination) -or (Get-FileHash -LiteralPath $destination).Hash.ToLowerInvariant() -ne $entrySha) {
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $true)
        }
    }
} finally { $archive.Dispose() }
$previousUtf8 = $env:PYTHONUTF8
try {
    $env:PYTHONUTF8 = '1'
    $options = @(
        "$source\tools\ci_build\build.py", '--config', 'Release', '--update', '--build',
        '--build_shared_lib', '--use_dml', '--dml_path', $dml, '--skip_tests', '--skip_submodule_sync',
        '--cmake_generator', 'Visual Studio 18 2026', '--cmake_path', $cmake, '--ctest_path', $ctest,
        '--parallel', "$Jobs", '--build_dir', $build, '--cmake_extra_defines',
        "Python3_EXECUTABLE=$Python", 'onnxruntime_BUILD_UNIT_TESTS=OFF', 'onnxruntime_ENABLE_MEMLEAK_CHECKER=OFF',
        'onnxruntime_BUILD_BENCHMARKS=OFF', "CMAKE_INSTALL_PREFIX=$sdk",
        'CMAKE_DISABLE_PRECOMPILE_HEADERS=ON',
        'CMAKE_VS_GLOBALS=UseMultiToolTask=true;EnforceProcessCountAcrossBuilds=true;UseStructuredOutput=false'
    )
    # VS 18 structured diagnostics fail on a Chinese source path in the PCH
    # compiler's JSON output. Conventional diagnostics keep the source intact.
    & $Python @options 2>&1 | Tee-Object -FilePath (Join-Path $work 'build.log')
    if ($LASTEXITCODE) { throw "ONNX Runtime build failed. See $work\build.log" }
} finally { $env:PYTHONUTF8 = $previousUtf8 }
# Package only public API headers plus DLL/import library; avoids redistributing
# static internal/dependency libraries installed by a general CMake install.
New-Item -ItemType Directory -Path "$sdk\include", "$sdk\lib" -Force | Out-Null
Get-ChildItem -LiteralPath "$source\include\onnxruntime\core\session" -Filter '*.h' | Copy-Item -Destination "$sdk\include"
Get-ChildItem -LiteralPath "$source\include\onnxruntime\core\session" -Filter '*.inc' | Copy-Item -Destination "$sdk\include"
Copy-Item -LiteralPath "$source\include\onnxruntime\core\providers\dml\dml_provider_factory.h" -Destination "$sdk\include"
Copy-Item -LiteralPath "$dml\include\DirectML.h" -Destination "$sdk\include"
Copy-Item -LiteralPath "$build\Release\Release\onnxruntime.dll", "$build\Release\Release\onnxruntime.lib" -Destination "$sdk\lib"
# CPU and DirectML are linked into onnxruntime.dll; omit the unused bridge DLL.
$unusedProviderDll = Join-Path $sdk 'lib\onnxruntime_providers_shared.dll'
if (Test-Path -LiteralPath $unusedProviderDll) { Remove-Item -LiteralPath $unusedProviderDll }
Copy-Item -LiteralPath "$dml\bin\DirectML.dll" -Destination "$sdk\lib"
foreach ($name in @('LICENSE', 'ThirdPartyNotices.txt')) {
    if (Test-Path -LiteralPath (Join-Path $source $name)) { Copy-Item -LiteralPath (Join-Path $source $name) -Destination $sdk }
}
$metadata = [ordered]@{
    name='onnxruntime'; version=$version; source='https://github.com/microsoft/onnxruntime/tree/v1.30.0'; commit=$commit;
    execution_providers=@('DmlExecutionProvider','CPUExecutionProvider'); directml_version='1.15.4'; directml_package_sha256=$dmlSha;
    generator='Visual Studio 18 2026'; config='Release'; architecture='x64'; shared=$true; runtime='MD'; source_built=$true;
    build_script='tools/build_onnxruntime_dml.ps1'; build_options=$options
}
$metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$sdk\manifest.json" -Encoding utf8
Get-ChildItem -LiteralPath $sdk -Recurse -File | Where-Object {$_.Name -ne 'file-sha256.json'} | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($sdk.Length+1).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
} | ConvertTo-Json | Set-Content -LiteralPath "$sdk\file-sha256.json" -Encoding utf8
Write-Output "Built ONNX Runtime $version with DirectML 1.15.4: $sdk"
