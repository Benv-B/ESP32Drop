#pragma once
#include <stdint.h>

// Explicit bench-only commands. Never run during an AirDrop transfer.
// M3 ORIGIN requires BASE and distinguishes native cropping from relocation.
// M3 PACKED A tests compact input stride, retaining full-frame padded transport.
// M3 SHORT A repeats that prefix but omits padding; diagnostic until confirmed.
bool iris_refresh_probe(const char *action, uint8_t *frame, const char **error);
