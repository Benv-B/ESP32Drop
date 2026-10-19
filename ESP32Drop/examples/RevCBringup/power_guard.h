#pragma once
#include <stdint.h>

// A failed/stale NFC read is not evidence that the RF field has disappeared.
struct PowerGuard {
  bool pending = false, quiet = false, fired = false;
  uint32_t armed_at = 0, delay_ms = 0, quiet_at = 0, sample_at = 0;

  void arm(uint32_t now, uint32_t wait_ms) {
    pending = true; fired = false; armed_at = now; delay_ms = wait_ms;
  }
  void cancel() { pending = false; fired = false; }
  void sample(uint32_t now, bool valid, bool rf_present) {
    if (uint32_t(now - sample_at) > 150) quiet = false;
    sample_at = now;
    if (!valid || rf_present) { quiet = false; return; }
    if (!quiet) { quiet = true; quiet_at = now; }
  }
  bool due(uint32_t now) const {
    return pending && !fired && quiet &&
      uint32_t(now - armed_at) >= delay_ms &&
      uint32_t(now - quiet_at) >= 500 && uint32_t(now - sample_at) <= 150;
  }
};

// NT3H2111/2211 rev. 3.6, section 9.8: STOP between the address and read phases.
// Only the volatile session-register read protocol is implemented; no tag writes.
template<class Bus>
bool ntag_read_register(Bus &bus, uint8_t address, uint8_t reg, uint8_t &value) {
  bus.beginTransmission(address);
  bus.write(uint8_t(0xfe));
  bus.write(reg);
  if (bus.endTransmission(true) != 0) return false;
  if (bus.requestFrom(address, uint8_t(1), true) != 1) return false;
  const int data = bus.read();
  if (data < 0) return false;
  value = uint8_t(data);
  return true;
}
