#ifdef PHOTO_FRAME_TCON
#include <Arduino.h>
#include <atomic>
#include "iris_stream_worker.h"
#include "iris_tcon.h"
#include "iris_tcon_protocol.h"
#include "iris_preview_region.h"

namespace {
TaskHandle_t task = nullptr;
uint8_t *frame = nullptr;
const uint8_t *job_jpeg = nullptr;
IrisImageOptions job_options;
std::atomic<bool> busy{false}, cancelled{false}, result_ready{false};
std::atomic<uint32_t> available{0}, total{0}, rows{0}, height{0}, previews{0};
std::atomic<uint8_t> committed{0}; // 0 awaiting archive validation, 1 valid, 2 failed
std::atomic<IrisStreamPhase> phase{IrisStreamPhase::Idle};
IrisStreamResult result;
bool tcon_ready = true; // init is called only after the main task's successful begin
bool full_frame_known = false;

struct PreviewContext {
  const char *error = "";
  uint32_t count = 0, next_percent = 10, last_at = 0, elapsed = 0;
  bool enabled = true;
  IrisImageRegion confirmed, decoded;
  uint32_t source_rows = 0, source_height = 0;
};
size_t input_available(void *) { return available.load(std::memory_order_acquire); }
bool input_cancelled(void *) { return cancelled.load() || committed.load() == 2; }
void input_wait(void *, uint32_t ms) { delay(ms ? ms : 1); }
bool ensure_tcon(PreviewContext &p) {
  if (!tcon_ready) { full_frame_known = false; tcon_ready = iris_tcon_begin(); }
  if (!tcon_ready) p.error = iris_tcon_error();
  return tcon_ready;
}
bool horizontal_coverage(const IrisImageRegion &r) {
  return r.x == 0 && r.width == iris::WIDTH && r.height &&
         uint32_t(r.y)+r.height <= iris::HEIGHT &&
         (r.y == 0 || uint32_t(r.y)+r.height == iris::HEIGHT);
}
bool unsupported_coverage(PreviewContext &p, const char *reason) {
  if (p.count) { p.error = reason; return false; }
  p.enabled = false;
  if (Serial) Serial.printf("IRIS PREVIEW: disabled (%s); final M1 only\n",reason);
  return true;
}
bool show_region(PreviewContext &p, const IrisImageRegion &r) {
  if (!ensure_tcon(p) || !iris_tcon_show_region(frame,iris::FRAME_BYTES,
      r.x,r.y,r.width,r.height)) {
    p.error = iris_tcon_error(); tcon_ready = false; full_frame_known = false; return false;
  }
  return true;
}
bool decoded_rows(void *context, const IrisDecodedRows &coverage) {
  auto &p = *static_cast<PreviewContext *>(context);
  rows.store(coverage.source_rows); height.store(coverage.scaled_height);
  p.decoded = coverage.region; p.source_rows = coverage.source_rows;
  p.source_height = coverage.scaled_height;
  if (input_cancelled(nullptr)) return false;
  if (!p.enabled) return true;
  const auto delta = iris::preview_region_delta(p.confirmed,coverage.region);
  using DeltaState = iris::PreviewRegionDelta::State;
  if (delta.state == DeltaState::Invalid)
    return unsupported_coverage(p,"non-nested/out-of-bounds decoded coverage");
  if (delta.state == DeltaState::Empty) return true; // includes cropped source rows
  // Only the native full-width horizontal-strip mapping is hardware verified.
  // A vertical layout is disabled for this entire job before its first preview;
  // never widen a narrow strip or guess the TCON's automatic COM/SEG mapping.
  if (!horizontal_coverage(coverage.region))
    return unsupported_coverage(p,"vertical or non-canvas coverage is unverified");
  if (delta.region.x || delta.region.width != iris::WIDTH ||
      delta.region.width <= delta.region.height)
    return unsupported_coverage(p,"unsupported preview strip orientation");
  // A fast transfer should go straight to the final verified image. The decoded
  // row fraction is NOT a byte percentage or a prediction of network completion.
  if (committed.load() || available.load() >= total.load() || p.count >= 5 ||
      !coverage.scaled_height || coverage.source_rows >= coverage.scaled_height ||
      coverage.source_rows * 100u < coverage.scaled_height * p.next_percent ||
      (p.count && uint32_t(millis() - p.last_at) < 5000)) return true;
  const IrisImageRegion region = delta.region;
  phase.store(IrisStreamPhase::Previewing);
  const uint32_t received_at_start = available.load();
  const uint32_t started = millis();
  if (!show_region(p,region)) return false;
  p.confirmed = coverage.region;
  p.elapsed += millis() - started; p.last_at = millis(); ++p.count;
  p.next_percent = (coverage.source_rows * 100u / coverage.scaled_height) + 20;
  previews.store(p.count);
  if (Serial) Serial.printf("IRIS PREVIEW: M3 confirmed #%lu rows=%lu/%lu new_strip=%u,%u,%u,%u received_start=%lu received_end=%lu total=%lu; provisional\n",
    (unsigned long)p.count, (unsigned long)coverage.source_rows,
    (unsigned long)coverage.scaled_height, unsigned(region.x), unsigned(region.y),
    unsigned(region.width), unsigned(region.height), (unsigned long)received_at_start, (unsigned long)available.load(),
    (unsigned long)total.load());
  phase.store(IrisStreamPhase::Receiving);
  return !input_cancelled(nullptr);
}
IrisStreamResult process_job() {
    PreviewContext preview;
    preview.enabled = job_options.crop && full_frame_known;
    if (!preview.enabled && Serial)
      Serial.println("IRIS PREVIEW: final M1 only (no confirmed full frame since reset, or CONTAIN)");
    IrisStreamResult completed;
    IrisJpegStreamHooks hooks;
    hooks.context = &preview; hooks.available = input_available;
    hooks.cancelled = input_cancelled; hooks.wait = input_wait; hooks.rows = decoded_rows;
    hooks.input_idle_timeout_ms = 30000; hooks.total_timeout_ms = 360000;
    const char *error = "";
    bool ok = iris_decode_jpeg_stream(job_jpeg, total.load(), frame, hooks, &error,
                                      job_options, &completed.image);
    if (preview.error[0]) error = preview.error;
    if (ok) {
      phase.store(IrisStreamPhase::AwaitingCommit);
      const uint32_t started = millis();
      while (!committed.load() && !cancelled.load() && uint32_t(millis() - started) < 60000)
        delay(5);
      if (input_cancelled(nullptr) || committed.load() != 1) {
        ok = false; error = "stream archive validation cancelled/timed out";
      }
    }
    if (ok) {
      phase.store(IrisStreamPhase::Refreshing);
      const uint32_t started = millis();
      if (preview.count) {
        // EOI and COMMIT have both succeeded. Require full final coverage before
        // accepting an image whose old screen pixels were deliberately retained
        // outside the provisional strips. A final M1 must not mask bad geometry.
        const IrisImageRegion full{0,0,uint16_t(iris::WIDTH),uint16_t(iris::HEIGHT)};
        const auto remainder = iris::preview_region_delta(preview.confirmed,full);
        using DeltaState = iris::PreviewRegionDelta::State;
        if (!preview.source_height || preview.source_rows != preview.source_height ||
            preview.decoded.x || preview.decoded.y ||
            preview.decoded.width != iris::WIDTH || preview.decoded.height != iris::HEIGHT ||
            remainder.state == DeltaState::Invalid ||
            (remainder.state == DeltaState::Strip &&
             (remainder.region.x || remainder.region.width != iris::WIDTH ||
              remainder.region.width <= remainder.region.height))) {
          ok = false; preview.error = "stream preview final coverage is incomplete or invalid";
        } else {
          if (remainder.state == DeltaState::Strip) {
            ok = show_region(preview,remainder.region);
            if (ok && Serial) Serial.printf("IRIS FINAL: M3 remaining_strip=%u,%u,%u,%u; full canvas confirmed\n",
              unsigned(remainder.region.x),unsigned(remainder.region.y),
              unsigned(remainder.region.width),unsigned(remainder.region.height));
          }
          if (ok) preview.confirmed = full;
        }
      }
      // Keep the incremental preview, then repaint the validated complete frame
      // once with M1 to remove visible boundaries between separate M3 waveforms.
      // show() restores the saved DC configuration and leaves the M3 session.
      // Do not start another waveform after a cancel during the last strip.
      if (ok && (input_cancelled(nullptr) || committed.load() != 1)) {
        ok = false; preview.error = "transfer cancelled before final full refresh";
      }
      if (ok) {
        const uint32_t full_started = millis();
        if (Serial) Serial.printf("IRIS FINAL: M1 full-frame refresh started; previews=%lu t=%lu\n",
          (unsigned long)preview.count, (unsigned long)full_started);
        ok = ensure_tcon(preview) && iris_tcon_show(frame,iris::FRAME_BYTES);
        full_frame_known = ok;
        if (!ok) { preview.error = iris_tcon_error(); tcon_ready = false; }
        else if (Serial) Serial.printf("IRIS FINAL: M1 full-frame refresh confirmed; total_ms=%lu t=%lu\n",
          (unsigned long)(millis()-full_started), (unsigned long)millis());
      }
      completed.refresh_ms = millis() - started;
      if (!ok) error = preview.error;
      // Finish an in-flight waveform safely, but never turn a transport cancel
      // into a successful receipt merely because the panel completed it.
      else if (input_cancelled(nullptr) || committed.load() != 1) {
        ok = false; error = "transfer cancelled during final display";
      }
    }
    completed.ok = ok; completed.error = ok ? "" : error;
    completed.previews = preview.count; completed.preview_ms = preview.elapsed;
    return completed;
}
void run(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    result = process_job();
    phase.store(IrisStreamPhase::Idle);
    result_ready.store(true, std::memory_order_release);
    busy.store(false, std::memory_order_release);
  }
}
}
bool iris_stream_init(uint8_t *bgr) {
  if (task) return frame == bgr;
  if (!bgr) return false;
  frame = bgr;
  return xTaskCreate(run, "iris_decode", 8192, nullptr, 1, &task) == pdPASS;
}
bool iris_stream_start(const uint8_t *jpeg, uint32_t length, uint32_t received,
                       const IrisImageOptions &options) {
  if (!task || !jpeg || !length || received > length || busy.load() || result_ready.load()) return false;
  job_jpeg = jpeg; job_options = options;
  total.store(length); available.store(received, std::memory_order_release);
  rows.store(0); height.store(0); previews.store(0);
  cancelled.store(false); committed.store(0);
  phase.store(IrisStreamPhase::Receiving); busy.store(true, std::memory_order_release);
  xTaskNotifyGive(task); return true;
}
void iris_stream_feed(uint32_t received) {
  if (busy.load() && received <= total.load() && received > available.load())
    available.store(received, std::memory_order_release);
}
void iris_stream_commit(bool valid) { committed.store(valid ? 1 : 2, std::memory_order_release); }
void iris_stream_cancel() { cancelled.store(true, std::memory_order_release); }
bool iris_stream_busy() { return busy.load(std::memory_order_acquire); }
bool iris_stream_idle() { return !busy.load(std::memory_order_acquire) && !result_ready.load(std::memory_order_acquire); }
IrisStreamStatus iris_stream_status() {
  IrisStreamStatus s;
  s.busy = iris_stream_busy(); s.phase = phase.load();
  s.source_rows = rows.load(); s.source_height = height.load();
  s.previews = previews.load(); s.available = available.load(); s.total = total.load();
  return s;
}
bool iris_stream_take_result(IrisStreamResult &out) {
  if (busy.load(std::memory_order_acquire) || !result_ready.exchange(false, std::memory_order_acquire)) return false;
  out = result; return true;
}
#endif
