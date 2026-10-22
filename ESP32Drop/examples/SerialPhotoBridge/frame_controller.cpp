#include "frame_controller.h"
#include <stdio.h>
#ifdef PHOTO_FRAME_REVC
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <ESP32Drop.h>
#include <ESP32AWDL.h>
#include <esp_system.h>
#include "frame_policy.h"
#ifdef PHOTO_FRAME_TCON
#include "iris_tcon.h"
#define FRAME_OUTPUT "iris_spi"
#else
#define FRAME_OUTPUT "usb_preview"
#endif
#if !CONFIG_IDF_TARGET_ESP32C5
#error "Photo Frame RevC requires XIAO ESP32-C5"
#endif

namespace {
constexpr int PWR_OFF = 1, BUS_EN = 7, CS_N = 0;
constexpr uint8_t NFC_ADDR = 0x55;
constexpr uint32_t MAGIC = 0x50465231;
struct Record { uint32_t magic, boot, rf_seen, off_requested, off_at; };
Record current{}, previous{};
Preferences prefs;
FramePolicy policy;
FrameNfcWake wake;
// Preserve the explicit software restart cause across ESP.restart(), so a USB
// reset is never reported as a successful NFC wake based only on a boot counter.
RTC_NOINIT_ATTR uint32_t pending_wake;
constexpr uint32_t WAKE_NFC = 0x46525701, WAKE_USB = 0x46525702;
// The radio would not come down during shutdown, so the MCU was restarted instead of
// being left able to advertise while unable to receive. Reported on the next boot.
constexpr uint32_t WAKE_STOPFAIL = 0x46525703;
const char *wake_reason = "reset";
uint32_t restart_cause = 0;
bool nvs_ok = false, wire_ok = false, restart_requested = false;
bool rf_seen = false;
bool stopping = false;
bool radio_stopped = false;
uint32_t stopping_at = 0, radio_stop_tries = 0;
uint8_t ns_reg = 0;
uint32_t errors = 0, last_poll = 0, uploads = 0, failures = 0;
uint32_t off_poll_count = 0, off_poll_gap_max = 0;
int radio_stop_error = 0;
const char *off_reason = "";
// Admission cannot be reopened once closed (AdPowerGate is one-way), so a shutdown that
// has begun must end in "radio stopped" or in a restart. These two bound the ways it can
// stall, and they exist because the alternative state is the worst one this device has:
// listener parked, MIFs and mDNS still on the air -- listed in AirDrop, refusing every
// transfer, with no way out short of a power cycle.
constexpr uint32_t STOP_FORCE_MS = 2000;     // stop deferring to a lingering NFC field
constexpr uint32_t RADIO_STOP_TRIES = 3;     // awdl_end() attempts before a restart
void save() {
  if (nvs_ok && prefs.putBytes("record", &current, sizeof(current)) != sizeof(current)) nvs_ok = false;
}
// The one way out of a shutdown that cannot finish. Not a noreturn function on purpose:
// ESP.restart() is not declared noreturn, so every caller returns afterwards and the
// compiler stays honest about reachability.
void stop_failure_restart(const char *why) {
  if (Serial) Serial.printf("IRIS SHUTDOWN: %s; restarting\n", why);
  pending_wake = WAKE_STOPFAIL;
  delay(10);
  ESP.restart();
}
bool read_nfc() {
  if (!wire_ok) return false;
  // NTAG I2C session register: STOP is required before the read transaction.
  Wire.beginTransmission(NFC_ADDR); Wire.write(0xfe); Wire.write(6);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom(NFC_ADDR, uint8_t(1), true) != 1) return false;
  int value = Wire.read();
  if (value < 0) return false;
  ns_reg = uint8_t(value); return true;
}
}

void frame_setup() {
  if (esp_reset_reason() == ESP_RST_SW) {
    if (pending_wake == WAKE_NFC) wake_reason = "nfc_field";
    if (pending_wake == WAKE_USB) wake_reason = "usb_command";
    if (pending_wake == WAKE_STOPFAIL) wake_reason = "stop_failed";
  }
  pending_wake = 0;
  // Raw RevC GPIOs, deliberately independent of the M5Stamp build aliases.
  pinMode(PWR_OFF, OUTPUT); digitalWrite(PWR_OFF, LOW);
  pinMode(BUS_EN, OUTPUT); digitalWrite(BUS_EN, LOW);
  pinMode(CS_N, OUTPUT); digitalWrite(CS_N, HIGH);
  policy.begin(millis());
  nvs_ok = prefs.begin("photoframe", false);
  if (nvs_ok && prefs.getBytesLength("record") == sizeof(previous))
    prefs.getBytes("record", &previous, sizeof(previous));
  if (previous.magic != MAGIC) previous = {};
  current = {MAGIC, previous.boot + 1, 0, 0, 0}; save();
  wire_ok = Wire.begin(23, 24, 100000); Wire.setTimeOut(25);
}

bool frame_command(const char *command) {
  const uint32_t now = millis();
  // Commands only set intent. In particular OFF cannot cut a stream while its
  // SAVED acknowledgment is being processed by the bridge's inner read loop.
  if (!strcmp(command, "FRAME OFF")) { policy.manual = true; return true; }
  if (!strcmp(command, "FRAME HOLD")) {
    if (!stopping && !policy.off) { policy.hold = true; policy.manual = false; }
    return true;
  }
  if (!strcmp(command, "FRAME AUTO")) {
    if (!stopping && !policy.off) { policy.hold = false; policy.renew(now); }
    return true;
  }
  if (!strcmp(command, "FRAME WAKE")) {
    if (policy.off) { restart_requested = true; restart_cause = WAKE_USB; }
    else if (!stopping) policy.renew(now);
    return true;
  }
  return false;
}

void frame_output_result(bool success) { policy.result(millis(), success); }

void frame_tick(bool receiver_ready, bool output_busy) {
  uint32_t now = millis();
  const uint32_t poll_ms = policy.off ? FrameNfcWake::POLL_MS : 50;
  if (now - last_poll >= poll_ms) {
    if (policy.off) {
      if (off_poll_count && now - last_poll > off_poll_gap_max) off_poll_gap_max = now - last_poll;
      ++off_poll_count;
    }
    last_poll = now;
    bool ok = read_nfc(); now = millis();
    if (!ok) ++errors;
    bool field = ok && (ns_reg & 1);
    bool rising = field && (!policy.valid || !policy.rf);
    policy.sample(now, ok, field);
    if (field && !rf_seen) { rf_seen = true; current.rf_seen = 1; save(); }
    if (!policy.off && !stopping && !policy.manual && rising) policy.renew(now);
    if (policy.off && wake.sample(ok, field) && !restart_requested) {
      restart_requested = true; restart_cause = WAKE_NFC;
    }
  }
  if (policy.off) {
    // USB can keep the MCU alive after the battery latch is cleared. Rebooting
    // brings up the radio cleanly; it does not claim the battery latch is on.
    if (restart_requested) { pending_wake = restart_cause; delay(10); ESP.restart(); }
    return;
  }
  AdStatus ad{};
  if (receiver_ready) ad_stats_read(&ad);
  if (ad.upload_ok != uploads) { uploads = ad.upload_ok; policy.result(now, true); }
  if (ad.file_err != failures) { failures = ad.file_err; policy.result(now, false); }
  policy.update(now, output_busy || (receiver_ready &&
    (ad_shutdown_busy() || ad.rx_active || ad.delivery_pending)));
  // A cancelled upload can leave a physical preview waveform in flight.
  // Keep power and SPI ownership until the display worker has finished it.
  if (output_busy) return;
  if (!stopping && !policy.due(now)) return;
  // The library closes admission atomically and parks the listener before the
  // AWDL netif is destroyed. A new TLS request racing this tick wins or waits.
  //
  // FROM HERE ON THE ONLY ACCEPTABLE OUTCOMES ARE "radio stopped" AND "restarted":
  // admission is one-way, so a shutdown that gives up leaves the device advertising
  // (MIF + mDNS are the link layer's, not the listener's) while every transfer is
  // refused -- and nothing short of a power cycle gets out of it.
  if (!stopping) {
    if (receiver_ready && !ad_shutdown_if_idle()) return;
    stopping = true; stopping_at = now;
  }
  const bool forced = (uint32_t)(now - stopping_at) >= STOP_FORCE_MS;
  if (receiver_ready && !ad_shutdown_quiesced()) {
    // The listener has not parked yet. Destroying the netif under a live TLS session can
    // assert inside lwIP, so the forced path restarts rather than tearing it down; the
    // unforced path keeps the contract and waits, as it always did.
    if (!forced) return;
    stop_failure_restart("listener did not park");
    return;
  }
  // A field that arrives while the listener parks may postpone the stop -- the phone is
  // on the coil and the user is about to send -- but only for STOP_FORCE_MS. Past that,
  // refusing to stop IS the failure: the badge looks alive and takes nothing until the
  // field goes away, which on a phone left on the reader can be minutes.
  if (!forced && (!policy.valid || !policy.quiet ||
                  now - policy.sample_at > 150 || now - policy.quiet_at < 500)) return;
  if (!radio_stopped) {
    radio_stop_error = awdl_end();
    if (radio_stop_error == ESP_OK || !receiver_ready) {
      // ESP_OK: stopped. !receiver_ready: nothing was ever running (a boot that failed
      // before ad_begin() leaves no netif, so awdl_end() answers ESP_ERR_INVALID_STATE) --
      // that is the old behaviour for a failed boot and must not become a restart loop.
      radio_stopped = true;
    } else if (++radio_stop_tries >= RADIO_STOP_TRIES) {
      // The link would not come down (awdl_end() leaves it RUNNING when a task does not
      // acknowledge; see ESP32AWDL.h). Cutting power over that, with USB attached, is
      // the zombie state. A restart is the only defined way back.
      stop_failure_restart("awdl_end refused three times");
      return;
    } else {
      return;                       // one more attempt on a later tick
    }
  }
  off_reason = policy.reason();
  current.off_requested = 1; current.off_at = millis(); save();
#ifdef PHOTO_FRAME_TCON
  iris_tcon_sleep();
#endif
  policy.off = true;
  wake = {}; off_poll_count = off_poll_gap_max = 0;
  digitalWrite(BUS_EN, LOW); digitalWrite(PWR_OFF, HIGH);
}

bool frame_is_off() { return policy.off; }
void frame_json(char *out, size_t capacity) {
  uint32_t now = millis();
  const char *state = policy.off ? "off_requested" : stopping ? "stopping" :
    policy.busy ? "receiving" : policy.pending(now) ? "waiting_to_off" :
    policy.retry ? "retry" : policy.saved ? "saved" : "waiting";
  snprintf(out, capacity,
    "{\"enabled\":true,\"state\":\"%s\",\"output\":\"" FRAME_OUTPUT "\","
    "\"auto_off\":%s,\"remaining_s\":%lu,\"off_requested\":%s,\"reason\":\"%s\","
    "\"nfc_ok\":%s,\"rf_present\":%s,\"rf_seen\":%s,\"ns_reg\":%u,\"i2c_errors\":%lu,"
    "\"boot\":%lu,\"previous_off_requested\":%s,\"previous_rf_seen\":%s,"
    "\"nvs_ok\":%s,\"usb_serial_open\":%s,\"radio_stop_error\":%d,"
    "\"stopping_ms\":%lu,\"stop_attempts\":%lu,"
    "\"wake_reason\":\"%s\",\"reset_reason\":%u,\"wake_armed\":%s,"
    "\"nfc_poll_ms\":%lu,\"off_poll_count\":%lu,\"off_poll_gap_max_ms\":%lu}",
    state, policy.hold ? "false" : "true", (unsigned long)policy.remaining(now),
    policy.off ? "true" : "false", policy.off ? off_reason : policy.pending(now) ? policy.reason() : "",
    policy.valid ? "true" : "false", policy.valid ? (policy.rf ? "true" : "false") : "null",
    rf_seen ? "true" : "false", ns_reg, (unsigned long)errors, (unsigned long)current.boot,
    previous.off_requested ? "true" : "false", previous.rf_seen ? "true" : "false",
    nvs_ok ? "true" : "false", Serial ? "true" : "false", radio_stop_error,
    (unsigned long)(stopping ? (uint32_t)(now - stopping_at) : 0u),
    (unsigned long)radio_stop_tries,
    wake_reason, (unsigned)esp_reset_reason(), wake.armed ? "true" : "false",
    (unsigned long)(policy.off ? FrameNfcWake::POLL_MS : 50),
    (unsigned long)off_poll_count, (unsigned long)off_poll_gap_max);
}
#else
void frame_setup() {}
void frame_output_result(bool) {}
void frame_tick(bool, bool) {}
bool frame_command(const char *) { return false; }
bool frame_is_off() { return false; }
void frame_json(char *out, size_t capacity) { snprintf(out, capacity, "{\"enabled\":false}"); }
#endif
