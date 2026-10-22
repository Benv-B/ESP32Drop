#pragma once
#include <stddef.h>
#include <stdint.h>

// Owned only by bridge_loop. Allocate the payload in PSRAM, outside radio tasks.
struct RxBenchHistory {
  static constexpr size_t CAPACITY = 128;
  static constexpr size_t LINE_SIZE = 200;
  uint64_t written;
  char lines[CAPACITY][LINE_SIZE];

  void init() { written = 0; }
  uint64_t oldest() const { return written > CAPACITY ? written - CAPACITY : 0; }
  void push(const char *line) {
    char *dst = lines[written % CAPACITY];
    size_t n = 0;
    while (n + 1 < LINE_SIZE && line[n] && line[n] != '\r' && line[n] != '\n') {
      dst[n] = line[n];
      ++n;
    }
    dst[n] = 0;
    ++written;
  }
  const char *read(uint64_t index) const {
    if (index < oldest() || index >= written) return nullptr;
    return lines[index % CAPACITY];
  }
};
