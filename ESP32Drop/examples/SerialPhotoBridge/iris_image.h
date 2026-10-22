#pragma once
#include <stddef.h>
#include <stdint.h>

constexpr uint16_t IRIS_ROTATION_AUTO = 360;

struct IrisImageOptions {
  // Clockwise from the desired viewing canvas into the native 1024 x 768 frame.
  // EXIF orientation is applied first, independently of the physical mounting.
  // AUTO chooses 0 for landscape/square or 90 for portrait, after EXIF.
  uint16_t rotation = IRIS_ROTATION_AUTO;
  uint8_t brightness = 0; // 0 original, 1 gentle, 2 brighter
  bool crop = true; // fill the canvas by a centered crop; false keeps the whole photo
};

struct IrisImageMetadata {
  uint32_t source_width = 0, source_height = 0, elapsed_ms = 0;
  uint16_t view_width = 0, view_height = 0;
  uint32_t fitted_width = 0, fitted_height = 0;
  uint16_t requested_rotation = IRIS_ROTATION_AUTO, effective_rotation = 0;
  uint8_t exif_orientation = 1, jpeg_scale = 0;
  bool cropped = false;
  const char *color_profile = "unexamined";
  const char *color_status = "unexamined";
  bool color_managed = false;
  uint32_t active_ms = 0, input_wait_ms = 0, preview_wait_ms = 0;
};

// Half-open native-panel rectangle [x,x+width) x [y,y+height).
struct IrisImageRegion { uint16_t x = 0, y = 0, width = 0, height = 0; };
struct IrisDecodedRows {
  uint32_t source_rows = 0, scaled_height = 0;
  IrisImageRegion region; // cumulative visible coverage of complete source MCU rows
};
struct IrisJpegStreamHooks {
  void *context = nullptr;
  // Stable JPEG storage must remain alive throughout the call. Only append to
  // its unpublished tail; publish prefix length after copying, with release/
  // acquire synchronization in these callbacks. Values are JPEG-relative.
  size_t (*available)(void *) = nullptr;
  bool (*cancelled)(void *) = nullptr;
  // May wait at most max_ms; nullptr uses Arduino delay. Never called on TLS.
  void (*wait)(void *, uint32_t max_ms) = nullptr;
  // Runs on the decoder owner, with a stable frame. May block for a preview.
  // Returning false cancels. This reports temporary coverage, not acceptance.
  bool (*rows)(void *, const IrisDecodedRows &) = nullptr;
  uint32_t input_idle_timeout_ms = 15000, total_timeout_ms = 180000;
};

bool iris_decode_jpeg(const uint8_t *jpeg, size_t size, uint8_t *bgr, const char **error,
                      const IrisImageOptions &options = IrisImageOptions{},
                      IrisImageMetadata *metadata = nullptr);
// Keeps one ROM jd_decomp invocation alive while waiting for appended bytes.
// Must run on a separate consumer/decoder task: DATA ACKs cannot depend on it.
// Success also requires every declared byte and the real terminal EOI; archive
// CRC/COMMIT validation and final display success remain the caller's job.
bool iris_decode_jpeg_stream(const uint8_t *jpeg, size_t declared_size, uint8_t *bgr,
                            const IrisJpegStreamHooks &hooks, const char **error,
                            const IrisImageOptions &options = IrisImageOptions{},
                            IrisImageMetadata *metadata = nullptr);
