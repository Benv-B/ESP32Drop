# Credits and third-party notices

- **ESP32Drop / AWDL / AirDrop implementation:** MIRO, [s-iwaki-d/ESP32Drop](https://github.com/s-iwaki-d/ESP32Drop), 0BSD. Upstream history and copyright are retained. This project adds the C5 carrier/display integration and related changes.
- **Arduino / ESP-IDF / M5Stack board package:** external dependencies installed by the developer, with their upstream licenses. These SDK packages are not redistributed in this repository.
- **ESP32-C5 ROM JPEG decoder:** the frame calls the decoder exposed by the pinned SDK/ROM. This repository does not distribute that ROM implementation.
- **IRIS panel and TCON:** separately obtained hardware and vendor firmware, screen parameters and waveforms. The project's driver is open source; vendor firmware and manuals are not relicensed or included.
- **AirDrop TLS certificate fixture:** `ESP32Drop/src/airdrop/core/airdrop_cert.h` is inherited from upstream and contains intentionally shared self-signed development certificate/key material. It is not a secret account credential or a unique device identity. The receiver uses the experimental Everyone-mode path; it does not implement Contacts Only authentication.
- **Test image:** `testdata/iris_baseline.jpg` is a 32 × 24 synthetic regression fixture without EXIF metadata. Personal photos, captured uploads, flash backups and local logs are excluded.

Project additions copyright (c) 2026 Benv-B, licensed under 0BSD. Existing notices continue to apply. AirDrop is a trademark of Apple Inc.; no vendor endorsement is implied.
