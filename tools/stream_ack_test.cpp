#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>
#include "../ESP32Drop/src/airdrop/port/ad_port_esp32.h"
struct Queue { size_t size; std::deque<std::vector<uint8_t>> entries; };
using QueueHandle_t = Queue *;
struct StaticQueue_t {};
constexpr int pdTRUE=1, MALLOC_CAP_SPIRAM=0;
static uint32_t clock_ms=0;
uint32_t millis() { return clock_ms; }
uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
void *heap_caps_malloc(size_t n,int) { return malloc(n); }
void heap_caps_free(void *p) { free(p); }
QueueHandle_t xQueueCreate(int,size_t n) { return new Queue{n,{}}; }
QueueHandle_t xQueueCreateStatic(int,size_t n,uint8_t *storage,StaticQueue_t *) {
  free(storage); // the host simulator stores queue values in its own vectors
  return xQueueCreate(1,n);
}
static std::function<void(QueueHandle_t)> on_send;
int xQueueSend(QueueHandle_t q,const void *p,uint32_t) {
  const auto *b=static_cast<const uint8_t *>(p); q->entries.emplace_back(b,b+q->size);
  if(on_send) on_send(q);
  return pdTRUE;
}
int xQueueReceive(QueueHandle_t q,void *p,uint32_t wait) {
  if(q->entries.empty()) { clock_ms+=wait; return 0; }
  memcpy(p,q->entries.front().data(),q->size);q->entries.pop_front();return pdTRUE;
}
#include "../ESP32Drop/src/airdrop/port/ad_stream_esp32.h"
int main() {
  assert(ad_stream_enable());
  for(auto kind : {AD_STREAM_DATA,AD_STREAM_COMMIT}) {
    on_send=[](QueueHandle_t q) {
      if(q!=g_stream_req)return;
      AdStreamPacket packet{}; assert(ad_stream_read(&packet));
      ad_stream_reject(packet.token-1,AD_STREAM_ERROR_UNSUPPORTED,"stale rejection");
      char message[]="file over 4 MiB archive cap";
      ad_stream_reject(packet.token,AD_STREAM_ERROR_LIMIT,message);
      memset(message,'x',sizeof(message)-1); // queue must own the message bytes
    };
    assert(!stream_send(kind,7,0,nullptr,0,0));
    assert(g_stream_error==AD_STREAM_ERROR_LIMIT);
    assert(!strcmp(g_stream_error_message,"file over 4 MiB archive cap"));
    assert(!strcmp(ad_stream_error_http(g_stream_error),"413 Content Too Large"));
  }
  on_send=[](QueueHandle_t q) {
    if(q!=g_stream_req)return;
    AdStreamPacket packet{}; assert(ad_stream_read(&packet));
    ad_stream_ack(packet.token,true,1);
  };
  assert(stream_send(AD_STREAM_COMMIT,8,0,nullptr,0,0));
  assert(g_stream_files==1 && !g_stream_error_message[0]);
  on_send=nullptr;
  assert(!stream_send(AD_STREAM_DATA,9,0,nullptr,0,0));
  assert(g_stream_error==AD_STREAM_ERROR_OUTPUT && !g_stream_error_message[0]);
  assert(!strcmp(ad_stream_error_http(AD_STREAM_ERROR_UNSUPPORTED),"415 Unsupported Media Type"));
  assert(!strcmp(ad_stream_error_http(AD_STREAM_ERROR_INVALID),"400 Bad Request"));
  assert(!strcmp(ad_stream_error_http(AD_STREAM_ERROR_OUTPUT),"502 Bad Gateway"));
  puts("Stream ACK: typed DATA/COMMIT rejection, owned reason, stale token, success and timeout passed");
  delete g_stream_req; delete g_stream_ackq;
}
