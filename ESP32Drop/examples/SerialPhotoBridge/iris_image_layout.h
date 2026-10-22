#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "iris_image.h"
#include "iris_tcon_protocol.h"
#include "iris_image_tone.h"

namespace iris {
inline uint16_t exif16(const uint8_t *p, bool le) {
  return le ? uint16_t(p[0] | (p[1] << 8)) : uint16_t((p[0] << 8) | p[1]);
}
inline uint32_t exif32(const uint8_t *p, bool le) {
  return le ? uint32_t(exif16(p, true)) | (uint32_t(exif16(p + 2, true)) << 16)
            : (uint32_t(exif16(p, false)) << 16) | exif16(p + 2, false);
}
inline uint8_t tiff_orientation(const uint8_t *t, size_t n) {
  if (n < 8 || !((t[0] == 'I' && t[1] == 'I') || (t[0] == 'M' && t[1] == 'M'))) return 1;
  bool le = t[0] == 'I';
  if (exif16(t + 2, le) != 42) return 1;
  uint32_t offset = exif32(t + 4, le);
  if (offset > n - 2) return 1;
  uint16_t count = exif16(t + offset, le);
  size_t p = size_t(offset) + 2;
  for (uint16_t i = 0; i < count && n - p >= 12; ++i, p += 12) {
    if (exif16(t + p, le) == 0x112 && exif16(t + p + 2, le) == 3 &&
        exif32(t + p + 4, le) == 1) {
      uint16_t o = exif16(t + p + 8, le);
      return o >= 1 && o <= 8 ? uint8_t(o) : 1;
    }
  }
  return 1;
}

struct JpegInfo { uint32_t width = 0, height = 0; uint8_t orientation = 1; };
enum class JpegHeaderResult : uint8_t { NeedMore, Ready, Invalid };
// Header readiness is distinct from a complete/accepted JPEG. Wait until all
// APP/ICC/EXIF/SOF and the complete SOS segment have arrived before decoding.
inline JpegHeaderResult jpeg_header_info(const uint8_t *d, size_t n, JpegInfo &out) {
  out = {};
  if (!d) return JpegHeaderResult::Invalid;
  if (!n) return JpegHeaderResult::NeedMore;
  if (d[0] != 0xff) return JpegHeaderResult::Invalid;
  if (n < 2) return JpegHeaderResult::NeedMore;
  if (d[1] != 0xd8) return JpegHeaderResult::Invalid;
  size_t p = 2;
  while (p < n) {
    if (d[p++] != 0xff) return JpegHeaderResult::Invalid;
    while (p < n && d[p] == 0xff) ++p;
    if (p == n) return JpegHeaderResult::NeedMore;
    uint8_t marker = d[p++];
    if (marker == 0xd9 || marker == 0 || marker == 0xd8) return JpegHeaderResult::Invalid;
    if (marker == 1 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (n - p < 2) return JpegHeaderResult::NeedMore;
    size_t length = (size_t(d[p]) << 8) | d[p + 1];
    if (length < 2) return JpegHeaderResult::Invalid;
    if (length > n - p) return JpegHeaderResult::NeedMore;
    const uint8_t *s = d + p + 2; size_t size = length - 2;
    if (marker == 0xda) {
      if (!out.width || !out.height || size < 4 || (s[0] != 1 && s[0] != 3) ||
          size != size_t(4+2*s[0])) return JpegHeaderResult::Invalid;
      return JpegHeaderResult::Ready;
    }
    if (marker == 0xe1 && size >= 6 && !memcmp(s, "Exif\0\0", 6))
      out.orientation = tiff_orientation(s + 6, size - 6);
    if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
      if (marker != 0xc0 || size < 6 || s[0] != 8 || (s[5] != 1 && s[5] != 3)) return JpegHeaderResult::Invalid;
      out.height = (uint32_t(s[1]) << 8) | s[2];
      out.width = (uint32_t(s[3]) << 8) | s[4];
      if (!out.width || !out.height || out.width > 16384 || out.height > 16384 ||
          uint64_t(out.width) * out.height > 36000000 || size != size_t(6 + 3 * s[5])) return JpegHeaderResult::Invalid;
    }
    p += length;
  }
  return JpegHeaderResult::NeedMore;
}
// Complete-file validation still requires the real terminal EOI. Header-ready
// prefixes must never be promoted to complete-file success by adding a fake EOI.
inline bool jpeg_info(const uint8_t *d, size_t n, JpegInfo &out) {
  out = {};
  return d && n >= 4 && d[n-2] == 0xff && d[n-1] == 0xd9 &&
         jpeg_header_info(d,n,out) == JpegHeaderResult::Ready;
}

struct Layout {
  uint32_t sw, sh, ow, oh, dw, dh;
  int32_t x0, y0;
  uint32_t view_width, view_height;
  uint8_t orientation;
  uint16_t rotation;
  bool wide_x, wide_y;
  Layout(uint32_t w, uint32_t h, uint8_t o, uint16_t r = IRIS_ROTATION_AUTO,
         bool crop = false)
      : sw(w), sh(h), orientation(o), rotation(r) {
    ow = o >= 5 ? h : w; oh = o >= 5 ? w : h;
    if (rotation == IRIS_ROTATION_AUTO) rotation = ow >= oh ? 0 : 90;
    const bool portrait = rotation == 90 || rotation == 270;
    view_width = portrait ? HEIGHT : WIDTH;
    view_height = portrait ? WIDTH : HEIGHT;
    const bool wide = uint64_t(ow) * view_height >= uint64_t(oh) * view_width;
    if (wide != crop) {
      dw = view_width; dh = uint32_t(uint64_t(oh) * view_width / ow);
    } else {
      dh = view_height; dw = uint32_t(uint64_t(ow) * view_height / oh);
    }
    if (!dw) dw = 1;
    if (!dh) dh = 1;
    x0 = (int32_t(view_width) - int32_t(dw)) / 2;
    y0 = (int32_t(view_height) - int32_t(dh)) / 2;
    // Real phone photos take the cheap 32-bit path. Extreme aspect ratios can
    // create a large virtual cover image, but never allocate it or overflow.
    wide_x = uint64_t(ow) * dw + ow - 1 > UINT32_MAX;
    wide_y = uint64_t(oh) * dh + oh - 1 > UINT32_MAX;
  }
  static uint32_t boundary(uint32_t index, uint32_t extent, uint32_t source,
                           bool wide) {
    return wide ? uint32_t((uint64_t(index) * extent + source-1) / source)
                : (index * extent + source-1) / source;
  }
  void oriented(uint32_t x, uint32_t y, uint32_t &ox, uint32_t &oy) const {
    switch (orientation) {
      case 2: ox = sw-1-x; oy = y; break;
      case 3: ox = sw-1-x; oy = sh-1-y; break;
      case 4: ox = x; oy = sh-1-y; break;
      case 5: ox = y; oy = x; break;
      case 6: ox = sh-1-y; oy = x; break;
      case 7: ox = sh-1-y; oy = sw-1-x; break;
      case 8: ox = y; oy = sw-1-x; break;
      default: ox = x; oy = y;
    }
  }
  IrisImageRegion completed_rows(uint32_t rows) const {
    if (rows > sh) rows = sh;
    if (!rows) return {};
    uint32_t ox0 = 0, ox1 = ow, oy0 = 0, oy1 = oh;
    switch (orientation) {
      case 3: case 4: oy0 = sh-rows; break;
      case 5: case 8: ox1 = rows; break;
      case 6: case 7: ox0 = sh-rows; break;
      default: oy1 = rows; break;
    }
    int32_t xa = int32_t(boundary(ox0,dw,ow,wide_x)) + x0;
    int32_t xb = int32_t(boundary(ox1,dw,ow,wide_x)) + x0;
    int32_t ya = int32_t(boundary(oy0,dh,oh,wide_y)) + y0;
    int32_t yb = int32_t(boundary(oy1,dh,oh,wide_y)) + y0;
    if (xa < 0) xa = 0;
    if (ya < 0) ya = 0;
    if (xb > int32_t(view_width)) xb = int32_t(view_width);
    if (yb > int32_t(view_height)) yb = int32_t(view_height);
    if (xa >= xb || ya >= yb) return {};
    switch (rotation) {
      case 90: return {uint16_t(WIDTH-yb),uint16_t(xa),uint16_t(yb-ya),uint16_t(xb-xa)};
      case 180: return {uint16_t(WIDTH-xb),uint16_t(HEIGHT-yb),uint16_t(xb-xa),uint16_t(yb-ya)};
      case 270: return {uint16_t(ya),uint16_t(HEIGHT-xb),uint16_t(yb-ya),uint16_t(xb-xa)};
      default: return {uint16_t(xa),uint16_t(ya),uint16_t(xb-xa),uint16_t(yb-ya)};
    }
  }
  // Destination spans implement inverse nearest-neighbour sampling exactly,
  // including upscaling and mirrored/rotated MCU blocks, with no gaps.
  void pixel(uint8_t *frame, uint32_t x, uint32_t y, const uint8_t *rgb,
             const ToneCurve *tone = nullptr) const {
    uint32_t ox, oy; oriented(x, y, ox, oy);
    int32_t xa = int32_t(boundary(ox,dw,ow,wide_x)) + x0;
    int32_t xb = int32_t(boundary(ox+1,dw,ow,wide_x)) + x0;
    int32_t ya = int32_t(boundary(oy,dh,oh,wide_y)) + y0;
    int32_t yb = int32_t(boundary(oy+1,dh,oh,wide_y)) + y0;
    if (xa < 0) xa = 0;
    if (ya < 0) ya = 0;
    if (xb > int32_t(view_width)) xb = int32_t(view_width);
    if (yb > int32_t(view_height)) yb = int32_t(view_height);
    if (xa >= xb || ya >= yb) return;
    uint8_t adjusted[3];
    if (tone) { tone->rgb(rgb, adjusted); rgb = adjusted; }
    for (int32_t dy = ya; dy < yb; ++dy) {
      const uint32_t vx = uint32_t(xa), vy = uint32_t(dy);
      ptrdiff_t start, step;
      switch (rotation) {
        case 90: start = (vx * WIDTH + WIDTH-1-vy) * 3; step = ROW_BYTES; break;
        case 180: start = ((HEIGHT-1-vy) * WIDTH + WIDTH-1-vx) * 3; step = -3; break;
        case 270: start = ((HEIGHT-1-vx) * WIDTH + vy) * 3; step = -ptrdiff_t(ROW_BYTES); break;
        default: start = (vy * WIDTH + vx) * 3; step = 3;
      }
      for (int32_t dx = xa; dx < xb; ++dx) {
        uint8_t *p = frame + start + ptrdiff_t(dx - xa) * step;
        p[0] = rgb[2]; p[1] = rgb[1]; p[2] = rgb[0];
      }
    }
  }
};
}
