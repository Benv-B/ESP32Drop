#pragma once
// Host-only timing/logging seam. No GPIO, scheduler, TLS or physical hardware.
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
extern uint32_t iris_test_ms;
inline uint32_t millis() { return iris_test_ms; }
inline void delay(uint32_t ms) { iris_test_ms += ms; }
struct IrisTestSerial {
  explicit operator bool() const { return false; }
  template<class... Args> void printf(const char *, Args...) {}
};
extern IrisTestSerial Serial;
