#pragma once
#include <airdrop/port/ad_port_esp32.h>
#include "iris_archive.h"

// Diagnostic contract: validate receipt of one JPEG archive in RAM, then discard.
// No JPEG decoding, display, persistent storage or USB payload forwarding.
struct RxBenchConsumer {
  iris::Archive archive;
  uint32_t jpeg_bytes = 0, jpeg_crc32 = 0, validated = 0;
  const char *error = "";
  bool consume(const AdStreamPacket &p, iris::Archive::Crc crc) {
    error = "";
    if (p.kind == AD_STREAM_DATA) {
      if (!p.length || p.length > sizeof(p.data)) { archive.active=false; error = "invalid benchmark packet length"; return false; }
      if (!p.offset) { jpeg_bytes = 0; jpeg_crc32 = 0; }
      if (archive.append(p.transfer_id,p.offset,p.data,p.length,crc)) return true;
      error = archive.error; return false;
    }
    if (p.kind != AD_STREAM_COMMIT || p.length) { archive.active=false; error = "invalid benchmark commit"; return false; }
    const uint8_t *jpeg = nullptr;
    if (!archive.commit(p.transfer_id,p.offset,p.crc,jpeg,jpeg_bytes)) {
      error = archive.error; return false;
    }
    jpeg_crc32 = crc(0, jpeg, jpeg_bytes);
    ++validated;
    return true;
  }
};
