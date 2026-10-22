#include <assert.h>
#include <stdio.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <vector>
#include "../ESP32Drop/examples/SerialPhotoBridge/iris_image_layout.h"
#include "../ESP32Drop/examples/SerialPhotoBridge/iris_preview_region.h"
#include "../ESP32Drop/examples/SerialPhotoBridge/iris_image_render.h"

static void tone_test() {
  iris::ToneCurve original(0), gentle(1), bright(2);
  uint8_t previous[3] = {};
  for (unsigned value = 0; value <= 255; ++value) {
    const uint8_t input[] = {uint8_t(value), uint8_t(value), uint8_t(value)};
    unsigned level = 0;
    for (const auto *curve : {&original, &gentle, &bright}) {
      uint8_t out[3]; curve->rgb(input, out);
      assert(out[0] == out[1] && out[1] == out[2]);
      assert(out[0] >= previous[level]);
      previous[level] = out[0];
      const unsigned amount = level == 1 ? 128 : level == 2 ? 192 : 0;
      const unsigned expected = value + (amount * value * (255 - value) + 32512) / 65025;
      assert(out[0] == expected);
      if (value == 0 || value == 255) assert(out[0] == value);
      ++level;
    }
  }
  // RGB cube samples include saturated colors, near black, mixed highlights
  // and neutrals. Compare shared-channel scaling to an independent division.
  for (unsigned r = 0; r <= 255; r += 15)
    for (unsigned g = 0; g <= 255; g += 15)
      for (unsigned b = 0; b <= 255; b += 15) {
        const uint8_t input[] = {uint8_t(r), uint8_t(g), uint8_t(b)};
        uint8_t exact[3], one[3], two[3];
        original.rgb(input, exact); gentle.rgb(input, one); bright.rgb(input, two);
        assert(!memcmp(input, exact, 3));
        const unsigned peak = std::max(r, std::max(g, b));
        for (unsigned c = 0; c < 3; ++c) {
          assert(one[c] >= input[c] && two[c] >= one[c]);
          if (!input[c]) assert(!one[c] && !two[c]);
          if (!peak) continue;
          for (unsigned level = 1; level <= 2; ++level) {
            const unsigned amount = level == 1 ? 128 : 192;
            const unsigned lifted = peak + (amount * peak * (255-peak) + 32512) / 65025;
            const int expected = int((unsigned(input[c]) * lifted + peak / 2) / peak);
            const int actual = level == 1 ? one[c] : two[c];
            assert(actual >= expected-1 && actual <= expected+1);
          }
        }
      }
}

static void layout_test(uint32_t w, uint32_t h, uint8_t orientation,
                        uint16_t rotation, uint8_t brightness = 0, bool crop = false) {
  iris::Layout l(w,h,orientation,rotation,crop);
  iris::ToneCurve tone(brightness);
  const uint32_t ow = orientation >= 5 ? h : w;
  const uint32_t oh = orientation >= 5 ? w : h;
  const uint16_t effective = rotation == IRIS_ROTATION_AUTO ? (ow >= oh ? 0 : 90) : rotation;
  const uint32_t vw = (effective % 180) ? 768 : 1024;
  const uint32_t vh = (effective % 180) ? 1024 : 768;
  assert(l.rotation == effective);
  // Independent contain/cover scale. Floating point is used only in this host
  // oracle; the MCU uses integer arithmetic and never allocates a virtual image.
  const long double sx = static_cast<long double>(vw)/ow, sy = static_cast<long double>(vh)/oh;
  const long double scale = crop ? std::max(sx,sy) : std::min(sx,sy);
  const uint32_t dw = std::max(1u,uint32_t(ow*scale+1e-7L));
  const uint32_t dh = std::max(1u,uint32_t(oh*scale+1e-7L));
  const int32_t x0 = (int32_t(vw)-int32_t(dw))/2, y0 = (int32_t(vh)-int32_t(dh))/2;
  assert(l.dw == dw && l.dh == dh && l.view_width == vw && l.view_height == vh);
  std::vector<uint8_t> frame(iris::FRAME_BYTES + 32, 0xcd);
  memset(frame.data()+16, 255, iris::FRAME_BYTES);
  for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
    uint8_t rgb[] = {uint8_t(x),uint8_t(y),uint8_t(x+y)};
    l.pixel(frame.data()+16,x,y,rgb,&tone);
  }
  struct CountColor {
    mutable uint32_t calls=0;
    void rgb(const uint8_t *in,uint8_t *out) const { ++calls; memcpy(out,in,3); }
  } color;
  std::vector<uint8_t> tiled(iris::FRAME_BYTES+32,0xcd);
  memset(tiled.data()+16,255,iris::FRAME_BYTES);
  // Odd blocks larger than a JPEG MCU also exercise tiling and short edges.
  for (uint32_t top=0; top<h; top+=19) for (uint32_t left=0; left<w; left+=23) {
    const uint32_t bw=std::min(23u,w-left),bh=std::min(19u,h-top);
    uint8_t block[23*19*3];
    for(uint32_t y=0;y<bh;++y) for(uint32_t x=0;x<bw;++x) {
      uint8_t *p=block+(y*bw+x)*3;
      p[0]=uint8_t(left+x);p[1]=uint8_t(top+y);p[2]=uint8_t(left+x+top+y);
    }
    iris::render_rgb_block(l,tiled.data()+16,block,left,top,bw,bh,color,tone);
  }
  assert(tiled==frame); // Includes canaries and untouched contain margins.
  assert(color.calls<=w*h && color.calls<=iris::WIDTH*iris::HEIGHT);
  if(w==1031 && h==773) assert(color.calls<w*h);
  for (int i = 0; i < 16; ++i) assert(frame[i] == 0xcd && frame[frame.size()-1-i] == 0xcd);
  for (uint32_t y = 0; y < iris::HEIGHT; ++y) for (uint32_t x = 0; x < iris::WIDTH; ++x) {
    const uint8_t *p = frame.data()+16+(y*iris::WIDTH+x)*3;
    // Invert the physical mounting first; fit and EXIF are in viewing space.
    uint32_t vx = 0, vy = 0;
    switch (effective) {
      case 0: vx=x; vy=y; break;
      case 90: vx=y; vy=1023-x; break;
      case 180: vx=1023-x; vy=767-y; break;
      case 270: vx=767-y; vy=x; break;
      default: assert(false);
    }
    if (int64_t(vx)<x0 || int64_t(vy)<y0 || int64_t(vx)>=int64_t(x0)+dw || int64_t(vy)>=int64_t(y0)+dh) {
      assert(!crop); // COVER must map every native pixel to the original image.
      assert(p[0] == 255 && p[1] == 255 && p[2] == 255); continue;
    }
    const uint32_t ox=uint32_t(uint64_t(int64_t(vx)-x0)*ow/dw);
    const uint32_t oy=uint32_t(uint64_t(int64_t(vy)-y0)*oh/dh);
    uint32_t sx=0,sy=0;
    switch (orientation) {
      case 1:sx=ox;sy=oy;break;
      case 2:sx=w-1-ox;sy=oy;break;
      case 3:sx=w-1-ox;sy=h-1-oy;break;
      case 4:sx=ox;sy=h-1-oy;break;
      case 5:sx=oy;sy=ox;break;
      case 6:sx=oy;sy=h-1-ox;break;
      case 7:sx=w-1-oy;sy=h-1-ox;break;
      case 8:sx=w-1-oy;sy=ox;break;
      default: assert(false);
    }
    uint8_t rgb[] = {uint8_t(sx),uint8_t(sy),uint8_t(sx+sy)}, adjusted[3];
    tone.rgb(rgb,adjusted);
    assert(p[0]==adjusted[2] && p[1]==adjusted[1] && p[2]==adjusted[0]);
  }
}

static void auto_rotation_test() {
  assert(IrisImageOptions{}.rotation == IRIS_ROTATION_AUTO);
  assert(IrisImageOptions{}.crop);
  for (auto wh : {std::pair<uint32_t,uint32_t>{7,5}, {5,7}, {9,9}, {1,257}, {257,1}})
    for (uint8_t orientation = 1; orientation <= 8; ++orientation)
      for (bool crop : {false,true}) {
      const uint32_t w = wh.first, h = wh.second;
      const uint32_t ow = orientation >= 5 ? h : w;
      const uint32_t oh = orientation >= 5 ? w : h;
      const uint16_t expected = ow >= oh ? 0 : 90;
      iris::Layout automatic(w,h,orientation,IRIS_ROTATION_AUTO,crop), fixed(w,h,orientation,expected,crop);
      std::vector<uint8_t> a(iris::FRAME_BYTES,255), b(iris::FRAME_BYTES,255);
      for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        const uint8_t pixel[] = {uint8_t(x*17),uint8_t(y*31),uint8_t(x+y)};
        automatic.pixel(a.data(),x,y,pixel); fixed.pixel(b.data(),x,y,pixel);
      }
      assert(a == b);
      layout_test(w,h,orientation,IRIS_ROTATION_AUTO,0,crop);
    }
  for (uint8_t orientation : {6,8}) {
    assert(iris::Layout(4000,3000,orientation).rotation == 90);
    assert(iris::Layout(3000,4000,orientation).rotation == 0);
  }
  // Decoder must keep the original decision if scaling makes dimensions equal.
  const iris::Layout full(2048,2049,1);
  const iris::Layout scaled(2048 >> 1,2049 >> 1,1,full.rotation);
  assert(full.rotation == 90 && scaled.rotation == 90);
}

static void completed_rows_test(uint32_t w, uint32_t h, uint8_t orientation,
                                uint16_t rotation, bool crop) {
  iris::Layout layout(w,h,orientation,rotation,crop);
  std::vector<uint8_t> frame(iris::FRAME_BYTES,255);
  for (uint32_t y=0;y<h;++y) for (uint32_t x=0;x<w;++x) {
    const uint8_t pixel[]={11,uint8_t(y),13};
    layout.pixel(frame.data(),x,y,pixel);
  }
  // pixel()'s full image is already checked against the independent inverse
  // map above. Here its encoded source row tells which output pixels actually
  // exist after each partial source prefix, including rotated/cropped edges.
  for (uint32_t rows : {0u,1u,h/2,h-1,h,h+1}) {
    const IrisImageRegion region=layout.completed_rows(rows);
    assert(uint32_t(region.x)+region.width<=iris::WIDTH);
    assert(uint32_t(region.y)+region.height<=iris::HEIGHT);
    for (uint32_t y=0;y<iris::HEIGHT;++y) for (uint32_t x=0;x<iris::WIDTH;++x) {
      const auto *p=frame.data()+(y*iris::WIDTH+x)*3;
      const bool expected=p[0]!=255 && p[1]<rows;
      const bool inside=x>=region.x && x<uint32_t(region.x)+region.width &&
                        y>=region.y && y<uint32_t(region.y)+region.height;
      assert(expected==inside);
    }
  }
}

static void preview_delta_edges_test() {
  using State = iris::PreviewRegionDelta::State;
  const auto check = [](IrisImageRegion old, IrisImageRegion current, State state,
                        IrisImageRegion expected = {}) {
    const auto result = iris::preview_region_delta(old,current);
    assert(result.state == state);
    assert(result.region.x == expected.x && result.region.y == expected.y &&
           result.region.width == expected.width && result.region.height == expected.height);
  };
  check({}, {}, State::Empty);
  check({1024,768,0,0},{0,0,0,768},State::Empty);
  check({}, {0,0,1024,768},State::Strip,{0,0,1024,768});
  check({0,0,1024,768},{0,0,1024,768},State::Empty);
  check({0,0,1024,768},{},State::Invalid);
  const IrisImageRegion old{100,100,200,200};
  check(old,{100,50,200,250},State::Strip,{100,50,200,50});
  check(old,{100,100,200,250},State::Strip,{100,300,200,50});
  check(old,{50,100,250,200},State::Strip,{50,100,50,200});
  check(old,{100,100,250,200},State::Strip,{300,100,50,200});
  check(old,{100,100,199,200},State::Invalid); // shrink
  check(old,{101,100,200,200},State::Invalid); // overlap, not nested
  check(old,{400,400,200,200},State::Invalid); // disjoint
  check(old,{50,100,300,200},State::Invalid); // left and right
  check(old,{100,50,200,300},State::Invalid); // top and bottom
  check(old,{50,50,250,250},State::Invalid); // L-shaped difference
  check({1024,0,1,1},{},State::Invalid); // malformed old, including empty new
  check({},{1024,0,1,1},State::Invalid);
  check({},{0,768,1,1},State::Invalid);
  check({},{65535,65535,65535,65535},State::Invalid); // no uint16 overflow
  check({1025,0,0,0},{},State::Invalid); // even empty coordinates are checked
  check({0,769,0,0},{},State::Invalid);
  check({0,0,0,769},{},State::Invalid);
  check({1023,767,1,1},{1022,767,2,1},State::Strip,{1022,767,1,1});
  assert(iris::preview_region_delta({}, {}, 0, 768).state == State::Invalid);
  assert(iris::preview_region_delta({}, {}, 1024, 0).state == State::Invalid);
  const iris::Layout cropped(9,16,1,IRIS_ROTATION_AUTO,true);
  assert(iris::preview_region_delta({},cropped.completed_rows(1)).state==State::Empty);
  assert(iris::preview_region_delta({},cropped.completed_rows(2)).state==State::Strip);
  assert(iris::preview_region_delta(cropped.completed_rows(15),cropped.completed_rows(16)).state==State::Empty);
  // Exhaust all rectangles on a small canvas against an independent set
  // difference oracle. A valid result must be empty or exactly one rectangle.
  std::vector<IrisImageRegion> rectangles(1);
  constexpr uint16_t extent = 4;
  for (uint16_t y=0;y<extent;++y) for (uint16_t x=0;x<extent;++x)
    for (uint16_t h=1;h<=extent-y;++h) for (uint16_t w=1;w<=extent-x;++w)
      rectangles.push_back({x,y,w,h});
  const auto inside = [](const IrisImageRegion &r, uint16_t x, uint16_t y) {
    return x>=r.x && x<uint32_t(r.x)+r.width && y>=r.y && y<uint32_t(r.y)+r.height;
  };
  for (const auto &a : rectangles) for (const auto &b : rectangles) {
    bool nested=true;
    uint16_t xa=extent,ya=extent,xb=0,yb=0,count=0;
    for (uint16_t y=0;y<extent;++y) for (uint16_t x=0;x<extent;++x) {
      const bool old_pixel=inside(a,x,y), new_pixel=inside(b,x,y);
      if (old_pixel && !new_pixel) nested=false;
      if (new_pixel && !old_pixel) {
        ++count; xa=std::min(xa,x); ya=std::min(ya,y);
        xb=std::max(xb,uint16_t(x+1)); yb=std::max(yb,uint16_t(y+1));
      }
    }
    const auto result=iris::preview_region_delta(a,b,extent,extent);
    const bool rectangular=!count || uint32_t(xb-xa)*(yb-ya)==count;
    const State expected=!nested || !rectangular ? State::Invalid : count ? State::Strip : State::Empty;
    assert(result.state==expected);
    if (expected==State::Strip) {
      assert(result.region.x==xa && result.region.y==ya &&
             result.region.width==xb-xa && result.region.height==yb-ya);
    } else assert(!result.region.x && !result.region.y && !result.region.width && !result.region.height);
  }
}

static void preview_delta_layout_test(uint32_t w, uint32_t h, uint8_t orientation,
                                      uint16_t rotation, bool crop, uint32_t confirm_step) {
  using State = iris::PreviewRegionDelta::State;
  iris::Layout layout(w,h,orientation,rotation,crop);
  std::vector<uint8_t> seen(iris::WIDTH*iris::HEIGHT,0);
  IrisImageRegion confirmed;
  uint32_t count=0;
  for (uint32_t rows=0;rows<=h+1;++rows) {
    const IrisImageRegion current=layout.completed_rows(rows);
    const auto delta=iris::preview_region_delta(confirmed,current);
    assert(delta.state!=State::Invalid);
    // Simulate callbacks skipped while a batch is too small or the display is
    // busy. Advancing confirmed coverage here would silently lose these rows.
    if (rows<h && rows%confirm_step) continue;
    if (delta.state==State::Strip) {
      const auto &r=delta.region;
      assert(r.width && r.height && uint32_t(r.x)+r.width<=iris::WIDTH &&
             uint32_t(r.y)+r.height<=iris::HEIGHT);
      assert(r.x>=current.x && r.y>=current.y &&
             uint32_t(r.x)+r.width<=uint32_t(current.x)+current.width &&
             uint32_t(r.y)+r.height<=uint32_t(current.y)+current.height);
      for (uint32_t y=r.y;y<uint32_t(r.y)+r.height;++y)
        for (uint32_t x=r.x;x<uint32_t(r.x)+r.width;++x) {
          auto &pixel=seen[y*iris::WIDTH+x];
          assert(!pixel); // Never revisit a previously displayed pixel.
          pixel=1; ++count;
        }
    }
    assert(count==uint32_t(current.width)*current.height);
    confirmed=current;
  }
  // Compare the union with actual final pixel destinations, not just another
  // completed_rows call. Includes CONTAIN borders and COVER's clipped rows.
  std::vector<uint8_t> frame(iris::FRAME_BYTES,255);
  const uint8_t rgb[]={11,13,17};
  for (uint32_t y=0;y<h;++y) for (uint32_t x=0;x<w;++x)
    layout.pixel(frame.data(),x,y,rgb);
  for (uint32_t i=0;i<seen.size();++i) assert(bool(seen[i])==(frame[i*3]!=255));
}

int main() {
  std::ifstream file("testdata/iris_baseline.jpg", std::ios::binary);
  std::vector<uint8_t> fixture((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  assert(!fixture.empty());
  iris::JpegInfo info;
  assert(iris::jpeg_info(fixture.data(), fixture.size(), info));
  assert(info.width && info.height && info.orientation == 1);
  size_t header_end=0;
  for (size_t n=0;n<=fixture.size();++n) {
    const auto result=iris::jpeg_header_info(fixture.data(),n,info);
    assert(result!=iris::JpegHeaderResult::Invalid);
    if (result==iris::JpegHeaderResult::Ready) {
      if (!header_end) header_end=n;
      assert(info.width && info.height);
    } else assert(!header_end);
  }
  assert(header_end && header_end<fixture.size()-2);
  assert(!iris::jpeg_info(fixture.data(),header_end,info));
  assert(iris::jpeg_header_info(nullptr,0,info)==iris::JpegHeaderResult::Invalid);
  const uint8_t malformed[]={0xff,0xd8,0xff,0xda,0,2};
  assert(iris::jpeg_header_info(malformed,sizeof(malformed),info)==iris::JpegHeaderResult::Invalid);
  for (size_t n = 0; n < fixture.size(); ++n)
    assert(!iris::jpeg_info(fixture.data(), n, info));
  std::vector<uint8_t> changed(fixture);
  for (size_t p = 1; p < changed.size(); ++p)
    if (changed[p-1] == 0xff && changed[p] == 0xc0) { changed[p] = 0xc2; break; }
  assert(!iris::jpeg_info(changed.data(), changed.size(), info));
  assert(iris::jpeg_header_info(changed.data(),changed.size(),info)==iris::JpegHeaderResult::Invalid);
  uint8_t le[] = {'I','I',42,0,8,0,0,0,1,0,0x12,1,3,0,1,0,0,0,6,0,0,0};
  uint8_t be[] = {'M','M',0,42,0,0,0,8,0,1,1,0x12,0,3,0,0,0,1,0,8,0,0};
  assert(iris::tiff_orientation(le, sizeof(le)) == 6);
  assert(iris::tiff_orientation(be, sizeof(be)) == 8);
  for (size_t n = 0; n < sizeof(le); ++n) assert(iris::tiff_orientation(le, n) == 1);
  le[4] = 0xff; le[5] = 0xff; le[6] = 0xff; le[7] = 0xff;
  assert(iris::tiff_orientation(le, sizeof(le)) == 1);
  // Every output pixel must agree with an independently computed inverse map.
  // Odd dimensions, up/downscaling, thin images, all eight EXIF transforms,
  // four mount rotations, full portrait fit and untouched white borders.
  for (auto wh : {std::pair<uint32_t,uint32_t>{7,5}, {1031,773}, {5,7}, {3,4}, {1,257}})
    for (uint8_t orientation = 1; orientation <= 8; ++orientation)
      for (uint16_t rotation : {0,90,180,270})
        layout_test(wh.first,wh.second,orientation,rotation);
  iris::Layout portrait(3,4,1,90);
  assert(portrait.dw == 768 && portrait.dh == 1024 && !portrait.x0 && !portrait.y0);
  iris::Layout exif_portrait(4,3,6,90);
  assert(exif_portrait.dw == 768 && exif_portrait.dh == 1024);
  auto_rotation_test();
  for (auto wh : {std::pair<uint32_t,uint32_t>{7,5}, {1031,773}, {5,7}, {9,16}, {16384,1}})
    for (uint8_t orientation = 1; orientation <= 8; ++orientation)
      for (uint16_t rotation : {0,90,180,270,360})
        layout_test(wh.first,wh.second,orientation,rotation,0,true);
  const iris::Layout tall_cover(9,16,1,IRIS_ROTATION_AUTO,true);
  assert(tall_cover.dw == 768 && tall_cover.dh == 1365 && tall_cover.y0 == -170);
  const iris::Layout extreme_cover(16384,1,1,0,true);
  assert(extreme_cover.wide_x && extreme_cover.dw == 12582912);
  tone_test();
  for (uint8_t brightness : {1,2}) layout_test(257,131,6,90,brightness);
  for (auto wh : {std::pair<uint32_t,uint32_t>{7,5},{5,7}})
    for (uint8_t orientation=1;orientation<=8;++orientation)
      for (uint16_t rotation : {0,90,180,270,360})
        for (bool crop : {false,true})
          completed_rows_test(wh.first,wh.second,orientation,rotation,crop);
  preview_delta_edges_test();
  for (auto wh : {std::pair<uint32_t,uint32_t>{7,5},{5,7},{9,16},{1031,773},{1,257},{257,1}})
    for (uint8_t orientation=1;orientation<=8;++orientation)
      for (uint16_t rotation : {0,90,180,270,360})
        for (bool crop : {false,true})
          for (uint32_t confirm_step : {1u,19u})
            preview_delta_layout_test(wh.first,wh.second,orientation,rotation,crop,confirm_step);
  puts("IRIS image: JPEG preflight, EXIF bounds, 8 EXIF x 4 mount rotations, AUTO, CONTAIN/COVER, BGR, tone and disjoint preview strips passed");
}
