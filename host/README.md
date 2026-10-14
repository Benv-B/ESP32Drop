# XIAO ESP32-S3 USB 照片桥接

**iPhone 原生隔空投送 → Photo Bridge / XIAO ESP32-S3 → 原生 USB CDC → 电脑保存原图、显示预览。**

当前固件为 `xiao-dual-2`。手机显示“已发送”表示它向 ESP 的传输已经结束；USB 接收和电脑写盘仍可能继续。以网页显示“图片已校验并保存”为最终交付结果。

## 硬件与安装

- XIAO ESP32S3 普通版：8 MB Flash、8 MB Octal PSRAM，配套外置天线和 USB 数据线。
- 固定 SDK：`m5stack:esp32@3.3.8`。复用 `m5stack_stopwatch` 编译目标，不初始化 StopWatch 外设，不使用其外设引脚映射。
- 原生 Hardware USB CDC / USB Serial-JTAG：GPIO19/20，VID:PID `303A:1001`。不是 UART 或 TinyUSB；`921600` 是串口 API 的占位参数，不限制原生 USB 速率。
- AWDL 独占无线接口，不调用 `WiFi.begin()`，ESP 无需加入手机所在的路由器。
- 接收上限为 **6 MiB 解包后归档**，包含同次投送的照片及附件。照片不写入 ESP Flash。

将 Arduino CLI 1.3.1 安装到 `.build-tools/arduino-cli.exe`，创建 `.build-tools/arduino.yaml`，把以下绝对路径替换为自己的项目目录：

```yaml
board_manager:
  additional_urls:
    - https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
directories:
  data: D:/toys/photo_frame/ESP32Drop/.build-tools/arduino-data
  downloads: D:/toys/photo_frame/ESP32Drop/.build-tools/arduino-downloads
  user: D:/toys/photo_frame/ESP32Drop/.build-tools/arduino-user
```

在仓库根目录运行（Python 3.11+，本机使用 3.12）：

```powershell
.\.build-tools\arduino-cli.exe --config-file .build-tools/arduino.yaml core update-index
.\.build-tools\arduino-cli.exe --config-file .build-tools/arduino.yaml core install m5stack:esp32@3.3.8
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r host/requirements.txt esptool==4.11.0
powershell -ExecutionPolicy Bypass -File tools/build-serial-bridge.ps1
```

脚本只编译，不烧录。参数：`PSRAM=opi,FlashMode=qio,PartitionScheme=default_8MB,USBMode=hwcdc,CDCOnBoot=cdc,UploadMode=default,DebugLevel=none`，另指定 `build.flash_size=8MB` 覆盖 StopWatch 菜单的 16/32 MB 选项。输出位于 `.build-serial/`。

## 备份与烧录

关闭占用串口的查看器，确认端口和设备。本机为 COM7，其他电脑应按实际结果替换。

```powershell
.\.venv\Scripts\python.exe -m serial.tools.list_ports -v
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM7 get_security_info
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM7 flash_id
New-Item -ItemType Directory -Force backups | Out-Null
$backup = "backups/xiao-$(Get-Date -Format yyyyMMdd-HHmmss).bin"
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM7 --baud 460800 read_flash 0 0x800000 $backup
if ($LASTEXITCODE -ne 0) { throw "Backup failed" }
(Get-Item $backup).Length # 8388608
Get-FileHash $backup -Algorithm SHA256
```

以下用于已确认未启用安全启动/Flash 加密的 XIAO。首次安装写入匹配的 bootloader、8 MB 分区表及应用，不做全片擦除：

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM7 --baud 460800 --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0 .build-serial/SerialPhotoBridge.ino.bootloader.bin 0x8000 .build-serial/SerialPhotoBridge.ino.partitions.bin 0xe000 .build-tools/arduino-data/packages/m5stack/hardware/esp32/3.3.8/tools/partitions/boot_app0.bin 0x10000 .build-serial/SerialPhotoBridge.ino.bin
if ($LASTEXITCODE -ne 0) { throw "Flash failed" }
```

已安装相同 bootloader 和分区表时，可只更新 app0：

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM7 --baud 460800 --after hard_reset write_flash --flash_size 8MB 0x10000 .build-serial/SerialPhotoBridge.ino.bin
```

SDK 的 QIO bootloader 使用 DIO 镜像头启动是正常配置。`default_8MB` 的最后一个分区结束于 `0x800000`。不使用旧 CH340 板专用的 `reset-serial-board.py`。XIAO 自动连接失败时，按住 BOOT、点按 RESET，再松开 BOOT 并重新检查端口。

## 启动与使用

```powershell
powershell -ExecutionPolicy Bypass -File tools/start-photo-viewer.ps1
```

打开 <http://127.0.0.1:8765/>。默认寻找唯一的 `303A:1001` 设备，通过 USB 序列号跟踪重连后的端口。多个候选时用 `-Port COM7` 指定。服务只监听本机，前台 Ctrl+C 结束；同一串口只能由一个程序打开。

1. 网页确认 USB 连接、8 MiB PSRAM 和固件就绪。
2. “测试板子 → 电脑显示”只验证蓝色合成图的 USB 保存/显示，不代表 iPhone 投送成功。
3. iPhone 开启 Wi-Fi、蓝牙并保持解锁，从照片 → 分享 → 隔空投送选择 **Photo Bridge**。
4. 网页分别显示无线接收、USB 转发、校验保存。JPEG / PNG 可逐步显示临时画面；HEIC 等格式等待完整文件。
5. 原图校验后保存到 `received/`，预览独立生成，不修改原图。不支持预览的文件仍可下载。

相册索引保存在进程内，重启查看器不会重新列出历史照片，但磁盘原图保留。备份、照片、日志、环境和构建目录均被 Git 忽略。

## 双核流水线与可靠交付

- TLS / HTTP / DvZip 解包在核心 0、优先级 2；核心 1 保留 AWDL 收包/发送时序和 USB 应用任务。系统 Wi-Fi/lwIP 优先级更高。CPU 为 240 MHz，没有超频。
- 接收任务发布 PSRAM 中不可变的归档前缀；USB 任务在短互斥锁内复制至多 8 KiB，解锁后输出。USB 等待不会持有接收锁，也不在 ESP 额外复制一份完整图片。
- DvZip 通常每完成一个约 128 KiB 记录才提供新数据；gzip 等非流式路径需等完整解包，并非每个无线包都会更新画面。
- 电脑在独立线程中最多约每 600 ms 解码一次临时预览、缩放到 960 像素。等待队列只保留最新快照，不阻塞 USB 接收。黑块、灰块、缺行是临时结果。
- 每个状态包续约 `LIVE` 和 `SAVE`，恢复短暂 USB 状态变化后失效的实时模式。已协商的保存确认要求保留到 ESP 重启，不因短暂断连降级。
- 完整归档校验后发送文件引用及原图 CRC，电脑已有全部分段时不重复传图。缺块、CRC 错误时回退完整文件。坏帧/短写后可重新同步；部分帧有总时限，状态消息不会无限延长等待。
- 新主机先写临时文件、flush / fsync、改名为原图，再发 `SAVED`。预览失败不丢原图。固件收到确认后才释放归档。
- USB、写盘或确认失败时，ESP 保留归档并重试。已接受的文件不再回调；主机按传输标识和内容摘要去重，确认丢失不重复保存。
- 等待电脑恢复时仅保留一份归档，新上传返回 503。PSRAM 断电即失，尚不支持断电续传。主机重启/重连后去重会话改变，确认丢失仍可能产生重复原图。

这是独立的 AirDrop 接收实现，会自动接受附近发送者，不验证 Apple 身份。CRC 验证完整性，不提供身份认证。发现和成功率仍受外置天线、干扰、距离及 iOS 行为影响。

## USB 协议

28 字节头：`ESPDROP1` + version(u8=1) + kind(u8) + reserved(u16=0) + sequence(u32) + payload_length(u32) + payload_crc32(u32) + header_crc32(u32)。整数小端；CRC 与 Python `zlib.crc32` 一致。头 CRC 覆盖前 24 字节，先校验再信任长度。

| 类型 | 载荷 |
| --- | --- |
| 1 LOG | UTF-8 日志 |
| 2 FILE | name_length(u16) + name + data（旧版完整文件 / BENCH） |
| 3 STATUS | JSON 状态 |
| 4 ARCHIVE | transfer_id(u32) + archive_offset(u32) + 最多 8192 字节归档增量 |
| 5 FILE_REF | transfer_id(u32) + file_offset(u32) + file_length(u32) + file_crc32(u32) + name_length(u16) + name |
| 6 ABORT | transfer_id(u32)，丢弃相应临时预览 |
| 7 FILE_DATA | transfer_id(u32) + archive_offset(u32) + 类型 2 的载荷 |

主机发送 `LIVE\nSAVE\n` 启用实时传输和保存确认，并在每个状态包后续约。引用校验并入队回复 `HAVE <sequence>\n`，保存完成回复 `SAVED <sequence>\n`；缺失/失败回复 `MISS <sequence>\n`。

协商 SAVE 的固件等待 SAVED 最多 10 秒，失败会回退或保留重试。未协商 SAVE 的旧主机使用 HAVE（2 秒）或类型 2，**不具备落盘确认保障**。新主机兼容旧固件，但不能补传旧固件已经释放的图片。序号和传输 ID 不是跨重启的永久身份。

诊断字段包括 `receive_core`、`usb_core`、`rx_elapsed_ms`、`inflate_us`、`usb_backlog`、`streamed_files`、`stream_fallbacks`、`live_enabled`、`save_ack_enabled`、`delivery_pending` 和 `delivery_retries`。inflate 耗时包含任务被抢占的时间，不等于纯 CPU 时间。

## 验证与复测

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s host -p "test_*.py" -v
```

覆盖 USB 分包/CRC/短写恢复、截断帧超时、归档校验、JPEG/PNG 临时预览、严格最终解码、模式续约、写盘失败、确认丢失去重及 HTTP 下载。GitHub Actions 运行 Windows Python 测试和 macOS C 核心测试。

不使用手机的限速回放（只验证 PC 软件）：

```powershell
.\.venv\Scripts\python.exe host/viewer.py --replay "你的照片.png" --replay-kib 48 --http-port 8766 --output received/replay
```

测 USB 时先关闭查看器：

```powershell
.\.venv\Scripts\python.exe tools/bench-usb.py --port auto --repeat 5
```

2026-10-15 验证记录：

- 前一双核版本实板 USB 连续 5 次发送 1 MiB 合成数据，307.6–309.2 KiB/s，CRC 和逐字节比较通过。这不是无线速率，也不是相对旧版的提速倍数。
- 本版隔离测试固件在核心 0 分段产生两文件归档，经真实 USB 和生产交付代码输出。PNG 未收齐时出现 4 次不同预览；注入 4 次电脑写盘失败并丢弃一次 SAVED，最终仅保存两份正确原文件，ESP 释放归档，无文件错误。
- 实际 PNG 的本地限速回放在完成前产生 18 次不同预览，最终原文件与输入一致。
- 测试归档生成入口不包含在正式固件中。更新后的 iPhone 连续投送速度和成功率仍需实测，不能用合成测试替代。

## Spectra 6 边界

目前没有 SPI 屏幕驱动，也没有 ESP 图片解码/六色量化。电脑预览只验证数据流水线。Spectra 6 是系列名；还需具体模组、控制器、分辨率和刷新模式才能设计实际刷新。

接收、解码、准备帧缓冲可以重叠；能否局部刷新、何时启动波形及变色耗时由模组决定。不能据此承诺最后一个图片字节抵达时，墨水屏也已刷新完成。
