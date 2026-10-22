#pragma once
#include <stddef.h>
#include <stdint.h>
#include "iris_image.h"

enum class IrisStreamPhase : uint8_t { Idle, Receiving, Previewing, AwaitingCommit, Refreshing };
struct IrisStreamResult {
  bool ok = false;
  const char *error = "";
  IrisImageMetadata image;
  uint32_t refresh_ms = 0, previews = 0, preview_ms = 0;
};
struct IrisStreamStatus {
  bool busy = false;
  IrisStreamPhase phase = IrisStreamPhase::Idle;
  uint32_t source_rows = 0, source_height = 0, previews = 0, available = 0, total = 0;
};

// One owner of JPEG/framebuffer/TCON. The main loop keeps copying and ACKing DATA.
// A JPEG prefix is provisional; only commit(true) permits final completion and
// success. Previewed images finish their remaining strip, then every validated
// image gets one final M1 repaint to remove visible strip boundaries.
bool iris_stream_init(uint8_t *frame);
bool iris_stream_start(const uint8_t *jpeg, uint32_t total, uint32_t available,
                       const IrisImageOptions &options);
void iris_stream_feed(uint32_t available);
void iris_stream_commit(bool valid);
void iris_stream_cancel();
bool iris_stream_busy();
// Also excludes a completed result that the main task has not consumed yet.
bool iris_stream_idle();
IrisStreamStatus iris_stream_status();
bool iris_stream_take_result(IrisStreamResult &result);
