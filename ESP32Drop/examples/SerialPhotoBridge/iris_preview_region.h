#pragma once
#include "iris_image.h"
#include "iris_tcon_protocol.h"

namespace iris {
struct PreviewRegionDelta {
  enum class State : uint8_t { Empty, Strip, Invalid };
  State state = State::Invalid;
  IrisImageRegion region;
};

// Pure geometry: current coverage must contain the last successfully displayed
// coverage and may grow along only one edge. No widening, alignment or TCON
// COM/SEG transformation is done here. Keep the old coverage until the returned
// strip has actually completed; skipped callbacks can be coalesced safely.
// A zero-area rectangle is empty, but its coordinates must still be in bounds.
inline PreviewRegionDelta preview_region_delta(
    const IrisImageRegion &old, const IrisImageRegion &current,
    uint16_t canvas_width = WIDTH, uint16_t canvas_height = HEIGHT) {
  using State = PreviewRegionDelta::State;
  const auto bounded = [=](const IrisImageRegion &r) {
    return uint32_t(r.x) + r.width <= canvas_width &&
           uint32_t(r.y) + r.height <= canvas_height;
  };
  if (!canvas_width || !canvas_height || !bounded(old) || !bounded(current))
    return {};
  const bool old_empty = !old.width || !old.height;
  const bool current_empty = !current.width || !current.height;
  if (current_empty) return old_empty ? PreviewRegionDelta{State::Empty,{}} : PreviewRegionDelta{};
  if (old_empty) return {State::Strip,current};
  const uint32_t old_right = uint32_t(old.x) + old.width;
  const uint32_t old_bottom = uint32_t(old.y) + old.height;
  const uint32_t right = uint32_t(current.x) + current.width;
  const uint32_t bottom = uint32_t(current.y) + current.height;
  if (old.x < current.x || old.y < current.y || old_right > right || old_bottom > bottom)
    return {};
  if (old.x == current.x && old.y == current.y && old_right == right && old_bottom == bottom)
    return {State::Empty,{}};
  if (old.x == current.x && old_right == right) {
    if (old.y == current.y)
      return {State::Strip,{current.x,uint16_t(old_bottom),current.width,uint16_t(bottom-old_bottom)}};
    if (old_bottom == bottom)
      return {State::Strip,{current.x,current.y,current.width,uint16_t(old.y-current.y)}};
  }
  if (old.y == current.y && old_bottom == bottom) {
    if (old.x == current.x)
      return {State::Strip,{uint16_t(old_right),current.y,uint16_t(right-old_right),current.height}};
    if (old_right == right)
      return {State::Strip,{current.x,current.y,uint16_t(old.x-current.x),current.height}};
  }
  // Containment alone is insufficient: two-sided growth produces two strips,
  // and growth in both axes produces an L shape. Never repaint old pixels to
  // turn either case into a rectangle.
  return {};
}
} // namespace iris
