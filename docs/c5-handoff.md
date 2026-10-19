# C5 换电脑开发交接

本分支 `feat/xiao-esp32c5-awdl` 保存 Wi-Fi 6 实验前的清理版源码，固件标识 `xiao-c5-radio-7-cleanup`。适用 XIAO ESP32-C5，8 MB Flash / 8 MB PSRAM，中国大陆 CN / 信道 149，当前目标是原生 AirDrop 接收静态照片。USB 是现阶段消费者，SPI 尚未实现。

## 已验证到哪里

- 清理版实板完整接收一张静态图：上传 2,209,812 字节，保存 JPG 2,216,989 字节，68.687 秒，`upload_ok=1`、`files_ok=1`、`file_errors=0`。时间包含输出等待。更多连续传输和冷启动发现仍需复测。
- 清理前的 radio-7 有两次连续静态照片成功记录，不能把它写成清理版的连续验收。详细故障、数据和回归要求见 [C5 故障记录](c5-bugs.md)。
- 本分支包含 C5 硬件 BSSID 匹配、MIF/PSF 同步、完整读取 Ask 请求、流式归档交付、USB 短写恢复、去重及对应测试。
- 本分支未启用 HE20。后续 Wi-Fi 6 实验仍有发现失败，不能称为更快或稳定；当前实验源码留在原电脑的 `feat/xiao-c5-he20-experiment` 工作区，没有混入此次基线推送。

## 在另一台 Windows 电脑开始

```powershell
git clone --branch feat/xiao-esp32c5-awdl git@github.com:Benv-B/ESP32Drop.git
cd ESP32Drop
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r host/requirements.txt
```

已克隆仓库时，先处理当地未提交修改，再 `git fetch origin` 并切换 `feat/xiao-esp32c5-awdl`。

按 [环境安装说明](../host/README.md#s3-硬件与安装) 安装 Arduino CLI 1.3.1，并把 `.build-tools/arduino.yaml` 中的绝对路径改为新电脑的仓库路径。C5 和 S3 使用同一套 **`m5stack:esp32@3.3.8`** SDK，C5 构建复用 `m5stack_stamp_c5` 目标。固定版本依赖不能直接升级：硬件 ACK 修复用到了该版本的私有接口。

```powershell
.\.build-tools\arduino-cli.exe --config-file .build-tools/arduino.yaml core update-index
.\.build-tools\arduino-cli.exe --config-file .build-tools/arduino.yaml core install m5stack:esp32@3.3.8
powershell -ExecutionPolicy Bypass -File tools/build-serial-bridge.ps1 -Board C5 -Channel 149 -Country CN
.\.venv\Scripts\python.exe -m serial.tools.list_ports -v
```

构建结果在 `.build-serial-c5-ch149/`。烧录步骤见 [C5 首次备份与烧录](../host/README.md#首次备份与烧录)，把示例 COM8 改为新电脑实际端口。C5 bootloader 地址是 `0x2000`，app 地址是 `0x10000`；不要用 S3 镜像。原板最后刷入的是 HE20 实验固件，拉取本分支不会自动替换板上固件。

烧录后运行查看器，默认自动查找唯一的设备：

```powershell
powershell -ExecutionPolicy Bypass -File tools/start-photo-viewer.ps1
```

访问 `http://127.0.0.1:8765/`，核对固件名、CN/149 和 ready 状态，再用 iPhone 连续单独投送同一张静态图。每次等手机显示“已发送”，同时核对电脑原图和错误计数。照片分享页关闭实况。

装有 Clang/Clang++ 后可运行本地回归：

```powershell
powershell -ExecutionPolicy Bypass -File tools/test-bridge.ps1
```

源码和依赖清单在 Git 中；工具链、虚拟环境、构建输出、私人照片、串口日志和设备备份均不上传，需要在新电脑重新安装或生成。原清理版 app 的 SHA-256 为 `31812f6f660e9846b954530adc2f9963a3ebfa28e4508ce943c4978fe405d3ea`，用于识别原电脑保留的固件，不保证换路径重编译后的二进制逐字节一致。
