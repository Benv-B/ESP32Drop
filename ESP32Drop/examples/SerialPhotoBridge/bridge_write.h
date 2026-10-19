#pragma once
#include <stddef.h>
#include <stdint.h>

// Port supplies connected(), available(), write(), now_ms() and pause().
// Keep the current frame/offset through transient disconnects. Only lack of
// progress expires the write; the receiver's CRC and ACK establish delivery.
template <class Port>
bool bridge_write_all(Port &port, const uint8_t *data, size_t length, uint64_t &bytes) {
  uint32_t last_progress = port.now_ms(), last_yield = last_progress;
  while (length) {
    size_t sent = 0;
    if (port.connected()) {
      const int available = port.available();
      const size_t chunk = available > 0
          ? (length < (size_t)available ? length : (size_t)available) : 0;
      if (chunk) sent = port.write(data, chunk);
    }
    if (sent) {
      data += sent; length -= sent; bytes += sent;
      last_progress = port.now_ms();
    } else if ((uint32_t)(port.now_ms() - last_progress) > 3000) {
      return false;
    }
    if (!sent || (uint32_t)(port.now_ms() - last_yield) >= 4) {
      port.pause(); last_yield = port.now_ms();
    }
  }
  return true;
}
