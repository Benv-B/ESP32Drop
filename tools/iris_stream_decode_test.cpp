#include <assert.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>
#include <atomic>

// Compile the actual firmware input/timing/layout/color implementation. Only
// Arduino time and the ROM entrypoints are replaced. These tests do NOT decode
// JPEG entropy or prove real-ROM, AirDrop, concurrent-task, or panel behavior.
#define PHOTO_FRAME_TCON 1
#include "../ESP32Drop/examples/SerialPhotoBridge/iris_image.cpp"
#include "../ESP32Drop/examples/SerialPhotoBridge/iris_stream_worker.h"
#include "../ESP32Drop/examples/SerialPhotoBridge/iris_tcon.h"
#include "../ESP32Drop/examples/SerialPhotoBridge/iris_preview_region.h"

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define IRIS_TEST_ASAN 1
#endif

uint32_t iris_test_ms = 0;
IrisTestSerial Serial;
using Bytes = std::vector<uint8_t>;
static const Bytes *reference;
static size_t rom_position;
static unsigned source_width, source_height, cpu_ms_per_row;
static constexpr size_t GUARD = 32;
static constexpr uint8_t CANARY = 0xa5;

static void poison(uint8_t *p, size_t n) {
#ifdef IRIS_TEST_ASAN
  if (n) __asan_poison_memory_region(p,n);
#else
  (void)p; (void)n;
#endif
}
static void unpoison(uint8_t *p, size_t n) {
#ifdef IRIS_TEST_ASAN
  if (n) __asan_unpoison_memory_region(p,n);
#else
  (void)p; (void)n;
#endif
}
struct Feed {
  Bytes storage;
  size_t published = 0;
  bool freeze = false, cancel = false, decline = false;
  unsigned row_calls = 0, input_calls = 0, skip_calls = 0;
  uint32_t row_pause_ms = 0;
  Feed() : storage(reference->size()+2*GUARD,CANARY) {
    poison(data(),reference->size());
  }
  ~Feed() { unpoison(data(),reference->size()); }
  uint8_t *data() { return storage.data()+GUARD; }
  void publish(size_t end) {
    end = std::min(end,reference->size());
    assert(end >= published);
    unpoison(data()+published,end-published);
    memcpy(data()+published,reference->data()+published,end-published);
    published = end;
  }
  void check_guards() const {
    for (size_t i=0;i<GUARD;++i) {
      assert(storage[i]==CANARY);
      assert(storage[GUARD+reference->size()+i]==CANARY);
    }
  }
};
static Feed *current_feed;

static bool rom_read(JDEC *jd, UINT count, bool skip = false) {
  uint8_t bytes[512]; assert(count<=sizeof(bytes));
  const UINT received = jd->infunc(jd,skip ? nullptr : bytes,count);
  if (received!=count) return false; // timeout/cancel aborts, never resume it
  // This catches a callback returning unreceived bytes even without ASan. ASan
  // additionally poisons the entire unpublished tail, including header reads.
  assert(rom_position+count<=current_feed->published);
  assert(rom_position+count<=reference->size());
  if (!skip) assert(!memcmp(bytes,reference->data()+rom_position,count));
  ++current_feed->input_calls;
  if (skip) ++current_feed->skip_calls;
  rom_position += count;
  return true;
}
JRESULT jd_prepare(JDEC *jd, UINT(*in)(JDEC *,BYTE *,UINT), void *, UINT, void *device) {
  jd->device = device; jd->infunc = in;
  jd->width = source_width; jd->height = source_height;
  rom_position = 0;
  return rom_read(jd,4) ? JDR_OK : JDR_INP;
}
JRESULT jd_decomp(JDEC *jd, UINT(*out)(JDEC *,void *,JRECT *), BYTE scale) {
  const unsigned w = source_width>>scale, h = source_height>>scale;
  Bytes pixels(w*3,100);
  for (unsigned y=0;y<h;++y) {
    const size_t wanted = 4+(reference->size()-6)*(y+1)/h;
    while (rom_position<wanted) {
      const UINT n = UINT(std::min(size_t(512),wanted-rom_position));
      if (!rom_read(jd,n,(y%3)==1)) return JDR_INP;
    }
    iris_test_ms += cpu_ms_per_row;
    JRECT rect{0,uint16_t(w-1),uint16_t(y),uint16_t(y)};
    if (!out(jd,pixels.data(),&rect)) return JDR_INTR;
  }
  // Deliberately finishes before consuming EOI. The real wrapper must still
  // wait for the final published bytes and inspect the actual terminator.
  return JDR_OK;
}
static size_t available(void *p) { return static_cast<Feed *>(p)->published; }
static bool cancelled(void *p) { return static_cast<Feed *>(p)->cancel; }
static void wait(void *p, uint32_t ms) {
  auto &f = *static_cast<Feed *>(p);
  iris_test_ms += ms;
  if (!f.freeze) f.publish(f.published+16);
}
static bool rows(void *p, const IrisDecodedRows &r) {
  auto &f = *static_cast<Feed *>(p);
  ++f.row_calls;
  assert(r.source_rows<=r.scaled_height);
  assert(uint32_t(r.region.x)+r.region.width<=iris::WIDTH);
  assert(uint32_t(r.region.y)+r.region.height<=iris::HEIGHT);
  if (f.row_calls<=5) iris_test_ms += f.row_pause_ms;
  return !f.decline;
}
static bool run(Feed &feed, const char **error, IrisImageMetadata *metadata = nullptr,
                uint32_t idle = 30000, uint32_t total = 360000, bool full = false) {
  current_feed = &feed;
  Bytes frame(iris::FRAME_BYTES+2*GUARD,CANARY);
  IrisJpegStreamHooks hooks{&feed,available,cancelled,wait,rows,idle,total};
  const bool ok = full
    ? iris_decode_jpeg(feed.data(),reference->size(),frame.data()+GUARD,error,{},metadata)
    : iris_decode_jpeg_stream(feed.data(),reference->size(),frame.data()+GUARD,hooks,error,{},metadata);
  feed.check_guards();
  for (size_t i=0;i<GUARD;++i) {
    assert(frame[i]==CANARY);
    assert(frame[GUARD+iris::FRAME_BYTES+i]==CANARY);
  }
  return ok;
}

namespace worker_test {
// Compile the actual worker with deterministic decoder/RTOS/TCON seams. This
// verifies decisions and commit gating, not real scheduler or panel behavior.
using TaskHandle_t = void *;
constexpr bool pdTRUE=true;
constexpr int pdPASS=1;
constexpr uint32_t portMAX_DELAY=UINT32_MAX;
static void ulTaskNotifyTake(bool,uint32_t) {}
static void xTaskNotifyGive(TaskHandle_t) {}
static int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,TaskHandle_t *task) {
  *task=reinterpret_cast<void *>(uintptr_t(1)); return pdPASS;
}
struct TestSerial {
  explicit operator bool() const { return false; }
  template<class... Args> void printf(const char *,Args...) {}
  void println(const char *) {}
} Serial;
bool iris_tcon_begin();
const char *iris_tcon_error();
bool iris_tcon_show(const uint8_t *,size_t);
bool iris_tcon_show_region(const uint8_t *,size_t,uint16_t,uint16_t,uint16_t,uint16_t,bool=false);
bool fake_stream_decode(const uint8_t *,size_t,uint8_t *,const IrisJpegStreamHooks &,
                        const char **,const IrisImageOptions &,IrisImageMetadata *);
#define iris_decode_jpeg_stream fake_stream_decode
#include "../ESP32Drop/examples/SerialPhotoBridge/iris_stream_worker.cpp"
#undef iris_decode_jpeg_stream

enum class Direction { Top, Bottom, Vertical, InvalidAfterFirst, IncompleteFinal };
struct Scenario {
  Direction direction=Direction::Top;
  bool valid_commit=true, missing_commit=false, decode_ok=true, fast=false;
  bool covered_before_eoi=false, cancel_final=false, fail_region=false;
  bool cancel_remainder=false, fail_remainder=false, fail_full=false;
} scenario;
struct DisplayCall { IrisImageRegion region; bool final; };
static std::vector<DisplayCall> calls;
static unsigned m1_calls=0, begins=0;
static bool decode_completed=false;
bool iris_tcon_begin() { ++begins; return true; }
const char *iris_tcon_error() { return "fake TCON failure"; }
bool iris_tcon_show(const uint8_t *pixels,size_t length) {
  assert(pixels==frame && length==iris::FRAME_BYTES && committed.load()==1 && decode_completed);
  assert(!m1_calls && !input_cancelled(nullptr));
  ++m1_calls;
  iris_test_ms+=1000; // Completion waits remain part of the final refresh time.
  if (scenario.cancel_final) iris_stream_cancel();
  return !scenario.fail_full;
}
bool iris_tcon_show_region(const uint8_t *pixels,size_t length,uint16_t x,uint16_t y,
                           uint16_t w,uint16_t h,bool fast) {
  assert(pixels==frame && !m1_calls && length==iris::FRAME_BYTES && !fast && w==iris::WIDTH && w>h);
  const bool final=committed.load()==1;
  if (final) assert(decode_completed);
  calls.push_back({{x,y,w,h},final});
  if (final && scenario.cancel_remainder) iris_stream_cancel();
  return !scenario.fail_region && !(final && scenario.fail_remainder);
}
bool fake_stream_decode(const uint8_t *,size_t,uint8_t *,const IrisJpegStreamHooks &hooks,
                        const char **error,const IrisImageOptions &,IrisImageMetadata *) {
  // An empty initial crop is not an error and must not trigger a clear.
  IrisDecodedRows cropped{1,1000,{}};
  assert(hooks.rows(hooks.context,cropped));
  for (uint32_t source : {100u,300u,500u,700u,900u,1000u}) {
    iris_test_ms+=6000;
    available.store(scenario.fast ? 10000 : source*10);
    uint16_t extent=uint16_t(uint32_t(iris::HEIGHT)*source/1000);
    if (scenario.covered_before_eoi && source>=900) extent=iris::HEIGHT;
    IrisImageRegion r{0,0,uint16_t(iris::WIDTH),extent};
    if (scenario.direction==Direction::Bottom) r.y=uint16_t(iris::HEIGHT-extent);
    if (scenario.direction==Direction::Vertical)
      r={0,0,uint16_t(iris::WIDTH*source/1000),uint16_t(iris::HEIGHT)};
    if (scenario.direction==Direction::InvalidAfterFirst && source==300) r.height=1;
    if (scenario.direction==Direction::IncompleteFinal && source==1000) --r.height;
    IrisDecodedRows coverage{source,1000,r};
    if (!hooks.rows(hooks.context,coverage)) { *error="fake callback cancelled"; return false; }
  }
  decode_completed=true;
  if (!scenario.missing_commit) iris_stream_commit(scenario.valid_commit);
  *error=scenario.decode_ok ? "" : "fake malformed EOI";
  return scenario.decode_ok;
}
static IrisStreamResult execute(Scenario requested={},bool crop=true,bool known=true) {
  scenario=requested; calls.clear(); m1_calls=0; begins=0; decode_completed=false;
  busy.store(false); result_ready.store(false); tcon_ready=true; iris_test_ms=0;
  full_frame_known=known;
  static Bytes pixels(iris::FRAME_BYTES);
  static const uint8_t jpeg[]={0xff,0xd8};
  assert(iris_stream_init(pixels.data()));
  IrisImageOptions options; options.crop=crop;
  assert(worker_test::iris_stream_start(jpeg,10000,10,options));
  return process_job();
}
static void tests() {
  { auto r=execute({},true,false);
    assert(r.ok && !r.previews && calls.empty() && m1_calls==1 && full_frame_known); }
  for (const auto direction : {Direction::Top,Direction::Bottom}) {
    Scenario request; request.direction=direction;
    const auto result=execute(request);
    assert(result.ok && result.previews==5 && m1_calls==1 && calls.size()==6 && result.refresh_ms>=1000);
    std::vector<uint8_t> seen(iris::WIDTH*iris::HEIGHT);
    for (size_t i=0;i<calls.size();++i) {
      const auto &call=calls[i]; const auto &r=call.region;
      assert(call.final==(i+1==calls.size()));
      for (uint32_t y=r.y;y<uint32_t(r.y)+r.height;++y)
        for (uint32_t x=r.x;x<uint32_t(r.x)+r.width;++x) {
          auto &pixel=seen[y*iris::WIDTH+x]; assert(!pixel); pixel=1;
        }
    }
    assert(std::all_of(seen.begin(),seen.end(),[](uint8_t pixel){ return pixel==1; }));
  }
  { Scenario s; s.covered_before_eoi=true; auto r=execute(s);
    assert(r.ok && r.previews==5 && calls.size()==5 && m1_calls==1);
    for (const auto &call:calls) assert(!call.final); }
  { Scenario s; s.fast=true; auto r=execute(s);
    assert(r.ok && !r.previews && calls.empty() && m1_calls==1); }
  { auto r=execute({},false); // CONTAIN must finish borders through M1.
    assert(r.ok && !r.previews && calls.empty() && m1_calls==1); }
  { Scenario s; s.direction=Direction::Vertical; auto r=execute(s);
    assert(r.ok && !r.previews && calls.empty() && m1_calls==1); }
  { Scenario s; s.valid_commit=false; auto r=execute(s);
    assert(!r.ok && calls.size()==5 && !m1_calls);
    for (const auto &call:calls) assert(!call.final); }
  { Scenario s; s.missing_commit=true; auto r=execute(s);
    assert(!r.ok && calls.size()==5 && !m1_calls); }
  { Scenario s; s.decode_ok=false; auto r=execute(s);
    assert(!r.ok && calls.size()==5 && !m1_calls && !strcmp(r.error,"fake malformed EOI")); }
  { Scenario s; s.direction=Direction::InvalidAfterFirst; auto r=execute(s);
    assert(!r.ok && r.previews==1 && calls.size()==1 && !m1_calls && strstr(r.error,"coverage")); }
  { Scenario s; s.direction=Direction::IncompleteFinal; auto r=execute(s);
    assert(!r.ok && r.previews==5 && calls.size()==5 && !m1_calls && strstr(r.error,"coverage")); }
  { Scenario s; s.fail_region=true; auto r=execute(s);
    assert(!r.ok && !r.previews && calls.size()==1 && !m1_calls && !full_frame_known && !strcmp(r.error,"fake TCON failure")); }
  { Scenario s; s.cancel_final=true; auto r=execute(s);
    assert(!r.ok && calls.size()==6 && m1_calls==1 && strstr(r.error,"cancelled")); }
  { Scenario s; s.fast=true; s.cancel_final=true; auto r=execute(s);
    assert(!r.ok && calls.empty() && m1_calls==1 && strstr(r.error,"cancelled")); }
  { Scenario s; s.cancel_remainder=true; auto r=execute(s);
    assert(!r.ok && calls.size()==6 && !m1_calls && strstr(r.error,"cancelled")); }
  { Scenario s; s.fail_remainder=true; auto r=execute(s);
    assert(!r.ok && calls.size()==6 && !m1_calls && !tcon_ready && !strcmp(r.error,"fake TCON failure")); }
  { Scenario s; s.fail_full=true; auto r=execute(s);
    assert(!r.ok && calls.size()==6 && m1_calls==1 && !tcon_ready && !strcmp(r.error,"fake TCON failure")); }
  puts("IRIS stream worker: disjoint strips, final remainder then one M1, EOI/commit gating, crop/vertical fallback and cancellation/failure passed; fake TCON/RTOS only");
}
} // namespace worker_test

int main() {
  std::ifstream file("testdata/iris_baseline.jpg",std::ios::binary);
  const Bytes jpeg((std::istreambuf_iterator<char>(file)),{});
  assert(!jpeg.empty()); reference = &jpeg;
  iris::JpegInfo info;
  assert(iris::jpeg_info(jpeg.data(),jpeg.size(),info));
  source_width = info.width; source_height = info.height;
  const char *error;
  {
    Feed feed; feed.row_pause_ms = 5000;
    IrisImageMetadata metadata;
    assert(run(feed,&error,&metadata));
    assert(feed.published==jpeg.size() && feed.row_calls && feed.input_calls && feed.skip_calls);
    assert(metadata.preview_wait_ms==25000 && metadata.input_wait_ms>0 && metadata.active_ms<20000);
    assert(metadata.elapsed_ms==metadata.active_ms+metadata.input_wait_ms+metadata.preview_wait_ms);
    printf("stream success: %u ms active, %u input wait, %u preview wait; %u row callbacks\n",
           metadata.active_ms,metadata.input_wait_ms,metadata.preview_wait_ms,feed.row_calls);
  }
  iris_test_ms = 0;
  { Feed feed; feed.freeze = true; assert(!run(feed,&error,nullptr,50)); assert(!strcmp(error,"JPEG stream input timeout")); }
  iris_test_ms = 0;
  { Feed feed; assert(!run(feed,&error,nullptr,30000,50)); assert(!strcmp(error,"JPEG stream session timeout")); }
  iris_test_ms = 0;
  { Feed feed; feed.cancel = true; assert(!run(feed,&error)); assert(!strcmp(error,"JPEG stream cancelled")); }
  iris_test_ms = 0;
  { Feed feed; feed.publish(jpeg.size()); feed.decline = true;
    assert(!run(feed,&error)); assert(!strcmp(error,"JPEG stream preview cancelled")); }
  // Exercise the real Timing wrapper with >20 s of active decode work. For the
  // streaming path add >90 s of display waits as well: those must not consume
  // the 90 s active budget. Both entrypoints share the same bound.
  assert(source_height);
  cpu_ms_per_row = 30000/source_height+1;
  for (bool full : {false,true}) {
    iris_test_ms=0;
    Feed feed; feed.row_pause_ms=20000;
    if (full) feed.publish(jpeg.size());
    IrisImageMetadata metadata;
    assert(run(feed,&error,&metadata,30000,360000,full));
    assert(metadata.active_ms>20000 && metadata.active_ms<90000);
    assert(metadata.preview_wait_ms==(full ? 0u : 100000u));
    assert(metadata.elapsed_ms==metadata.active_ms+metadata.input_wait_ms+metadata.preview_wait_ms);
  }
  cpu_ms_per_row = 90000/source_height+1;
  for (bool full : {false,true}) {
    iris_test_ms=0;
    Feed feed; feed.publish(jpeg.size());
    IrisImageMetadata metadata;
    assert(!run(feed,&error,&metadata,30000,360000,full));
    assert(!strcmp(error,"JPEG decode processing timeout"));
    assert(metadata.active_ms>=90000);
  }
  iris_test_ms = 0; cpu_ms_per_row = 0;
  { Feed feed; feed.publish(jpeg.size()); feed.data()[jpeg.size()-1] = 0;
    assert(!run(feed,&error)); assert(!strcmp(error,"JPEG stream incomplete or changed header")); }
  iris_test_ms = 0;
  { Feed feed; feed.publish(jpeg.size()); assert(run(feed,&error,nullptr,30000,360000,true)); }
  puts("IRIS stream wrapper: publication boundaries, canaries, timing/cancel/EOI/full API passed; fake ROM only");
  worker_test::tests();
}
