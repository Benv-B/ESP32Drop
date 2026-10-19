#pragma once
#include <stdint.h>

// USB may keep the MCU alive after OFF. A fresh field must follow a confirmed
// clear sample; a field already present at shutdown must not cause a reboot loop.
struct FrameNfcWake {
  static constexpr uint32_t POLL_MS = 2;
  bool armed = false;
  bool sample(bool valid, bool field) {
    if (!valid) return false;
    if (!field) { armed = true; return false; }
    return armed;
  }
};

// Pure timing policy. Hardware/NVS/USB are kept outside so boundary cases can be
// tested with a fake clock. All durations are below half the uint32_t range.
struct FramePolicy {
  static constexpr uint32_t IDLE_MS = 180000, SAVED_MS = 30000;
  uint32_t activity = 0, window = IDLE_MS, sample_at = 0, quiet_at = 0;
  bool hold = false, manual = false, off = false, busy = false;
  bool valid = false, rf = false, quiet = false, sampled = false;
  bool saved = false, retry = false;

  void begin(uint32_t now) { activity = now; }
  void renew(uint32_t now) {
    activity = now; window = IDLE_MS; manual = saved = retry = false;
  }
  void result(uint32_t now, bool success) {
    activity = now; saved = success; retry = !success;
    window = success ? SAVED_MS : IDLE_MS;
  }
  void sample(uint32_t now, bool ok, bool present) {
    if (!sampled || now - sample_at > 150) quiet = false;
    sampled = true; sample_at = now; valid = ok; rf = ok && present;
    if (!ok || present) { quiet = false; return; }
    if (!quiet) { quiet = true; quiet_at = now; }
  }
  void update(uint32_t now, bool working) {
    // Busy time never consumes the user's retry/next-photo window.
    if (working || busy) activity = now;
    busy = working;
  }
  bool pending(uint32_t now) const {
    return !off && (manual || (!hold && now - activity >= window));
  }
  bool due(uint32_t now) const {
    return pending(now) && !busy && valid && quiet &&
      now - sample_at <= 150 && now - quiet_at >= 500;
  }
  uint32_t remaining(uint32_t now) const {
    if (off || manual || hold) return 0;
    uint32_t elapsed = now - activity;
    return elapsed >= window ? 0 : (window - elapsed + 999) / 1000;
  }
  const char *reason() const { return manual ? "manual" : saved ? "saved" : "idle"; }
};
