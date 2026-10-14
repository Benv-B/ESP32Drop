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
#if !ARDUINO_USB_MODE || !ARDUINO_USB_CDC_ON_BOOT
#error "Photo bridge requires Hardware USB CDC and CDC On Boot (USBMode=hwcdc, CDCOnBoot=cdc)"
#endif

// Matches host/protocol.py. All integers are little-endian; no packed-struct assumptions.
static const uint8_t MAGIC[8] = {'E','S','P','D','R','O','P','1'};
enum { FRAME_LOG = 1, FRAME_FILE = 2, FRAME_STATUS = 3,
       FRAME_ARCHIVE = 4, FRAME_FILE_REF = 5, FRAME_ABORT = 6, FRAME_FILE_DATA = 7 };
static const uint32_t RECEIVE_CAP = 6u * 1024u * 1024u;
static uint32_t sequence = 0, forwarded = 0;
static bool ready = false;
static const char *boot_error = "";
static bool live_host = false, live_failed = false;
static bool saved_host = false;
static uint32_t live_id = 0, live_offset = 0, live_abort = 0;
static uint32_t streamed_files = 0, stream_fallbacks = 0;
static uint32_t live_available = 0, usb_stalls = 0;
static uint64_t usb_bytes = 0;

static void put32(uint8_t *out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8 * i));
}

static bool write_all(const uint8_t *data, size_t length) {
  uint32_t last_progress = millis();
  uint32_t last_yield = last_progress;
  while (length) {
    // HWCDC otherwise reports queued/dropped data as written when disconnected.
    if (!Serial) return false;
    // Feed only available FIFO space. Large HWCDC writes can spin waiting for
    // space; bounded writes let the scheduler run instead. One task owns USB.
    const int available = Serial.availableForWrite();
    const size_t chunk = available > 0 ? min(length, (size_t)available) : 0;
    const size_t sent = chunk ? Serial.write(data, chunk) : 0;
    if (!Serial) return false;
    if (sent) { data += sent; length -= sent; usb_bytes += sent; last_progress = millis(); }
    else if ((uint32_t)(millis() - last_progress) > 3000) { ++usb_stalls; return false; }
    // Yield under backpressure or after a CPU time budget, rather than at every
    // 8 KiB boundary. Idle/watchdog still runs during a long legacy file send.
    if (!sent || (uint32_t)(millis() - last_yield) >= 4) {
      delay(1); last_yield = millis();
    }
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

// Copy under the library's short mutex, then release it BEFORE USB writes. A slow
// PC never holds up TLS reception. Unsent data remains in the existing PSRAM archive.
static bool forward_live_chunk() {
  if (!live_host || !Serial) return false;
  static uint8_t packet[8 + 8192];
  struct AdPreview part;
  ad_preview_read(live_id, live_offset, packet + 8, 8192, &part);
  live_available = part.available;
  if (part.transfer_id != live_id) {
    live_id = part.transfer_id; live_offset = 0; live_failed = false;
  }
  if (part.state == 3 && live_abort != live_id) {
    put32(packet, live_id);
    if (send_header(FRAME_ABORT, 4, esp_rom_crc32_le(0, packet, 4))) write_all(packet, 4);
    live_abort = live_id;
  }
  if (!part.length || live_failed) return false;
  put32(packet, live_id); put32(packet + 4, part.offset);
  const uint32_t size = part.length + 8;
  if (!send_header(FRAME_ARCHIVE, size, esp_rom_crc32_le(0, packet, size))
      || !write_all(packet, size)) { live_failed = true; return false; }
  live_offset = part.offset + part.length;
  return true;
}

static bool wait_file_ack(uint32_t seq, bool durable) {
  char line[32]; size_t n = 0;
  const uint32_t start = millis();
  while ((uint32_t)(millis() - start) < (durable ? 10000u : 2000u)) {
    while (Serial.available()) {
      const char c = (char)Serial.read();
      if (c == '\n') {
        line[n] = 0; n = 0;
        unsigned long value = 0;
        if (strcmp(line, "LIVE") == 0) live_host = true;
        if (strcmp(line, "SAVE") == 0) saved_host = true;
        if (sscanf(line, durable ? "SAVED %lu" : "HAVE %lu", &value) == 1 && value == seq) return true;
        if (sscanf(line, "MISS %lu", &value) == 1 && value == seq) return false;
      } else if (c != '\r' && n < sizeof(line) - 1) line[n++] = c;
    }
    delay(1);
  }
  return false;
}

static bool retry_file() {
  ad_file_retry_later();
  return false;
}

static uint32_t file_crc32(uint32_t crc, const uint8_t *data, uint32_t length) {
  uint32_t last_yield = millis();
  while (length) {
    const uint32_t chunk = length > 16384u ? 16384u : length;
    crc = esp_rom_crc32_le(crc, data, chunk);
    data += chunk; length -= chunk;
    if ((uint32_t)(millis() - last_yield) >= 2) { delay(1); last_yield = millis(); }
  }
  return crc;
}

static bool on_file(void *, const struct AdFile *file) {
  if (!file || !file->name || !file->data || file->len > RECEIVE_CAP) return false;
  // AppleDouble metadata is not a photo. Ignore it without reporting a receive failure.
  if (strncmp(file->name, "._", 2) == 0) return true;
  if (!Serial) return retry_file();
  const bool durable = saved_host;
  const size_t name_length = strnlen(file->name, 256);
  if (!name_length || name_length > 255) return false;
  const uint8_t name_size[2] = {(uint8_t)name_length, (uint8_t)(name_length >> 8)};
  if (live_host && file->transfer_id) {
    while (forward_live_chunk()) { /* drain only the final tail */ }
    if (!live_failed && live_id == file->transfer_id
        && live_offset >= file->archive_offset + file->len) {
      uint8_t ref[18 + 255];
      put32(ref, live_id); put32(ref + 4, file->archive_offset);
      put32(ref + 8, file->len); put32(ref + 12, file_crc32(0, file->data, file->len));
      memcpy(ref + 16, name_size, 2); memcpy(ref + 18, file->name, name_length);
      const uint32_t len = 18 + name_length;
      if (send_header(FRAME_FILE_REF, len, esp_rom_crc32_le(0, ref, len))
          && write_all(ref, len) && wait_file_ack(sequence, durable)) {
        ++forwarded; ++streamed_files;
        return true;
      }
    }
    ++stream_fallbacks;
  }
  // Only calculate the complete-frame CRC when it is actually needed. Successful
  // live delivery no longer scans the whole photo twice.
  uint8_t identity[8];
  put32(identity, file->transfer_id); put32(identity + 4, file->archive_offset);
  uint32_t crc = durable ? esp_rom_crc32_le(0, identity, sizeof(identity)) : 0;
  crc = esp_rom_crc32_le(crc, name_size, 2);
  crc = esp_rom_crc32_le(crc, (const uint8_t *)file->name, (uint32_t)name_length);
  crc = file_crc32(crc, file->data, file->len);
  // No diagnostics can interleave: only setup()/loop() owns this USB application's output.
  const bool ok = send_header(durable ? FRAME_FILE_DATA : FRAME_FILE,
                             (uint32_t)((durable ? 8 : 0) + 2 + name_length + file->len), crc)
      && (!durable || write_all(identity, sizeof(identity)))
      && write_all(name_size, 2)
      && write_all((const uint8_t *)file->name, name_length)
      && write_all(file->data, file->len);
  const bool accepted = ok && (durable ? wait_file_ack(sequence, true) : (Serial.flush(), (bool)Serial));
  if (accepted) ++forwarded;
  // file->data is borrowed from PSRAM. Do NOT retain it after this callback returns.
  return accepted ? true : retry_file();
}

// One MiB of deterministic bytes, without allocating a second image in PSRAM.
// Run only when idle. This measures USB delivery, not AirDrop or screen refresh.
static void usb_benchmark() {
  AdStatus status = {};
  ad_stats_read(&status);
  if (status.rx_active) { send_text(FRAME_LOG, "BENCH busy: receiving AirDrop"); return; }
  static uint8_t block[1024];
  for (size_t i = 0; i < sizeof(block); ++i) block[i] = (uint8_t)i;
  const char name[] = "bridge_usb_benchmark.bin";
  const uint8_t name_size[2] = {sizeof(name) - 1, 0};
  uint32_t crc = esp_rom_crc32_le(0, name_size, 2);
  crc = esp_rom_crc32_le(crc, (const uint8_t *)name, sizeof(name) - 1);
  for (int i = 0; i < 1024; ++i) {
    crc = esp_rom_crc32_le(crc, block, sizeof(block));
    if ((i & 31) == 31) delay(1);
  }
  bool ok = send_header(FRAME_FILE, 1048576 + 2 + sizeof(name) - 1, crc)
      && write_all(name_size, 2) && write_all((const uint8_t *)name, sizeof(name) - 1);
  for (int i = 0; ok && i < 1024; ++i) ok = write_all(block, sizeof(block));
  Serial.flush();
  send_text(FRAME_LOG, ok ? "BENCH sent 1048576 synthetic bytes; verify on host" : "BENCH USB write failed");
}

static void send_status() {
  struct AdStatus ad = {};
  struct AwdlStatus awdl = {};
  struct AwdlDiag diag = {};
  if (ready) { ad_stats_read(&ad); awdl_status_read(&awdl); awdl_diag_read(&diag, false); }
  AdPreview preview = {};
  if (ready) ad_preview_read(live_id, live_offset, nullptr, 0, &preview);
  live_available = preview.available;
  for (char *p = ad.file_errmsg; *p; ++p)
    if ((unsigned char)*p < 32 || *p == '"' || *p == '\\') *p = '?';
  char json[2800];
  const int length = snprintf(json, sizeof(json),
      "{\"ready\":%s,\"name\":\"Photo Bridge\",\"uptime_ms\":%lu,"
      "\"transport\":\"usb_hwcdc\",\"flash_bytes\":%lu,"
      "\"psram_bytes\":%lu,\"psram_free\":%lu,\"internal_free\":%lu,"
      "\"have_master\":%s,\"locked\":%s,\"peers\":%ld,\"mif_sent\":%lu,"
      "\"query_seen\":%lu,\"tcp_accept\":%lu,\"tls_ok\":%lu,\"discover_ok\":%lu,"
      "\"ask_ok\":%lu,\"upload_ok\":%lu,\"rx_active\":%s,\"rx_bytes\":%lu,"
      "\"rx_max\":%lu,\"forwarded\":%lu,\"file_errors\":%lu,\"boot_error\":\"%s\","
      "\"tls_alg\":\"%s\",\"internal_largest\":%lu,\"resp_tx\":%lu,"
      "\"notus\":%lu,\"ka_suppress\":%lu,\"syn_airdrop\":%lu,\"tcp_tous\":%lu,"
      "\"tx_now_errors\":%lu,\"tx_now_last_err\":%ld,\"mif_tx_errors\":%lu,"
      "\"firmware\":\"xiao-dual-2\",\"file_error\":\"%s\",\"files_ok\":%lu,"
      "\"ch6_mask\":%u,\"win_state\":%u,\"rssi\":%d,\"ring_drop\":%lu,"
      "\"netif_rx\":%lu,\"netif_tx_sent\":%lu,\"netif_tx_errors\":%lu,"
      "\"txq_drop\":%lu,\"txq_depth_max\":%u,\"tx_done_ok\":%lu,\"tx_done_fail\":%lu,"
      "\"retry_queued\":%lu,\"retry_drop\":%lu,\"retry_exhausted\":%lu,"
      "\"inflight\":%u,\"inflight_max\":%u,\"streamed_files\":%lu,\"stream_fallbacks\":%lu,"
      "\"cpu_mhz\":%u,\"receive_core\":%d,\"usb_core\":%d,\"receive_stack_free\":%lu,"
      "\"rx_elapsed_ms\":%lu,\"inflate_us\":%lu,\"inflate_max_us\":%lu,"
      "\"archive_available\":%lu,\"usb_archive_sent\":%lu,\"usb_backlog\":%lu,"
      "\"usb_bytes\":%llu,\"usb_stalls\":%lu,\"ad_log_dropped\":%lu,"
      "\"live_enabled\":%s,\"live_failed\":%s,\"preview_state\":%u,\"transfer_id\":%lu,"
      "\"save_ack_enabled\":%s,\"delivery_pending\":%s,\"delivery_retries\":%lu}",
      ready ? "true" : "false", (unsigned long)millis(),
      (unsigned long)ESP.getFlashChipSize(), (unsigned long)ESP.getPsramSize(),
      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      awdl.have_master ? "true" : "false", awdl.locked ? "true" : "false", (long)awdl.peers,
      (unsigned long)awdl.mif_sent, (unsigned long)ad.query_seen,
      (unsigned long)ad.tcp_accept, (unsigned long)ad.tls_ok, (unsigned long)ad.discover_ok,
      (unsigned long)ad.ask_ok, (unsigned long)ad.upload_ok, ad.rx_active ? "true" : "false",
      (unsigned long)ad.rx_bytes, (unsigned long)ad.rx_max, (unsigned long)forwarded,
      (unsigned long)ad.file_err, boot_error, ad.tls_alg ? ad.tls_alg : "none",
      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned long)ad.resp_tx, (unsigned long)ad.notus, (unsigned long)ad.ka_suppress,
      (unsigned long)ad.syn_airdrop, (unsigned long)ad.tcp_tous,
      (unsigned long)awdl_tx_now_errors(), (long)awdl_tx_now_last_err(),
      (unsigned long)awdl.tx_errors, ad.file_errmsg, (unsigned long)ad.files_ok,
      (unsigned)awdl.ch6_mask, (unsigned)awdl.win_state, (int)diag.last_rssi,
      (unsigned long)diag.ring_drop, (unsigned long)diag.netif_rx,
      (unsigned long)diag.netif_tx_sent, (unsigned long)diag.netif_tx_err,
      (unsigned long)diag.netif_txq_drop, (unsigned)diag.txq_depth_max,
      (unsigned long)diag.txdone_ucast_ok, (unsigned long)diag.txdone_ucast_fail,
      (unsigned long)diag.netif_retry_queued, (unsigned long)diag.netif_retry_drop,
      (unsigned long)diag.netif_retry_exhausted,
      (unsigned)diag.netif_inflight, (unsigned)diag.netif_inflight_max,
      (unsigned long)streamed_files, (unsigned long)stream_fallbacks,
      (unsigned)getCpuFrequencyMhz(), ad.listener_core, xPortGetCoreID(),
      (unsigned long)ad_listen_stack_min_free(), (unsigned long)ad.rx_elapsed_ms,
      (unsigned long)ad.decode_us, (unsigned long)ad.decode_max_us,
      (unsigned long)live_available, (unsigned long)live_offset,
      (unsigned long)(live_available > live_offset ? live_available - live_offset : 0),
      (unsigned long long)usb_bytes, (unsigned long)usb_stalls, (unsigned long)ad_diag_dropped(),
      live_host ? "true" : "false", live_failed ? "true" : "false",
      (unsigned)preview.state, (unsigned long)preview.transfer_id,
      saved_host ? "true" : "false", ad.delivery_pending ? "true" : "false",
      (unsigned long)ad.delivery_retries);
  if (length > 0 && (size_t)length < sizeof(json)) send_text(FRAME_STATUS, json);
  else send_text(FRAME_LOG, "Status JSON buffer too small");
}

void bridge_setup() {
  // XIAO USB-C uses the S3 hardware CDC on GPIO19/20, not UART0 GPIO43/44.
  // Baud is a host API placeholder; native USB throughput is not UART baud-limited.
  Serial.setRxBufferSize(256);
  Serial.setTxBufferSize(8192);
  Serial.setTxTimeoutMs(250);
  Serial.begin(PHOTO_BRIDGE_BAUD);
  Serial.setDebugOutput(false);
  esp_log_level_set("*", ESP_LOG_NONE);  // unframed logs must not corrupt binary photo frames
  delay(300);
  // Do not wait for a PC: AirDrop starts even if the viewer is not open yet.
  send_text(FRAME_LOG, "SerialPhotoBridge boot; XIAO ESP32S3 native USB CDC");
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
  if (!ad_preview_enable()) send_text(FRAME_LOG, "Live preview unavailable; using complete files");
  if (ad_begin(PHOTO_BRIDGE_NAME, RECEIVE_CAP) != 0) {
    boot_error = "AirDrop listener initialization failed";
    send_text(FRAME_LOG, boot_error); send_status(); return;
  }
  ready = true;
  send_text(FRAME_LOG, "Ready. On iPhone: Photos -> Share -> AirDrop -> Photo Bridge");
  send_status();
}

static void read_commands() {
  // A deterministic local test proves USB/display only, never AirDrop compatibility.
  static char command[16];
  static size_t used = 0;
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\n') {
      command[used] = 0;
      if (strcmp(command, "LIVE") == 0) live_host = true;
      if (strcmp(command, "SAVE") == 0) saved_host = true;
      if (strcmp(command, "BENCH") == 0) usb_benchmark();
      if (strcmp(command, "TEST") == 0) {
        struct AdFile file = {};
        file.name = "bridge_selftest.jpg";
        file.data = SELFTEST_JPEG; file.len = sizeof(SELFTEST_JPEG);
        send_text(FRAME_LOG, "Synthetic USB CDC test; not an iPhone photo");
        on_file(nullptr, &file);
      }
      used = 0;
    } else if (c != '\r') {
      if (used < sizeof(command) - 1) command[used++] = c;
      else used = 0;
    }
  }
}

void bridge_loop() {
  if (!Serial) live_host = false;
  // Once negotiated, keep the save requirement through transient HWCDC dips.
  // Losing the USB connection must never downgrade a pending file to fire-and-forget.
  // Negotiate before polling a completed archive or sending another chunk.
  read_commands();
  bool forwarded_live = false;
  for (int i = 0; i < 4 && forward_live_chunk(); ++i) forwarded_live = true;
  if (ready) ad_poll();
  char line[200];
  int budget = 8;
  while (ready && Serial && budget-- > 0 && ad_diag_read_line(line, sizeof(line)))
    send_text(FRAME_LOG, line);
  budget = 4;
  while (ready && Serial && budget-- > 0 && awdl_diag_read_line(line, sizeof(line)))
    send_text(FRAME_LOG, line);
  static uint32_t last_status = 0;
  if ((uint32_t)(millis() - last_status) >= 2000) {
    last_status = millis(); send_status();
  }
  // Drain accumulated records promptly; keep idle polling inexpensive.
  delay(forwarded_live ? 1 : 10);
}
