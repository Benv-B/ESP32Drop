#pragma once
#include <stdint.h>

enum class IrisProgress : uint8_t { Off, Waiting, Receiving, Processing, Refreshing, Done, Error };
bool iris_progress_begin();
void iris_progress_set(IrisProgress phase);
