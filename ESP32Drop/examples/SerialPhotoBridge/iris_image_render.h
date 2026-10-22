#pragma once
#include "iris_image_layout.h"

namespace iris {
struct RenderSpan { int32_t first, last; };
inline RenderSpan render_span(const Layout &l, uint32_t index, bool horizontal) {
  const uint32_t extent = horizontal ? l.dw : l.dh;
  const uint32_t source = horizontal ? l.ow : l.oh;
  const bool wide = horizontal ? l.wide_x : l.wide_y;
  const int32_t origin = horizontal ? l.x0 : l.y0;
  const int32_t limit = int32_t(horizontal ? l.view_width : l.view_height);
  RenderSpan s{int32_t(Layout::boundary(index,extent,source,wide))+origin,
               int32_t(Layout::boundary(index+1,extent,source,wide))+origin};
  if (s.first < 0) s.first = 0;
  if (s.last > limit) s.last = limit;
  return s;
}

// Reuse each axis mapping across an MCU tile. Color correction is needed only
// for source pixels selected by the existing inverse nearest-neighbour rule.
// Arbitrarily large ROM rectangles are tiled; no extra image allocation.
template<class Color>
void render_rgb_block(const Layout &l, uint8_t *frame, const uint8_t *rgb,
                      uint32_t left, uint32_t top, uint32_t width, uint32_t height,
                      const Color &color, const ToneCurve &tone) {
  constexpr uint32_t TILE = 16;
  const bool swapped = l.orientation >= 5;
  for (uint32_t by=0; by<height; by+=TILE) {
    const uint32_t nh = height-by < TILE ? height-by : TILE;
    RenderSpan ys[TILE];
    for (uint32_t y=0; y<nh; ++y) {
      uint32_t ox,oy; l.oriented(left,top+by+y,ox,oy);
      ys[y] = render_span(l,swapped ? ox : oy,swapped);
    }
    for (uint32_t bx=0; bx<width; bx+=TILE) {
      const uint32_t nw = width-bx < TILE ? width-bx : TILE;
      RenderSpan xs[TILE];
      for (uint32_t x=0; x<nw; ++x) {
        uint32_t ox,oy; l.oriented(left+bx+x,top,ox,oy);
        xs[x] = render_span(l,swapped ? oy : ox,!swapped);
      }
      for (uint32_t y=0; y<nh; ++y) {
        if (ys[y].first >= ys[y].last) continue;
        for (uint32_t x=0; x<nw; ++x) {
          if (xs[x].first >= xs[x].last) continue;
          const RenderSpan &xx = swapped ? ys[y] : xs[x];
          const RenderSpan &yy = swapped ? xs[x] : ys[y];
          uint8_t corrected[3], adjusted[3];
          color.rgb(rgb+((by+y)*width+bx+x)*3,corrected);
          tone.rgb(corrected,adjusted);
          for (int32_t dy=yy.first; dy<yy.last; ++dy) {
            const uint32_t vx=uint32_t(xx.first),vy=uint32_t(dy);
            ptrdiff_t start,step;
            switch (l.rotation) {
              case 90: start=(vx*WIDTH+WIDTH-1-vy)*3; step=ROW_BYTES; break;
              case 180: start=((HEIGHT-1-vy)*WIDTH+WIDTH-1-vx)*3; step=-3; break;
              case 270: start=((HEIGHT-1-vx)*WIDTH+vy)*3; step=-ptrdiff_t(ROW_BYTES); break;
              default: start=(vy*WIDTH+vx)*3; step=3;
            }
            uint8_t *dst=frame+start;
            for (int32_t dx=xx.first; dx<xx.last; ++dx) {
              dst[0]=adjusted[2]; dst[1]=adjusted[1]; dst[2]=adjusted[0];
              if (dx+1 < xx.last) dst+=step;
            }
          }
        }
      }
    }
  }
}
}
