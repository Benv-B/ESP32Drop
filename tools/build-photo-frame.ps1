param([switch]$DisableIris)
$ErrorActionPreference = 'Stop'
# This assembled frame is used in mainland China. Keep the measured C5 radio
# baseline explicit; world-domain 01 cannot enable channel 149 on this SDK.
& (Join-Path $PSScriptRoot 'build-serial-bridge.ps1') -Board C5 -Channel 149 -Country CN -RevC -DisableIris:$DisableIris
if ($LASTEXITCODE -ne 0) { throw "Photo Frame build failed: $LASTEXITCODE" }
