#pragma once
#include <stdint.h>

namespace iris {
// A bounded highlight-preserving lift on max(R,G,B), followed by the same gain
// for all three channels. This preserves channel ratios instead of changing hue
// with independent channel curves. Black, white and fully saturated highlights
// are fixed points. At neutral 128, gentle gives 160 and brighter gives 176.
// The 1 KiB lookup avoids floating point and divisions in the decoder callback.
class ToneCurve {
 public:
  explicit ToneCurve(uint8_t level = 0) : level_(level > 2 ? 2 : level) {
    const uint32_t amount = level_ == 1 ? 128 : level_ == 2 ? 192 : 0;
    gain_[0] = 65536;
    for (uint32_t v = 1; v <= 255; ++v) {
      const uint32_t lifted = v + (amount * v * (255 - v) + 32512) / 65025;
      gain_[v] = (lifted * 65536 + v / 2) / v;
    }
  }
  void rgb(const uint8_t *in, uint8_t *out) const {
    if (!level_) {
      out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
      return;
    }
    uint8_t peak = in[0] > in[1] ? in[0] : in[1];
    if (in[2] > peak) peak = in[2];
    const uint32_t gain = gain_[peak];
    for (unsigned c = 0; c < 3; ++c) {
      const uint32_t value = (uint32_t(in[c]) * gain + 32768) >> 16;
      out[c] = uint8_t(value > 255 ? 255 : value);
    }
  }
 private:
  uint32_t gain_[256];
  uint8_t level_;
};
}
