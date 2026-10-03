param([string]$DependencyRoot = (Join-Path $PSScriptRoot '..\.deps'))
$ErrorActionPreference = 'Stop'
$version = '11.3.0.99'
$url = 'https://developer.download.nvidia.com/compute/machine-learning/tensorrt/11.3.0/zip/TensorRT-Enterprise-11.3.0.99-Windows-amd64-cuda-13.4-Release-external.zip'
$sha256 = '8ffd112d7bab97bc81eed818358b85df8c317bcdfc3cd3c731c220df7ca737eb'
$DependencyRoot = [IO.Path]::GetFullPath($DependencyRoot)
$download = Join-Path $DependencyRoot 'downloads'
$destination = Join-Path $DependencyRoot "tensorrt-$version"
New-Item -ItemType Directory -Force $download, $destination | Out-Null
$archive = Join-Path $download "TensorRT-$version-cuda13.4.zip"
if (!(Test-Path -LiteralPath $archive)) {
    & curl.exe --fail --location --retry 3 --output $archive $url
    if ($LASTEXITCODE) { throw 'TensorRT download failed' }
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha256) {
    throw 'TensorRT archive SHA256 mismatch; remove the failed download before retrying.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    foreach ($entry in $zip.Entries) {
        $relative = $entry.FullName -replace '^TensorRT-11\.3\.0\.99/', ''
        if (($relative -notmatch '^(bin|include|lib)/' -and $relative -notin @('doc/Acknowledgements.txt','doc/README.txt')) -or !$entry.Name) { continue }
        $target = [IO.Path]::GetFullPath((Join-Path $destination $relative))
        if (!$target.StartsWith($destination + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid archive path' }
        New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($target)) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
    }
} finally { $zip.Dispose() }
$licenseUrl = 'https://docs.nvidia.com/deeplearning/tensorrt/latest/reference/sla.html'
$licenseDirectory = Join-Path $destination 'doc'
New-Item -ItemType Directory -Force $licenseDirectory | Out-Null
$licenseFile = Join-Path $licenseDirectory 'LICENSE-AGREEMENT.html'
$licenseTemporary = Join-Path $licenseDirectory 'LICENSE-AGREEMENT.html.download'
try {
    & curl.exe --fail --location --retry 3 --output $licenseTemporary $licenseUrl
    if ($LASTEXITCODE) { throw 'TensorRT license download failed' }
    Move-Item -LiteralPath $licenseTemporary -Destination $licenseFile -Force
} finally { if (Test-Path -LiteralPath $licenseTemporary) { Remove-Item -LiteralPath $licenseTemporary -Force } }
[ordered]@{
    version=$version; platform='Windows amd64'; cuda='13.4'; url=$url; sha256=$sha256
    releaseNotes='https://docs.nvidia.com/deeplearning/tensorrt/latest/getting-started/release-notes.html'
    license=$licenseUrl
    note='Official NVIDIA binary SDK. CUDA toolkit and NVIDIA driver must be installed separately. No system files are changed.'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'provenance.json') -Encoding utf8
Write-Host "TensorRT $version installed: $destination"
