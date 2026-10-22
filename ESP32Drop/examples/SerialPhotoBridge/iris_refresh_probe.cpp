#ifdef PHOTO_FRAME_TCON
#include <Arduino.h>
#include <string.h>
#include "iris_refresh_probe.h"
#include "iris_tcon.h"
#include "iris_tcon_protocol.h"

namespace {
bool base_ready = false;
void rectangle(uint8_t *frame, unsigned x, unsigned y, unsigned w, unsigned h,
               uint8_t r, uint8_t g, uint8_t b) {
  for (unsigned row=y; row<y+h; ++row) for (unsigned col=x; col<x+w; ++col) {
    auto p=frame+(row*iris::WIDTH+col)*3;
    p[0]=b; p[1]=g; p[2]=r;
  }
}
void base(uint8_t *frame) {
  memset(frame, 224, iris::FRAME_BYTES);
  for (unsigned x=0;x<iris::WIDTH;x+=64) rectangle(frame,x,0,2,iris::HEIGHT,64,64,64);
  for (unsigned y=0;y<iris::HEIGHT;y+=64) rectangle(frame,0,y,iris::WIDTH,2,64,64,64);
  rectangle(frame,32,32,96,96,255,0,0);
  rectangle(frame,896,32,96,96,0,255,0);
  rectangle(frame,32,640,96,96,0,0,255);
  rectangle(frame,896,640,96,96,0,0,0);
}
}
bool iris_refresh_probe(const char *action, uint8_t *frame, const char **error) {
  if (error) *error="";
  if (!frame) { if(error)*error="test framebuffer unavailable"; return false; }
  uint16_t x=0,y=0,w=0,h=0;
  bool fast=false, is_base=!strcmp(action,"BASE"), is_origin=false, is_packed=false, native=false, compact=false;
  if (!is_base) {
    if (!strcmp(action,"M3 A")) { x=128;y=256;w=256;h=128; }
    else if (!strcmp(action,"M3 B")) { x=640;y=480;w=256;h=128; }
    else if (!strcmp(action,"M3 ORIGIN")) { x=0;y=384;w=1024;h=128;is_origin=true; }
    else if (!strcmp(action,"M3 PACKED A")) { x=128;y=256;w=256;h=128;is_packed=true; }
    else if (!strcmp(action,"M3 SHORT A")) { x=128;y=256;w=256;h=128;is_packed=true;compact=true; }
    else if (!strcmp(action,"M3 SHORT B")) { x=640;y=480;w=256;h=128;is_packed=true;compact=true; }
    else if (!strcmp(action,"M3 SHORT V")) { x=640;y=128;w=128;h=256;is_packed=true;compact=true; }
    else if (!strcmp(action,"M3 NATIVE A")) { x=128;y=256;w=256;h=128;native=true; }
    else if (!strcmp(action,"M3 NATIVE B")) { x=640;y=480;w=256;h=128;native=true; }
    else if (!strcmp(action,"M3 NATIVE V")) { x=640;y=128;w=128;h=256;native=true; }
    else if (!strcmp(action,"M4 A")) { x=128;y=256;w=256;h=128;fast=true; }
    else if (!strcmp(action,"M4 B")) { x=640;y=480;w=256;h=128;fast=true; }
    else { if(error)*error="use TEST PARTIAL BASE|M3 A/B/ORIGIN/PACKED A/SHORT A/B/V/NATIVE A/B/V|M4 A/B"; return false; }
    if (!base_ready) { if(error)*error="run TEST PARTIAL BASE first"; return false; }
  }
  const uint32_t start=millis();
  if (Serial) Serial.printf("IRIS BENCH: start %s region=%u,%u,%u,%u\n",action,x,y,w,h);
  if(is_base) base(frame);
  else if(is_origin) {
    // Deliberately different source-origin and native-target contents distinguish
    // relocation from native cropping. Full width rules out row-stride changes;
    // the transport still sends every full 1024-pixel row (768 rows in total).
    base(frame);
    rectangle(frame,0,0,iris::WIDTH,128,255,0,0);
    rectangle(frame,32,32,64,64,255,255,255);
    rectangle(frame,x,y,w,h,0,0,255);
    rectangle(frame,768,y+32,32,64,0,0,0);
    rectangle(frame,800,y+32,32,64,255,255,255);
    if(Serial)Serial.println("IRIS BENCH: ORIGIN target blue=native crop; target red=source-origin relocation; outside top changed=mask ineffective");
  }
  else if(is_packed) {
    // Test the unconfirmed packed-W*H input hypothesis without shortening the
    // existing transport. Only the first w*h pixels contain the diagnostic tile;
    // all remaining bytes are neutral padding, still sent as 768 full rows.
    memset(frame,224,iris::FRAME_BYTES);
    for(unsigned row=0;row<h;++row) for(unsigned col=0;col<w;++col) {
      const bool white=row>=8 && row<40 && col>=8 && col<40;
      const bool black=col>=w/2 && col<w/2+4;
      auto p=frame+(row*w+col)*3;
      p[0]=white?255:(!black && x!=128?255:0);
      p[1]=white?255:0;
      p[2]=white?255:(!black && x==128?255:0);
    }
    if(Serial)Serial.printf("IRIS BENCH: PACKED hypothesis: target red tile with white corner and black centre; payload=%lu bytes (%s)\n",
      (unsigned long)(compact?uint32_t(w)*h*3:iris::FRAME_BYTES),compact?"exact ROI length experiment":"full-frame padding");
  }
  else {
    // A red/yellow block or a blue/cyan block, with a white corner and black
    // centre marker, makes offsets and accidental scaling visually apparent.
    rectangle(frame,x,y,w,h,x==128?255:0,fast?255:0,x==128?0:255);
    rectangle(frame,x+8,y+8,32,32,255,255,255);
    rectangle(frame,x+w/2,y,4,h,0,0,0);
  }
  const bool ok=is_base?iris_tcon_show(frame,iris::FRAME_BYTES):
    compact?iris_tcon_show_partial_compact(frame,size_t(w)*h*3,x,y,w,h,fast):
    native?iris_tcon_show_region(frame,iris::FRAME_BYTES,x,y,w,h,fast):
    iris_tcon_show_partial(frame,iris::FRAME_BYTES,x,y,w,h,fast);
  base_ready=ok;
  if(error)*error=ok?"":iris_tcon_error();
  uint8_t ints[4];iris_tcon_status(ints);
  if(Serial)Serial.printf("IRIS BENCH: end %s ok=%u elapsed=%lu ints=%02X %02X %02X %02X error=%s\n",
    action,unsigned(ok),(unsigned long)(millis()-start),ints[0],ints[1],ints[2],ints[3],ok?"":iris_tcon_error());
  return ok;
}
#endif
