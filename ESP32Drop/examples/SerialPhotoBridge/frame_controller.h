#pragma once
#include <stddef.h>

void frame_setup();
void frame_tick(bool receiver_ready, bool output_busy = false);
bool frame_command(const char *command);
bool frame_is_off();
void frame_json(char *out, size_t capacity);
void frame_output_result(bool success);
