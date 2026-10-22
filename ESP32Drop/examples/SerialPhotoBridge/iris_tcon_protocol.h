#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// IRIS TCON application note v1.23: pp.19-23,26,28-29,39. No flash/PMIC writes.
namespace iris {
constexpr uint32_t WIDTH = 1024, HEIGHT = 768, ROW_BYTES = WIDTH * 3;
constexpr uint32_t FRAME_BYTES = ROW_BYTES * HEIGHT;
// The manual gives no maximum boot latency; leave a generous bounded window.
// Its >=15 s guidance on p.39 refers to refresh completion, not boot duration.
constexpr uint32_t BOOT_TIMEOUT_MS = 20000, REFRESH_TIMEOUT_MS = 20000;

struct Status {
  uint8_t bytes[4] = {};
  bool fault() const { return (bytes[1] & 0xef) || (bytes[3] & 0x0f); }
  bool booted() const { return !fault() && (bytes[1] & 0x10); }
  bool displayed(uint8_t completion = 0x02) const { return !fault() && (bytes[2] & completion); }
  const char *fault_reason() const {
    // INTS S(1)/S(3), application note v1.23 p.21.
    if (bytes[1] & 0x08) return "TCON thermal shutdown";
    if (bytes[1] & 0x02) return "TCON undervoltage (UVLO)";
    if (bytes[1] & 0x01) return "TCON DRAM parity error";
    if (bytes[1] & 0x04) return "TCON display start failed";
    if (bytes[1] & 0x20) return "TCON flash boot failed";
    if (bytes[1] & 0x40) return "TCON DRM command failed";
    if (bytes[1] & 0x80) return "TCON VDTIF command failed";
    if (bytes[3] & 0x02) return "TCON PMIC1 NACK";
    if (bytes[3] & 0x01) return "TCON temperature sensor NACK";
    if (bytes[3] & 0x04) return "TCON EEPROM NACK";
    if (bytes[3] & 0x08) return "TCON no LVDS input";
    return "TCON refresh fault";
  }
};

// IO keeps one CS assertion around opcode + parameters (and the read dummy).
// This same sequencer is exercised with a scripted TCON in host tests.
template<class IO> class Tcon {
 public:
  explicit Tcon(IO &io) : io_(io) {}
  const char *error = "not initialized";
  Status status;
  bool ready = false;

  bool begin() {
    ready = false;
    clear_partial_session();
    if (!io_.enable()) return fail("TCON supply/latch or SPI unavailable");
    // Cold boot leaves DF high. A warm MCU reset may find an already idle TCON.
    if (!wait_high(BOOT_TIMEOUT_MS)) {
      if (!write(0x05, 0x01) || !wait_high(BOOT_TIMEOUT_MS))
        return fail("TCON boot/reset DF timeout");
    }
    // v1.23 requires 300 ms AFTER DF rises, not just after issuing reset.
    io_.pause(300);
    if (!read_status() || !status.booted()) return fail("TCON boot status failed");
    if (!wait_low()) return fail("TCON DF stuck high after INTS read");
    if (!write(0x06, 0x01)) return fail("TCON wake failed");
    io_.pause(5);
    uint8_t pm = 0;
    if (!io_.read(0x46, &pm, 1) || pm != 0x01)
      return fail("TCON power mode readback failed");
    if (!write(0x08, 0x02) || !write(0x07, 0x01))
      return fail("TCON SPI/M1 configuration failed");
    ready = true; error = ""; return true;
  }

  bool show(const uint8_t *bgr, size_t length) {
    if (!ready || !bgr || length != FRAME_BYTES) return fail("invalid TCON frame/state");
    // Drain latched faults/stale completion BEFORE starting a new refresh.
    if (!io_.powered() || !read_status() || status.fault() || !wait_low())
      return fail("TCON not idle before refresh");
    if (!restore_m1()) return false;
    return send_frame_and_wait(bgr, 0x02, "TCON M1 completion timeout");
  }

  // Current experimental transport sends a full-frame buffer in 1024-pixel
  // chunks. The examples on pp.28-29 show such chunks but do not specify B_last;
  // they do not establish native-row stride or the required partial byte count.
  // This raw-input entry point is retained for bench comparisons. It does not
  // crop source pixels at the target coordinates; use show_region() for that.
  // Coordinates are native panel pixels; auto COM/SEG swap is left to TCON.
  // Keep a partial session configured across calls (pp.28-29 repeat steps 7-15).
  // The next full-frame show() restores the original DC bytes and M1 mode.
  bool show_partial(const uint8_t *bgr, size_t length, uint16_t x, uint16_t y,
                    uint16_t width, uint16_t height, bool fast = false) {
    return show_partial_impl(bgr, length, x, y, width, height, fast, false, false);
  }

  // Crop the native full-frame ROI into exactly width*height packed BGR pixels,
  // without modifying the source or sending padding. The 256x128 SHORT A bench
  // confirmed this input layout and length; other dimensions use the same layout.
  // Do not transpose W<H: the controller's auto COM/SEG behavior still needs
  // physical validation independently of this byte-packing operation.
  bool show_region(const uint8_t *bgr, size_t length, uint16_t x, uint16_t y,
                   uint16_t width, uint16_t height, bool fast = false) {
    return show_partial_impl(bgr, length, x, y, width, height, fast, true, false);
  }

  // Explicit diagnostic comparator, bypassing the native-frame ROI crop.
  // Send exactly width*height BGR pixels already packed by the caller. Omitting
  // full-frame padding tests whether overrun/length explains the narrow-ROI
  // bench result; it does not establish the controller's input stride behavior.
  bool show_partial_compact(const uint8_t *bgr, size_t length, uint16_t x, uint16_t y,
                            uint16_t width, uint16_t height, bool fast = false) {
    return show_partial_impl(bgr, length, x, y, width, height, fast, false, true);
  }

  void sleep() {
    if (ready) { write(0x06, 0x04); io_.pause(5); }
    ready = false; clear_partial_session(); io_.disable();
  }

 private:
  struct PixelRegion { uint16_t x, y, width, height; };
  IO &io_;
  uint8_t partial_mode_ = 0;
  uint8_t saved_dc_[2] = {};
  // Reused for each 0x0C chunk; no second full-frame allocation or stack buffer.
  uint8_t region_chunk_[ROW_BYTES];

  bool show_partial_impl(const uint8_t *bgr, size_t length, uint16_t x, uint16_t y,
                         uint16_t width, uint16_t height, bool fast, bool pack, bool compact) {
    const size_t expected_length = compact ? size_t(width) * height * 3 : FRAME_BYTES;
    if (!ready || !bgr || length != expected_length || !width || !height ||
        uint32_t(x) + width > WIDTH || uint32_t(y) + height > HEIGHT) {
      // Reject caller mistakes before any hardware traffic or recovery reset.
      error = "invalid TCON partial frame/region/state"; return false;
    }
    if (!io_.powered() || !read_status() || status.fault() || !wait_low())
      return fail("TCON not idle before partial refresh");
    const uint8_t mode = fast ? 0x08 : 0x04;
    if (!partial_mode_) {
      if (!io_.read(0x44, saved_dc_, sizeof(saved_dc_)))
        return fail("TCON display configuration read failed");
      // Match the initial M3/M4 setup, including auto COM/SEG swap. Repeating
      // this setup between windows is not part of the documented sequence.
      // The DC command has two bytes (pp.20-21), despite the abbreviated
      // single-byte example on p.28. Preserve CFG(1) and verify both bytes.
      const uint8_t partial_dc[2] = {0x14, saved_dc_[1]};
      uint8_t observed_dc[2] = {};
      if (!io_.write(0x04, partial_dc, sizeof(partial_dc)))
        return fail("TCON partial configuration failed");
      if (!io_.read(0x44, observed_dc, sizeof(observed_dc)) ||
          observed_dc[0] != partial_dc[0] || observed_dc[1] != partial_dc[1])
        return fail("TCON partial configuration readback failed");
      if (!write(0x08, 0x02) || !write(0x07, mode))
        return fail("TCON partial configuration failed");
      partial_mode_ = mode;
    } else if (partial_mode_ != mode) {
      // A completed partial session may change waveform without passing via M1.
      if (!write(0x07, mode)) return fail("TCON partial mode switch failed");
      partial_mode_ = mode;
    }
    const uint8_t region[6] = {
      uint8_t(x >> 4), uint8_t((x << 4) | (y >> 8)), uint8_t(y),
      uint8_t(width >> 4), uint8_t((width << 4) | (height >> 8)), uint8_t(height)
    };
    if (!io_.write(0x0b, region, sizeof(region)))
      return fail("TCON partial region failed");
    const PixelRegion source{x, y, width, height};
    if (!send_frame_and_wait(bgr, fast ? 0x10 : 0x08,
                             fast ? "TCON M4 completion timeout" : "TCON M3 completion timeout",
                             pack ? &source : nullptr,
                             pack ? size_t(width) * height * 3 : compact ? length : FRAME_BYTES))
      return false;
    error = ""; return true;
  }
  void clear_partial_session() {
    partial_mode_ = 0;
    saved_dc_[0] = saved_dc_[1] = 0;
  }
  bool restore_m1() {
    if (!partial_mode_) return true;
    // Change modes only after the matching completion event has been cleared.
    // Restore both readable DC bytes, once, when the caller requests M1.
    if (!io_.powered() || !io_.write(0x04, saved_dc_, sizeof(saved_dc_)) || !write(0x07, 0x01))
      return fail("TCON M1 configuration restore failed");
    clear_partial_session();
    return true;
  }
  const uint8_t *pack_region_chunk(const uint8_t *bgr, const PixelRegion &region,
                                   uint32_t chunk) {
    // The transport sends only the copied pixels, including the last short
    // chunk; bytes beyond the ROI are neither read nor transmitted.
    uint32_t pixel = chunk * WIDTH, copied = 0;
    const uint32_t pixels = uint32_t(region.width) * region.height;
    while (pixel < pixels && copied < WIDTH) {
      const uint32_t row = pixel / region.width, column = pixel % region.width;
      uint32_t count = region.width - column;
      if (count > WIDTH - copied) count = WIDTH - copied;
      const uint8_t *source = bgr + ((uint32_t(region.y) + row) * WIDTH + region.x + column) * 3;
      memcpy(region_chunk_ + copied * 3, source, count * 3);
      copied += count; pixel += count;
    }
    return region_chunk_;
  }
  bool send_frame_and_wait(const uint8_t *bgr, uint8_t completion, const char *timeout_error,
                           const PixelRegion *region = nullptr, size_t payload_bytes = FRAME_BYTES) {
    if (!io_.write(0x09, nullptr, 0)) return fail("TCON display start failed");
    uint32_t chunk_index = 0;
    for (size_t offset = 0; offset < payload_bytes; ++chunk_index) {
      if ((chunk_index & 7) == 0 && !io_.powered()) return fail("TCON latch lost during pixels");
      size_t count = payload_bytes - offset;
      if (count > ROW_BYTES) count = ROW_BYTES;
      const uint8_t *chunk = region ? pack_region_chunk(bgr, *region, chunk_index) : bgr + offset;
      if (!io_.write(0x0c, chunk, count))
        return fail("TCON pixel transfer failed");
      offset += count;
      // C5 has one application core. Return CPU time after each complete row
      // (CS is already high), including while receiving a preview's next bytes.
      io_.pause(1);
    }
    if (!io_.write(0x0d, nullptr, 0)) return fail("TCON display end failed");
    const uint32_t started = io_.now();
    while (uint32_t(io_.now() - started) < REFRESH_TIMEOUT_MS) {
      if (!io_.powered()) return fail("TCON latch lost during refresh");
      if (io_.df()) {
        if (!read_status()) return fail("TCON status read failed");
        if (status.fault()) return fail(status.fault_reason());
        // A boot/restart interrupt is not a completed image.
        if ((status.bytes[0] & 4) || (status.bytes[1] & 0x10))
          return fail("TCON restarted during refresh");
        if (!wait_low()) return fail("TCON DF did not clear");
        if (status.displayed(completion)) { error = ""; return true; }
      }
      io_.pause(2);
    }
    return fail(timeout_error);
  }
  bool write(uint8_t command, uint8_t value) { return io_.write(command, &value, 1); }
  bool read_status() { return io_.read(0x57, status.bytes, 4); }
  bool wait_high(uint32_t timeout) {
    uint32_t start = io_.now();
    while (uint32_t(io_.now() - start) < timeout) {
      if (!io_.powered()) return false;
      if (io_.df()) return true;
      io_.pause(2);
    }
    return false;
  }
  bool wait_low() {
    uint32_t start = io_.now();
    while (io_.df()) {
      if (uint32_t(io_.now() - start) >= 20) return false;
      io_.pause(1);
    }
    return true;
  }
  bool fail(const char *message) {
    error = message; ready = false;
    clear_partial_session();
    const Status failed_status = status;
    // On an incomplete scan, try the documented reset before isolation. Never
    // report success or immediately retry a waveform after an unknown outcome.
    if (io_.powered()) {
      if (io_.df()) read_status();
      if (wait_low() && write(0x05, 0x01) && wait_high(BOOT_TIMEOUT_MS)) {
        io_.pause(300); read_status();
      }
    }
    status = failed_status; // preserve the fault, not the recovery boot interrupt
    io_.disable(); return false;
  }
};
}
