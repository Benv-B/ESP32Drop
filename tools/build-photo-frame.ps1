param(
    [switch]$TestPattern,
    [ValidateRange(2000000, 10000000)][int]$SpiHz = 4000000
)
$ErrorActionPreference = 'Stop'
# This assembled frame is used in mainland China. Keep the measured C5 radio
# baseline explicit; world-domain 01 cannot enable channel 149 on this SDK.
# IRIS here means the AirDrop Live Photo capability, not the TCON brand.
# Advertise static-photo capability and still refuse unexpected video/HEIC.
& (Join-Path $PSScriptRoot 'build-serial-bridge.ps1') -Board C5 -Channel 149 -Country CN -RevC -DisableIris -Tcon -TestPattern:$TestPattern -SpiHz $SpiHz
if ($LASTEXITCODE -ne 0) { throw "Photo Frame build failed: $LASTEXITCODE" }
