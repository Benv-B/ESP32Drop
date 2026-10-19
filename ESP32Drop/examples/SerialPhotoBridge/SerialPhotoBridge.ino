// Native USB photo bridge for XIAO ESP32-S3 / C5; optional RevC frame control.
#include <ESP32AWDL.h>
#include <ESP32Drop.h>
#include "bridge_app.h"

void setup() { bridge_setup(); }
void loop() { bridge_loop(); }
