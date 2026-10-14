# ESP32-S3 串口照片桥接

目标链路：iPhone 原生 AirDrop → ESP32-S3（8 MB PSRAM）→ UART0 / CH340 → Windows 本机浏览器。

## 硬件与固件

- 目标硬件：ESP32-S3、16 MB Flash、8 MB Octal PSRAM。
- 串口：UART0，TX=GPIO43、RX=GPIO44，经板载 CH340 连接电脑；921600 baud、8N1、无硬件流控。
- 接收设备名：`Photo Bridge`。接收固件不调用 `WiFi.begin()`，不初始化屏幕或电机引脚。
- 目录：`ESP32Drop/examples/SerialPhotoBridge/`。
- 使用作者验证过的 `m5stack:esp32` 3.3.8 核心。构建复用它的 `m5stack_stopwatch` ESP32-S3/16 MB Flash/OPI PSRAM 配置，但固件明确指定 UART0 引脚，不依赖 StopWatch 外设，也不表示你的硬件就是 StopWatch。
- 禁用开机 USB CDC，避免 `Serial` 指向原生 USB 而不是 CH340。应用输出统一采用二进制帧，不能使用普通串口监视器看照片。

## 编译

本机已经在 `.build-tools/` 安装 Arduino CLI 和核心，运行：

```powershell
cd D:\toy\photo_drop\ESP32Drop
powershell -ExecutionPolicy Bypass -File .\tools\build-serial-bridge.ps1
```

新环境：安装 Arduino CLI，配置 `directories.data/downloads/user` 到项目的 `.build-tools` 目录，并添加包索引：
`https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json`。
随后 `arduino-cli --config-file .build-tools/arduino.yaml core update-index` 和
`arduino-cli --config-file .build-tools/arduino.yaml core install m5stack:esp32@3.3.8`。

生成文件位于 `.build-serial/`。编译命令本身不会烧录。

## 烧录安全

如果板子来自 BK80 设备，先断开电机/执行机构并备份现有固件、分区及配置。不要为了测试执行全片擦除、修改 eFuse、禁用 Secure Boot 或 Flash Encryption。

使用 esptool 先读取 `get_security_info` 和 `flash_id`。本次实测的 CH340 主板使用与默认下载顺序相反的控制线：DTR 控制 EN，RTS 控制 GPIO0。无需手动按 BOOT；先关闭查看器及其他占用串口的软件，再执行：

```powershell
.\.venv\Scripts\python.exe .\tools\reset-serial-board.py --port COM3 --mode download
```

然后用 `--before no_reset` 连接。烧录时使用 `--after no_reset`，完成后执行：

```powershell
.\.venv\Scripts\python.exe .\tools\reset-serial-board.py --port COM3 --mode run
```

这个复位脚本针对本次板子的接线，不应盲目用于其他板子。其他板子自动复位无效时，才尝试按住 BOOT、按一下 RESET/EN、松开 BOOT。

仅在确认是目标板、安全启动和 Flash 加密均未启用、原固件已备份后，写入构建产物。esptool 4.x 示例（`python` 须来自装有 esptool 的环境）：

```text
python -m esptool --chip esp32s3 --port COM3 --before no_reset --after no_reset get_security_info
python -m esptool --chip esp32s3 --port COM3 --before no_reset --after no_reset flash_id
python -m esptool --chip esp32s3 --port COM3 --baud 921600 --before no_reset --after no_reset read_flash 0 0x1000000 backups/original-flash.bin
python -m esptool --chip esp32s3 --port COM3 --baud 460800 --before no_reset --after no_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0 .build-serial/SerialPhotoBridge.ino.bootloader.bin 0x8000 .build-serial/SerialPhotoBridge.ino.partitions.bin 0xe000 .build-tools/arduino-data/packages/m5stack/hardware/esp32/3.3.8/tools/partitions/boot_app0.bin 0x10000 .build-serial/SerialPhotoBridge.ino.bin
```

备份文件名应包含时间，避免覆盖上一份。`read_flash` 会运行 RAM 中的辅助程序，但不会修改 Flash。备份成功还应记录 SHA-256 和文件长度。烧录是覆盖原应用，不是将 AirDrop 自动整合进 BK80 固件；不允许普通 Wi-Fi/云通信与此 AWDL 实现同时运行。

## 电脑显示程序

本机已经创建 `.venv`。新环境可以用 Python 3.11 或更新版本：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r .\host\requirements.txt
powershell -ExecutionPolicy Bypass -File .\tools\start-photo-viewer.ps1 -Port COM3
```

打开 `http://127.0.0.1:8765`。服务只监听本机；Ctrl+C 停止。其他软件不能同时占用 COM3。

- 每张照片完成串口 CRC32 校验后，原文件保存到 `received/`，页面自动显示。
- JPEG/PNG/HEIC 等由 Pillow 和 pillow-heif 解码，网页显示 JPEG 预览，原图不会被替换。超大图片或不支持的格式仍保存原文件，只是不预览。
- 图像解码在独立线程中进行，不阻塞串口接收下一张照片。
- 文件名仅用于显示；磁盘使用生成的安全名称，避免路径穿越与覆盖。
- 板子完成 AirDrop 接收后，通过回调将 PSRAM 内的文件发到电脑，再释放 PSRAM。不会写入板子的 Flash，也不保证断电后能重发。
- 921600 baud 下 UART 理论净字节速率约 90 KiB/s，3 MB 另需约 35 秒。AirDrop 与 UART 是先后两个阶段，请耐心等待。
- CRC 是串口完整性校验，不是身份验证。此固件自动接收附近发送者的文件，仅在可信环境中开启。

## 实机测试

1. 页面显示串口连接、PSRAM 大小和开发板就绪。
2. 点击“测试板子 → 电脑显示”：应出现蓝色 `bridge_selftest.jpg`。**这只证明 UART 和页面链路，不证明 AirDrop。**
3. iPhone 打开 Wi-Fi/蓝牙并保持解锁；先选择一张小照片或截图，分享 → 隔空投送 → `Photo Bridge`。
4. 保持分享页面打开。页面里的 `have_master / locked / query_seen / tls_ok / discover_ok / ask_ok / upload_ok` 能帮助定位发现与传输阶段。
5. 等待串口传输结束，确认页面出现你的照片，并可下载原图。真正的 iPhone 兼容性必须以此实机测试为准。

默认接收上限为 **6 MiB 的解包后归档**，不是单张 JPEG 的像素大小。大照片、多张照片或 Live Photo 的整包可能超过上限；先单张测试。系统版本或无线环境可能影响 AWDL 发现，不保证每个 iPhone/iOS 版本都可用。

## 串口协议与测试

28 字节头：`ESPDROP1` + 版本(u8) + 类型(u8) + 保留(u16=0) + 序号(u32) + 载荷长度(u32) + 载荷 CRC32(u32) + 头 CRC32(u32)。整数小端，CRC32 与 Python `zlib.crc32` 一致。

类型 1=日志，2=文件，3=JSON 状态。文件载荷是 `文件名 UTF-8 字节长度(u16) + 文件名 + 原文件字节`。头 CRC 在信任载荷长度之前检查；载荷 CRC 验证失败不会保存照片。启动日志、断线残帧、无效头能重新同步。**iPhone 显示 AirDrop 完成不等于电脑已经保存，最终以电脑端 CRC 和文件落盘为准。**

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s host -p "test_*.py" -v
```

测试覆盖串口分包、3 MiB 二进制照片、头/载荷损坏、断线、文件名边界和 PC 图像/HTTP 预览。
