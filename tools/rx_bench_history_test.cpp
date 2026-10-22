#include <cassert>
#include <cstdio>
#include <cstring>
#include "../ESP32Drop/examples/SerialPhotoBridge/rx_bench_history.h"

int main() {
  RxBenchHistory h;
  h.init();
  assert(h.oldest() == 0 && h.read(0) == nullptr);
  h.push("EV 123 discover\r\n");
  assert(!strcmp(h.read(0), "EV 123 discover"));
  const uint64_t dump_end = h.written;
  for (unsigned i = 0; i < 200; ++i) {
    char line[50];
    snprintf(line, sizeof line, "EV %u event", i);
    h.push(line);
  }
  assert(h.written == 201 && h.oldest() == 73);
  assert(h.read(dump_end - 1) == nullptr); // A dump never reads a recycled slot as old data.
  assert(!strcmp(h.read(73), "EV 72 event"));
  assert(!strcmp(h.read(200), "EV 199 event"));
  assert(h.read(201) == nullptr);
  char large[300];
  memset(large, 'x', sizeof large - 1); large[sizeof large - 1] = 0;
  h.push(large);
  assert(strlen(h.read(201)) == RxBenchHistory::LINE_SIZE - 1);
  h.init();
  assert(h.written == 0 && h.read(0) == nullptr);
  puts("rx_bench_history: retention, wrap, stale reads, truncation and reset passed");
}
