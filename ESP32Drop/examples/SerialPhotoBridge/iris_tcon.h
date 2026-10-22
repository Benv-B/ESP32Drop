#pragma once
#include <stddef.h>
#include <stdint.h>
bool iris_tcon_begin();
bool iris_tcon_show(const uint8_t *bgr, size_t length);
// Synchronous raw-input M3 (or M4 with fast=true), retained for bench comparisons.
// bgr must remain stable for the call and contain FRAME_BYTES of input data.
// This function sends the buffer unchanged; it does not crop at the target ROI.
// Call only from the same owner as begin/show/sleep. Success retains M3/M4 mode;
// the next iris_tcon_show() restores the original DC bytes and M1 mode.
bool iris_tcon_show_partial(const uint8_t *bgr, size_t length, uint16_t x, uint16_t y,
                            uint16_t width, uint16_t height, bool fast = false);
// Native full-frame input: send exactly width*height*3 packed ROI bytes using
// one 3072-byte buffer; source remains untouched and no padding is transmitted.
// Shares the same session/ownership rules. W<H COM/SEG behavior is unvalidated.
// The 256x128 SHORT A bench confirmed this compact layout and byte count.
bool iris_tcon_show_region(const uint8_t *bgr, size_t length, uint16_t x, uint16_t y,
                           uint16_t width, uint16_t height, bool fast = false);
// Diagnostic only: send exactly width*height*3 caller-packed BGR
// bytes with no padding, in <=3072-byte chunks. Shares session/error handling.
// This comparator tests input length separately from ROI packing/stride.
bool iris_tcon_show_partial_compact(const uint8_t *bgr, size_t length, uint16_t x, uint16_t y,
                                    uint16_t width, uint16_t height, bool fast = false);
void iris_tcon_sleep();
const char *iris_tcon_error();
void iris_tcon_status(uint8_t out[4]);
uint32_t iris_latch_mv();
#ifdef PHOTO_FRAME_TEST_PATTERN
void iris_tcon_probe();
void iris_tcon_bus_probe();
bool iris_tcon_write_test(const uint8_t *bgr, size_t length);
#endif
