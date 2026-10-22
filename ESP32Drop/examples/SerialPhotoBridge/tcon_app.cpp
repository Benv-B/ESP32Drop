#ifdef PHOTO_FRAME_TCON
#include <Arduino.h>
#include <ESP32Drop.h>
#include <ESP32DropConfig.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_rom_crc.h>
#include <Preferences.h>
#include "bridge_app.h"
#include "frame_controller.h"
#include "iris_archive.h"
#include "iris_image.h"
#include "iris_progress.h"
#include "iris_stream_worker.h"
#include "iris_refresh_probe.h"
#include "iris_tcon.h"
#include "iris_tcon_protocol.h"

#if !CONFIG_IDF_TARGET_ESP32C5 || !defined(PHOTO_FRAME_REVC)
#error "The IRIS output requires the RevC XIAO ESP32-C5 carrier"
#endif
namespace {
iris::Archive archive;
uint8_t *frame = nullptr;
AdStreamPacket *packet = nullptr;
bool ready = false, screen_ready = false;
uint32_t displayed = 0, failures = 0;
const char *state = "booting", *last_error = "";
IrisImageOptions image_options{IRIS_ROTATION_AUTO, 0, true}; // match photo, cover, original tone
IrisImageMetadata image_metadata{};
Preferences image_prefs;
bool image_prefs_ok = false, progress_led_ok = false;
uint32_t refresh_ms = 0;
uint32_t observed_file_errors = 0;
bool packet_pending = false, commit_pending = false, stream_started = false, transfer_failed = false;
AdStreamError transfer_error = AD_STREAM_ERROR_OUTPUT;
uint32_t commit_token = 0, current_transfer = 0, stream_size = 0;
uint32_t packet_file_errors = 0;
const uint8_t *stream_jpeg = nullptr;
IrisImageOptions transfer_options;
uint8_t cached_ints[4]{};

void set_state(const char *next, IrisProgress phase) {
  if (strcmp(state, next) && Serial) Serial.printf("IRIS PHASE: %s\n", next);
  state = next; iris_progress_set(phase);
}
void load_image_options() {
  image_prefs_ok = image_prefs.begin("iris-image", false);
  if (!image_prefs_ok) return;
  // Versioned key retires the previous fixed-portrait/brightening default.
  uint32_t encoded = image_prefs.getUInt("config2", 36); // AUTO, original, COVER
  if ((encoded & ~63u) || ((encoded & 7u) > 4) || (((encoded >> 3) & 3u) > 2)) return;
  image_options.rotation = (encoded & 7u) == 4 ? IRIS_ROTATION_AUTO : uint16_t((encoded & 7u) * 90);
  image_options.brightness = uint8_t((encoded >> 3) & 3u);
  image_options.crop = (encoded & 32u) != 0;
}

void report_error(const char *message) {
  last_error = message; set_state("error", IrisProgress::Error); ++failures;
  if (Serial) Serial.printf("IRIS ERROR: %s\n", message);
}
void status() {
  if (!Serial) return;
  AdStatus ad{}; if (ready) ad_stats_read(&ad);
  AwdlStatus link{}; if (ready && !frame_is_off()) awdl_status_read(&link);
  AwdlDiag radio{}; if (ready && !frame_is_off()) awdl_diag_read(&radio, false);
  uint8_t radio_channel = 0; wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
  if (ready && !frame_is_off() && esp_wifi_get_channel(&radio_channel, &secondary) != ESP_OK)
    radio_channel = 0;
  // WHY THIS IS PRINTED AT ALL: `file_err` is a COUNT, and a count cannot tell a user
  // which of the three refusals happened (over the archive cap, HEIC/multi-photo, a
  // transport failure). The library's one-line reason is the difference between "send a
  // smaller photo" and "this firmware cannot take HEIC", so it goes out to the page.
  // The struct is a local copy, so sanitising it in place is safe (it must not break the
  // JSON: this is a serial line, not a length-prefixed frame).
  for (char *p = ad.file_errmsg; *p; ++p)
    if ((unsigned char)*p < 32 || *p == '"' || *p == '\\') *p = '?';
  // The decoder task owns TCON while busy; status reads only its completed snapshot.
  if (!iris_stream_busy()) iris_tcon_status(cached_ints);
  const uint8_t *ints = cached_ints;
  const IrisStreamStatus stream = iris_stream_status();
  char power[1200]; frame_json(power, sizeof(power));
  Serial.printf("{\"firmware\":\"xiao-c5-iris-tcon-3\",\"output\":\"iris_spi\","
                "\"uptime_ms\":%lu,\"name\":\"Photo Frame\",\"transport\":\"usb_hwcdc\","
                "\"network\":{\"have_master\":%s,\"locked\":%s,\"peers\":%ld,"
                "\"mif_sent\":%lu,\"mif_age_ms\":%lu,\"tx_errors\":%lu,\"tx_refused\":%lu,"
                "\"query_seen\":%lu,\"resp_tx\":%lu,\"announce_tx\":%lu,\"announce_errors\":%lu,"
                "\"tcp_accept\":%lu,\"tls_ok\":%lu,\"discover_ok\":%lu,\"ask_ok\":%lu,\"desync\":%lu,"
                "\"syn_airdrop\":%lu,\"syn_pair1\":%lu,\"syn_pair2\":%lu,\"tcp_tous\":%lu,"
                "\"notus\":%lu,\"ka_suppress\":%lu,\"ns_to_us\":%lu,\"ch_mask\":%u,"
                "\"netif_rx\":%lu,\"netif_tx_sent\":%lu,\"netif_tx_errors\":%lu,\"txq_drop\":%lu,"
                "\"tx_done_ok\":%lu,\"tx_done_fail\":%lu,\"retry_exhausted\":%lu,\"inflight\":%u,"
                "\"rx_to_us\":%lu,\"rx_matched\":%lu,\"rx_retry\":%lu,\"ring_drop\":%lu,"
                "\"internal_free\":%lu,\"internal_largest\":%lu,\"radio_channel\":%u,"
                "\"rssi\":%d,\"rx_phy_format\":%u,\"rx_phy_rate\":%u},"
                "\"ready\":%s,\"state\":\"%s\",\"error\":\"%s\",\"displayed\":%lu,"
                "\"display_errors\":%lu,\"rx_bytes\":%lu,\"archive_bytes\":%lu,"
                "\"archive_cap\":%lu,\"upload_ok\":%lu,\"file_err\":%lu,"
                "\"file_error\":\"%s\","
                "\"ints\":[%u,%u,%u,%u],\"latch_estimate_mv\":%lu,\"psram_free\":%lu,"
                "\"receiver_flags\":%u,\"rx_active\":%s,\"progress_led\":%s,"
                "\"image\":{\"rotation\":%u,\"rotation_mode\":\"%s\",\"effective_rotation\":%u,"
                "\"fit\":\"%s\",\"brightness\":%u,\"settings_storage\":%s,"
                "\"source_width\":%lu,\"source_height\":%lu,\"exif_orientation\":%u,"
                "\"view_width\":%u,\"view_height\":%u,\"fitted_width\":%lu,\"fitted_height\":%lu,"
                "\"cropped\":%s,\"color_profile\":\"%s\",\"color_status\":\"%s\",\"color_managed\":%s,"
                "\"processing_ms\":%lu,\"active_ms\":%lu,\"input_wait_ms\":%lu,\"preview_wait_ms\":%lu,\"refresh_ms\":%lu},"
                "\"stream\":{\"busy\":%s,\"jpeg_received\":%lu,\"jpeg_total\":%lu,\"source_rows\":%lu,"
                "\"source_height\":%lu,\"previews\":%lu},\"frame\":%s}\n",
    (unsigned long)millis(), link.have_master ? "true" : "false", link.locked ? "true" : "false",
    (long)link.peers, (unsigned long)link.mif_sent, (unsigned long)link.ms_since_mif,
    (unsigned long)link.tx_errors, (unsigned long)awdl_tx_now_refused(),
    (unsigned long)ad.query_seen, (unsigned long)ad.resp_tx,
    (unsigned long)ad.announce_tx, (unsigned long)ad.announce_errors,
    (unsigned long)ad.tcp_accept, (unsigned long)ad.tls_ok,
    (unsigned long)ad.discover_ok, (unsigned long)ad.ask_ok, (unsigned long)ad.desync,
    (unsigned long)ad.syn_airdrop, (unsigned long)ad.syn_pair1,
    (unsigned long)ad.syn_pair2, (unsigned long)ad.tcp_tous,
    (unsigned long)ad.notus, (unsigned long)ad.ka_suppress,
    (unsigned long)radio.ns_to_us, unsigned(link.ch6_mask),
    (unsigned long)radio.netif_rx, (unsigned long)radio.netif_tx_sent,
    (unsigned long)radio.netif_tx_err, (unsigned long)radio.netif_txq_drop,
    (unsigned long)radio.txdone_ucast_ok, (unsigned long)radio.txdone_ucast_fail,
    (unsigned long)radio.netif_retry_exhausted, unsigned(radio.netif_inflight),
    (unsigned long)radio.rx_to_us, (unsigned long)radio.rx_to_us_matched,
    (unsigned long)radio.rx_to_us_retry, (unsigned long)radio.ring_drop,
    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
    (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
    unsigned(radio_channel), int(radio.last_rssi), unsigned(radio.rx_phy_format), unsigned(radio.rx_phy_rate),
    ready && !frame_is_off() ? "true" : "false", state, last_error,
    (unsigned long)displayed, (unsigned long)failures, (unsigned long)ad.rx_bytes,
    (unsigned long)archive.used, (unsigned long)iris::ARCHIVE_CAP,
    (unsigned long)ad.upload_ok, (unsigned long)ad.file_err, ad.file_errmsg,
    ints[0], ints[1], ints[2], ints[3], (unsigned long)iris_latch_mv(),
    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
    (unsigned)AWDL_RECEIVER_FLAGS_VALUE, ad.rx_active ? "true" : "false",
    progress_led_ok ? "true" : "false", unsigned(image_options.rotation),
    image_options.rotation == IRIS_ROTATION_AUTO ? "auto" : "fixed", unsigned(image_metadata.effective_rotation),
    image_options.crop ? "cover" : "contain", unsigned(image_options.brightness),
    image_prefs_ok ? "true" : "false", (unsigned long)image_metadata.source_width,
    (unsigned long)image_metadata.source_height, unsigned(image_metadata.exif_orientation),
    unsigned(image_metadata.view_width), unsigned(image_metadata.view_height),
    (unsigned long)image_metadata.fitted_width, (unsigned long)image_metadata.fitted_height,
    image_metadata.cropped ? "true" : "false", image_metadata.color_profile, image_metadata.color_status,
    image_metadata.color_managed ? "true" : "false",
    (unsigned long)image_metadata.elapsed_ms, (unsigned long)image_metadata.active_ms,
    (unsigned long)image_metadata.input_wait_ms, (unsigned long)image_metadata.preview_wait_ms,
    (unsigned long)refresh_ms, stream.busy ? "true" : "false",
    (unsigned long)stream.available, (unsigned long)stream.total,
    (unsigned long)stream.source_rows, (unsigned long)stream.source_height,
    (unsigned long)stream.previews, power);
}
bool show_frame() {
  set_state("refreshing", IrisProgress::Refreshing);
  const uint32_t started = millis();
  if (!screen_ready) screen_ready = iris_tcon_begin();
  if (!screen_ready || !iris_tcon_show(frame, iris::FRAME_BYTES)) {
    refresh_ms = millis() - started;
    screen_ready = false; report_error(iris_tcon_error()); return false;
  }
  refresh_ms = millis() - started;
  ++displayed; set_state("displayed", IrisProgress::Done); last_error = "";
  if (Serial) Serial.println("IRIS: M1 finished; real TCON status confirmed");
  return true;
}
void stream_failure(const char *error, AdStreamError reason = AD_STREAM_ERROR_OUTPUT) {
  iris_stream_cancel();
  if (!transfer_failed) { transfer_error = reason; report_error(error); }
  transfer_failed = true;
}
bool start_stream(const uint8_t *jpeg, uint32_t total, uint32_t available) {
  if (!iris_stream_start(jpeg, total, available, transfer_options)) {
    stream_failure("JPEG worker unavailable"); return false;
  }
  stream_jpeg = jpeg; stream_size = total; stream_started = true; return true;
}
void stream_tick() {
  IrisStreamResult result;
  if (iris_stream_take_result(result)) {
    image_metadata = result.image; refresh_ms = result.refresh_ms;
    stream_started = false;
    const bool accepted = result.ok && !transfer_failed;
    if (accepted) {
      ++displayed; last_error = ""; set_state("displayed", IrisProgress::Done);
      if (Serial) Serial.printf("IRIS: final M1 confirmed; %lu provisional M3 previews; decode active=%lu wait=%lu preview=%lu ms\n",
        (unsigned long)result.previews, (unsigned long)result.image.active_ms,
        (unsigned long)result.image.input_wait_ms, (unsigned long)result.image.preview_wait_ms);
    } else stream_failure(result.error);
    if (commit_pending) {
      if (accepted) ad_stream_ack(commit_token, true, 1);
      else ad_stream_reject(commit_token, transfer_error, last_error);
      commit_pending = false;
    }
  }
  if (iris_stream_busy() && !transfer_failed) {
    switch (iris_stream_status().phase) {
      case IrisStreamPhase::Previewing: set_state("previewing", IrisProgress::Refreshing); break;
      case IrisStreamPhase::Refreshing: set_state("refreshing", IrisProgress::Refreshing); break;
      case IrisStreamPhase::AwaitingCommit: set_state("processing", IrisProgress::Processing); break;
      case IrisStreamPhase::Receiving: set_state("receiving", IrisProgress::Receiving); break;
      default: break;
    }
  }
}
void consume() {
  if (commit_pending) return;
  if (!packet_pending) {
    if (!ad_stream_read(packet)) return;
    AdStatus arrival{}; ad_stats_read(&arrival);
    packet_file_errors = arrival.file_err;
    packet_pending = true;
  }
  bool ok = false;
  if (packet->kind == AD_STREAM_DATA) {
    const bool first = packet->offset == 0 && (!archive.active || packet->transfer_id != current_transfer);
    if (first) {
      // Never overwrite storage still borrowed by a cancelled decoder/scan.
      if (!iris_stream_idle()) { iris_stream_cancel(); return; }
      last_error = ""; image_metadata = {}; refresh_ms = 0;
      current_transfer = packet->transfer_id; transfer_options = image_options;
      // Errors from the preceding TLS request must not cancel this new JPEG.
      // Capture at packet arrival, so a timeout while waiting for old output
      // ownership is still observed as a failure of this new request.
      observed_file_errors = packet_file_errors;
      transfer_failed = stream_started = false; stream_jpeg = nullptr; stream_size = 0;
      transfer_error = AD_STREAM_ERROR_OUTPUT;
      set_state("receiving", IrisProgress::Receiving);
    }
    ok = !transfer_failed && archive.append(packet->transfer_id, packet->offset,
                                            packet->data, packet->length, esp_rom_crc32_le);
    if (!ok && !transfer_failed) stream_failure(archive.error);
    if (ok) {
      const auto prefix = archive.preview();
      using Preview = iris::ArchivePreview::State;
      if (prefix.state == Preview::Jpeg) {
        if (!stream_started) ok = start_stream(prefix.jpeg, prefix.total, prefix.available);
        else if (prefix.jpeg != stream_jpeg || prefix.total != stream_size) {
          stream_failure("JPEG prefix changed during decode"); ok = false;
        } else iris_stream_feed(prefix.available);
      } else if (prefix.state != Preview::Waiting) {
        if (prefix.rejected_size && Serial)
          Serial.printf("IRIS ARCHIVE LIMIT: declared_file=%lu offset=%lu capacity=%lu received=%lu bytes\n",
            (unsigned long)prefix.rejected_size, (unsigned long)prefix.rejected_offset,
            (unsigned long)archive.capacity, (unsigned long)archive.used);
        const AdStreamError reason = prefix.rejected_size ? AD_STREAM_ERROR_LIMIT :
          (prefix.state == Preview::Unsupported || prefix.state == Preview::Multiple)
            ? AD_STREAM_ERROR_UNSUPPORTED : AD_STREAM_ERROR_INVALID;
        stream_failure(prefix.rejected_size ? "file over 4 MiB archive cap" : prefix.error, reason);
        ok = false;
      }
    }
  } else if (packet->kind == AD_STREAM_COMMIT) {
    const uint8_t *jpeg = nullptr; uint32_t size = 0;
    ok = !transfer_failed && packet->length == 0 &&
      archive.commit(packet->transfer_id, packet->offset, packet->crc, jpeg, size);
    if (!ok) {
      if (!transfer_failed) stream_failure(archive.error[0] ? archive.error : "invalid commit packet");
      iris_stream_commit(false);
    } else {
      if (!stream_started) ok = start_stream(jpeg, size, size);
      else if (jpeg != stream_jpeg || size != stream_size) {
        stream_failure("validated JPEG differs from preview"); ok = false;
      }
      if (ok) {
        iris_stream_feed(size);
        commit_token = packet->token; commit_pending = true;
        packet_pending = false;
        iris_stream_commit(true);
        return; // ACK requires validated decode and confirmed full display coverage.
      }
    }
  } else stream_failure("unexpected archive packet");
  if (ok) ad_stream_ack(packet->token, true, 0);
  else ad_stream_reject(packet->token, transfer_error, last_error);
  packet_pending = false;
}
bool image_command(const char *line) {
  if (strncmp(line, "IMAGE ", 6)) return false;
  IrisImageOptions next = image_options;
  if (!strcmp(line, "IMAGE ROTATE AUTO")) next.rotation = IRIS_ROTATION_AUTO;
  else if (!strcmp(line, "IMAGE ROTATE 0")) next.rotation = 0;
  else if (!strcmp(line, "IMAGE ROTATE 90")) next.rotation = 90;
  else if (!strcmp(line, "IMAGE ROTATE 180")) next.rotation = 180;
  else if (!strcmp(line, "IMAGE ROTATE 270")) next.rotation = 270;
  else if (!strcmp(line, "IMAGE BRIGHT 0")) next.brightness = 0;
  else if (!strcmp(line, "IMAGE BRIGHT 1")) next.brightness = 1;
  else if (!strcmp(line, "IMAGE BRIGHT 2")) next.brightness = 2;
  else if (!strcmp(line, "IMAGE FIT COVER")) next.crop = true;
  else if (!strcmp(line, "IMAGE FIT CONTAIN")) next.crop = false;
  else {
    if (Serial) Serial.println("IRIS IMAGE: use IMAGE ROTATE AUTO|0|90|180|270, IMAGE FIT COVER|CONTAIN, IMAGE BRIGHT 0|1|2; next photo");
    return true;
  }
  if (next.rotation != image_options.rotation || next.brightness != image_options.brightness || next.crop != image_options.crop) {
    image_options = next;
    uint32_t encoded = (next.rotation == IRIS_ROTATION_AUTO ? 4u : next.rotation / 90u) |
                       (uint32_t(next.brightness) << 3) | (next.crop ? 32u : 0u);
    if (image_prefs_ok && image_prefs.putUInt("config2", encoded) != 4)
      image_prefs_ok = false;
  }
  if (Serial) Serial.printf("IRIS IMAGE: rotate=%u fit=%s bright=%u; next photo; storage=%s\n",
    unsigned(next.rotation), next.crop ? "cover" : "contain", unsigned(next.brightness),
    image_prefs_ok ? "ok" : "unavailable (RAM only)");
  status(); return true;
}
#ifdef PHOTO_FRAME_TEST_PATTERN
void fill_test_pattern() {
  const uint8_t colors[8][3] = {{255,255,255},{255,255,0},{0,255,255},{0,255,0},
                               {255,0,255},{255,0,0},{0,0,255},{0,0,0}}; // BGR
  for (uint32_t y = 0; y < iris::HEIGHT; ++y)
    for (uint32_t x = 0; x < iris::WIDTH; ++x)
      memcpy(frame + (y * iris::WIDTH + x) * 3, colors[x / 128], 3);
}
#endif
void commands() {
  static char line[48]; static size_t used = 0; static bool overflow = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line[used] = 0;
      if (!overflow) {
        if (!strcmp(line, "STATUS")) status();
        else if (image_command(line)) {}
        else if (!strncmp(line,"TEST PARTIAL ",13)) {
          AdStatus current{}; ad_stats_read(&current);
          if (!ready || frame_is_off() || !iris_stream_idle() || packet_pending ||
              commit_pending || current.rx_active || current.delivery_pending) {
            if(Serial)Serial.println("IRIS BENCH: rejected; receiver must be idle and powered");
          } else {
            const char *error="";
            if(iris_refresh_probe(line+13,frame,&error)) { last_error="";state="test_pattern"; }
            else report_error(error);
            status();
          }
        }
#ifdef PHOTO_FRAME_TEST_PATTERN
        else if (!strcmp(line, "TCON PROBE")) iris_tcon_probe();
        else if (!strcmp(line, "TCON BUS")) iris_tcon_bus_probe();
        else if (!strcmp(line, "TCON WRITE TEST") && frame && !frame_is_off()) {
          frame_command("FRAME HOLD"); fill_test_pattern(); state = "diagnostic_refresh";
          bool confirmed = iris_tcon_write_test(frame, iris::FRAME_BYTES);
          screen_ready = false; // diagnostic always isolates the bus afterward
          if (confirmed) { ++displayed; state = "displayed"; last_error = ""; }
          else report_error(iris_tcon_error());
          status();
        }
#endif
        else frame_command(line);
      }
      used = 0; overflow = false;
    } else if (c != '\r') {
      if (used + 1 < sizeof(line)) line[used++] = c; else overflow = true;
    }
  }
}
}

void bridge_setup() {
  frame_setup();
  Serial.begin(115200); Serial.setTxTimeoutMs(10); Serial.setDebugOutput(false);
  load_image_options();
  progress_led_ok = iris_progress_begin();
  iris_progress_set(IrisProgress::Waiting);
  // USB is optional diagnostics only. Battery boot never waits for a host.
  if (!psramFound() || ESP.getPsramSize() < 8u * 1024u * 1024u) {
    report_error("IRIS firmware requires 8 MiB PSRAM"); return;
  }
  frame = static_cast<uint8_t *>(heap_caps_malloc(iris::FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!frame) { report_error("framebuffer allocation failed"); return; }
  screen_ready = iris_tcon_begin();
  if (!screen_ready) { report_error(iris_tcon_error()); return; }
#ifdef PHOTO_FRAME_TEST_PATTERN
  // Separate first-light build: no radio/listener can race the test waveform.
  fill_test_pattern();
  frame_output_result(show_frame());
#else
  archive.data = static_cast<uint8_t *>(heap_caps_malloc(iris::ARCHIVE_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  archive.capacity = iris::ARCHIVE_CAP;
  packet = static_cast<AdStreamPacket *>(heap_caps_malloc(sizeof(AdStreamPacket), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!archive.data || !packet || !ad_stream_enable()) {
    report_error("archive/stream allocation failed"); return;
  }
  if (!iris_stream_init(frame)) { report_error("JPEG worker allocation failed"); return; }
  if (awdl_begin() != ESP_OK) { report_error("AWDL initialization failed"); return; }
  if (ad_begin("Photo Frame", iris::ARCHIVE_CAP) != 0) {
    awdl_end(); report_error("AirDrop initialization failed"); return;
  }
  ready = true; set_state("waiting", IrisProgress::Waiting);
#endif
  status();
}

void bridge_loop() {
  commands();
  stream_tick();
  if (ready && !frame_is_off()) consume();
  frame_tick(ready, iris_stream_busy());
  if (frame_is_off()) iris_progress_set(IrisProgress::Off);
  else if (ready) {
    AdStatus ad{}; ad_stats_read(&ad);
    if (ad.file_err != observed_file_errors) {
      observed_file_errors = ad.file_err;
      // A body/decompression failure can abort before a COMMIT reaches consume().
      // Preserve a more specific consumer error and never retain a stack pointer.
      stream_failure("AirDrop transfer failed; resend one JPEG");
      if (Serial) Serial.printf("IRIS RECEIVE ERROR: %s\n", ad.file_errmsg);
    }
    if (strcmp(state, "error") && ad.rx_active && !ad.delivery_pending &&
        (!strcmp(state, "waiting") || !strcmp(state, "receiving")))
      set_state("receiving", IrisProgress::Receiving);
    else if (!ad.rx_active && !ad.delivery_pending && !strcmp(state, "receiving"))
      set_state("waiting", IrisProgress::Waiting);
  }
  char line[200];
  // Drain even without USB, so diagnostics cannot fill a queue on battery.
  for (int i = 0; i < 8 && ad_diag_read_line(line, sizeof(line)); ++i)
    if (Serial) Serial.print(line);
  for (int i = 0; i < 4 && awdl_diag_read_line(line, sizeof(line)); ++i)
    if (Serial) Serial.print(line);
  static uint32_t last = 0;
  if (millis() - last >= 2000) { last = millis(); status(); }
  delay(frame_is_off() ? 1 : 2);
}
#endif
