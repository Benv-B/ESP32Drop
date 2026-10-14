param(
    [string]$ArduinoCli = "",
    [string]$Fqbn = "m5stack:esp32:m5stack_stopwatch:PSRAM=opi,FlashSize=16M,PartitionScheme=huge_app,USBMode=hwcdc,CDCOnBoot=default,UploadMode=default,DebugLevel=none"
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if (-not $ArduinoCli) { $ArduinoCli = Join-Path $root ".build-tools/arduino-cli.exe" }
if (-not (Test-Path $ArduinoCli)) { throw "Install arduino-cli into .build-tools first; see host/README.md." }
$config = Join-Path $root ".build-tools/arduino.yaml"
if (-not (Test-Path $config)) { throw "Arduino CLI config missing; see host/README.md." }
& $ArduinoCli --config-file $config compile --fqbn $Fqbn `
    --library (Join-Path $root "ESP32Drop") `
    --build-path (Join-Path $root ".build-serial") --warnings default `
    (Join-Path $root "ESP32Drop/examples/SerialPhotoBridge")
if ($LASTEXITCODE -ne 0) { throw "Firmware compilation failed: $LASTEXITCODE" }
Write-Host "Built: $root/.build-serial/SerialPhotoBridge.ino.bin"
Write-Host "Build only: no device was erased or flashed."
