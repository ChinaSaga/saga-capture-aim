param([string]$Proxy = 'http://127.0.0.1:7897')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$downloadDir = Join-Path $projectRoot '.deps/downloads'
$headerDir = Join-Path $projectRoot '.deps/cuda-headers-13.0'
New-Item -ItemType Directory -Force $downloadDir,$headerDir | Out-Null
# This project loads CUDA DLLs dynamically: official 13.0 runtime/CRT headers
# suffice for the APIs used. Do not replace the deployed CUDA 13.4 DLLs.
$packages = @(
    @{name='cuda_cudart';version='13.0.96';sha='a2ed875f9997aa24904fb70cc9db3acd9308433cde99bc8e63ec1271c9da31b4'},
    @{name='cuda_crt';version='13.0.88';sha='ff413c89dbd96ed3b3dcfac923437de78acea32fb90309277197c8a127588112'}
)
foreach ($package in $packages) {
    $archiveName = "$($package.name)-windows-x86_64-$($package.version)-archive.zip"
    $archive = Join-Path $downloadDir $archiveName
    if (!(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $package.sha) {
        $curlArgs = @('-f','-sS','-L','--max-time','120','-o',$archive)
        if ($Proxy) { $curlArgs += @('--proxy',$Proxy) }
        $curlArgs += "https://developer.download.nvidia.com/compute/cuda/redist/$($package.name)/windows-x86_64/$archiveName"
        & curl.exe @curlArgs
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $archiveName" }
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $package.sha) { throw "Checksum mismatch: $archiveName" }
    Expand-Archive -LiteralPath $archive -DestinationPath $headerDir -Force
}
Copy-Item -LiteralPath (Join-Path $headerDir 'cuda_crt-windows-x86_64-13.0.88-archive/include/crt') -Destination (Join-Path $headerDir 'cuda_cudart-windows-x86_64-13.0.96-archive/include') -Recurse -Force
