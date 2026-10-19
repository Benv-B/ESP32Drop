// Standalone test firmware for the assembled IRIS RevC carrier + XIAO ESP32-C5.
// No Wi-Fi and no SPI. Use SerialPhotoBridge separately for actual AirDrop tests.
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include "power_guard.h"

#if !CONFIG_IDF_TARGET_ESP32C5
#error "RevCBringup is only for XIAO ESP32-C5 on the IRIS RevC carrier"
#endif
#if !ARDUINO_USB_MODE || !ARDUINO_USB_CDC_ON_BOOT
#error "Enable native Hardware USB CDC on boot"
#endif

// Raw GPIO numbers from the RevC netlist; NOT the M5Stamp board aliases.
constexpr int PWR_OFF = 1, BUS_EN = 7, CS_N = 0;
constexpr int NFC_SDA = 23, NFC_SCL = 24, BAT_ADC = 5;
constexpr uint8_t NFC_ADDR = 0x55;
constexpr uint32_t RECORD_MAGIC = 0x52435631;
constexpr size_t PSRAM_TEST_BYTES = 256 * 1024;

struct Record {
  uint32_t magic, boot, reset_reason, rf_seen, off_requested, off_at_ms;
};
static Record previous = {}, current = {};
static Preferences prefs;
static PowerGuard guard;
static bool nvs_ok = false, wire_ok = false, nfc_ok = false, ram_ok = false;
static uint8_t ns_reg = 0;
static uint32_t errors = 0, armed_seconds = 0;
static bool monitor = true;

static void save_record() {
  if (nvs_ok && prefs.putBytes("record", &current, sizeof(current)) != sizeof(current))
    Serial.println("ERROR: cannot persist test record");
}

static bool test_psram() {
  if (!psramFound()) return false;
  auto *memory = static_cast<volatile uint32_t *>(
    heap_caps_malloc(PSRAM_TEST_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!memory) return false;
  bool ok = true;
  for (uint32_t invert : {0u, 0xffffffffu}) {
    for (size_t i = 0; i < PSRAM_TEST_BYTES / 4; ++i)
      memory[i] = (uint32_t(i) * 2654435761u) ^ invert;
    for (size_t i = 0; i < PSRAM_TEST_BYTES / 4; ++i)
      if (memory[i] != ((uint32_t(i) * 2654435761u) ^ invert)) { ok = false; break; }
    delay(1);
  }
  heap_caps_free(const_cast<uint32_t *>(memory));
  return ok;
}

static void print_record(const char *kind, const Record &record) {
  Serial.printf("{\"type\":\"%s\",\"valid\":%s,\"boot\":%lu,\"reset_reason\":%lu,"
                "\"rf_seen\":%s,\"off_requested\":%s,\"off_at_ms\":%lu}\n",
    kind, record.magic == RECORD_MAGIC ? "true" : "false", (unsigned long)record.boot,
    (unsigned long)record.reset_reason, record.rf_seen ? "true" : "false",
    record.off_requested ? "true" : "false", (unsigned long)record.off_at_ms);
}

static void status() {
  uint32_t adc_mv = analogReadMilliVolts(BAT_ADC);
  Serial.printf("{\"type\":\"status\",\"firmware\":\"revc-bringup-1\",\"uptime_ms\":%lu,"
                "\"boot\":%lu,\"flash_bytes\":%lu,\"psram_bytes\":%lu,"
                "\"psram_test_bytes\":%lu,\"psram_test_ok\":%s,\"nvs_ok\":%s,"
                "\"nfc_ok\":%s,\"nfc_addr\":85,\"ns_reg\":%u,\"rf_present\":%s,"
                "\"rf_seen\":%s,\"i2c_errors\":%lu,\"adc_mv\":%lu,"
                "\"latch_estimate_mv\":%lu,\"arm_seconds\":%lu,\"off_pending\":%s,"
                "\"off_requested\":%s,\"usb_serial_open\":%s}\n",
    (unsigned long)millis(), (unsigned long)current.boot,
    (unsigned long)ESP.getFlashChipSize(), (unsigned long)ESP.getPsramSize(),
    (unsigned long)PSRAM_TEST_BYTES, ram_ok ? "true" : "false", nvs_ok ? "true" : "false",
    nfc_ok ? "true" : "false", ns_reg,
    nfc_ok ? ((ns_reg & 1) ? "true" : "false") : "null",
    current.rf_seen ? "true" : "false", (unsigned long)errors,
    (unsigned long)adc_mv, (unsigned long)(adc_mv * 1330ul / 330ul),
    (unsigned long)armed_seconds, guard.pending ? "true" : "false",
    guard.fired ? "true" : "false", Serial ? "true" : "false");
}

static void command(const char *line) {
  if (!strcmp(line, "STATUS") || !strcmp(line, "NFC")) status();
  else if (!strcmp(line, "LAST")) print_record("previous_boot", previous);
  else if (!strcmp(line, "ARM30")) {
    if (!nvs_ok || prefs.putUInt("arm_s", 30) != sizeof(uint32_t)) {
      Serial.println("ERROR: ARM30 not saved; no automatic shutdown armed"); return;
    }
    armed_seconds = 30;
    digitalWrite(PWR_OFF, LOW);
    guard.arm(millis(), 30000);
    Serial.println("ARM30 saved: shutdown after 30s on this and subsequent boots,"
                   " only after valid RF-absent readings for >=500ms. DISARM cancels.");
  } else if (!strcmp(line, "DISARM")) {
    if (!nvs_ok || prefs.putUInt("arm_s", 0) != sizeof(uint32_t)) {
      Serial.println("ERROR: cannot save DISARM; canceling this boot only");
    }
    armed_seconds = 0;
    guard.cancel(); digitalWrite(PWR_OFF, LOW);
    Serial.println("Automatic shutdown canceled for this boot.");
  } else if (!strcmp(line, "PWROFF")) {
    guard.arm(millis(), 1000);
    Serial.println("Power-off requested: waiting >=1s and confirmed RF absent >=500ms.");
  } else if (!strcmp(line, "QUIET")) monitor = false;
  else if (!strcmp(line, "MONITOR")) monitor = true;
  else Serial.println("Commands: STATUS NFC LAST ARM30 DISARM PWROFF QUIET MONITOR HELP");
}

void setup() {
  digitalWrite(PWR_OFF, LOW); pinMode(PWR_OFF, OUTPUT);
  digitalWrite(BUS_EN, LOW); pinMode(BUS_EN, OUTPUT);
  digitalWrite(CS_N, HIGH); pinMode(CS_N, OUTPUT);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(20);
  // No while(!Serial): battery/NFC boot MUST work with the USB cable removed.
  delay(250);
  nvs_ok = prefs.begin("revctest", false);
  if (nvs_ok) {
    if (prefs.getBytesLength("record") == sizeof(previous))
      prefs.getBytes("record", &previous, sizeof(previous));
    if (previous.magic != RECORD_MAGIC) previous = {};
    armed_seconds = prefs.getUInt("arm_s", 0);
    if (armed_seconds != 30) armed_seconds = 0;
  }
  current = {RECORD_MAGIC, previous.boot + 1, (uint32_t)esp_reset_reason(), 0, 0, 0};
  save_record();
  wire_ok = Wire.begin(NFC_SDA, NFC_SCL, 100000);
  Wire.setTimeOut(25);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC, ADC_11db);
  ram_ok = test_psram();
  if (armed_seconds) guard.arm(millis(), armed_seconds * 1000);
  Serial.println("RevCBringup: no SPI, no AirDrop; default tag address 0x55, SDA23/SCL24.");
  Serial.println("LAST records intent only; measure 3V3 with USB removed to prove power-off.");
  print_record("previous_boot", previous);
}

void loop() {
  static char line[32];
  static size_t used = 0;
  static bool overflow = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      line[used] = 0;
      if (overflow) Serial.println("ERROR: command too long");
      else if (used) command(line);
      used = 0; overflow = false;
    } else if (c != '\r') {
      if (used + 1 < sizeof(line)) line[used++] = c;
      else overflow = true;
    }
  }
  uint32_t now = millis();
  static uint32_t sampled = 0, reported = 0;
  if (uint32_t(now - sampled) >= 50) {
    sampled = now;
    nfc_ok = wire_ok && ntag_read_register(Wire, NFC_ADDR, 6, ns_reg);
    if (!nfc_ok) ++errors;
    guard.sample(millis(), nfc_ok, (ns_reg & 1) != 0);
    if (nfc_ok && (ns_reg & 1) && !current.rf_seen) {
      current.rf_seen = 1; save_record();
    }
  }
  if (guard.due(millis())) {
    guard.fired = true;
    current.off_requested = 1; current.off_at_ms = millis();
    save_record();
    // USB logging is best-effort; no flush/wait may prevent battery shutdown.
    if (Serial) Serial.println("PWR_OFF=1 now. USB may keep XIAO alive; this is NOT proof of shutdown.");
    digitalWrite(BUS_EN, LOW);
    digitalWrite(PWR_OFF, HIGH);
  }
  if (monitor && Serial && uint32_t(now - reported) >= 1000) {
    reported = now; status();
  }
  delay(2);
}
