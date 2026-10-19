param([string]$Python = "", [string]$CC = "clang", [string]$CXX = "clang++")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if (-not $Python) { $Python = Join-Path $root '.venv/Scripts/python.exe' }
if (-not (Test-Path $Python)) { throw "Python missing; install host/requirements.txt first." }
$out = Join-Path $root '.build-host-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed: $LASTEXITCODE" }
}

Push-Location $root
try {
    # Focused bridge regressions. CI also runs the full core suite with sanitizers.
    foreach ($suite in @('frame', 'window', 'http', 'serve', 'dvzip', 'cpio', 'dedup')) {
        $exe = Join-Path $out "$suite.exe"
        Invoke-Checked $CC @('-O2', '-Wall', '-Wextra', '-Wno-unused-function', '-o', $exe, "tools/${suite}_test.c")
        Invoke-Checked $exe @()
    }
    $exe = Join-Path $out 'bridge_write.exe'
    foreach ($noIris in @(0, 1)) {
        $flagsExe = Join-Path $out "frame_flags_$noIris.exe"
        Invoke-Checked $CC @('-std=c11', '-O2', '-Wall', '-Wextra', '-Wno-unused-function', "-DAWDL_AIRDROP_NO_IRIS=$noIris", '-o', $flagsExe, 'tools/frame_flags_test.c')
        Invoke-Checked $flagsExe @()
    }
    Invoke-Checked $CXX @('-std=c++17', '-O2', '-Wall', '-Wextra', '-o', $exe, 'tools/bridge_write_test.cpp')
    Invoke-Checked $exe @()
    $exe = Join-Path $out 'revc_guard.exe'
    Invoke-Checked $CXX @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-o', $exe, 'tools/revc_guard_test.cpp')
    Invoke-Checked $exe @()
    $exe = Join-Path $out 'frame_policy.exe'
    Invoke-Checked $CXX @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-o', $exe, 'tools/frame_policy_test.cpp')
    Invoke-Checked $exe @()
    Invoke-Checked $Python @('-m', 'unittest', 'discover', '-s', 'host', '-p', 'test_*.py', '-v')
} finally {
    Pop-Location
}
