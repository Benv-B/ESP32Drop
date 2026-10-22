# XIAO ESP32-C5 RevC 相框：NFC 唤醒、AirDrop、USB 预览

> 本页记录历史 USB 预览版本。当前 `tools/build-photo-frame.ps1` 已改为真实 TCON 输出；使用方法见 [IRIS 实屏说明](iris-tcon.md)。本页下文的“当前板上固件”、构建参数与 USB 实测仅指当时记录，不能作为新固件已烧录或实屏已通过的证据。若需要复现旧版本，使用 `build-serial-bridge.ps1 -Board C5 -Channel 149 -Country CN -RevC -DisableIris`。

接收名称：**Photo Frame**。当前板上固件为 `xiao-c5-frame-noiris-2`，保留静态照片能力协商，并增加 USB 逻辑关机后的快速 NFC 检测和准确的重启原因记录。普通整合版构建为 `xiao-c5-frame-usb-2`。历史实测版本与当前版本分别记录在下文。

## 使用

1. USB 接电脑，运行仓库根目录的 `tools/start-photo-viewer.ps1`，打开 http://127.0.0.1:8765。
2. iPhone 打开 Wi-Fi、蓝牙，亮屏触碰 NFC。无需 NFC Tools。冷启动后程序直接开始接收，不依赖再次读到 RF 在场。
3. 在“照片”选择一张照片 → 分享 → 隔空投送 → **Photo Frame**。
4. 手机显示完成、电脑出现照片，原件保存在仓库 `received/`。照片不会提交进 Git。
5. 默认空闲 180 秒自动关机；成功保存后留 30 秒继续发送。实际通信期间不消耗等待时间，失败后重新留 180 秒重试。电脑页面可选择“保持唤醒”。此设置仅对本次启动有效。

USB 调试时，关机指令会停止 AirDrop 并拉高 RevC 的 PWR_OFF，但 USB 仍可给主控供电。页面显示“已请求断电”，**不能据此声称已验证整板断电或关机电流**。“恢复接收”会重启 MCU；USB 保持供电时也可以移开手机后再次触碰 NFC 重启。最终独立电池使用仍需要真正的屏幕输出或其他持久存储消费者。

NFC 不负责传图片，也不会自动替用户在照片里选图或点击 AirDrop。当前阶段 USB 代替屏幕输出，图像由电脑解码预览；未实现或验证 MCU 图片转换、SPI/TCON 和屏幕刷新。

## 构建与烧录

在仓库根目录执行：

```powershell
.\tools\test-bridge.ps1
.\tools\build-photo-frame.ps1
```

使用已固定的 `m5stack:esp32@3.3.8`，8 MB Flash / 8 MB PSRAM、原生 USB CDC、CN 地区 / 149 信道。脚本等同于 `build-serial-bridge.ps1 -Board C5 -Channel 149 -Country CN -RevC`。不要换 SDK；AWDL 使用已验证的私有 ABI。原普通桥接构建不加 `-RevC`，不会控制相框 GPIO。

产物位于 `.build-photo-frame-c5-ch149/SerialPhotoBridge.ino.bin`。当前板分区应用偏移为 `0x10000`；保留已有 NVS 和全 Flash 备份，使用项目固定版本 esptool 烧录应用。不需要 erase-flash。

### 实况照片协商试验

`tools/build-photo-frame.ps1 -DisableIris` 构建 `xiao-c5-frame-noiris-2`，输出到 `.build-photo-frame-noiris-c5-ch149/`，单独保留普通整合版。

[OpenDrop 的接收能力定义](https://github.com/seemoo-lab/opendrop/blob/master/opendrop/config.py) 将 `0x40` 命名为 `SUPPORTS_IRIS`。试验只从原 `0x3FB` 清掉此位，得到 `0x3BB`（955）；AWDL 服务 TLV 和 mDNS TXT 使用同一个值，其他能力、无线速率和文件接收流程保持原样。页面完整状态中的 `receiver_flags` 用于核对。不会在接收后偷偷删掉视频来伪装成功。

目标是让手机在发送端选择静态照片表示，不能仅依据标志名称就认定所有 iOS 会省掉视频。验收时应保持 iPhone 分享中的实况开启、关闭并重新打开 AirDrop 分享页，发送同一张实况照片，核对原件列表是否只有图片以及总传输量。2026-10-19 的试验版已完整收到一个 JPG、没有视频，仍待用户确认该次发送时实况开关保持开启；在确认前不将自动省略视频标为通过。可靠的手动方法仍是分享前关闭“实况”。

## 验证顺序

- 页面“测试板子 → 电脑显示”：真实 USB 合成 JPEG，不能代表 iPhone 投送成功。
- `tools/bench-stream.py --port COM5`：须先关闭占用串口的预览服务。7 MiB 合成包、SHA-256 比对，并主动丢弃一次 ACK 验证重传。不是无线速度测试。
- iPhone 单张小图，再测试 HEIC、多张静态图。分享实况来源时先在 iPhone 分享页关闭“实况”；当前目标是静态照片，不开发实况播放或视频显示。检查手机完成、原件数量和电脑预览，不能仅根据设备出现在 AirDrop 列表里判定成功。
- 收图期间请求关机：应继续收完并保存，通信结束和 RF 场移开后再停止服务；失败会明确报错，不应出现一半原件被标为成功。
- 关闭接收后点击恢复，再次发送；也测试 NFC 移开后再次触碰的恢复。
- USB 接着时的逻辑关机验证与拔 USB 后的电池断电验证分开记录。

## 软件保证与边界

C5 分块解包上限分两种构建：USB 桥接版为 128 MiB（逐块转发给电脑，板上不保留整包）；TCON 实屏版必须整包保留以完成 CRC、cpio 校验和 JPEG 解码，上限是 `iris_archive.h` 的 `ARCHIVE_CAP`（4 MiB，约合一张不超过 4 MiB 的 JPEG），超出返回 413，并在状态中报 `file over 4 MiB cap`；HEIC、多选和实况视频在提交阶段被拒绝。电脑先校验、落盘原件并确认，ESP 才完成 AirDrop 成功回复。断线、CRC 错误、保存失败都不能伪装成功；断线不保证自动恢复，手机报失败后需重发。合成测试文件在页面有明确标记。

关机先原子关闭新 TLS 会话入口，等待现有 TLS 会话（包含最后 HTTP 回复）及合成流任务结束、待交付文件清空，再停 AWDL。NFC 读数必须有效且连续至少 500 ms 不在场；读取失败或过期会阻止关机。PWR_OFF 只在外层主循环执行，不会在处理文件 ACK 的内层循环中切断电源。

`frame` 状态包含：`state`、`remaining_s`、`auto_off`、`nfc_ok`、`rf_present`、`rf_seen`、`i2c_errors`、`off_requested`、`previous_off_requested`、`boot`。启动、首次看到 RF、发出断电指令各记录一次 NVS；不持续写 EEPROM，不修改 NFC NDEF。

第 2 版在 USB 仍供电、已停止接收时，将 NFC 检查目标间隔从 50 ms 改为 2 ms，主循环等待改为 1 ms；正常收图期间仍为 50 ms。检测到有效的无场读数后才重新允许触碰唤醒，I²C 读取失败不会冒充移开或触碰。`wake_reason` 用 `nfc_field`、`usb_command`、`reset` 区分启动来源；原因保存在 RTC 不初始化区域，只有对应的软件重启才采用。`wake_armed`、`off_poll_count`、`off_poll_gap_max_ms` 可检查待唤醒检测是否实际运行。2 ms 是目标间隔，不是硬实时保证，也不能使手机在没有发射 NFC 场时唤醒。

调试命令：`FRAME HOLD`、`FRAME AUTO`、`FRAME OFF`、`FRAME WAKE`。这些只用于电脑端串口控制；网页只提供白名单按钮，并拒绝异源请求。

实验接收器自动接受附近发送者，不验证 Apple 身份。仅在可信环境测试；电脑服务绑定本机回环地址。

## 2026-10-19 整合版验证记录

- 固件构建、烧录校验通过，COM5 回报 `xiao-c5-frame-usb-1`、CN / 149、8 MB PSRAM、NFC I²C 正常。
- 本机应用镜像 SHA-256：`de9a754a812aed22ee017928623fbf7fe206b50c1306015ffdaf0e9a68d86b63`。
- C 核心回归、USB 短写回归、NFC 断电保护回归、新增电源状态策略及 500 次并发关机竞争测试通过；Python 38 项通过。
- 实板 7 MiB 合成 USB 流测试完成，故意丢失一次 ACK 后成功重传；SHA-256 为 `ebeb5f5d8b80b2a9180c96ce8b54d23e3c8e7af5a5a10d4162162e9e7861fb33`，41.234 秒、173.8 KiB/s。此速度不是 AirDrop 无线速度。
- 网页按钮触发合成 JPEG，电脑保存并显示；“保持唤醒”、手动关机（AWDL 停止返回 0）、恢复接收实板通过。重启后 NVS 保留上次断电指令记录。
- 整合固件运行时检测到手机 NFC 射频场，I²C 错误计数为 0。
- 真实 iPhone 静态照片 `IMG_9002.jpg` 接收成功：原件 3,726,766 字节，上传流 3,688,333 字节，约 202.831 秒；`upload_ok=1`、`files_ok=1`，电脑预览正常。此前用户取消一笔实况投送，错误计数为 1；接收器无需复位即可接受下一笔静态照片。此实测速率仍较慢，去掉实况视频不能解决无线吞吐本身的问题。
- 真实照片保存后约 31 秒自动结束接收，状态 `off_requested`、原因 `saved`、AWDL 停止返回 0。长于 180 秒的实际接收过程未被空闲计时中断。
- `xiao-c5-frame-noiris-1` 在 USB 逻辑关机状态下，移开手机后重新触碰 NFC，启动计数由 4 变为 5，恢复 AirDrop 并开始接收；期间电脑未发送恢复命令。USB 未拔除，不将逻辑关机当作整板断电测量。
- 试验版应用 SHA-256：`988e68ab2b7abe3316185801901e67b0d812ba9fb5472bd46b31698af1173122`；实板回报 `receiver_flags=955`。同一照片再次接收成功：上传流 3,688,427 字节、解包文件包 3,727,360 字节，仅含 `IMG_9002.jpg`（3,726,766 字节），没有 MOV/MP4；约 176.023 秒、`upload_ok=1`、`files_ok=1`、文件错误为 0，USB 校验与电脑预览通过。此记录不把接收后丢弃视频误算为节省无线传输。
- 两次收到的 JPG SHA-256 相同；试验版收图后也自动结束接收（`reason=saved`、AWDL 停止返回 0）。当前电脑预览服务仍运行，下一次可亮屏触碰 NFC，或点击页面“恢复接收”。

### 重复唤醒问题与第 2 版

- 用户随后报告再次亮屏触碰不能恢复；第 1 版仍在启动 #5、`off_requested`，I²C 错误为 0，但没有记录到新的 RF 场。此前一次启动计数增长不能视为重复唤醒可靠性验收。
- 第 2 版已构建并烧录，应用 SHA-256：`b2a419b4ac65b41eadb9371a7c78a95a22bc849730fabf871db54a3d69ca2003`。核心及新增短脉冲/重新允许唤醒回归通过，Python 38 项通过。
- 实板电脑恢复命令后启动 #7，正确回报 `wake_reason=usb_command`；重新关闭接收后 `wake_armed=true`，目标检测间隔 2 ms，初次观测 1757 次轮询、最大间隔 7 ms、I²C 错误 0。重复 NFC 触碰正在等待用户实测，尚未宣称修复完成。
- 第 2 版后台触碰仍被用户报告无效；随后用户使用电脑恢复接收，启动 #8 的来源确认为 `usb_command`。17:25:46 用户用 NFC Tools 主动读取，手机识别成功，同时固件记录 `rf_seen=true`、I²C 错误 0。当时固件正在接收照片，因此这只证明运行中的 NFC 检测链路能工作，不是关机后 NFC 唤醒的验收。手机继续贴在线圈上不等同于持续射频场；页面区分“本次已捕获信号”和“当前无射频场”。
- 17:28:12 收图后的自动关机完成；17:29:44 用户使用 NFC Tools 主动读取后，启动 #9 明确回报 `wake_reason=nfc_field`、`ready=true`，期间没有电脑恢复命令。USB 供电下“停止接收 → NFC 主动扫描 → 重启并恢复 AirDrop”通过。此结果不等同于 iPhone 不打开读取应用时的后台触碰可靠性通过。
- 用户提供的 NFC Tools 截图只显示 ISO 14443-3A / Type A、应用识别名称和 UID，未展示 NDEF 内容；不能仅凭该页判定标签为空或芯片型号错误。

后台使用的下一项验证可在 iPhone“快捷指令 → 自动化 → NFC”中扫描并登记该标签，动作设为“打开 App → 照片”，允许直接运行。苹果说明该触发条件匹配标签唯一标识符，忽略其他标签内容，因此此路线不必先写入网址。自动化负责打开照片，板子依然需要检测到真实 NFC 射频场；设置后仍需验证退出 NFC Tools、亮屏再次触碰能否得到 `wake_reason=nfc_field`，不能提前承诺改善后台扫描成功率。参见 [Apple NFC 触发条件](https://support.apple.com/zh-cn/guide/shortcuts/apde31e9638b/ios) 和 [允许自动运行](https://support.apple.com/zh-cn/guide/shortcuts/apd602971e63/ios)。
