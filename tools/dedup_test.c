#include <assert.h>
#include <stdio.h>
#include "../ESP32Drop/src/awdl/core/awdl_dedup.h"
int main(void) {
  struct AwdlDedup d = {0};
  uint8_t f[26] = {0x88, 8}; f[10] = 4; f[4] = 2; f[22] = 16;
  assert(!awdl_dedup_seen(&d, f, 0, 123, 10)); /* original missed */
  awdl_dedup_remember(&d, f, 0, 123, 10);
  assert(awdl_dedup_seen(&d, f, 0, 123, 11));
  assert(!awdl_dedup_seen(&d, f, 1, 123, 11)); /* QoS sequence spaces */
  assert(!awdl_dedup_seen(&d, f, 0, 124, 11)); /* changed data */
  f[1] = 0; assert(!awdl_dedup_seen(&d, f, 0, 123, 11));
  f[1] = 8; f[22]++; assert(!awdl_dedup_seen(&d, f, 0, 123, 11));
  f[22]--; f[10]++; assert(!awdl_dedup_seen(&d, f, 0, 123, 11));
  f[10]--; f[4]++; assert(!awdl_dedup_seen(&d, f, 0, 123, 11));
  f[4]--; assert(!awdl_dedup_seen(&d, f, 0, 123, 210));
  awdl_dedup_remember(&d, f, 0, 123, 0xfffffff0u);
  assert(awdl_dedup_seen(&d, f, 0, 123, 4)); /* millis wrap */
  puts("dedup: 10 checks passed");
}
