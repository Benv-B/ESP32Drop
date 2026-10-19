#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "../ESP32Drop/examples/RevCBringup/power_guard.h"

struct Bus {
  std::vector<int> trace;
  int error = 0, count = 1, value = 1;
  void beginTransmission(uint8_t a) { trace.push_back(a); }
  void write(uint8_t v) { trace.push_back(v); }
  int endTransmission(bool stop) { trace.push_back(stop ? 1001 : 1000); return error; }
  int requestFrom(uint8_t a, uint8_t n, bool stop) {
    trace.push_back(a); trace.push_back(n); trace.push_back(stop ? 1001 : 1000); return count;
  }
  int read() { return value; }
};

int main() {
  PowerGuard g;
  g.arm(0, 30000);
  g.sample(100, true, false);
  assert(!g.due(1000)); // not time yet
  assert(!g.due(30000)); // old RF evidence cannot authorize shutdown
  g.sample(30000, false, false);
  assert(!g.due(30000)); // I2C failure is unknown, not RF absent
  g.sample(30010, true, false);
  for (uint32_t t = 30110; t < 30509; t += 100) g.sample(t, true, false);
  g.sample(30509, true, false);
  assert(!g.due(30509));
  g.sample(30510, true, false);
  assert(g.due(30510));
  g.sample(30511, true, true);
  assert(!g.due(30511)); // phone reappears
  g.sample(31000, true, false);
  for (uint32_t t = 31100; t < 31500; t += 100) g.sample(t, true, false);
  g.sample(31500, true, false);
  assert(g.due(31500));
  g.fired = true;
  assert(!g.due(31500)); // one-shot
  g.cancel();
  assert(!g.due(31500));
  g.arm(UINT32_MAX - 300, 500);
  g.quiet = false;
  g.sample(UINT32_MAX - 300, true, false);
  for (uint32_t delta = 100; delta < 500; delta += 100)
    g.sample(uint32_t(UINT32_MAX - 300 + delta), true, false);
  g.sample(199, true, false);
  assert(g.due(199)); // millis rollover
  g.sample(5000, true, false);
  assert(!g.due(5000)); // a polling gap restarts the absence observation

  Bus bus; uint8_t value = 0xa5;
  assert(ntag_read_register(bus, 0x55, 6, value) && value == 1);
  assert((bus.trace == std::vector<int>{0x55, 0xfe, 6, 1001, 0x55, 1, 1001}));
  bus.trace.clear(); bus.error = 2; value = 0xa5;
  assert(!ntag_read_register(bus, 0x55, 6, value) && value == 0xa5);
  assert(bus.trace.size() == 4); // don't start the read after a NACK
  bus.error = 0; bus.count = 0;
  assert(!ntag_read_register(bus, 0x55, 6, value));
  bus.count = 1; bus.value = -1;
  assert(!ntag_read_register(bus, 0x55, 6, value));
  puts("RevC: RF loss/error/staleness, timer rollover, one-shot and I2C STOP protocol passed");
}
