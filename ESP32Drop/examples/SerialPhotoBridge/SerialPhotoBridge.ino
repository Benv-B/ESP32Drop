// UART0/CH340 photo bridge for a generic ESP32-S3 with 8 MB Octal PSRAM.
#include <ESP32AWDL.h>
#include <ESP32Drop.h>
#include "bridge_app.h"

void setup() { bridge_setup(); }
void loop() { bridge_loop(); }
