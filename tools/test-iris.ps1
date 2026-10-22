param([string]$CXX = 'clang++')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root '.build-host-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
Push-Location $root
try {
    foreach ($suite in @('iris_tcon', 'iris_image', 'iris_color', 'iris_archive', 'iris_stream_decode', 'iris_latch_sample', 'stream_ack', 'rx_bench', 'rx_bench_history', 'frame_policy', 'revc_guard')) {
        $exe = Join-Path $out "$suite.exe"
        $suiteIncludes = @()
        if ($suite -eq 'iris_stream_decode') { $suiteIncludes = @('-I', 'tools/iris_stream_stubs') }
        & $CXX -std=c++17 -O2 -Wall -Wextra -Werror -Wno-unused-function -I ESP32Drop/src @suiteIncludes -o $exe "tools/${suite}_test.cpp"
        if ($LASTEXITCODE -ne 0) { throw "$suite compile failed" }
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "$suite failed" }
    }
} finally { Pop-Location }
