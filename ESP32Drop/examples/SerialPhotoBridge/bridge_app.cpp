#include <Arduino.h>
#include <ESP32AWDL.h>
#include <ESP32Drop.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "bridge_app.h"
#include "selftest_jpeg.h"

#ifndef PHOTO_BRIDGE_BAUD
#define PHOTO_BRIDGE_BAUD 921600
#endif
#ifndef PHOTO_BRIDGE_NAME
#define PHOTO_BRIDGE_NAME "Photo Bridge"
#endif
#ifndef PHOTO_BRIDGE_RX_PIN
#define PHOTO_BRIDGE_RX_PIN 44
#endif
#ifndef PHOTO_BRIDGE_TX_PIN
#define PHOTO_BRIDGE_TX_PIN 43
#endif

// Matches host/protocol.py. All integers are little-endian; no packed-struct assumptions.
static const uint8_t MAGIC[8] = {'E','S','P','D','R','O','P','1'};
enum { FRAME_LOG = 1, FRAME_FILE = 2, FRAME_STATUS = 3 };
static const uint32_t RECEIVE_CAP = 6u * 1024u * 1024u;
static uint32_t sequence = 0, forwarded = 0;
static bool ready = false;
static const char *boot_error = "";

static void put32(uint8_t *out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8 * i));
}

static bool write_all(const uint8_t *data, size_t length) {
  uint32_t last_progress = millis();
  while (length) {
    const size_t chunk = length > 2048 ? 2048 : length;
    const size_t sent = Serial0.write(data, chunk);
    if (sent) { data += sent; length -= sent; last_progress = millis(); }
    else if ((uint32_t)(millis() - last_progress) > 3000) return false;
    // AWDL owns high-priority tasks; never monopolise its core while forwarding a photo.
    delay(1);
  }
  return true;
}

static bool send_header(uint8_t kind, uint32_t length, uint32_t crc) {
  uint8_t header[28] = {};
  memcpy(header, MAGIC, sizeof(MAGIC));
  header[8] = 1; header[9] = kind;
  put32(header + 12, ++sequence);
  put32(header + 16, length);
  put32(header + 20, crc);
  put32(header + 24, esp_rom_crc32_le(0, header, 24));
  return write_all(header, sizeof(header));
}

static void send_text(uint8_t kind, const char *text) {
  const uint32_t length = (uint32_t)strlen(text);
  const uint8_t *bytes = (const uint8_t *)text;
  if (send_header(kind, length, esp_rom_crc32_le(0, bytes, length)))
    write_all(bytes, length);
}

static bool on_file(void *, const struct AdFile *file) {
  if (!file || !file->name || !file->data || file->len > RECEIVE_CAP) return false;
  // AppleDouble metadata is not a photo. Ignore it without reporting a receive failure.
  if (strncmp(file->name, "._", 2) == 0) return true;
  const size_t name_length = strnlen(file->name, 256);
  if (!name_length || name_length > 255) return false;
  const uint8_t name_size[2] = {(uint8_t)name_length, (uint8_t)(name_length >> 8)};
  uint32_t crc = esp_rom_crc32_le(0, name_size, 2);
  crc = esp_rom_crc32_le(crc, (const uint8_t *)file->name, (uint32_t)name_length);
  for (uint32_t pos = 0; pos < file->len;) {
    uint32_t chunk = file->len - pos;
    if (chunk > 16384) chunk = 16384;
    crc = esp_rom_crc32_le(crc, file->data + pos, chunk);
    pos += chunk;
    delay(1);
  }
  // No diagnostics can interleave: only setup()/loop() owns this UART's application output.
  const bool ok = send_header(FRAME_FILE, (uint32_t)(2 + name_length + file->len), crc)
      && write_all(name_size, 2)
      && write_all((const uint8_t *)file->name, name_length)
      && write_all(file->data, file->len);
  Serial0.flush();
  if (ok) ++forwarded;
  // file->data is borrowed from PSRAM. Do NOT retain it after this callback returns.
  return ok;
}

static void send_status() {
  struct AdStatus ad = {};
  struct AwdlStatus awdl = {};
  if (ready) { ad_stats_read(&ad); awdl_status_read(&awdl); }
  char json[640];
  snprintf(json, sizeof(json),
      "{\"ready\":%s,\"name\":\"Photo Bridge\",\"uptime_ms\":%lu,"
      "\"psram_bytes\":%lu,\"psram_free\":%lu,\"internal_free\":%lu,"
      "\"have_master\":%s,\"locked\":%s,\"peers\":%ld,\"mif_sent\":%lu,"
      "\"query_seen\":%lu,\"tcp_accept\":%lu,\"tls_ok\":%lu,\"discover_ok\":%lu,"
      "\"ask_ok\":%lu,\"upload_ok\":%lu,\"rx_active\":%s,\"rx_bytes\":%lu,"
      "\"rx_max\":%lu,\"forwarded\":%lu,\"file_errors\":%lu,\"boot_error\":\"%s\"}",
      ready ? "true" : "false", (unsigned long)millis(),
      (unsigned long)ESP.getPsramSize(),
      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      awdl.have_master ? "true" : "false", awdl.locked ? "true" : "false", (long)awdl.peers,
      (unsigned long)awdl.mif_sent, (unsigned long)ad.query_seen,
      (unsigned long)ad.tcp_accept, (unsigned long)ad.tls_ok, (unsigned long)ad.discover_ok,
      (unsigned long)ad.ask_ok, (unsigned long)ad.upload_ok, ad.rx_active ? "true" : "false",
      (unsigned long)ad.rx_bytes, (unsigned long)ad.rx_max, (unsigned long)forwarded,
      (unsigned long)ad.file_err, boot_error);
  send_text(FRAME_STATUS, json);
}

void bridge_setup() {
  // CH340 is attached to UART0, not the S3's native USB CDC port.
  Serial0.setRxBufferSize(256);
  Serial0.setTxBufferSize(2048);
  Serial0.begin(PHOTO_BRIDGE_BAUD, SERIAL_8N1, PHOTO_BRIDGE_RX_PIN, PHOTO_BRIDGE_TX_PIN);
  esp_log_level_set("*", ESP_LOG_NONE);  // unframed logs must not corrupt binary photo frames
  delay(300);
  send_text(FRAME_LOG, "SerialPhotoBridge boot; UART0 RX44/TX43, 921600 baud");
  if (!psramFound() || ESP.getPsramSize() < 4u * 1024u * 1024u) {
    boot_error = "PSRAM missing or too small";
    send_text(FRAME_LOG, boot_error); send_status(); return;
  }
  const esp_err_t link = awdl_begin();
  if (link != ESP_OK) {
    boot_error = "AWDL initialization failed";
    send_text(FRAME_LOG, esp_err_to_name(link)); send_status(); return;
  }
  ad_on_file(on_file, nullptr);
  if (ad_begin(PHOTO_BRIDGE_NAME, RECEIVE_CAP) != 0) {
    boot_error = "AirDrop listener initialization failed";
    send_text(FRAME_LOG, boot_error); send_status(); return;
  }
  ready = true;
  send_text(FRAME_LOG, "Ready. On iPhone: Photos -> Share -> AirDrop -> Photo Bridge");
  send_status();
}

void bridge_loop() {
  if (ready) ad_poll();
  char line[200];
  int budget = 8;
  while (ready && budget-- > 0 && ad_diag_read_line(line, sizeof(line)))
    send_text(FRAME_LOG, line);
  // A deterministic local test proves UART/display only, never AirDrop compatibility.
  static char command[16];
  static size_t used = 0;
  while (Serial0.available()) {
    const char c = (char)Serial0.read();
    if (c == '\n') {
      command[used] = 0;
      if (strcmp(command, "TEST") == 0) {
        struct AdFile file = {};
        file.name = "bridge_selftest.jpg";
        file.data = SELFTEST_JPEG; file.len = sizeof(SELFTEST_JPEG);
        send_text(FRAME_LOG, "Synthetic UART test; not an iPhone photo");
        on_file(nullptr, &file);
      }
      used = 0;
    } else if (c != '\r') {
      if (used < sizeof(command) - 1) command[used++] = c;
      else used = 0;
    }
  }
  static uint32_t last_status = 0;
  if ((uint32_t)(millis() - last_status) >= 2000) {
    last_status = millis(); send_status();
  }
  delay(10);
}
