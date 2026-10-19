#pragma once

/* One outstanding chunk, copied into PSRAM. Neither queue contains borrowed data.
 * A late ACK is matched by token and cannot release a different chunk. */
static QueueHandle_t g_stream_req = nullptr, g_stream_ackq = nullptr;
static StaticQueue_t g_stream_control;
static uint32_t g_stream_token = 0, g_stream_files = 0;
static uint32_t g_stream_next_id = 0;
struct StreamAck { uint32_t token, files; bool accepted; };
bool ad_stream_enable(void) {
  if (g_stream_req) return true;
  uint8_t *storage = (uint8_t *)heap_caps_malloc(sizeof(AdStreamPacket), MALLOC_CAP_SPIRAM);
  if (!storage) return false;
  g_stream_ackq = xQueueCreate(2, sizeof(StreamAck));
  if (!g_stream_ackq) { heap_caps_free(storage); return false; }
  g_stream_req = xQueueCreateStatic(1, sizeof(AdStreamPacket), storage, &g_stream_control);
  return g_stream_req != nullptr;
}
bool ad_stream_read(AdStreamPacket *out) {
  return g_stream_req && xQueueReceive(g_stream_req, out, 0) == pdTRUE;
}
void ad_stream_ack(uint32_t token, bool accepted, uint32_t files) {
  if (!g_stream_ackq) return;
  StreamAck ack = {token, files, accepted};
  xQueueSend(g_stream_ackq, &ack, 0);
}
static bool stream_send(uint8_t kind, uint32_t id, uint32_t offset,
                        const uint8_t *data, uint32_t len, uint32_t crc) {
  /* Allocate one bounded chunk, never the entire archive. */
  AdStreamPacket *p = (AdStreamPacket *)heap_caps_malloc(sizeof(AdStreamPacket), MALLOC_CAP_SPIRAM);
  if (!p) return false;
  p->token = ++g_stream_token; p->kind = kind; p->transfer_id = id;
  p->offset = offset; p->length = len; p->crc = crc;
  if (len) memcpy(p->data, data, len);
  const uint32_t token = p->token;
  bool queued = xQueueSend(g_stream_req, p, pdMS_TO_TICKS(1000)) == pdTRUE;
  heap_caps_free(p);
  if (!queued) return false;
  const uint32_t start = millis();
  while ((uint32_t)(millis() - start) < (kind == AD_STREAM_COMMIT ? 60000u : 25000u)) {
    StreamAck ack;
    if (xQueueReceive(g_stream_ackq, &ack, pdMS_TO_TICKS(100)) == pdTRUE && ack.token == token) {
      g_stream_files = ack.files;
      return ack.accepted;
    }
  }
  return false;
}
