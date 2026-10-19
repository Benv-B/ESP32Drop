param(
    [string]$ArduinoCli = "",
    [string]$Fqbn = "",
    [ValidateSet("S3", "C5")][string]$Board = "S3",
    [ValidateSet(6, 44, 149)][int]$Channel = 6,
    [ValidatePattern("^(01|[A-Z]{2})$")][string]$Country = "01",
    [switch]$DisableAwdlBssid,
    [switch]$RevC,
    [switch]$DisableIris
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
if ($RevC -and $Board -ne "C5") { throw "RevC requires -Board C5." }
if ($DisableIris -and -not $RevC) { throw "The no-IRIS experiment requires -RevC." }
if ($Board -eq "S3" -and $Channel -ne 6) { throw "ESP32-S3 does not support 5 GHz." }
if ($Board -ne "C5" -and $DisableAwdlBssid) { throw "DisableAwdlBssid is C5-only." }
if (-not $Fqbn) {
    if ($Board -eq "C5") {
        # Reuse the installed, pinned C5 SDK, without initializing M5 peripherals.
        $Fqbn = "m5stack:esp32:m5stack_stamp_c5:PSRAM=enabled,FlashSize=8M,PartitionScheme=default_8MB,CDCOnBoot=cdc,DebugLevel=none"
    } else {
        $Fqbn = "m5stack:esp32:m5stack_stopwatch:PSRAM=opi,FlashMode=qio,PartitionScheme=default_8MB,USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default,DebugLevel=none"
    }
}
$buildDir = if ($Board -eq "C5") { ".build-serial-c5-ch$Channel" } else { ".build-serial" }
if ($RevC) { $buildDir = ".build-photo-frame-c5-ch$Channel" }
if ($DisableIris) { $buildDir = ".build-photo-frame-noiris-c5-ch$Channel" }
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
$extraProperties = @()
$countryCode = $Country.ToUpperInvariant()
$radioFlags = "-DAWDL_RADIO_CHANNEL=$Channel -DAWDL_COUNTRY_0=$([int][char]$countryCode[0]) -DAWDL_COUNTRY_1=$([int][char]$countryCode[1])"
if ($RevC) { $radioFlags += " -DPHOTO_FRAME_REVC=1" }
if ($DisableIris) { $radioFlags += " -DAWDL_AIRDROP_NO_IRIS=1" }
if ($Board -eq "C5") {
    # Required for the measured hardware ACK fix; the private ABI is SDK-pinned.
    # Disabling is for diagnostics only, not the normal build.
    $radioFlags += " -DAWDL_C5_BSSID_FILTER=$([int](-not $DisableAwdlBssid))"
}
if ($Board -eq "C5") {
    # GCC's unnormalised relative C++ include paths exceed MAX_PATH in this tree.
    # A short junction preserves the SDK files and makes those paths usable.
    $rvTarget = Join-Path $root ".build-tools/arduino-data/packages/m5stack/tools/esp-rv32/2601"
    $rvShort = Join-Path $root ".build-tools/rv32"
    if (-not (Test-Path $rvTarget)) { throw "Pinned RISC-V toolchain missing: $rvTarget" }
    if (-not (Test-Path $rvShort)) { New-Item -ItemType Junction -Path $rvShort -Target $rvTarget | Out-Null }
    $rvLink = Get-Item -LiteralPath $rvShort
    if ($rvLink.LinkType -ne "Junction" -or [IO.Path]::GetFullPath(@($rvLink.Target)[0]) -ne [IO.Path]::GetFullPath($rvTarget)) {
        throw "Expected $rvShort to be a junction to the pinned RISC-V toolchain."
    }
    $extraProperties = @("--build-property", "compiler.path=$($rvShort.Replace('\', '/'))/bin/")
}
& $ArduinoCli --config-file $config compile --fqbn $Fqbn `
    --build-property "build.flash_size=8MB" `
    --build-property "compiler.cpp.extra_flags=$radioFlags" `
    @extraProperties `
    --library (Join-Path $root "ESP32Drop") `
    --build-path (Join-Path $root $buildDir) --warnings default `
    (Join-Path $root "ESP32Drop/examples/SerialPhotoBridge")
if ($LASTEXITCODE -ne 0) { throw "Firmware compilation failed: $LASTEXITCODE" }
Write-Host "Built: $root/$buildDir/SerialPhotoBridge.ino.bin"
Write-Host "Build only: no device was erased or flashed."
