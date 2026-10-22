#ifdef PHOTO_FRAME_TCON
#include <Arduino.h>
#include <esp32c5/rom/tjpgd.h>
#include "iris_image.h"
#include "iris_image_layout.h"
#include "iris_image_color.h"
#include "iris_image_render.h"

namespace {
// Accepted photos can reach 36 MP. Color conversion and resampling on C5 need
// a bounded CPU budget independent of network stalls and display waveforms.
constexpr uint32_t JPEG_ACTIVE_PROCESSING_BUDGET_MS = 90000;
struct Timing {
  const IrisJpegStreamHooks *hooks;
  IrisImageMetadata *metadata;
  uint32_t started, input_wait = 0, preview_wait = 0;
  const char *error = nullptr;
  ~Timing() {
    if (!metadata) return;
    metadata->elapsed_ms = millis() - started;
    metadata->input_wait_ms = input_wait;
    metadata->preview_wait_ms = preview_wait;
    metadata->active_ms = metadata->elapsed_ms - input_wait - preview_wait;
  }
  bool check() {
    if (error) return false;
    if (hooks && hooks->cancelled && hooks->cancelled(hooks->context))
      error = "JPEG stream cancelled";
    else if (hooks && hooks->total_timeout_ms && millis()-started >= hooks->total_timeout_ms)
      error = "JPEG stream session timeout";
    else if (millis()-started-input_wait-preview_wait >= JPEG_ACTIVE_PROCESSING_BUDGET_MS)
      error = "JPEG decode processing timeout";
    return !error;
  }
  size_t available(size_t limit) const {
    const size_t n = hooks ? hooks->available(hooks->context) : limit;
    return n < limit ? n : limit;
  }
  bool await(size_t limit, size_t wanted) {
    uint32_t last_progress = millis();
    size_t previous = available(limit);
    while (previous < wanted) {
      if (!check()) return false;
      if (!hooks) { error = "JPEG input truncated"; return false; }
      if (hooks->input_idle_timeout_ms && millis()-last_progress >= hooks->input_idle_timeout_ms) {
        error = "JPEG stream input timeout"; return false;
      }
      const uint32_t before = millis();
      if (hooks->wait) hooks->wait(hooks->context, 25); else delay(1);
      input_wait += millis()-before;
      const size_t now = available(limit);
      if (now < previous) { error = "JPEG stream prefix moved backwards"; return false; }
      if (now > previous) last_progress = millis();
      previous = now;
    }
    return check();
  }
};
struct Decode {
  const uint8_t *input;
  size_t length, position;
  uint8_t *frame;
  iris::Layout layout;
  iris::ToneCurve tone;
  iris::ImageColor color;
  Timing *timing;
  uint32_t yielded, completed = 0;
};
UINT input(JDEC *jd, BYTE *data, UINT count) {
  auto &d = *static_cast<Decode *>(jd->device);
  const size_t left = d.length-d.position;
  if (count > left) count = UINT(left);
  // Never signal temporary EOF to the ROM. A short read aborts jd_decomp and
  // cannot be resumed. The independent receiver must continue DATA ACKs here.
  if (!d.timing->await(d.length,d.position+count)) return 0;
  if (data && count) memcpy(data,d.input+d.position,count);
  d.position += count;
  return count;
}
UINT output(JDEC *jd, void *bitmap, JRECT *r) {
  auto &d = *static_cast<Decode *>(jd->device);
  if (!d.timing->check()) return 0;
  if (r->right >= d.layout.sw || r->bottom >= d.layout.sh ||
      r->left > r->right || r->top > r->bottom) {
    d.timing->error = "JPEG decoder emitted invalid rectangle"; return 0;
  }
  iris::render_rgb_block(d.layout,d.frame,static_cast<const uint8_t *>(bitmap),
                        r->left,r->top,uint32_t(r->right)-r->left+1,
                        uint32_t(r->bottom)-r->top+1,d.color,d.tone);
  if (uint32_t(r->right)+1 == d.layout.sw && uint32_t(r->bottom)+1 > d.completed) {
    d.completed = uint32_t(r->bottom)+1;
    const auto *hooks = d.timing->hooks;
    if (hooks && hooks->rows) {
      IrisDecodedRows progress;
      progress.source_rows = d.completed; progress.scaled_height = d.layout.sh;
      progress.region = d.layout.completed_rows(d.completed);
      const uint32_t before = millis();
      const bool proceed = hooks->rows(hooks->context,progress);
      d.timing->preview_wait += millis()-before;
      if (!proceed) { d.timing->error = "JPEG stream preview cancelled"; return 0; }
      if (!d.timing->check()) return 0;
    }
  }
  if (millis()-d.yielded >= 4) { delay(1); d.yielded = millis(); }
  return 1;
}
bool decode(const uint8_t *jpeg, size_t size, uint8_t *bgr, const char **error,
            const IrisImageOptions &options, IrisImageMetadata *metadata,
            const IrisJpegStreamHooks *hooks) {
  const char *ignored_error = "";
  if (!error) error = &ignored_error;
  if (metadata) *metadata = {};
  Timing timing{hooks,metadata,millis()};
  if ((options.rotation != 0 && options.rotation != 90 && options.rotation != 180 &&
       options.rotation != 270 && options.rotation != IRIS_ROTATION_AUTO) || options.brightness > 2) {
    *error = "invalid image options (rotation auto/0/90/180/270, brightness 0/1/2)"; return false;
  }
  *error = "unsupported/corrupt JPEG (use baseline RGB JPEG, <=36 MP)";
  if (!jpeg || !bgr || size < 4 || (hooks && !hooks->available)) return false;
  iris::JpegInfo info;
  if (hooks) {
    for (;;) {
      if (!timing.check()) { *error = timing.error; return false; }
      const size_t available = timing.available(size);
      const auto result = iris::jpeg_header_info(jpeg,available,info);
      if (result == iris::JpegHeaderResult::Ready) break;
      if (result == iris::JpegHeaderResult::Invalid || available == size) return false;
      if (!timing.await(size,available+1)) { *error = timing.error; return false; }
    }
  } else if (!iris::jpeg_info(jpeg,size,info)) return false;
  uint8_t *work = static_cast<uint8_t *>(malloc(4096));
  if (!work) { *error = "JPEG workspace allocation failed"; return false; }
  Decode d{jpeg,size,0,bgr,
           iris::Layout(info.width,info.height,info.orientation,options.rotation,options.crop),
           iris::ToneCurve(options.brightness),iris::ImageColor{},&timing,millis()};
  // Header is complete through SOS, therefore ICC segments and EXIF are frozen.
  d.color.initJpeg(jpeg,timing.available(size));
  if (Serial) Serial.printf("IRIS COLOR: %s; %s; output=sRGB reference\n",
                            d.color.profileName(),d.color.statusName());
  const uint16_t effective_rotation = d.layout.rotation;
  if (metadata) {
    metadata->source_width = info.width; metadata->source_height = info.height;
    metadata->exif_orientation = info.orientation;
    metadata->view_width = uint16_t(d.layout.view_width);
    metadata->view_height = uint16_t(d.layout.view_height);
    metadata->requested_rotation = options.rotation;
    metadata->effective_rotation = effective_rotation;
    metadata->color_profile = d.color.profileName(); metadata->color_status = d.color.statusName();
    metadata->color_managed = d.color.managed();
  }
  JDEC jd{};
  JRESULT result = jd_prepare(&jd,input,work,4096,&d);
  if (result == JDR_OK && (jd.width != info.width || jd.height != info.height)) result = JDR_FMT1;
  if (result == JDR_OK) {
    uint8_t scale = 0;
    while (scale < 3 && (d.layout.ow >> (scale+1)) >= d.layout.dw &&
           (d.layout.oh >> (scale+1)) >= d.layout.dh) ++scale;
    d.layout = iris::Layout(jd.width >> scale,jd.height >> scale,info.orientation,
                           effective_rotation,options.crop);
    if (metadata) {
      metadata->jpeg_scale = scale; metadata->fitted_width = d.layout.dw;
      metadata->fitted_height = d.layout.dh;
      metadata->cropped = d.layout.x0 < 0 || d.layout.y0 < 0;
    }
    memset(bgr,255,iris::FRAME_BYTES);
    result = jd_decomp(&jd,output,scale);
  }
  free(work);
  if (result != JDR_OK) {
    *error = timing.error ? timing.error : "JPEG decoder rejected image"; return false;
  }
  // The ROM can finish all MCUs before reading EOI. A successful prefix decode
  // is still a preview: wait for all declared bytes and check the actual file.
  if (!timing.await(size,size)) { *error = timing.error; return false; }
  iris::JpegInfo final_info;
  if (!iris::jpeg_info(jpeg,size,final_info) || final_info.width != info.width ||
      final_info.height != info.height || final_info.orientation != info.orientation) {
    *error = "JPEG stream incomplete or changed header"; return false;
  }
  *error = "";
  return true;
}
}  // namespace
bool iris_decode_jpeg(const uint8_t *jpeg, size_t size, uint8_t *bgr, const char **error,
                      const IrisImageOptions &options, IrisImageMetadata *metadata) {
  return decode(jpeg,size,bgr,error,options,metadata,nullptr);
}
bool iris_decode_jpeg_stream(const uint8_t *jpeg, size_t size, uint8_t *bgr,
                            const IrisJpegStreamHooks &hooks, const char **error,
                            const IrisImageOptions &options, IrisImageMetadata *metadata) {
  return decode(jpeg,size,bgr,error,options,metadata,&hooks);
}
#endif
