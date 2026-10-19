param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$cli = Join-Path $root '.build-tools/arduino-cli.exe'
$config = Join-Path $root '.build-tools/arduino.yaml'
$coreData = (& $cli --config-file $config core list --format json) | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect installed SDK.' }
$core = $coreData.platforms | Where-Object { $_.id -eq 'm5stack:esp32' }
if ($core.installed_version -ne '3.3.8') { throw 'Requires pinned m5stack:esp32@3.3.8.' }
$target = Join-Path $root '.build-tools/arduino-data/packages/m5stack/tools/esp-rv32/2601'
$short = Join-Path $root '.build-tools/rv32'
if (-not (Test-Path $target)) { throw "Missing toolchain: $target" }
if (-not (Test-Path $short)) { New-Item -ItemType Junction -Path $short -Target $target | Out-Null }
$link = Get-Item -LiteralPath $short
if ($link.LinkType -ne 'Junction' -or [IO.Path]::GetFullPath(@($link.Target)[0]) -ne [IO.Path]::GetFullPath($target)) {
    throw 'Unexpected rv32 junction target.'
}
$fqbn = 'm5stack:esp32:m5stack_stamp_c5:PSRAM=enabled,FlashSize=8M,PartitionScheme=default_8MB,CDCOnBoot=cdc,DebugLevel=none'
& $cli --config-file $config compile --fqbn $fqbn `
    --build-property 'build.flash_size=8MB' `
    --build-property "compiler.path=$($short.Replace('\', '/'))/bin/" `
    --build-path (Join-Path $root '.build-revc-bringup') --warnings default `
    (Join-Path $root 'ESP32Drop/examples/RevCBringup')
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
Write-Host 'Build only. No device was erased or flashed.'
