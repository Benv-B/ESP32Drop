# Build the C5 ChLCD frame

This is an experimental prototype using XIAO ESP32-C5 (8 MB flash / 8 MB PSRAM), the RevC carrier, IRIS AW1005S102 and a matching, configured IRIS TCON. The TCON is a required purchased component; firmware here does not replace its internal firmware or calibration.

## Windows build

1. Clone this repository. Install Arduino CLI 1.3.1 as `.build-tools/arduino-cli.exe` and use the pinned `m5stack:esp32@3.3.8` package. Do not silently upgrade the SDK: the C5 radio adaptation depends on it.
2. From the repository root, configure the package directory (PowerShell):

```powershell
$frameRoot = (Get-Location).Path.Replace('\', '/')
New-Item -ItemType Directory -Force .build-tools | Out-Null
@"
board_manager:
  additional_urls:
    - https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
directories:
  data: '$frameRoot/.build-tools/arduino-data'
  downloads: '$frameRoot/.build-tools/downloads'
  user: '$frameRoot/.build-tools/sketchbook'
"@ | Set-Content .build-tools/arduino-setup.yaml -Encoding utf8
& ./.build-tools/arduino-cli.exe --config-file .build-tools/arduino-setup.yaml core update-index
& ./.build-tools/arduino-cli.exe --config-file .build-tools/arduino-setup.yaml core install m5stack:esp32@3.3.8
```

3. Build without flashing:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build-photo-frame.ps1
```

The wrapper selects the tested **CN / channel 149** profile and outputs `.build-photo-frame-tcon-c5-ch149/`. Outside that region, use the actual country code and an appropriate supported channel through `build-serial-bridge.ps1`; other region/phone combinations remain unvalidated:

```powershell
# Replace COUNTRY with the actual two-letter country code and choose a valid channel.
powershell -ExecutionPolicy Bypass -File tools/build-serial-bridge.ps1 -Board C5 -RevC -DisableIris -Tcon -Country COUNTRY -Channel 6
```

`-DisableIris` refers to an AirDrop Live Photo capability bit; it does **not** disable the IRIS-brand display. `-Tcon` enables the real display driver.

For backup, C5 flash offsets and first-time flashing, follow [the C5 instructions](../host/README.md#首次备份与烧录), substituting the TCON build directory above for the USB bridge directory. Use the actual serial port. The build scripts do not automatically flash a device.

## Assembly and first photo

Read [the carrier assembly instructions](https://github.com/Benv-B/photo-frame-hardware/blob/main/hardware/iris_xiao_c5_revC/README.md) and [TCON bring-up notes](iris-tcon.md) before applying power. In particular, verify the FPC mapping and observe the JP1 USB-debug versus battery configuration. USB alone does not supply the TCON in the documented debug configuration.

Start with a single small baseline JPEG, with Live Photo disabled. Keep iPhone Wi-Fi and Bluetooth enabled, open Photos → Share → AirDrop and select **Photo Frame**. Wait for the screen to finish; the phone's check mark can appear before rendering completes.

## Host checks

With Clang/Clang++ on PATH and Python 3.11+:

```powershell
python -m venv .venv
& ./.venv/Scripts/python.exe -m pip install -r host/requirements.txt
powershell -ExecutionPolicy Bypass -File tools/test-iris.ps1
powershell -ExecutionPolicy Bypass -File tools/test-bridge.ps1
```

These checks cover software behavior and mocked hardware boundaries; they do not certify real-panel behavior or phone compatibility. CI also runs the shell host suites with sanitizers.

## Receive-only benchmark

`tools/build-receive-benchmark.ps1` builds a separate experiment named **Photo Frame RX Test**. It receives and validates data in RAM; it does not initialize or refresh the display. Use the normal frame build for demonstrations. Details and measured limitations are in [iris-tcon.md](iris-tcon.md).
