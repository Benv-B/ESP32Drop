#include <cassert>
#include <cstdio>
#include <vector>
#include "../ESP32Drop/examples/SerialPhotoBridge/rx_bench_consumer.h"

static uint32_t crc32(uint32_t crc,const uint8_t *p,uint32_t n) {
  crc=~crc;
  while(n--) { crc^=*p++; for(int i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1))); }
  return ~crc;
}
static void entry(std::vector<uint8_t> &out,const char *name,const std::vector<uint8_t> &data) {
  char h[111];
  snprintf(h,sizeof(h),"070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x",
    1u,0100644u,0u,0u,1u,0u,unsigned(data.size()),0u,0u,0u,0u,unsigned(strlen(name)+1),0u);
  out.insert(out.end(),h,h+110);out.insert(out.end(),name,name+strlen(name)+1);
  while(out.size()%4)out.push_back(0);
  out.insert(out.end(),data.begin(),data.end());while(out.size()%4)out.push_back(0);
}
int main() {
  std::vector<uint8_t> bytes,storage(4096);
  entry(bytes,"photo.jpg",{0xff,0xd8,0xff,0xd9});entry(bytes,"TRAILER!!!",{});
  RxBenchConsumer s;s.archive.data=storage.data();s.archive.capacity=storage.size();
  AdStreamPacket p{};p.kind=AD_STREAM_DATA;p.transfer_id=1;p.length=unsigned(bytes.size());
  memcpy(p.data,bytes.data(),bytes.size());assert(s.consume(p,crc32));assert(s.validated==0);
  p.kind=AD_STREAM_COMMIT;p.offset=p.length;p.length=0;p.crc=crc32(0,bytes.data(),unsigned(bytes.size()));
  assert(s.consume(p,crc32)&&s.validated==1&&s.jpeg_bytes==4);
  assert(!s.consume(p,crc32)&&s.validated==1); // duplicate COMMIT
  auto restart=[&] { ++p.transfer_id;p.kind=AD_STREAM_DATA;p.offset=0;p.length=unsigned(bytes.size());return s.consume(p,crc32); };
  auto commit=[&](uint32_t crc) { p.kind=AD_STREAM_COMMIT;p.offset=unsigned(bytes.size());p.length=0;p.crc=crc;return s.consume(p,crc32); };
  const uint32_t checksum=crc32(0,bytes.data(),unsigned(bytes.size()));
  assert(restart());assert(!commit(checksum^1));assert(s.validated==1);
  assert(restart());p.offset=1;assert(!s.consume(p,crc32));assert(!commit(checksum));
  assert(restart());p.length=sizeof(p.data)+1;assert(!s.consume(p,crc32));assert(!commit(checksum));
  assert(restart());p.kind=AD_STREAM_COMMIT;p.length=1;assert(!s.consume(p,crc32));assert(!commit(checksum));
  assert(restart());assert(commit(checksum));assert(s.validated==2);
  s.archive.capacity=bytes.size()-1;assert(!restart());assert(!commit(checksum));
  puts("Receive benchmark: complete archive required, CRC/offset/size failures rejected, recovery passed");
}
