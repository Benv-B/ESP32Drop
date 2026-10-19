#pragma once
#include <atomic>
#include <stdint.h>

// A shutdown closes admission atomically with observing the last owner leave.
// Owners cover TLS (including the final HTTP reply) and diagnostic stream jobs.
class AdPowerGate {
  static constexpr uint32_t CLOSED = 0x80000000u;
  std::atomic<uint32_t> owners{0};
public:
  bool enter() {
    uint32_t value = owners.load();
    while (!(value & CLOSED)) {
      if (owners.compare_exchange_weak(value, value + 1)) return true;
    }
    return false;
  }
  void leave() { owners.fetch_sub(1); }
  bool close() {
    uint32_t empty = 0;
    return owners.compare_exchange_strong(empty, CLOSED) || empty == CLOSED;
  }
  bool closed() const { return owners.load() == CLOSED; }
  bool busy() const { uint32_t value = owners.load(); return value && value != CLOSED; }
};
