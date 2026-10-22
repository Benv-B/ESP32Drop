#pragma once
#include <stdint.h>

// A failed conversion has no voltage. Retry only the driver's transient timeout,
// never a valid zero/low sample, and never replace failure with an older sample.
namespace iris {
struct AdcReadResult { int error, raw; uint8_t attempts; };
template<class Read, class Pause>
AdcReadResult read_latch_raw(Read read, Pause pause, int timeout_error) {
  AdcReadResult result{timeout_error, 0, 0};
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    int raw = 0;
    result.error = read(&raw);
    result.attempts = attempt + 1;
    if (!result.error) { result.raw = raw; return result; }
    if (result.error != timeout_error) return result;
    if (attempt < 2) pause(1);
  }
  return result;
}
}
