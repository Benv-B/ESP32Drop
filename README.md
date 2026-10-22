# AirDrop ChLCD Photo Frame

A DIY color e-paper photo frame that receives photos directly from an iPhone through **AirDrop**. No companion app, account, cloud service or Wi-Fi router is needed for the transfer.

The frame uses an **IRIS AW1005S102 ChLCD panel (1024 × 768)** and a **XIAO ESP32-C5**. ChLCD is cholesteric liquid crystal e-paper, a different display technology from E Ink Spectra 6. The ESP32 receives and processes the photo locally, then drives the matching IRIS TCON. Photos can appear progressively while they are still being received.

**Hardware and firmware are open source.** The real-screen photo path works on the prototype; transfer speed, reliability and power management are still being improved. The renderer currently supports one baseline JPEG at a time. No measured superiority over Spectra 6 is claimed.

- [Hardware: KiCad, schematic, BOM and Gerbers](https://github.com/Benv-B/photo-frame-hardware)
- [Build instructions](docs/getting-started.md) · [Development notes / 中文](docs/iris-tcon.md)
- Based on [ESP32Drop by MIRO](https://github.com/s-iwaki-d/ESP32Drop), with C5 and ChLCD frame integration added here.

Software: [0BSD](LICENSE). Carrier design: CERN-OHL-P-2.0. [Third-party credits](THIRD_PARTY_NOTICES.md). The commercial IRIS panel/TCON internals, firmware and waveforms are not part of this source release. Development and documentation were assisted by AI. This is an independent project, not affiliated with Apple or the display vendors.

## 中文

一个可以直接从 **iPhone 原生 AirDrop 传照片**的 **ChLCD 彩色电子纸相框**，无需额外 App、账号、云服务或路由器。

采用 IRIS AW1005S102 胆甾相液晶屏和 XIAO ESP32-C5，由 ESP 本地处理照片并驱动屏幕，支持边接收边分段显示。ChLCD 与 Spectra 6 的电泳显示技术不同，目前没有同条件对照，不宣称谁更好。

**固件和自制载板设计均已开源。** 样机已成功显示 AirDrop 照片，速度、稳定性和电源管理仍在完善；目前每次接收一张 baseline JPEG。开源范围不包含 IRIS 屏幕/TCON 内部固件和波形。
