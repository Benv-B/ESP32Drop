// Native USB CDC photo bridge for XIAO ESP32S3 (8 MB Flash / 8 MB Octal PSRAM).
#include <ESP32AWDL.h>
#include <ESP32Drop.h>
#include "bridge_app.h"

void setup() { bridge_setup(); }
void loop() { bridge_loop(); }
