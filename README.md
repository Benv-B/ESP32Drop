# AirDrop ChLCD Photo Frame

Send a photo from the iPhone share sheet to a **color ChLCD e-paper frame** using AirDrop. The frame receives, decodes and renders the photo on an ESP32-C5. No companion phone app, account, cloud service or Wi-Fi router is required for this transfer.

**Working prototype · Open-source firmware and carrier-board design · English / 中文**

- [Hardware: KiCad, schematic, BOM and Gerbers](https://github.com/Benv-B/photo-frame-hardware)
- [Build and use the C5 frame](docs/getting-started.md)
- [Detailed implementation and dated validation notes (中文)](docs/iris-tcon.md)
- [Original ESP32Drop library / S3 documentation](README.library.md)

## What makes this project interesting

1. **Native AirDrop interaction.** Select a photo on the iPhone, choose the frame, and let the frame process it locally.
2. **ChLCD color e-paper.** The prototype uses an IRIS AW1005S102 panel and its matching IRIS TCON, with a 1024 × 768 pixel image. ChLCD means cholesteric liquid crystal display. It is a different reflective display technology from E Ink Spectra 6's electrophoretic display; this project does not claim a measured image-quality or speed advantage over Spectra 6.
3. **An inspectable embedded pipeline.** C5 adaptation, JPEG layout/color handling, SPI/TCON control and progressive image previews are included, together with the custom carrier-board source.

The upstream AirDrop/AWDL implementation is [s-iwaki-d/ESP32Drop](https://github.com/s-iwaki-d/ESP32Drop), by MIRO. This repository builds on that work; it does not claim to have invented AirDrop or ChLCD.

Technology references: [IRIS on ChLCD](https://iris-opt.com/en/about/) · [E Ink on Spectra 6](https://shopkits.eink.com/en/product/detail/7.3%27%27Spectra6ePaperDisplay).

## How it works

```text
iPhone Photos → AirDrop / AWDL → XIAO ESP32-C5
             → JPEG decode / crop / color processing
             → SPI → IRIS TCON → ChLCD panel
```

The USB connection is optional for diagnostics and flashing; it is not an image-processing computer in this path. The carrier includes NFC-triggered power latching and a software power-off path. NFC is for waking the electronics, not for carrying the photo. A complete battery-only NFC wake → transfer → display → power-off cycle still needs separate validation.

## Current status — 22 October 2026

| Area | Evidence and limits |
| --- | --- |
| End-to-end display | iPhone → AirDrop → C5 → TCON → real screen has worked on the assembled prototype, with the displayed photo confirmed by the builder. |
| Progressive previews | Photos have appeared in successive regions while reception continues. A final full refresh was subsequently added to reduce seams; its visual improvement is still awaiting confirmation. |
| Phone compatibility | Tested with the builder's iPhone 15 Pro, reported iOS 26.2.2, using CN / channel 149. Other phones, OS versions and regions are not validated. |
| Photo formats | One baseline JPEG per transfer; 4 MiB decoded archive limit. HEIC, PNG, progressive JPEG and video are not supported by the frame renderer. Disable Live Photo when sharing. |
| Reliability | Discovery/refusal failures and variable transfer times remain under investigation. This is not a finished consumer product. |
| Power | Battery runtime, final shutdown current and the complete NFC wake/display/shutdown cycle are not quantified. |

**Timing measurements are not interchangeable:** one progressive-display run took about **135 seconds** end to end. A later **receive-only benchmark** received and validated about **2.74 MB in 26.9 seconds**; that benchmark does not decode or display the image. Files, firmware and radio conditions differed, so these are not a speed comparison or a guaranteed latency. The iPhone's check mark is not a reliable screen-completion indicator.

## Reproduce and contribute

Start with [getting started](docs/getting-started.md), then the [hardware assembly notes](https://github.com/Benv-B/photo-frame-hardware/blob/main/hardware/iris_xiao_c5_revC/README.md). The hardware repository contains the carrier design, not the commercial IRIS panel/TCON internals. Obtain the matched panel, TCON, screen parameters and vendor documents from the supplier.

Useful contributions include reproducible AirDrop compatibility reports, controlled transfer measurements, color comparisons and power measurements. For a report, include your board, firmware commit, phone/OS, country/channel, image format/size and the stage that failed. Remove personal photo content and device identifiers from logs before posting.

## License and credits

Firmware and project software use the [0BSD license](LICENSE), retaining the upstream MIRO notice. The carrier-board design uses CERN-OHL-P-2.0 in the linked hardware repository; third-party libraries retain their own terms. See [third-party notices](THIRD_PARTY_NOTICES.md).

Development and documentation were assisted by AI; the validation notes distinguish automated checks, device logs and observations made on the physical prototype. AirDrop is an Apple trademark. This independent project is not affiliated with or endorsed by Apple, IRIS, Seeed or E Ink.

## 中文

这是一个 **ChLCD 彩色电子纸相框**：从 iPhone 相册原生 AirDrop 投送照片，由 XIAO ESP32-C5 本地解码、缩放/裁剪、处理颜色，再通过 SPI 驱动 IRIS TCON 和 AW1005S102 屏幕。传图不需要额外 App、账号、云服务或路由器。

主打三点：**ChLCD 显示技术、原生 AirDrop 使用体验、软硬件源码开放**。ChLCD 属于胆甾相液晶显示，与 Spectra 6 的电泳显示技术不同；目前没有同条件对照数据，不宣称优于 Spectra 6。

实屏链路和边接收边分段显示已经在样机上成功；速度、偶发拒绝、条带接缝和断电流程仍在完善。26.9 秒是纯接收测速，不能当作完整换图耗时。NFC 负责唤醒，不传照片；续航和整套电池独立工作流程尚未完成验证。

软件基于 [ESP32Drop 上游](https://github.com/s-iwaki-d/ESP32Drop)，保留 0BSD 许可和作者署名。自制载板的 KiCad、原理图、BOM、Gerbers 见[硬件仓库](https://github.com/Benv-B/photo-frame-hardware)。IRIS 屏幕、TCON 固件/波形和厂商手册不属于本项目自行开源的内容。
