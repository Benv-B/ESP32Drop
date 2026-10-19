#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Only suppress a link retry whose original was successfully submitted to lwIP.
 * A first-seen retry is data, and a TCP retransmission has a new link sequence.
 * Expiration also bounds sequence reuse/reboots; hash collisions merely miss. */
struct AwdlDedupEntry {
  uint8_t src[6], dst[6], tid;
  uint16_t seq;
  uint32_t signature, seen_ms;
  bool valid;
};
struct AwdlDedup { struct AwdlDedupEntry entries[64]; };
static inline uint32_t awdl_payload_signature(const uint8_t *p, uint32_t n) {
  uint32_t h = 2166136261u;
  while (n--) h = (h ^ *p++) * 16777619u;
  return h;
}
static inline struct AwdlDedupEntry *awdl_dedup_entry(struct AwdlDedup *d,
    const uint8_t *f, uint8_t tid) {
  return &d->entries[(f[22] ^ f[23] ^ f[15] ^ tid) & 63];
}
static inline bool awdl_dedup_seen(struct AwdlDedup *d, const uint8_t *f,
    uint8_t tid, uint32_t sig, uint32_t now) {
  struct AwdlDedupEntry *e = awdl_dedup_entry(d, f, tid);
  return (f[1] & 8) && e->valid && (uint32_t)(now - e->seen_ms) < 200u &&
    e->seq == ((uint16_t)f[22] | ((uint16_t)f[23] << 8)) &&
    e->tid == tid && e->signature == sig &&
    memcmp(e->src, f + 10, 6) == 0 && memcmp(e->dst, f + 4, 6) == 0;
}
static inline void awdl_dedup_remember(struct AwdlDedup *d, const uint8_t *f,
    uint8_t tid, uint32_t sig, uint32_t now) {
  struct AwdlDedupEntry *e = awdl_dedup_entry(d, f, tid);
  memcpy(e->src, f + 10, 6); memcpy(e->dst, f + 4, 6);
  e->seq = (uint16_t)f[22] | ((uint16_t)f[23] << 8);
  e->tid = tid; e->signature = sig; e->seen_ms = now; e->valid = true;
}
