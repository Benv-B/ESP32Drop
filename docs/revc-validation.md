# RevC 无屏幕验证

后续整合版已把 NFC、电源状态和 AirDrop 接到同一固件，见 [Photo Frame 使用说明](photo-frame.md)。本文保留早期独立测试程序的操作记录。

使用 `feat/xiao-esp32c5-awdl` 分支。开发目录为 `D:/toy/photo_drop/ESP32Drop`，`D:/toy/photo_frame/ESP32Drop` 也已同步到该分支；运行命令前确认当前目录和分支。

## 两个测试程序

- `SerialPhotoBridge`：现有 C5 AirDrop、流式归档、USB 保存和电脑图片预览。
- `RevCBringup`：新增的载板测试程序，测试 NFC I²C、射频场检测、电源关机引脚、ADC 和一段 PSRAM 读写。没有 AirDrop，不发送任何 SPI。

独立测试便于定位。后续整合版已有收图与关机状态管理，但仍没有脱离 USB 的照片消费者、ESP 图像解码/1024×768 BGR888 转换。电脑正确显示照片，不能证明这些缺失步骤已经通过。

## 软件回归和构建

```powershell
powershell -ExecutionPolicy Bypass -File tools/test-bridge.ps1
powershell -ExecutionPolicy Bypass -File tools/build-revc-bringup.ps1
powershell -ExecutionPolicy Bypass -File tools/build-serial-bridge.ps1 -Board C5 -Channel 149 -Country CN
```

这三条命令不会烧录。Bringup 输出在 `.build-revc-bringup/`，C5 桥接输出在 `.build-serial-c5-ch149/`。
首次烧录前，按 `host/README.md` 备份 Flash 并确认芯片、Flash 大小和分区表。不要用 S3 镜像。
烧录 Bringup 后，桥接网页不能解析其文本协议，改用下面的控制台。

## 1. USB 通信和内存

USB 接好，J1 外电暂时关闭，JP1 可以保持插上（已核对的 XIAO C5 V1.1 电源方案）。

```powershell
.\.venv\Scripts\python.exe tools/revc-console.py --port auto --seconds 8 --command DISARM STATUS LAST
```

期望 `flash_bytes=8388608`、`psram_bytes=8388608`、`psram_test_ok=true`、`nvs_ok=true`。
PSRAM 测试只写读申请到的 256 KiB，两种数据模式；不是整片存储器压力认证。
`latch_estimate_mv` 是 GPIO5 测到的 EN_LATCH 分压估算，不是直接电池电压，也不是 5V_SYS。

## 2. NFC 通信与手机场检测

默认 NT3H2111 地址为 0x55，SDA=GPIO23，SCL=GPIO24。正常应有 `nfc_ok=true`。
若失败，先检查 U3 焊接、3.3V、SDA/SCL；地址曾被改过时需核对实际地址。

运行 30 秒采集；期间用 iPhone 的 NFC 读取功能主动扫描，并将手机顶部靠近线圈、随后移开：

```powershell
.\.venv\Scripts\python.exe tools/revc-console.py --seconds 30 --command STATUS
```

期望捕获 `rf_present=false → true → false`，手机能识别标签。
程序每 50ms 读 FEh 的 NS_REG（偏移6，bit0是RF_FIELD_PRESENT），采用手册规定的 STOP/START。
只读会话寄存器，不写标签 EEPROM、密码、锁定位或 NDEF。读 NS_REG 会清除 NDEF_DATA_READ，这是芯片定义的读取副作用。
手机扫描场可能短促，单次漏采不等于线圈失败；可重复几次。
本板 U3.FD 未连接，真正开机由 VOUT/RF_EH 采能触发锁存，不是 GPIO 中断唤醒。

## 3. 外电、锁存和软件关机

外电设 4.5V、限流先设0.5A，接 J1 BAT+/BAT-，不接屏幕。
开外电，按一下 SW1，万用表检查5V_SYS约5.3V，松开后仍保持。

```powershell
.\.venv\Scripts\python.exe tools/revc-console.py --seconds 8 --command PWROFF
```

程序等手机场消失至少500ms后拉高GPIO1。万用表应看到5V_SYS降到接近0V。
USB仍插着时XIAO仍有3.3V是正常的，`off_requested=true`只证明发出指令，不证明电源真的断开。
`DISARM`可撤销计时并将GPIO1恢复为低，然后再次使用SW1。

## 4. 真正的 NFC 冷启动 → 自动断电

```powershell
.\.venv\Scripts\python.exe tools/revc-console.py --seconds 5 --command ARM30
```

ARM30将“每次启动30秒后关机”的测试模式写入XIAO自己的NVS，直到DISARM取消。
它不等价于正式相框的传图/刷新完成后关机。

1. 关闭外接电源、拔USB；JP1插上。移开手机，等待电压消失。
2. 打开4.5V外电，先不按SW1。关机待机时XIAO的3.3V应接近0V。
3. iPhone主动扫描并靠近线圈，3.3V升至约3.3V；移开手机后仍保持。
4. 等约30秒，3.3V应降至接近0V。只有连续有效I²C读取确认场消失>=500ms才关机。
5. 重复3次。RF一直存在或I²C通信失败时程序会等待，不假装成功关机。
6. 再接USB，执行 `DISARM LAST` 查看前次启动的RF记录和关机请求。接USB会产生新启动，LAST是前一次；中间多次重启会覆盖。

记录只证明程序观察到的事件和关机意图，不能区分SW1与NFC启动，也不能证明实际电压。判断开关成功用万用表。
断电后屏幕能保留图片，但本次没有屏幕，不验证刷新电流和波形。

## 5. AirDrop 接收和异常恢复

恢复C5桥接固件，外电可关闭，只用USB。启动 `tools/start-photo-viewer.ps1`，核对固件、CN/149、ready和内存。

- 同一张静态照片连续单独投送至少5次；每次同时核对iPhone“已发送”、电脑原图、files_ok和file_errors。
- 换横图、竖图、较大照片；HEIC若由电脑显示，仍不是ESP解码通过。
- 取消一次发送后再发，确认恢复；重启后再次发现并发送，确认冷启动。
- 停止查看器、板子空闲后运行 `tools/bench-stream.py --port 实际端口`，验证7MiB合成流、丢ACK恢复和SHA-256。这不是无线测速。
- 手机不靠近时的超时关机、收图中禁止关机、图片转换后的帧内容，需要后续正式相框状态机实现后再测；当前不标记通过。

源依据：本项目RevC `design_manifest.json` / `README.md`；[NXP NT3H2111/2211数据手册](https://www.nxp.com/docs/en/data-sheet/NT3H2111_2211.pdf)，§8.3.12、§8.6、§9.8。
