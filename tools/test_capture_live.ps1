param(
    [string]$Executable = '.workbuddy/capture-optimization/test_capture_stress.exe',
    [ValidateRange(1,30)][int]$SecondsPerMode = 2
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = (Resolve-Path -LiteralPath (Join-Path $root $Executable)).Path
$output = Join-Path $root ('.workbuddy/capture-live-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $output | Out-Null
$listing = & $exe --list
if ($LASTEXITCODE) { throw 'Capture format enumeration failed' }
$formats = @($listing | Where-Object { $_ -match '^\d+@\d+@\d+@' }) -join '|'
$results = @()
foreach ($entry in ($formats -split '\|')) {
    if ($entry -notmatch '^(\d+)@(\d+)@(\d+)@([A-Z0-9]+)$') { throw "Unexpected format: $entry" }
    $width = [int]$Matches[1]; $height = [int]$Matches[2]; $fps = [int]$Matches[3]; $format = $Matches[4]
    $name = "$format-$width-$height-$fps"
    $stdout = Join-Path $output "$name.log"
    $stderr = Join-Path $output "$name-error.log"
    $process = Start-Process -FilePath $exe -ArgumentList @('--live',$format,$fps,320,$SecondsPerMode,'fixed',$width,$height) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $finished = $process.WaitForExit(($SecondsPerMode + 25) * 1000)
    if (!$finished) { Stop-Process -Id $process.Id -Force }
    $process.Refresh()
    $text = (Get-Content -LiteralPath $stdout -Raw) + (Get-Content -LiteralPath $stderr -Raw)
    $ok = $finished -and $process.ExitCode -eq 0 -and $text.Contains('LIVE format=')
    $results += [pscustomobject]@{ Mode=$name; Passed=$ok; Timeout=(!$finished); Output=$text.Trim() }
    Write-Output "$name passed=$ok $($text.Trim())"
}
$results | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding UTF8
$passed = @($results | Where-Object Passed).Count
Write-Output "Live modes: $passed/$($results.Count) passed. Logs: $output"
if ($passed -ne $results.Count) { exit 1 }
