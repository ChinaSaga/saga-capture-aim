param(
    [string]$VisualStudio = 'C:/Program Files/Microsoft Visual Studio/18/Community',
    [string]$StartAt = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$relative = '.workbuddy/review-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8)
$output = Join-Path $root $relative
New-Item -ItemType Directory -Path $output | Out-Null
$tests = @(
    @{ Name='test_runtime_safety' }, @{ Name='test_move_transaction' },
    @{ Name='test_inference_inputs'; Sources=@('src/NCNN.cpp','src/ONNX.cpp') },
    @{ Name='test_model_files' }, @{ Name='test_model_http' },
    @{ Name='test_aim_profiles' }, @{ Name='test_adaptive_capture' }, @{ Name='test_adaptive_loop' }, @{ Name='test_tensor_pixels' },
    @{ Name='test_letterbox_geometry' }, @{ Name='test_yolo_hotpath' },
    @{ Name='test_capture_pipeline' }, @{ Name='test_capture_preview' },
    @{ Name='test_capture_start_async' }, @{ Name='test_makcu_serial_protocol' },
    @{ Name='test_wch_hardware_id' }, @{ Name='test_box_controller' },
    @{ Name='test_tensorrt_api' }, @{ Name='test_trt_converter' }, @{ Name='test_onnx_fp16' },
    @{ Name='test_ort_runtime_compatibility' }, @{ Name='test_app_exit' },
    @{ Name='test_startup_session' }, @{ Name='test_process_watchdog' },
    @{ Name='test_runtime_log'; Sources=@('src/RuntimeLog.cpp') }
)
$started = !$StartAt
$passed = 0
foreach ($test in $tests) {
    $name = $test.Name
    if ($name -eq $StartAt) { $started = $true }
    if (!$started) { continue }
    $sources = @()
    if ($test.ContainsKey('Sources')) { $sources = @($test.Sources) }
    try {
        & (Join-Path $PSScriptRoot 'build_performance.ps1') -Source "tools/$name.cpp" -Name $name `
            -OutputDirectory $relative -VisualStudio $VisualStudio -AdditionalSources $sources *> (Join-Path $output "$name-build.log")
        & (Join-Path $output "$name.exe") *> (Join-Path $output "$name-result.log")
        if ($LASTEXITCODE) { throw "Test exited with $LASTEXITCODE" }
        Write-Output "PASS $name"
        ++$passed
    } catch {
        Get-Content (Join-Path $output "$name-build.log") -Tail 20
        Get-Content (Join-Path $output "$name-result.log") -Tail 20 -ErrorAction SilentlyContinue
        throw "Failed $name; logs: $output; $_"
    }
}
foreach ($name in @('test_web_models','test_model_ui')) {
    & node (Join-Path $PSScriptRoot "$name.cjs")
    if ($LASTEXITCODE) { throw "Web regression failed: $name" }
}
if (!$started) { throw "Unknown starting test: $StartAt" }
Write-Output "All $passed selected native regressions passed with assertions enabled. Logs: $output"
