#include <assert.h>
#include <stdio.h>
#include <vector>
#include "../ESP32Drop/examples/SerialPhotoBridge/iris_latch_sample.h"

int main() {
  constexpr int timeout = 0x107, invalid = 0x102;
  struct Attempt { int error, raw; };
  auto check = [&](const std::vector<Attempt> &sequence, int error, int raw,
                   unsigned attempts, unsigned pauses) {
    unsigned called=0, waited=0;
    const auto result=iris::read_latch_raw([&](int *value) {
      assert(called<sequence.size());
      *value=sequence[called].raw; return sequence[called++].error;
    }, [&](uint32_t ms) { assert(ms==1); ++waited; }, timeout);
    assert(result.error==error && result.raw==raw && result.attempts==attempts);
    assert(called==attempts && waited==pauses);
  };
  check({{0,0},{0,4095}},0,0,1,0); // Genuine zero is returned immediately.
  check({{0,100},{0,4095}},0,100,1,0); // A valid low value is never retried away.
  check({{0,1200}},0,1200,1,0);
  check({{timeout,4095},{0,0}},0,0,2,1); // Invalid raw is discarded; next valid low wins.
  check({{timeout,4095},{timeout,4095},{0,1000}},0,1000,3,2);
  check({{timeout,4095},{timeout,4095},{timeout,4095},{0,2000}},timeout,0,3,2);
  check({{invalid,4095},{0,2000}},invalid,0,1,0); // No retry for configuration errors.
  check({{timeout,4095},{invalid,4095},{0,2000}},invalid,0,2,1);
  puts("IRIS latch ADC: bounded timeout retries, invalid data discarded, valid low/zero preserved passed");
}
