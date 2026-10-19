#include <assert.h>
#include <stdio.h>
#include "../ESP32Drop/src/awdl/core/awdl_frame.h"

int main(void) {
  struct AwdlMifParams m = {0};
  const uint8_t src[6] = {2, 3, 4, 5, 6, 7}, master[6] = {2, 4, 6, 8, 10, 12};
  m.instance = "123456abcdef"; m.src = src; m.master = master;
  uint8_t buf[AWDL_MIF_MAX];
  size_t size = awdl_build_mif(buf, sizeof(buf), &m), p = 40;
  int found_txt = 0, found_ht = 0;
  assert(size > 40);
  while (p + 3 <= size) {
    unsigned len = buf[p+1] | (buf[p+2] << 8);
    assert(p + 3 + len <= size);
    if (buf[p] == 2) {
      size_t record = p + 3;
      unsigned name_len = buf[record] | (buf[record+1] << 8);
      assert(name_len > 0 && 2 + name_len + 4 <= len);
      if (buf[record + 1 + name_len] == 16) {
        size_t d = record + 2 + name_len;
        unsigned data_len = buf[d] | (buf[d+1] << 8);
        assert(data_len + 2 + name_len + 4 == len);
        assert(data_len == sizeof(AWDL_RECEIVER_FLAGS_TEXT));
        assert(buf[d+4] == data_len - 1);
        assert(!memcmp(buf + d + 5, AWDL_RECEIVER_FLAGS_TEXT, data_len - 1));
        ++found_txt;
      }
    }
    if (buf[p] == 7) ++found_ht;
    p += 3 + len;
  }
  assert(p == size && found_txt == 1 && found_ht == 1);
  printf("MIF/TXT length consistency passed: %s (%zu bytes)\n", AWDL_RECEIVER_FLAGS_TEXT, size);
}
