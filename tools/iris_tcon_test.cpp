#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "../ESP32Drop/examples/SerialPhotoBridge/iris_tcon_protocol.h"

struct FakeIO {
  uint32_t clock = 0, boot_at = 10, finish_at = 0, end_at = 0;
  uint32_t rows = 0, bytes = 0, resets = 0, sleeps = 0;
  bool enabled = false, power = true, no_boot = false, no_finish = false;
  bool stuck = false, bad_pm = false, read_fail = false, boot_pending = true;
  bool scanning = false, finished = false, dc_read_fail = false;
  bool dc_partial_read_fail = false;
  uint8_t dc_readback_xor[2] = {};
  uint8_t fault1 = 0, fault3 = 0, done_bit = 0, finish_bit = 2;
  uint8_t mode = 1, fail_write = 0, dc[2] = {0x10, 0}, region[6] = {};
  uint32_t region_writes = 0, dc_restores = 0, dc_reads = 0;
  struct Write { uint8_t command; size_t size; uint8_t first; };
  std::vector<Write> writes;
  std::vector<uint8_t> commands;
  bool capture_pixels = false;
  size_t expected_payload = iris::FRAME_BYTES;
  std::vector<uint8_t> pixels;
  bool enable() { enabled = power; clock += 100; return enabled; }
  void disable() { enabled = false; }
  bool powered() { return power && enabled; }
  uint32_t now() { return clock; }
  void pause(uint32_t ms) { clock += ms; }
  bool df() {
    return enabled && (stuck || (boot_pending && !no_boot && clock >= boot_at) ||
                       (finished && !no_finish && clock >= finish_at));
  }
  bool write(uint8_t cmd, const uint8_t *p, size_t n) {
    if (!enabled) return false;
    commands.push_back(cmd);
    writes.push_back({cmd, n, n ? p[0] : uint8_t(0)});
    if (cmd == fail_write) return false;
    if (cmd == 4) {
      assert(!scanning && !finished && n == 2);
      memcpy(dc, p, n);
      if (mode == 4 || mode == 8) ++dc_restores;
    }
    if (cmd == 5) {
      assert(n == 1 && p[0] == 1); ++resets; boot_pending = true; boot_at = clock + 10;
      scanning = finished = false; mode = 1; dc[0] = 0x10; dc[1] = 0;
    }
    if (cmd == 6) { assert(n == 1); if (p[0] == 4) ++sleeps; }
    if (cmd == 8) assert(n == 1 && p[0] == 2);
    if (cmd == 7) {
      assert(!scanning && !finished && n == 1 && (p[0] == 1 || p[0] == 4 || p[0] == 8));
      mode = p[0];
    }
    if (cmd == 0xb) {
      assert(!scanning && !finished && (mode == 4 || mode == 8) && n == 6 && dc[0] == 0x14);
      memcpy(region, p, n); ++region_writes;
    }
    if (cmd == 9) { assert(n == 0); scanning = true; rows = bytes = 0; pixels.clear(); }
    if (cmd == 0xc) {
      assert(scanning && n > 0 && n <= 3072 && n % 3 == 0 && bytes+n <= expected_payload);
      if (capture_pixels) pixels.insert(pixels.end(), p, p+n);
      else assert(p[0] == uint8_t(rows));
      ++rows; bytes += n;
    }
    if (cmd == 0xd) {
      assert(scanning && n == 0 && rows == (expected_payload+3071)/3072 && bytes == expected_payload);
      scanning = false; finished = true; end_at = clock; finish_at = clock + 15000;
      finish_bit = done_bit ? done_bit : uint8_t(mode << 1);
    }
    return true;
  }
  bool read(uint8_t cmd, uint8_t *p, size_t n) {
    if (read_fail) return false;
    if (cmd == 0x44) {
      assert(n == 2); ++dc_reads; if (dc_read_fail) return false;
      memcpy(p, dc, n);
      if (dc[0] == 0x14) {
        if (dc_partial_read_fail) return false;
        p[0] ^= dc_readback_xor[0]; p[1] ^= dc_readback_xor[1];
      }
      return true;
    }
    if (cmd == 0x46) { assert(n == 1); p[0] = bad_pm ? 0xff : 1; return true; }
    assert(cmd == 0x57 && n == 4);
    memset(p, 0, n);
    if (boot_pending && !no_boot && clock >= boot_at) {
      assert(clock >= boot_at + 300); // command quiet time AFTER DF
      p[1] = 0x10; boot_pending = false;
    } else if (finished && !no_finish && clock >= finish_at) {
      p[1] = fault1; p[2] = finish_bit; p[3] = fault3; finished = false;
    }
    return true;
  }
};
int main() {
  std::vector<uint8_t> frame(iris::FRAME_BYTES);
  for (uint32_t y = 0; y < 768; ++y) memset(frame.data() + y * 3072, uint8_t(y), 3072);
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    assert(t.show(frame.data(), frame.size())); assert(io.clock >= io.finish_at);
    assert(io.resets == 0 && t.status.bytes[2] == 2);
    assert(t.show(frame.data(), frame.size())); // fresh completion, not last frame
    t.sleep(); assert(!io.enabled && io.sleeps == 1);
  }
  for (uint8_t fault : {uint8_t(1),uint8_t(2),uint8_t(4),uint8_t(8),uint8_t(0x20),uint8_t(0x40),uint8_t(0x80),uint8_t(0xff)}) {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin()); io.fault1 = fault;
    assert(!t.show(frame.data(), frame.size())); assert(!io.enabled && io.resets == 1);
    assert(t.status.bytes[1] == fault);
    if (fault == 1) assert(!strcmp(t.error,"TCON DRAM parity error"));
    if (fault == 2) assert(!strcmp(t.error,"TCON undervoltage (UVLO)"));
  }
  for (uint8_t fault : {uint8_t(1),uint8_t(2),uint8_t(4),uint8_t(8)}) {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin()); io.fault3 = fault;
    assert(!t.show(frame.data(), frame.size()));
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin()); io.no_finish = true;
    assert(!t.show(frame.data(), frame.size()));
    assert(io.clock - io.end_at >= 20000 && !io.enabled);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin()); io.done_bit = 4;
    assert(!t.show(frame.data(), frame.size())); // M2 is not M1
  }
  {
    FakeIO io; io.boot_at = 8000; iris::Tcon<FakeIO> t(io);
    assert(t.begin() && io.resets == 0 && io.clock >= 8300); // slow boot must not be reset after 3 s
  }
  {
    FakeIO io; io.boot_pending = false; iris::Tcon<FakeIO> t(io);
    assert(t.begin() && io.resets == 1); // warm MCU restart
  }
  {
    FakeIO io; io.power = false; iris::Tcon<FakeIO> t(io);
    assert(!t.begin() && io.commands.empty()); // USB-only: no TCON traffic
  }
  {
    FakeIO io; io.no_boot = true; iris::Tcon<FakeIO> t(io); assert(!t.begin() && !io.enabled);
  }
  {
    FakeIO io; io.bad_pm = true; iris::Tcon<FakeIO> t(io); assert(!t.begin() && !io.enabled);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin()); io.power = false;
    assert(!t.show(frame.data(), frame.size()) && !io.enabled);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    assert(!t.show(frame.data(), frame.size()-1)); assert(io.rows == 0);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin()); io.stuck = true;
    assert(!t.show(frame.data(), frame.size()) && io.rows == 0);
  }
  // Both published partial-window examples, followed by a normal M1 refresh.
  for (bool fast : {false, true}) {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    io.dc[0] = 0x18; io.dc[1] = 0x5a; // prove restore preserves both original bytes
    const size_t start = io.writes.size();
    const uint16_t height = fast ? 50 : 200;
    assert(t.show_partial(frame.data(), frame.size(), 128, 256, 100, height, fast));
    const uint8_t expected[6] = {0x08, 0x01, 0x00, 0x06, 0x40, uint8_t(height)};
    assert(memcmp(io.region, expected, sizeof(expected)) == 0 && io.region_writes == 1);
    assert(io.rows == 768 && io.bytes == iris::FRAME_BYTES); // never send compact w*h data
    assert(t.status.bytes[2] == (fast ? 0x10 : 0x08));
    assert(io.clock >= io.finish_at && io.dc_restores == 0 && io.mode == (fast ? 8 : 4));
    assert(io.dc_reads == 2 && io.dc[0] == 0x14 && io.dc[1] == 0x5a && io.resets == 0);
    assert(io.writes[start].command == 4 && io.writes[start].first == 0x14 && io.writes[start].size == 2);
    assert(io.writes[start + 1].command == 8 && io.writes[start + 1].first == 2);
    assert(io.writes[start + 2].command == 7 && io.writes[start + 2].first == (fast ? 8 : 4));
    assert(io.writes[start + 3].command == 0xb && io.writes[start + 4].command == 9);
    assert(io.writes.back().command == 0xd);
    const size_t full_start = io.writes.size();
    assert(t.show(frame.data(), frame.size()) && t.status.bytes[2] == 2);
    assert(io.dc_restores == 1 && io.mode == 1 && io.dc[0] == 0x18 && io.dc[1] == 0x5a);
    assert(io.writes[full_start].command == 4 && io.writes[full_start].size == 2);
    assert(io.writes[full_start + 1].command == 7 && io.writes[full_start + 1].first == 1);
    assert(io.writes[full_start + 2].command == 9);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    io.dc[0] = 0x18; io.dc[1] = 0x5a;
    assert(t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    // Repeated M3 windows must begin at step 7, without touching DC, SPI or MD.
    const size_t repeated_start = io.writes.size();
    assert(t.show_partial(frame.data(), frame.size(), 0, 128, 1024, 128));
    assert(io.writes.size() - repeated_start == iris::HEIGHT + 3);
    assert(io.writes[repeated_start].command == 0xb);
    assert(io.writes[repeated_start + 1].command == 9);
    assert(io.writes.back().command == 0xd);
    assert(io.dc_reads == 2 && io.dc_restores == 0 && io.mode == 4);
    // M3->M4 changes MD only, retains the same saved original DC configuration.
    const size_t switch_start = io.writes.size();
    assert(t.show_partial(frame.data(), frame.size(), 0, 256, 1024, 128, true));
    assert(io.writes.size() - switch_start == iris::HEIGHT + 4);
    assert(io.writes[switch_start].command == 7 && io.writes[switch_start].first == 8);
    assert(io.writes[switch_start + 1].command == 0xb);
    assert(io.dc_reads == 2 && io.dc_restores == 0 && t.status.bytes[2] == 0x10);
    assert(t.show(frame.data(), frame.size()));
    assert(io.dc_restores == 1 && io.dc[0] == 0x18 && io.dc[1] == 0x5a);
    const size_t second_full_start = io.writes.size();
    assert(t.show(frame.data(), frame.size()));
    assert(io.writes[second_full_start].command == 9 && io.dc_restores == 1);
    // A new session captures any configuration changed after returning to M1.
    io.dc[0] = 0x10; io.dc[1] = 0x6a;
    assert(t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    assert(io.dc_reads == 4);
    assert(t.show(frame.data(), frame.size()));
    assert(io.dc_restores == 2 && io.dc[0] == 0x10 && io.dc[1] == 0x6a);
  }
  for (bool sleep_first : {false, true}) {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    io.dc[0] = 0x18; io.dc[1] = 0x5a;
    assert(t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    if (sleep_first) t.sleep();
    assert(t.begin()); // Warm restart resets TCON and invalidates the old session.
    assert(io.resets == 1);
    const size_t after_begin = io.writes.size();
    assert(t.show(frame.data(), frame.size()));
    assert(io.writes[after_begin].command == 9 && io.dc_restores == 0);
    assert(io.dc[0] == 0x10 && io.dc[1] == 0);
    assert(t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    assert(io.dc_reads == 4);
    assert(t.show(frame.data(), frame.size()));
    assert(io.dc[0] == 0x10 && io.dc[1] == 0 && io.dc_restores == 1);
  }
  for (uint8_t failed_command : {uint8_t(4), uint8_t(7)}) {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    assert(t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    io.fail_write = failed_command;
    assert(!t.show(frame.data(), frame.size()));
    assert(!strcmp(t.error, "TCON M1 configuration restore failed") && !t.ready && !io.enabled);
    io.fail_write = 0;
    assert(t.begin());
    const size_t after_begin = io.writes.size();
    assert(t.show(frame.data(), frame.size()));
    assert(io.writes[after_begin].command == 9); // No stale session restoration.
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    assert(t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    io.fail_write = 7;
    assert(!t.show_partial(frame.data(), frame.size(), 0, 128, 1024, 128, true));
    assert(!strcmp(t.error, "TCON partial mode switch failed") && !t.ready && !io.enabled);
    io.fail_write = 0;
    assert(t.begin());
    assert(t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    assert(io.dc_reads == 4); // Recovery must capture reset DC, not the stale copy.
  }
  for (unsigned readback_error = 0; readback_error < 3; ++readback_error) {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    if (readback_error == 2) io.dc_partial_read_fail = true;
    else io.dc_readback_xor[readback_error] = 1;
    assert(!t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    assert(!strcmp(t.error, "TCON partial configuration readback failed"));
    assert(io.dc_reads == 2 && io.region_writes == 0 && io.rows == 0);
    assert(!t.ready && !io.enabled && io.resets == 1);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    // Latched old M1 completion is drained before starting M3, never reused.
    io.finished = true; io.finish_at = io.clock; io.finish_bit = 2;
    assert(t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 768));
    const uint8_t expected[] = {0, 0, 0, 0x40, 0x03, 0};
    assert(memcmp(io.region, expected, sizeof(expected)) == 0 && t.status.bytes[2] == 8);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    assert(t.show_partial(frame.data(), frame.size(), 1023, 767, 1, 1));
    const uint8_t expected[] = {0x3f, 0xf2, 0xff, 0, 0x10, 1};
    assert(memcmp(io.region, expected, sizeof(expected)) == 0);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    const size_t before = io.commands.size();
    for (const auto &r : std::vector<std::vector<uint16_t>>{
         {0,0,0,1}, {0,0,1,0}, {1024,0,1,1}, {0,768,1,1},
         {1023,0,2,1}, {0,767,1,2}, {65535,0,2,1}, {0,65535,1,2}})
      assert(!t.show_partial(frame.data(), frame.size(), r[0], r[1], r[2], r[3]));
    assert(!t.show_partial(nullptr, frame.size(), 0, 0, 1, 1));
    assert(!t.show_partial(frame.data(), frame.size()-1, 0, 0, 1, 1));
    assert(io.commands.size() == before && t.ready && io.enabled); // no hardware side effects
  }
  for (uint8_t wrong_done : {uint8_t(2), uint8_t(4), uint8_t(0x10)}) {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin()); io.done_bit = wrong_done;
    assert(!t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    assert(!strcmp(t.error, "TCON M3 completion timeout") && !io.enabled && io.resets == 1);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin()); io.done_bit = 8;
    assert(!t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128, true));
    assert(!strcmp(t.error, "TCON M4 completion timeout") && !io.enabled);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin()); io.no_finish = true;
    assert(!t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    assert(io.clock - io.end_at >= 20000 && !io.enabled && io.dc_restores == 0);
  }
  for (uint8_t fault : {uint8_t(1), uint8_t(0x10), uint8_t(0x80)}) {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin()); io.fault1 = fault;
    assert(!t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    assert(!io.enabled && io.resets == 1 && t.status.bytes[1] == fault);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin()); io.dc_read_fail = true;
    assert(!t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    assert(io.rows == 0 && io.region_writes == 0 && !io.enabled);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin()); io.fail_write = 0x0b;
    assert(!t.show_partial(frame.data(), frame.size(), 0, 0, 1024, 128));
    assert(io.rows == 0 && !io.enabled && io.resets == 1);
  }
  {
    // Independent byte oracle: build a native image with distinct B/G/R and
    // x/y-dependent values, then append ROI pixels in native row/column order.
    // The production packer instead splits copies at both ROI and chunk edges.
    std::vector<uint8_t> native(iris::FRAME_BYTES);
    for (uint32_t y=0;y<iris::HEIGHT;++y) for (uint32_t x=0;x<iris::WIDTH;++x)
      for (uint32_t c=0;c<3;++c)
        native[(y*iris::WIDTH+x)*3+c]=uint8_t(x*17+y*131+c*47+x*y*3);
    const auto untouched=native;
    struct Region { uint16_t x,y,w,h; };
    const Region cases[] = {
      {128,256,256,128}, {13,17,257,130}, // Nonzero origin, many chunk/ROI crossings.
      {1023,767,1,1}, {0,767,1024,1}, // Last pixel, last native row.
      {0,0,1024,768}, {999,727,25,41}, // Full frame, ROI touching bottom/right.
      {3,5,1,763}, {17,0,1007,768}, {7,9,7,7} // Narrow/tall, odd width, square.
    };
    for (bool fast : {false,true}) for (const auto &r : cases) {
      const size_t payload=size_t(r.w)*r.h*3;
      FakeIO io; io.capture_pixels=true; io.expected_payload=payload;
      iris::Tcon<FakeIO> t(io); assert(t.begin());
      assert(t.show_region(native.data(),native.size(),r.x,r.y,r.w,r.h,fast));
      assert(io.rows==(payload+iris::ROW_BYTES-1)/iris::ROW_BYTES && io.bytes==payload);
      assert(t.status.bytes[2]==(fast?0x10:0x08));
      std::vector<uint8_t> expected(payload);
      size_t out=0;
      for (uint32_t y=r.y;y<uint32_t(r.y)+r.h;++y)
        for (uint32_t x=r.x;x<uint32_t(r.x)+r.w;++x)
          for (uint32_t c=0;c<3;++c) expected[out++]=native[(y*iris::WIDTH+x)*3+c];
      assert(out==uint32_t(r.w)*r.h*3 && io.pixels==expected);
      const auto &last_chunk=io.writes[io.writes.size()-2];
      assert(last_chunk.command==0x0c && io.writes.back().command==0x0d);
      assert(last_chunk.size==(payload%iris::ROW_BYTES?payload%iris::ROW_BYTES:iris::ROW_BYTES));
      assert(native==untouched); // Every input byte remains available to the JPEG decoder.
      const uint8_t expected_region[] = {
        uint8_t(r.x>>4),uint8_t((r.x<<4)|(r.y>>8)),uint8_t(r.y),
        uint8_t(r.w>>4),uint8_t((r.w<<4)|(r.h>>8)),uint8_t(r.h)
      };
      assert(memcmp(io.region,expected_region,sizeof(expected_region))==0);
    }
  }
  {
    // Raw bench calls and cropped calls share one controller/session. Switching
    // source layout must not reconfigure hardware or change old raw semantics.
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    assert(t.show_partial(frame.data(),frame.size(),0,0,1024,128));
    io.capture_pixels=true;
    io.expected_payload=257*130*3;
    const size_t packed_start=io.writes.size();
    assert(t.show_region(frame.data(),frame.size(),13,17,257,130));
    assert(io.writes[packed_start].command==0xb && io.dc_reads==2);
    assert(io.pixels[0]==17 && io.pixels[(257*130-1)*3]==146);
    assert(io.pixels.size()==257*130*3);
    const size_t next_start=io.writes.size();
    io.expected_payload=1024*128*3;
    assert(t.show_region(frame.data(),frame.size(),0,512,1024,128));
    assert(io.writes[next_start].command==0xb && io.dc_reads==2);
    io.expected_payload=iris::FRAME_BYTES;
    assert(t.show_partial(frame.data(),frame.size(),0,0,1024,128));
    assert(io.pixels==frame && io.dc_reads==2 && io.dc_restores==0);
    assert(t.show(frame.data(),frame.size()));
    assert(io.pixels==frame && io.dc_restores==1 && io.mode==1);
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    const size_t before=io.commands.size();
    for (const auto &r : std::vector<std::vector<uint16_t>>{
         {0,0,0,1},{0,0,1,0},{1024,0,1,1},{0,768,1,1},
         {1023,0,2,1},{0,767,1,2},{65535,0,2,1},{0,65535,1,2}})
      assert(!t.show_region(frame.data(),frame.size(),r[0],r[1],r[2],r[3]));
    assert(!t.show_region(nullptr,frame.size(),0,0,1,1));
    assert(!t.show_region(frame.data(),frame.size()-1,0,0,1,1));
    assert(io.commands.size()==before && t.ready && io.enabled);
  }
  for (uint8_t failed_command : {uint8_t(0x0b),uint8_t(0x09),uint8_t(0x0c),uint8_t(0x0d)}) {
    FakeIO io; io.capture_pixels=true; io.expected_payload=257*130*3;
    iris::Tcon<FakeIO> t(io); assert(t.begin());
    const auto untouched=frame;
    io.fail_write=failed_command;
    assert(!t.show_region(frame.data(),frame.size(),13,17,257,130));
    assert(!t.ready && !io.enabled && io.resets==1 && frame==untouched);
  }
  {
    FakeIO io; io.capture_pixels=true; io.expected_payload=257*130*3;
    iris::Tcon<FakeIO> t(io); assert(t.begin());
    io.done_bit=2; // A stale/wrong M1 bit is not completion of a cropped M3.
    assert(!t.show_region(frame.data(),frame.size(),13,17,257,130));
    assert(!strcmp(t.error,"TCON M3 completion timeout") && !io.enabled && io.resets==1);
  }
  {
    struct Region { uint16_t x,y,w,h; };
    const Region cases[] = {
      {128,256,256,128}, // The physical PACKED A comparator: exactly 32 full chunks.
      {13,17,257,130}, {50,60,37,19}, // Final short chunk and a single short chunk.
      {1023,767,1,1}, {0,0,1024,768}
    };
    for (bool fast : {false,true}) for (const auto &r : cases) {
      const size_t size=size_t(r.w)*r.h*3;
      std::vector<uint8_t> compact(size);
      for (size_t i=0;i<size;++i) compact[i]=uint8_t(i*17+(i/3)*11+(i/3072)*23);
      const auto untouched=compact;
      FakeIO io; io.capture_pixels=true; io.expected_payload=size;
      iris::Tcon<FakeIO> t(io); assert(t.begin());
      const size_t start=io.writes.size();
      assert(t.show_partial_compact(compact.data(),compact.size(),r.x,r.y,r.w,r.h,fast));
      assert(io.pixels==compact && compact==untouched && io.bytes==size);
      assert(io.rows==(size+iris::ROW_BYTES-1)/iris::ROW_BYTES);
      size_t sent=0, starts=0, ends=0;
      for(size_t i=start;i<io.writes.size();++i) {
        const auto &write=io.writes[i];
        if(write.command==0x09) ++starts;
        if(write.command==0x0c) {
          const size_t remaining=size-sent;
          const size_t expected=remaining<iris::ROW_BYTES?remaining:iris::ROW_BYTES;
          assert(write.size==expected && write.size%3==0);
          sent+=write.size;
        }
        if(write.command==0x0d) { ++ends; assert(sent==size && i==io.writes.size()-1); }
      }
      assert(starts==1 && ends==1 && sent==size);
      assert(t.status.bytes[2]==(fast?0x10:0x08));
      const auto &last_chunk=io.writes[io.writes.size()-2];
      assert(last_chunk.command==0x0c);
      assert(last_chunk.size==(size%iris::ROW_BYTES?size%iris::ROW_BYTES:iris::ROW_BYTES));
    }
  }
  {
    FakeIO io; iris::Tcon<FakeIO> t(io); assert(t.begin());
    const size_t before=io.commands.size();
    constexpr size_t valid=256*128*3;
    for(size_t bad : {size_t(0),valid-1,valid+1,size_t(iris::FRAME_BYTES)})
      assert(!t.show_partial_compact(frame.data(),bad,128,256,256,128));
    assert(!t.show_partial_compact(nullptr,valid,128,256,256,128));
    assert(!t.show_partial_compact(frame.data(),0,0,0,0,1));
    assert(!t.show_partial_compact(frame.data(),6,1023,0,2,1));
    assert(!t.show_partial_compact(frame.data(),6,0,767,1,2));
    assert(io.commands.size()==before && t.ready && io.enabled);
  }
  {
    // Length changes share the same partial configuration and restore M1 once.
    FakeIO io; io.capture_pixels=true; io.expected_payload=256*128*3;
    iris::Tcon<FakeIO> t(io); assert(t.begin());
    assert(t.show_partial_compact(frame.data(),io.expected_payload,128,256,256,128));
    io.expected_payload=iris::FRAME_BYTES;
    const size_t raw_start=io.writes.size();
    assert(t.show_partial(frame.data(),frame.size(),128,256,256,128));
    assert(io.writes[raw_start].command==0x0b && io.dc_reads==2 && io.pixels==frame);
    assert(t.show(frame.data(),frame.size()));
    assert(io.dc_restores==1 && io.mode==1 && io.pixels==frame);
  }
  for(uint8_t failed_command : {uint8_t(0x09),uint8_t(0x0c),uint8_t(0x0d)}) {
    FakeIO io; io.capture_pixels=true; io.expected_payload=257*130*3;
    iris::Tcon<FakeIO> t(io); assert(t.begin()); io.fail_write=failed_command;
    assert(!t.show_partial_compact(frame.data(),io.expected_payload,13,17,257,130));
    assert(!t.ready && !io.enabled && io.resets==1);
  }
  puts("IRIS TCON: M1/M3/M4 sessions, raw/packed/compact input, regions, faults, timeout, isolation passed");
}
