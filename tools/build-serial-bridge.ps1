param(
    [string]$ArduinoCli = "",
    [string]$Fqbn = "m5stack:esp32:m5stack_stopwatch:PSRAM=opi,FlashMode=qio,PartitionScheme=default_8MB,USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default,DebugLevel=none"
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if (-not $ArduinoCli) { $ArduinoCli = Join-Path $root ".build-tools/arduino-cli.exe" }
if (-not (Test-Path $ArduinoCli)) { throw "Install arduino-cli into .build-tools first; see host/README.md." }
$config = Join-Path $root ".build-tools/arduino.yaml"
if (-not (Test-Path $config)) { throw "Arduino CLI config missing; see host/README.md." }
$coresJson = & $ArduinoCli --config-file $config core list --format json
if ($LASTEXITCODE -ne 0) { throw "Cannot inspect installed Arduino cores." }
$cores = $coresJson | ConvertFrom-Json
$core = $cores.platforms | Where-Object { $_.id -eq "m5stack:esp32" }
if (-not $core -or $core.installed_version -ne "3.3.8") {
    throw "This project requires m5stack:esp32@3.3.8. Install the pinned SDK; do not upgrade it."
}
# The pinned StopWatch menu only lists 16/32 MB; override the actual XIAO flash size.
& $ArduinoCli --config-file $config compile --fqbn $Fqbn `
    --build-property "build.flash_size=8MB" `
    --library (Join-Path $root "ESP32Drop") `
    --build-path (Join-Path $root ".build-serial") --warnings default `
    (Join-Path $root "ESP32Drop/examples/SerialPhotoBridge")
if ($LASTEXITCODE -ne 0) { throw "Firmware compilation failed: $LASTEXITCODE" }
Write-Host "Built: $root/.build-serial/SerialPhotoBridge.ino.bin"
Write-Host "Build only: no device was erased or flashed."
