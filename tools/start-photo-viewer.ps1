param(
    [string]$Port = "auto",
    [int]$Baud = 921600,
    [int]$HttpPort = 8765,
    [switch]$NoBrowser
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$python = Join-Path $root ".venv/Scripts/python.exe"
if (-not (Test-Path $python)) {
    throw "Create .venv and install host/requirements.txt first; see host/README.md."
}
$env:PYTHONUTF8 = "1"
$argsForViewer = @((Join-Path $root "host/viewer.py"), "--port", $Port,
                  "--baud", "$Baud", "--http-port", "$HttpPort")
if ($NoBrowser) { $argsForViewer += "--no-browser" }
& $python @argsForViewer
if ($LASTEXITCODE -ne 0) { throw "Viewer failed: $LASTEXITCODE" }
