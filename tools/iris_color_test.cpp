#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <algorithm>
#include "../ESP32Drop/examples/SerialPhotoBridge/iris_image_color.h"

using Bytes = std::vector<uint8_t>;
using Color = iris::ImageColor;
static void put32(Bytes &b, size_t o, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) b[o+i] = uint8_t(n >> (24-8*i));
}
static void fixed(Bytes &b, size_t o, double n) { put32(b,o,uint32_t(int32_t(lround(n*65536)))); }
static Bytes xyz(double x, double y, double z) {
  Bytes t(20); memcpy(t.data(),"XYZ ",4); fixed(t,8,x); fixed(t,12,y); fixed(t,16,z); return t;
}
static double linear(double v) { return v <= .04045 ? v/12.92 : pow((v+.055)/1.055,2.4); }
static uint8_t encode(double v) {
  v = std::max(0.0,std::min(1.0,v));
  return uint8_t(lround(255*(v <= .0031308 ? v*12.92 : 1.055*pow(v,1/2.4)-.055)));
}
static Bytes profile(bool p3 = true, bool sampled = false) {
  Bytes b(132+7*12); b[8]=4; memcpy(b.data()+12,"mntrRGB XYZ ",12);
  memcpy(b.data()+36,"acsp",4); fixed(b,68,.9642); fixed(b,72,1); fixed(b,76,.8249); put32(b,128,7);
  unsigned n = 0;
  auto add = [&](const char *sig, const Bytes &data) {
    while (b.size()%4) b.push_back(0);
    const size_t at = 132+12*n++;
    memcpy(b.data()+at,sig,4); put32(b,at+4,uint32_t(b.size())); put32(b,at+8,uint32_t(data.size()));
    b.insert(b.end(),data.begin(),data.end());
  };
  add("wtpt",xyz(.9642,1,.8249));
  add("rXYZ",p3 ? xyz(.51512146,.24119568,-.00105286) : xyz(.43607,.22250,.01393));
  add("gXYZ",p3 ? xyz(.29197693,.69224548,.04188538) : xyz(.38506,.71688,.09710));
  add("bXYZ",p3 ? xyz(.15710449,.06657410,.78407288) : xyz(.14308,.06062,.71417));
  Bytes trc(sampled ? 12+512 : 32);
  if (sampled) {
    memcpy(trc.data(),"curv",4); put32(trc,8,256);
    for (unsigned i=0;i<256;++i) {
      const uint16_t v = uint16_t(lround(linear(double(i)/255)*65535));
      trc[12+i*2] = uint8_t(v>>8); trc[13+i*2] = uint8_t(v);
    }
  } else {
    memcpy(trc.data(),"para",4); trc[9]=3;
    const double p[] = {2.4,1/1.055,.055/1.055,1/12.92,.04045};
    for (unsigned i=0;i<5;++i) fixed(trc,12+4*i,p[i]);
  }
  add("rTRC",trc); add("gTRC",trc); add("bTRC",trc);
  put32(b,0,uint32_t(b.size())); return b;
}
static void chunk(Bytes &j, const Bytes &p, size_t begin, size_t end, unsigned seq, unsigned count) {
  const unsigned size = unsigned(end-begin)+16;
  j.insert(j.end(),{0xff,0xe2,uint8_t(size>>8),uint8_t(size)});
  const char *signature="ICC_PROFILE";
  j.insert(j.end(),signature,signature+12); j.push_back(uint8_t(seq)); j.push_back(uint8_t(count));
  j.insert(j.end(),p.begin()+begin,p.begin()+end);
}
static Bytes jpeg(const Bytes &p, unsigned chunks=1, bool reverse=false) {
  Bytes j={0xff,0xd8};
  for (unsigned i=0;i<chunks;++i) {
    unsigned k=reverse ? chunks-1-i : i;
    chunk(j,p,p.size()*k/chunks,p.size()*(k+1)/chunks,k+1,chunks);
  }
  j.insert(j.end(),{0xff,0xd9}); return j;
}
static Color parse(const Bytes &b, Color::Status expected) {
  Color c; assert(c.initJpeg(b.data(),b.size()) == expected); return c;
}
static void identity(const Color &c) {
  for (unsigned r=0;r<256;r+=17) for (unsigned g=0;g<256;g+=17) for (unsigned b=0;b<256;b+=17) {
    uint8_t v[]={uint8_t(r),uint8_t(g),uint8_t(b)}, out[3]; c.rgb(v,out); assert(!memcmp(v,out,3));
  }
}
static void conversion(const Color &c) {
  assert(c.managed() && c.status() == Color::DisplayP3);
  // Independent double-precision matrices, transfer functions and rounding.
  // The firmware integer/LUT path must be within one 8-bit code value.
  unsigned max_error=0;
  for (unsigned r=0;r<256;r+=5) for (unsigned g=0;g<256;g+=5) for (unsigned b=0;b<256;b+=5) {
    const uint8_t input[]={uint8_t(r),uint8_t(g),uint8_t(b)};
    uint8_t out[3]; c.rgb(input,out);
    uint8_t inplace[3]; memcpy(inplace,input,3); c.rgb(inplace,inplace); assert(!memcmp(inplace,out,3));
    const double lr=linear(double(r)/255),lg=linear(double(g)/255),lb=linear(double(b)/255);
    const uint8_t ref[]={encode(1.2249401762805596*lr-.22494017628055993*lg),
      encode(-.04205695470968818*lr+1.042056954709688*lg),
      encode(-.019637554590334436*lr-.0786360455506319*lg+1.0982736001409663*lb)};
    for (unsigned ch=0;ch<3;++ch) {
      const unsigned e=unsigned(abs(int(out[ch])-int(ref[ch]))); max_error=std::max(max_error,e); assert(e<=1);
    }
  }
  for (unsigned n=0;n<256;++n) { uint8_t v[]={uint8_t(n),uint8_t(n),uint8_t(n)},out[3]; c.rgb(v,out); assert(!memcmp(v,out,3)); }
  // Independent LittleCMS/Pillow relative-colorimetric results using the actual
  // 536-byte Apple Display P3 profile supplied with the user's JPEG (2026-10-21).
  const uint8_t lcms[][6] = {{180,120,90,190,117,84},{80,140,200,56,142,205},
    {30,100,60,0,102,56},{12,50,240,0,51,250}};
  for (const auto &sample : lcms) {
    uint8_t out[3]; c.rgb(sample,out);
    for (unsigned ch=0;ch<3;++ch) assert(abs(int(out[ch])-int(sample[ch+3]))<=1);
  }
  printf("P3 conversion: cube max error %u/255; all neutral levels exact\n",max_error);
}
int main(int argc,char **argv) {
  static_assert(sizeof(Color)<=4,"Color management state must stay small on the decode task stack");
  identity(parse({0xff,0xd8,0xff,0xd9},Color::Unprofiled));
  const Bytes p=profile(), j=jpeg(p);
  const Color p3=parse(j,Color::DisplayP3); conversion(p3);
  assert(!strcmp(p3.profileName(),"Display P3"));
  identity(parse(jpeg(profile(false)),Color::SRGB));
  identity(parse(jpeg(profile(false,true)),Color::SRGB));
  for (unsigned n=2;n<=8;++n) { parse(jpeg(p,n),Color::DisplayP3); parse(jpeg(p,n,true),Color::DisplayP3); }
  // Every possible single split also checks multi-byte reads across APP2 chunks.
  for (size_t cut=1;cut<p.size();++cut) {
    Bytes split={0xff,0xd8}; chunk(split,p,cut,p.size(),2,2); chunk(split,p,0,cut,1,2);
    split.insert(split.end(),{0xff,0xd9}); parse(split,Color::DisplayP3);
  }
  for (size_t size=0;size<j.size();++size) { Color c; assert(c.initJpeg(j.data(),size)!=Color::DisplayP3); }
  Bytes bad=j; bad[18]=2; parse(bad,Color::InvalidProfile); // seq > total
  bad=j; bad[19]=2; parse(bad,Color::InvalidProfile); // missing second chunk
  bad=j; bad[19]=9; parse(bad,Color::UnsupportedProfile); // explicit parser resource bound
  bad={0xff,0xd8}; chunk(bad,p,0,p.size()/2,1,2); chunk(bad,p,p.size()/2,p.size(),1,2);
  bad.insert(bad.end(),{0xff,0xd9}); parse(bad,Color::InvalidProfile);
  Bytes altered=p; put32(altered,136,0xfffffff0U); parse(jpeg(altered),Color::InvalidProfile);
  altered=p; put32(altered,140,0xffffffffU); parse(jpeg(altered),Color::InvalidProfile);
  altered=p; put32(altered,128,0xffffffffU); parse(jpeg(altered),Color::InvalidProfile);
  altered=p; memcpy(altered.data()+144,"wtpt",4); parse(jpeg(altered),Color::InvalidProfile);
  altered=p; put32(altered,148,220); parse(jpeg(altered),Color::InvalidProfile); // partial tag overlap
  altered=p; altered[36]='x'; parse(jpeg(altered),Color::InvalidProfile);
  altered=p; altered[8]=5; identity(parse(jpeg(altered),Color::UnsupportedProfile));
  altered=p; memcpy(altered.data()+16,"CMYK",4); identity(parse(jpeg(altered),Color::UnsupportedProfile));
  altered=p; fixed(altered,68,.95); identity(parse(jpeg(altered),Color::UnsupportedProfile)); // non-D50 PCS
  // Numeric profile changes cannot be concealed by a familiar name/string.
  altered=p; fixed(altered,216+20+8,.6); identity(parse(jpeg(altered),Color::UnsupportedProfile));
  altered=p; fixed(altered,216+80+12,2.2); identity(parse(jpeg(altered),Color::UnsupportedProfile));
  altered=p; memcpy(altered.data()+132+6*12,"A2B0",4); identity(parse(jpeg(altered),Color::UnsupportedProfile));
  // Entropy after SOS must not be interpreted as another ICC marker.
  bad=j; bad.resize(bad.size()-2); bad.insert(bad.end(),{0xff,0xda,0,2,0xff,0xe2,0,0}); parse(bad,Color::DisplayP3);
  // Resource-bounded parser fuzz: arbitrary input must not crash/read outside it.
  uint32_t seed=0x12345678;
  for (unsigned n=0;n<5000;++n) {
    bad=j; seed=seed*1664525+1013904223; const size_t index=seed%bad.size();
    seed=seed*1664525+1013904223; bad[index]^=uint8_t(seed>>24); Color c; c.initJpeg(bad.data(),bad.size());
  }
  for (int i=1;i<argc;++i) {
    std::ifstream f(argv[i],std::ios::binary); assert(f.good());
    const Bytes real((std::istreambuf_iterator<char>(f)),{});
    Color c; c.initJpeg(real.data(),real.size());
    printf("Real file: %s => %s / %s\n",argv[i],c.profileName(),c.statusName());
    assert(c.status()==Color::DisplayP3); conversion(c);
  }
  puts("iris_color tests passed");
}
