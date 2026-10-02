param(
    [string]$Python = '',
    [string]$VisualStudio = 'C:\Program Files\Microsoft Visual Studio\18\Community',
    [int]$Jobs = 8
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Get-ProjectDependencies.ps1')
$deps = Get-ProjectDependencies -ProjectRoot $repo
$lock = Get-Content -LiteralPath (Join-Path $repo 'dependencies.lock.json') -Raw | ConvertFrom-Json
$cache = Join-Path $repo '.deps\downloads'
New-Item -ItemType Directory -Path $cache -Force | Out-Null

function Get-VerifiedArchive($entry, [string]$name) {
    $path = Join-Path $cache $name
    if (Test-Path -LiteralPath $path) {
        if ((Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -eq $entry.sha256) { return $path }
        throw "Cached archive failed SHA256 verification: $path. Preserve or remove it before retrying."
    }
    $temporary = $path + '.partial'
    try { Invoke-WebRequest -Uri $entry.url -OutFile $temporary -TimeoutSec 180 }
    catch {
        if (!$entry.mirror) { throw }
        Invoke-WebRequest -Uri $entry.mirror -OutFile $temporary -TimeoutSec 180
    }
    $hash = (Get-FileHash -LiteralPath $temporary).Hash.ToLowerInvariant()
    if ($hash -ne $entry.sha256 -and $entry.mirror) {
        Invoke-WebRequest -Uri $entry.mirror -OutFile $temporary -TimeoutSec 180
        $hash = (Get-FileHash -LiteralPath $temporary).Hash.ToLowerInvariant()
    }
    if ($hash -ne $entry.sha256) { throw "Download failed SHA256 verification: $name ($hash)" }
    Move-Item -LiteralPath $temporary -Destination $path
    return $path
}
foreach ($pair in @(@('SagaOpenCvVersion','opencv'),@('SagaNcnnVersion','ncnn'),@('SagaDirectMLVersion','directml'),@('SagaOrtVersion','onnxruntime'))) {
    if ($deps[$pair[0]] -ne $lock.($pair[1]).version) { throw "Dependencies.props and dependencies.lock.json disagree for $($pair[1])." }
}

if (!(Test-Path -LiteralPath "$($deps.SagaOpenCvDir)\x64\vc16\lib\$($deps.SagaOpenCvLibrary)") -or
    !(Test-Path -LiteralPath "$($deps.SagaOpenCvDir)\x64\vc16\bin\$($deps.SagaOpenCvRuntime)") -or
    !(Test-Path -LiteralPath "$($deps.SagaOpenCvDir)\include\opencv2\core\version.hpp") -or
    !(Test-Path -LiteralPath "$($deps.SagaOpenCvDir)\..\LICENSE.txt")) {
    $archive = Get-VerifiedArchive $lock.opencv "opencv-$($lock.opencv.version)-windows-verified.exe"
    $destination = Join-Path $repo ".deps\opencv-$($lock.opencv.version)"
    $process = Start-Process -FilePath $archive -ArgumentList @('-y','-gm2',('-o"' + $destination + '"')) -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode) { throw "OpenCV extraction failed: $($process.ExitCode)" }
}
if (!(Test-Path -LiteralPath "$($deps.SagaNcnnDir)\lib\ncnn.lib") -or
    !(Test-Path -LiteralPath "$($deps.SagaNcnnDir)\bin\ncnn.dll") -or
    !(Test-Path -LiteralPath "$($deps.SagaNcnnDir)\include\ncnn\net.h")) {
    $archive = Get-VerifiedArchive $lock.ncnn "ncnn-$($lock.ncnn.version)-windows-vs2022-shared.zip"
    $unpacked = Join-Path $cache ('ncnn-extract-' + [guid]::NewGuid().ToString('N'))
    Expand-Archive -LiteralPath $archive -DestinationPath $unpacked
    $sdk = Join-Path $unpacked "ncnn-$($lock.ncnn.version)-windows-vs2022-shared\x64"
    if (!(Test-Path -LiteralPath "$sdk\lib\ncnn.lib")) { throw 'NCNN archive layout changed.' }
    New-Item -ItemType Directory -Path $deps.SagaNcnnDir -Force | Out-Null
    foreach ($child in Get-ChildItem -LiteralPath $sdk) {
        Copy-Item -LiteralPath $child.FullName -Destination $deps.SagaNcnnDir -Recurse -Force
    }
}
if (!(Test-Path -LiteralPath "$($deps.SagaNcnnDir)\..\LICENSE.txt")) {
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/Tencent/ncnn/$($lock.ncnn.version)/LICENSE.txt" -OutFile "$($deps.SagaNcnnDir)\..\LICENSE.txt"
}
if (!(Test-Path -LiteralPath "$($deps.SagaDirectMLDir)\DirectML.dll") -or
    !(Test-Path -LiteralPath "$($deps.SagaDirectMLDir)\LICENSE.txt") -or
    !(Test-Path -LiteralPath "$($deps.SagaDirectMLDir)\ThirdPartyNotices.txt")) {
    $package = Get-VerifiedArchive $lock.directml "microsoft.ai.directml.$($lock.directml.version).nupkg"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    New-Item -ItemType Directory -Path $deps.SagaDirectMLDir -Force | Out-Null
    $zip = [IO.Compression.ZipFile]::OpenRead($package)
    try {
        foreach ($name in @('bin/x64-win/DirectML.dll','LICENSE.txt','ThirdPartyNotices.txt')) {
            $entry = $zip.GetEntry($name)
            if (!$entry) { throw "DirectML package missing $name" }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $deps.SagaDirectMLDir (Split-Path $name -Leaf)), $true)
        }
    } finally { $zip.Dispose() }
}
if (!(Test-Path -LiteralPath "$($deps.SagaOrtDir)\lib\onnxruntime.dll") -or
    !(Test-Path -LiteralPath "$($deps.SagaOrtDir)\lib\onnxruntime.lib") -or
    !(Test-Path -LiteralPath "$($deps.SagaOrtDir)\include\onnxruntime_cxx_api.h") -or
    !(Test-Path -LiteralPath "$($deps.SagaOrtDir)\LICENSE")) {
    $buildArguments = @{VisualStudio=$VisualStudio; Jobs=$Jobs}
    if ($Python) { $buildArguments.Python = $Python }
    & (Join-Path $PSScriptRoot 'build_onnxruntime_dml.ps1') @buildArguments
}
foreach ($file in @("$($deps.SagaOpenCvDir)\include\opencv2\core\version.hpp", "$($deps.SagaOpenCvDir)\x64\vc16\bin\$($deps.SagaOpenCvRuntime)",
    "$($deps.SagaNcnnDir)\include\ncnn\net.h", "$($deps.SagaNcnnDir)\bin\ncnn.dll", "$($deps.SagaOrtDir)\include\onnxruntime_cxx_api.h",
    "$($deps.SagaOrtDir)\lib\onnxruntime.lib", "$($deps.SagaOrtDir)\lib\onnxruntime.dll", "$($deps.SagaDirectMLDir)\DirectML.dll")) {
    if (!(Test-Path -LiteralPath $file)) { throw "Required SDK file missing: $file" }
}
Write-Output 'Project dependencies are ready. Build SagaApp.vcxproj with MSBuild.'
