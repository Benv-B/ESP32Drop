# XIAO ESP32-C5 / ESP32-S3 照片桥接

**iPhone 原生隔空投送 → Photo Bridge / XIAO → 原生 USB CDC → 电脑保存静态照片。**

当前优先维护 C5 的无线发现、静态照片接收与稳定性。USB 是现阶段输出消费者，后续可由 SPI 替换；SPI 驱动尚未实现。不开发实况播放或视频功能，分享实况来源的照片时请先在 iPhone 分享页关闭“实况”。

换电脑开发请从 [C5 交接说明](../docs/c5-handoff.md) 开始；本分支保存已实测接收静态图的清理版。

## XIAO ESP32-C5

固定 SDK 为 `m5stack:esp32@3.3.8`，板子使用 8 MB Flash、8 MB PSRAM 和原生 USB Serial/JTAG CDC。C5 是单核 RISC-V，不能烧录 S3 二进制。清理版标识为 `xiao-c5-radio-7-cleanup`。

在中国大陆使用本次验证的 CN / 信道 149 配置：

```powershell
powershell -ExecutionPolicy Bypass -File tools/build-serial-bridge.ps1 -Board C5 -Channel 149 -Country CN
```

脚本只编译。默认启用已验证的 C5 硬件 BSSID 匹配修复；`-DisableAwdlBssid` 仅供排错对照。该修复依赖固定 SDK 的私有接口，因此适配仍是实验性的。不要直接升级 SDK。

每个构建固定一个信道：6（2.4 GHz）、44 或 149（5 GHz），不支持同时多信道传输。5 GHz 当前采用 20 MHz 配置，并通过旧接口请求 OFDM 6 Mbps；这不是空口实测速率，也不代表 iPhone 上传受限于 6 Mbps。当前未实现协商 Wi-Fi 5/6 高速模式。其他地区应使用真实国家代码；默认 `01` 在本机拒绝信道 149。

构建复用 `m5stack_stamp_c5` 目标，不初始化 M5 外设或使用其 GPIO 映射。Windows 自动创建 `.build-tools/rv32` 工具链目录联接，避免头文件路径超长。结果在 `.build-serial-c5-ch149/`（其他信道对应改名）；同信道不同地区的构建会覆盖同一目录。环境安装方式见下方，C5 使用同一 Arduino SDK。

### 首次备份与烧录

关闭占用串口的查看器，确认板子没有正在接收/交付，核实实际端口。下面是本机 COM8、未启用安全启动和 Flash 加密的示例，使用 SDK 自带 esptool 5.2.0：

```powershell
$espTool = '.build-tools/arduino-data/packages/m5stack/tools/esptool_py/5.2.0/esptool.exe'
& $espTool --chip esp32c5 --port COM8 get-security-info
& $espTool --chip esp32c5 --port COM8 flash-id
New-Item -ItemType Directory -Force backups | Out-Null
& $espTool --chip esp32c5 --port COM8 read-flash 0 0x800000 "backups/c5-$(Get-Date -Format yyyyMMdd-HHmmss).bin"
if ($LASTEXITCODE -ne 0) { throw 'Backup failed' }
$c5Build = '.build-serial-c5-ch149'
& $espTool --chip esp32c5 --port COM8 --baud 460800 write-flash --flash-mode dio --flash-freq 80m --flash-size 8MB 0x2000 "$c5Build/SerialPhotoBridge.ino.bootloader.bin" 0x8000 "$c5Build/SerialPhotoBridge.ino.partitions.bin" 0xe000 .build-tools/arduino-data/packages/m5stack/hardware/esp32/3.3.8/tools/partitions/boot_app0.bin 0x10000 "$c5Build/SerialPhotoBridge.ino.bin"
if ($LASTEXITCODE -ne 0) { throw 'Flash failed' }
```

C5 bootloader 地址是 **0x2000**。已装相同 bootloader 和分区表后，日常更新只写 app：

```powershell
& $espTool --chip esp32c5 --port COM8 --baud 460800 write-flash --flash-size 8MB 0x10000 "$c5Build/SerialPhotoBridge.ino.bin"
if ($LASTEXITCODE -ne 0) { throw 'Flash failed' }
powershell -ExecutionPolicy Bypass -File tools/start-photo-viewer.ps1 -Port COM8
```

打开 <http://127.0.0.1:8765/>，核对芯片、实际信道和地区，然后在 iPhone 隔空投送中选 **Photo Bridge**。

### 接收方式与已知边界

C5 使用 256 KiB 解包记录缓冲和 8 KiB 消费块，总解包归档上限 128 MiB，支持本次 iPhone 的 chunked DvZip 上传格式。电脑逐块校验，临时存到磁盘，完成总 CRC/cpio 校验并保存后才回最终确认，板子随后向 iPhone 回复上传成功。接收中显示进度，完成后显示静态照片。S3 保留原有整包/分段预览路径，不能套用 C5 的确认时序。

清理前 `radio-7` 已在 CN/149 连续完整接收两次静态图：48.8 秒和 32.9 秒，`file_errors=0`。文件大小不同，时间包含输出等待，不是同文件速度比较或纯无线峰值。长期稳定性仍需更多实测。

故障原因、证据、回归要求及 SPI 消费者约束统一放在 **[C5 故障记录](../docs/c5-bugs.md)**。参考：[Seeed C5 文档](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/)、[Espressif Wi-Fi 驱动](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32c5/api-guides/wifi.html)、[Apple 静态方式共享实况照片](https://support.apple.com/zh-cn/104966)。

## S3 硬件与安装

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

## S3 双核流水线与可靠交付

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
| 8 STREAM_DATA | transfer_id(u32) + archive_offset(u32) + 最多 8192 字节解包归档增量 |
| 9 STREAM_COMMIT | transfer_id(u32) + archive_length(u32) + archive_crc32(u32) |

主机发送 `LIVE\nSAVE\n` 启用实时传输和保存确认，并在每个状态包后续约。引用校验并入队回复 `HAVE <sequence>\n`，保存完成回复 `SAVED <sequence>\n`；缺失/失败回复 `MISS <sequence>\n`。

S3 协商 SAVE 后等待 SAVED 最多 10 秒，失败会回退或保留重试。未协商 SAVE 的旧主机使用 HAVE（2 秒）或类型 2，**不具备落盘确认保障**。新主机兼容旧固件，但不能补传旧固件已经释放的图片。序号和传输 ID 不是跨重启的永久身份。

C5 的 STREAM_DATA 始终要求 `SAVED <sequence> 0`，表示块已接收；STREAM_COMMIT 要求 `SAVED <sequence> <files>`，表示整包校验和原文件保存完成。等待分别为 1 秒和 15 秒，最多尝试 3 次；同一传输/偏移的重复块与重复 COMMIT 可幂等确认。最终失败返回上传错误，不提前报告成功。当前暂存流不支持断电续传。

清理版状态增加 `bridge_protocol=1` 和 `archive_stream_enabled`，基准工具按能力检查，不再枚举历次固件名称。硬件匹配状态字段从历史 `bssid_filter_experiment` 改名为 `bssid_filter_enabled`。

诊断字段包括 `receive_core`、`usb_core`、`rx_elapsed_ms`、`inflate_us`、`usb_backlog`、`streamed_files`、`stream_fallbacks`、`live_enabled`、`save_ack_enabled`、`delivery_pending` 和 `delivery_retries`。inflate 耗时包含任务被抢占的时间，不等于纯 CPU 时间。

## 验证与复测

```powershell
powershell -ExecutionPolicy Bypass -File tools/test-bridge.ps1
```

需要本机 Clang/Clang++ 和安装了 `host/requirements.txt` 的 `.venv`。覆盖无线帧与窗口、HTTP、归档、去重，以及 12 个固件写入故障场景和 35 个 Python 测试。单独运行查看器测试可用 `.venv/Scripts/python.exe -m unittest discover -s host -p "test_*.py" -v`。GitHub Actions 另运行完整核心套件及新加入的去重/写入测试。

不使用手机的限速回放（只验证 PC 软件）：

```powershell
.\.venv\Scripts\python.exe host/viewer.py --replay "你的照片.png" --replay-kib 48 --http-port 8766 --output received/replay
```

测 USB 时先关闭查看器并确认板子空闲，使用清理版固件：

```powershell
.\.venv\Scripts\python.exe tools/bench-usb.py --port auto --repeat 5
.\.venv\Scripts\python.exe tools/bench-stream.py --port COM8
```

第二项为 C5 7 MiB 合成归档测试，故意丢一次 ACK，再检查完整原件 SHA-256；两项都不测无线速度。

2026-10-15 验证记录：

- 前一双核版本实板 USB 连续 5 次发送 1 MiB 合成数据，307.6–309.2 KiB/s，CRC 和逐字节比较通过。这不是无线速率，也不是相对旧版的提速倍数。
- 本版隔离测试固件在核心 0 分段产生两文件归档，经真实 USB 和生产交付代码输出。PNG 未收齐时出现 4 次不同预览；注入 4 次电脑写盘失败并丢弃一次 SAVED，最终仅保存两份正确原文件，ESP 释放归档，无文件错误。
- 实际 PNG 的本地限速回放在完成前产生 18 次不同预览，最终原文件与输入一致。
- 测试归档生成入口不包含在正式固件中。更新后的 iPhone 连续投送速度和成功率仍需实测，不能用合成测试替代。

## Spectra 6 边界

目前没有 SPI 屏幕驱动，也没有 ESP 图片解码/六色量化。电脑预览只验证数据流水线。Spectra 6 是系列名；还需具体模组、控制器、分辨率和刷新模式才能设计实际刷新。

接收、解码、准备帧缓冲可以重叠；能否局部刷新、何时启动波形及变色耗时由模组决定。不能据此承诺最后一个图片字节抵达时，墨水屏也已刷新完成。
