param([ValidateSet(0, 5760, 16384)][int]$TcpRxWindow = 0,
      [ValidateSet(16, 32)][int]$NetifTxQueue = 16,
      [ValidateSet(1280, 1460)][int]$NetifMtu = 1280)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build-serial-bridge.ps1') -Board C5 -Channel 149 -Country CN -RevC -DisableIris -ReceiveBenchmark -TcpRxWindow $TcpRxWindow -NetifTxQueue $NetifTxQueue -NetifMtu $NetifMtu
if ($LASTEXITCODE -ne 0) { throw "Receive benchmark build failed: $LASTEXITCODE" }
