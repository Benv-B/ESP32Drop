#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ESP32Drop/examples/SerialPhotoBridge/bridge_write.h"

// Model SOF flaps independently of bytes accepted by the USB FIFO.
struct Port {
  uint32_t now = 0, origin = 0, offline_until = 0, write_ms = 0;
  size_t max_write = 3;
  unsigned writes = 0, zero_writes = 0, disconnect_after = 0;
  uint32_t no_space_until = 0;
  bool flap_after_write = false, forever_offline = false;
  uint8_t output[128] = {};
  size_t output_size = 0;
  bool connected() {
    return !forever_offline && (!disconnect_after || writes < disconnect_after)
        && (uint32_t)(now - origin) >= offline_until;
  }
  int available() { return (uint32_t)(now - origin) < no_space_until ? 0 : 8; }
  size_t write(const uint8_t *data, size_t length) {
    assert(connected()); // Writing while disconnected invokes HWCDC's lossy path.
    assert(length <= (size_t)available());
    ++writes;
    if (zero_writes) { --zero_writes; return 0; }
    const size_t sent = length < max_write ? length : max_write;
    assert(output_size + sent <= sizeof(output));
    memcpy(output + output_size, data, sent); output_size += sent;
    now += write_ms;
    if (flap_after_write) offline_until = now - origin + 7;
    return sent;
  }
  uint32_t now_ms() { return now; }
  void pause() { ++now; }
};

static const uint8_t frame[] = "ESPDROP1:header/payload/CRC";
static void complete(Port &port) {
  uint64_t bytes = 0;
  assert(bridge_write_all(port, frame, sizeof(frame), bytes));
  assert(bytes == sizeof(frame));
  assert(port.output_size == sizeof(frame) && !memcmp(port.output, frame, sizeof(frame)));
}

int main() {
  { Port p; complete(p); assert(p.writes > 1); }
  { Port p; p.offline_until = 20; complete(p); }
  // The connected flag drops AFTER accepting bytes, including the last write.
  { Port p; p.flap_after_write = true; complete(p); assert(!p.connected()); }
  { Port p; p.no_space_until = 20; complete(p); }
  { Port p; p.zero_writes = 5; complete(p); }
  { Port p; p.forever_offline = true; uint64_t bytes = 0;
    assert(!bridge_write_all(p, frame, sizeof(frame), bytes));
    assert(p.now == 3001 && !bytes && !p.writes && !p.output_size); }
  { Port p; p.disconnect_after = 1; uint64_t bytes = 0;
    assert(!bridge_write_all(p, frame, sizeof(frame), bytes));
    assert(bytes == p.max_write && p.output_size == bytes);
    assert(!memcmp(p.output, frame, (size_t)bytes)); }
  // Progress resets the deadline even when a whole frame takes over 3 seconds.
  { Port p; p.write_ms = 500; complete(p); assert(p.now > 3000); }
  { Port p; p.now = p.origin = UINT32_MAX - 2; p.offline_until = 20; complete(p); }
  { Port p; p.now = p.origin = UINT32_MAX - 2; p.forever_offline = true;
    uint64_t bytes = 0;
    assert(!bridge_write_all(p, frame, sizeof(frame), bytes));
    assert((uint32_t)(p.now - p.origin) == 3001); }
  // Back-to-back frames must remain byte-exact through repeated SOF changes.
  { Port p; p.flap_after_write = true; uint64_t bytes = 0;
    for (int i = 0; i < 2; ++i) assert(bridge_write_all(p, frame, sizeof(frame), bytes));
    assert(p.output_size == 2 * sizeof(frame) && bytes == p.output_size);
    assert(!memcmp(p.output, frame, sizeof(frame)));
    assert(!memcmp(p.output + sizeof(frame), frame, sizeof(frame))); }
  { Port p; uint64_t bytes = 0;
    assert(bridge_write_all(p, nullptr, 0, bytes)); assert(!bytes && !p.writes); }
  puts("bridge_write: 12 scenarios passed");
}
