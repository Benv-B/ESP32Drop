#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

namespace iris {
// Small, deliberately limited ICC reader for JPEGs. Recognizes matrix/TRC
// sRGB and Display P3 by their numbers, never by a profile description string.
// Unknown profiles retain their input bytes and are reported as unmanaged.
// ICC header/tag encoding: https://www.color.org/specification/ICC.1-2022-05.pdf
// JPEG APP2 framing: https://www.color.org/profile_embedding/
// Conversion matrices/TRCs: https://www.w3.org/TR/css-color-4/#color-conversion-code
class ImageColor {
 public:
  enum Status : uint8_t {
    Unprofiled, SRGB, DisplayP3, UnsupportedProfile, InvalidProfile, InvalidJpeg
  };
  ImageColor() : status_(Unprofiled) {}
  Status status() const { return status_; }
  bool managed() const { return status_ == SRGB || status_ == DisplayP3; }
  const char *profileName() const {
    return status_ == DisplayP3 ? "Display P3" : status_ == SRGB ? "sRGB" :
           status_ == Unprofiled ? "untagged" : "unknown";
  }
  const char *statusName() const {
    switch (status_) {
      case Unprofiled: return "untagged_assumed_srgb";
      case SRGB: return "srgb_identity";
      case DisplayP3: return "display_p3_to_srgb";
      case UnsupportedProfile: return "unsupported_icc_identity_fallback";
      case InvalidProfile: return "invalid_icc_identity_fallback";
      default: return "invalid_jpeg_identity_fallback";
    }
  }
  Status initJpeg(const uint8_t *jpeg, size_t length) {
    status_ = InvalidJpeg;
    if (!jpeg || length < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) return status_;
    Profile p;
    size_t pos = 2;
    bool end = false;
    while (pos < length) {
      if (jpeg[pos++] != 0xff) return status_;
      while (pos < length && jpeg[pos] == 0xff) ++pos;
      if (pos == length) return status_;
      const uint8_t marker = jpeg[pos++];
      if (marker == 0xd9) { end = true; break; }
      if (marker == 0 || marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7)) return status_;
      if (marker == 1) continue;  // TEM, the other standalone marker.
      if (length - pos < 2) return status_;
      const size_t n = (size_t(jpeg[pos]) << 8) | jpeg[pos+1];
      if (n < 2 || n > length - pos) return status_;
      const uint8_t *data = jpeg + pos + 2;
      const size_t bytes = n - 2;
      if (marker == 0xe2 && bytes >= 12 && !memcmp(data, "ICC_PROFILE\0", 12)) {
        if (bytes < 14 || data[12] == 0 || data[13] == 0 || data[12] > data[13])
          return status_ = InvalidProfile;
        if (data[13] > Profile::MAX_CHUNKS) return status_ = UnsupportedProfile;
        if ((p.count && p.count != data[13]) || p.parts[data[12]-1].data || bytes == 14)
          return status_ = InvalidProfile;
        p.count = data[13];
        p.parts[data[12]-1] = {data + 14, uint32_t(bytes - 14)};
        if (p.size > Profile::MAX_BYTES - (bytes - 14)) return status_ = UnsupportedProfile;
        p.size += uint32_t(bytes - 14);
      }
      pos += n;
      if (marker == 0xda) { end = true; break; }  // Never parse compressed scan bytes.
    }
    if (!end) return status_;
    if (!p.count) return status_ = Unprofiled;
    for (unsigned i = 0; i < p.count; ++i)
      if (!p.parts[i].data) return status_ = InvalidProfile;
    status_ = identify(p);
    if (status_ == DisplayP3) (void)tables();
    return status_;
  }
  void rgb(const uint8_t *in, uint8_t *out) const {
    const uint8_t r = in[0], g = in[1], b = in[2];
    if (status_ != DisplayP3 || (r == g && g == b)) {
      out[0] = r; out[1] = g; out[2] = b;
      return;
    }
    const Tables &t = tables();
    const int32_t lr = t.linear[r], lg = t.linear[g], lb = t.linear[b];
    // Q14 product of the W3C D65 XYZ->sRGB and Display-P3->XYZ matrices.
    // Each row sums to 16384, keeping neutral white fixed. Q12 light values
    // bound all sums to signed int32. Out-of-gamut channels are clipped.
    out[0] = encoded(t, 20069*lr - 3685*lg);
    out[1] = encoded(t, -689*lr + 17073*lg);
    out[2] = encoded(t, -322*lr - 1288*lg + 17994*lb);
  }

 private:
  struct Profile {
    static constexpr unsigned MAX_CHUNKS = 8;
    static constexpr uint32_t MAX_BYTES = 65536;
    struct Part { const uint8_t *data; uint32_t size; } parts[MAX_CHUNKS] = {};
    uint32_t size = 0;
    uint8_t count = 0;
    uint8_t byte(uint32_t offset) const {
      for (unsigned i = 0; i < count; ++i) {
        if (offset < parts[i].size) return parts[i].data[offset];
        offset -= parts[i].size;
      }
      return 0;  // All reads also have enclosing structure bounds checks.
    }
    uint16_t u16(uint32_t o) const { return uint16_t((uint16_t(byte(o)) << 8) | byte(o+1)); }
    uint32_t u32(uint32_t o) const {
      return (uint32_t(byte(o)) << 24) | (uint32_t(byte(o+1)) << 16) |
             (uint32_t(byte(o+2)) << 8) | byte(o+3);
    }
    float fixed(uint32_t o) const {
      const uint32_t n = u32(o);
      const int64_t signed_value = n & 0x80000000U ? int64_t(n) - 0x100000000LL : int64_t(n);
      return float(signed_value) / 65536.0f;
    }
  };
  struct Tag { uint32_t offset = 0, size = 0; };
  static constexpr uint32_t sig(char a, char b, char c, char d) {
    return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | uint32_t(d);
  }
  static Tag tag(const Profile &p, uint32_t signature) {
    for (uint32_t i = 0; i < p.u32(128); ++i) {
      const uint32_t o = 132 + i*12;
      if (p.u32(o) == signature) return {p.u32(o+4), p.u32(o+8)};
    }
    return {};
  }
  static bool near(float a, float b, float tolerance) { return fabsf(a-b) <= tolerance; }
  static bool xyz(const Profile &p, const Tag &t, const float *expected) {
    if (t.size != 20 || p.u32(t.offset) != sig('X','Y','Z',' ') || p.u32(t.offset+4)) return false;
    for (unsigned c = 0; c < 3; ++c)
      if (!near(p.fixed(t.offset+8+c*4), expected[c], 0.001f)) return false;
    return true;
  }
  static float linear(float v) {
    return v <= 0.04045f ? v / 12.92f : powf((v+0.055f)/1.055f, 2.4f);
  }
  static bool srgbCurve(const Profile &p, const Tag &t) {
    if (t.size < 12 || p.u32(t.offset+4)) return false;
    const uint32_t type = p.u32(t.offset);
    if (type == sig('p','a','r','a')) {
      const uint16_t fn = p.u16(t.offset+8);
      if ((fn != 3 && fn != 4) || p.u16(t.offset+10) || t.size != (fn == 3 ? 32u : 40u)) return false;
      const float expected[7] = {2.4f, 1.0f/1.055f, 0.055f/1.055f, 1.0f/12.92f, 0.04045f, 0, 0};
      for (unsigned i = 0; i < (fn == 3 ? 5u : 7u); ++i)
        if (!near(p.fixed(t.offset+12+4*i), expected[i], 0.0001f)) return false;
      return true;
    }
    if (type == sig('c','u','r','v')) {
      const uint32_t count = p.u32(t.offset+8);
      if (count < 32 || count > 4096 || t.size != 12 + count*2) return false;
      uint16_t previous = 0;
      for (uint32_t i = 0; i < count; ++i) {
        const uint16_t sample = p.u16(t.offset+12+i*2);
        if (sample < previous || !near(float(sample)/65535.0f, linear(float(i)/float(count-1)), 0.001f)) return false;
        previous = sample;
      }
      return true;
    }
    return false;
  }
  static Status identify(const Profile &p) {
    if (p.size < 132 || p.u32(0) != p.size || p.u32(36) != sig('a','c','s','p')) return InvalidProfile;
    const uint32_t count = p.u32(128);
    if (count > (p.size-132)/12) return InvalidProfile;
    if (count > 64) return UnsupportedProfile;
    const uint32_t table_end = 132+count*12;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t o = 132+i*12, start = p.u32(o+4), n = p.u32(o+8);
      if (start < table_end || start%4 || n < 8 || start > p.size || n > p.size-start) return InvalidProfile;
      for (uint32_t j = 0; j < i; ++j) {
        const uint32_t other = 132+j*12;
        if (p.u32(other) == p.u32(o)) return InvalidProfile;
        const uint32_t old_start = p.u32(other+4), old_n = p.u32(other+8);
        // ICC permits exact shared tag data (e.g. the three Apple P3 TRCs),
        // but unrelated tag blocks must not partially overlap.
        if (start < old_start+old_n && old_start < start+n &&
            (start != old_start || n != old_n)) return InvalidProfile;
      }
    }
    if ((p.byte(8) != 2 && p.byte(8) != 4) || p.u32(16) != sig('R','G','B',' ') ||
        p.u32(20) != sig('X','Y','Z',' ')) return UnsupportedProfile;
    const uint32_t cls = p.u32(12);
    if (cls != sig('m','n','t','r') && cls != sig('s','c','n','r') && cls != sig('s','p','a','c')) return UnsupportedProfile;
    // A profile containing a LUT/processing-element transform is outside this
    // matrix-only implementation, even when fallback matrix tags are present.
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t s = p.u32(132+i*12) & 0xffffff00U;
      if (s == (sig('A','2','B','0') & 0xffffff00U) || s == (sig('B','2','A','0') & 0xffffff00U) ||
          s == (sig('D','2','B','0') & 0xffffff00U) || s == (sig('B','2','D','0') & 0xffffff00U)) return UnsupportedProfile;
    }
    const float white[3] = {0.9642f, 1.0f, 0.8249f};
    for (unsigned c = 0; c < 3; ++c)
      if (!near(p.fixed(68+c*4), white[c], 0.001f)) return UnsupportedProfile;
    if (!xyz(p, tag(p, sig('w','t','p','t')), white)) return UnsupportedProfile;
    const Tag curves[3] = {tag(p, sig('r','T','R','C')), tag(p, sig('g','T','R','C')), tag(p, sig('b','T','R','C'))};
    for (unsigned c = 0; c < 3; ++c) if (!srgbCurve(p, curves[c])) return UnsupportedProfile;
    // D50-adapted XYZ columns, allowing ICC fixed-point/whitepoint rounding.
    const float p3[3][3] = {{0.5151f,0.2412f,-0.00105f},{0.2920f,0.69225f,0.04189f},{0.1571f,0.06657f,0.78407f}};
    const float srgb[3][3] = {{0.43607f,0.22250f,0.01393f},{0.38506f,0.71688f,0.09710f},{0.14308f,0.06062f,0.71417f}};
    const Tag primaries[3] = {tag(p, sig('r','X','Y','Z')), tag(p, sig('g','X','Y','Z')), tag(p, sig('b','X','Y','Z'))};
    bool is_p3 = true, is_srgb = true;
    for (unsigned c = 0; c < 3; ++c) {
      is_p3 = is_p3 && xyz(p, primaries[c], p3[c]);
      is_srgb = is_srgb && xyz(p, primaries[c], srgb[c]);
    }
    return is_p3 ? DisplayP3 : is_srgb ? SRGB : UnsupportedProfile;
  }
  struct Tables {
    uint16_t linear[256];
    uint8_t encoded[4097];
    Tables() {
      for (unsigned i = 0; i < 256; ++i) linear[i] = uint16_t(ImageColor::linear(float(i)/255.0f)*4096.0f+0.5f);
      for (unsigned i = 0; i <= 4096; ++i) {
        const float v = float(i)/4096.0f;
        const float encoded_value = v <= 0.0031308f ? 12.92f*v : 1.055f*powf(v,1.0f/2.4f)-0.055f;
        encoded[i] = uint8_t(encoded_value*255.0f+0.5f);
      }
    }
  };
  static const Tables &tables() { static const Tables value; return value; }
  static uint8_t encoded(const Tables &t, int32_t q26) {
    if (q26 <= 0) return 0;
    if (q26 >= 4096*16384) return 255;
    return t.encoded[uint32_t(q26+8192)/16384];
  }
  Status status_;
};
}  // namespace iris
