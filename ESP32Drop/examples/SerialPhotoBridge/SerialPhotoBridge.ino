// PHOTO_FRAME_TCON selects the standalone IRIS display consumer.
// Without it, this remains the historical USB bridge for S3/C5 diagnostics.
#include <ESP32AWDL.h>
#include <ESP32Drop.h>
#include "bridge_app.h"

void setup() { bridge_setup(); }
void loop() { bridge_loop(); }
