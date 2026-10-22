#ifdef PHOTO_FRAME_TCON
#include <Arduino.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali_scheme.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#ifdef PHOTO_FRAME_TEST_PATTERN
#include <soc/gpio_struct.h>
#include <soc/gpio_sig_map.h>
#endif
#include "iris_tcon.h"
#include "iris_tcon_protocol.h"
#include "iris_latch_sample.h"

#ifndef IRIS_SPI_HZ
#define IRIS_SPI_HZ 4000000
#endif
static_assert(IRIS_SPI_HZ >= 2000000 && IRIS_SPI_HZ <= 10000000, "Bring-up SPI range: 2-10 MHz");
namespace {
constexpr int CS = 0, BUS_EN = 7, CLK = 8, PIN_MISO = 9, PIN_MOSI = 10, DF = 12, ADC = 5;
struct LatchSample { esp_err_t error; uint32_t mv; };
StaticSemaphore_t latch_mutex_storage;
SemaphoreHandle_t latch_mutex = nullptr;
portMUX_TYPE latch_mutex_init = portMUX_INITIALIZER_UNLOCKED;
adc_oneshot_unit_handle_t latch_adc = nullptr;
adc_cali_handle_t latch_calibration = nullptr;
adc_channel_t latch_channel = ADC_CHANNEL_4;

esp_err_t latch_init_locked() {
  if (latch_adc && latch_calibration) return ESP_OK;
  adc_unit_t unit = ADC_UNIT_1;
  esp_err_t error = adc_oneshot_io_to_channel(ADC, &unit, &latch_channel);
  if (error != ESP_OK) return error;
  if (!latch_adc) {
    adc_oneshot_unit_init_cfg_t config{};
    config.unit_id = unit; config.ulp_mode = ADC_ULP_MODE_DISABLE;
    error = adc_oneshot_new_unit(&config, &latch_adc);
    if (error != ESP_OK) return error;
  }
  adc_oneshot_chan_cfg_t channel{};
  channel.atten = ADC_ATTEN_DB_12; channel.bitwidth = ADC_BITWIDTH_12;
  error = adc_oneshot_config_channel(latch_adc, latch_channel, &channel);
  if (error != ESP_OK) return error;
  adc_cali_curve_fitting_config_t calibration{};
  calibration.unit_id = unit; calibration.chan = latch_channel;
  calibration.atten = channel.atten; calibration.bitwidth = channel.bitwidth;
  return adc_cali_create_scheme_curve_fitting(&calibration, &latch_calibration);
}

LatchSample read_latch() {
  // Serialize initialization, conversion and calibration across main STATUS and
  // the display worker. IDF oneshot uses a try-lock and may otherwise return a
  // transient timeout even though its API is thread-safe.
  portENTER_CRITICAL(&latch_mutex_init);
  if (!latch_mutex) latch_mutex = xSemaphoreCreateMutexStatic(&latch_mutex_storage);
  SemaphoreHandle_t mutex = latch_mutex;
  portEXIT_CRITICAL(&latch_mutex_init);
  if (!mutex || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE)
    return {ESP_ERR_NO_MEM, 0};
  LatchSample sample{latch_init_locked(), 0};
  if (sample.error == ESP_OK) {
    const auto result = iris::read_latch_raw(
      [](int *raw) { return adc_oneshot_read(latch_adc, latch_channel, raw); },
      [](uint32_t ms) { delay(ms); }, ESP_ERR_TIMEOUT);
    sample.error = esp_err_t(result.error);
    if (sample.error == ESP_OK) {
      int mv = 0;
      sample.error = adc_cali_raw_to_voltage(latch_calibration, result.raw, &mv);
      if (sample.error == ESP_OK) sample.mv = uint32_t(mv > 0 ? mv : 0) * 1330u / 330u;
    }
  }
  xSemaphoreGive(mutex);
  if (sample.error != ESP_OK && Serial)
    Serial.printf("IRIS ADC: latch sample unavailable (%s); no voltage assumed\n", esp_err_to_name(sample.error));
  return sample;
}

struct SpiIO {
  spi_device_handle_t device = nullptr;
  static constexpr size_t CHUNK = 64;
  static constexpr size_t PIXEL_CHUNK = iris::ROW_BYTES;
  uint8_t *dma_pixels = nullptr;
  uint32_t pixel_us = 0;
  bool enabled = false;
  esp_err_t latch_error = ESP_OK;
  uint32_t now() { return millis(); }
  void pause(uint32_t ms) { delay(ms); }
  bool df() { return enabled && digitalRead(DF); }
  // RevC ADC senses EN_LATCH, NOT 5V_SYS. This only prevents USB-only bus
  // enable; it is not a power-good or calibrated battery-voltage measurement.
  bool latch_ok() {
    const LatchSample sample = read_latch();
    if (sample.error != ESP_OK) {
      if (latch_error == ESP_OK) latch_error = sample.error;
      return false;
    }
    // A valid low reading, including 0 V, is never retried or replaced by cache.
    return sample.mv >= 2500;
  }
  bool powered() { return enabled && latch_ok(); }
  bool spi_error(esp_err_t error) {
    if (error == ESP_OK) return true;
    if (Serial) Serial.printf("IRIS SPI: %s\n", esp_err_to_name(error));
    return false;
  }
  bool add_device(uint32_t hz, spi_device_handle_t *out) {
    spi_device_interface_config_t config{};
    config.clock_source = SPI_CLK_SRC_DEFAULT;
    config.clock_speed_hz = hz; config.mode = 0;
    config.spics_io_num = -1; config.queue_size = 1;
    return spi_error(spi_bus_add_device(SPI2_HOST, &config, out));
  }
  bool start_bus() {
    if (device) return true;
    spi_bus_config_t bus{};
    bus.mosi_io_num = PIN_MOSI; bus.miso_io_num = PIN_MISO; bus.sclk_io_num = CLK;
    bus.quadwp_io_num = bus.quadhd_io_num = -1;
    bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
    bus.max_transfer_sz = PIXEL_CHUNK;
    bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;
    // The bundled IDF driver handles the C5 PCR clock source/reset. SPI2 is
    // separate from memory SPI. Copy PSRAM pixels into one internal DMA row;
    // the interrupt-driven transfer lets radio/TLS tasks run during SPI output.
    dma_pixels = static_cast<uint8_t *>(heap_caps_malloc(PIXEL_CHUNK,MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!dma_pixels) return false;
    if (!spi_error(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO))) {
      heap_caps_free(dma_pixels); dma_pixels=nullptr; return false;
    }
    if (!add_device(IRIS_SPI_HZ, &device)) {
      spi_bus_free(SPI2_HOST); heap_caps_free(dma_pixels); dma_pixels=nullptr; return false;
    }
    int khz = 0; spi_device_get_actual_freq(device, &khz);
    if (Serial) Serial.printf("IRIS SPI: IDF SPI2 mode0, actual %d kHz, interrupt DMA row buffer\n", khz);
    return true;
  }
  bool enable() {
    latch_error = ESP_OK;
    pinMode(BUS_EN, OUTPUT); digitalWrite(BUS_EN, LOW);
    pinMode(CS, OUTPUT); digitalWrite(CS, HIGH);
    pinMode(DF, INPUT_PULLDOWN);
    for (int i = 0; i < 10; ++i) {
      if (!latch_ok()) return false;
      delay(10);
    }
    if (!start_bus()) return false;
    digitalWrite(BUS_EN, HIGH); enabled = true;
    delay(1); return true;
  }
  void disable() {
    digitalWrite(CS, HIGH); digitalWrite(BUS_EN, LOW);
    enabled = false;
  }
  bool transfer(spi_device_handle_t target, const uint8_t *tx, uint8_t *rx, size_t size) {
    spi_transaction_t transaction{};
    transaction.length = size * 8; transaction.tx_buffer = tx; transaction.rx_buffer = rx;
    return spi_error(spi_device_polling_transmit(target, &transaction));
  }
  bool write(uint8_t command, const uint8_t *data, size_t size) {
    if (!enabled || (size && !data)) return false;
    const uint32_t started = micros();
    if (!spi_error(spi_device_acquire_bus(device, portMAX_DELAY))) return false;
    digitalWrite(CS, LOW); delayMicroseconds(1);
    bool ok = transfer(device, &command, nullptr, 1);
    for (size_t offset = 0; ok && offset < size; ) {
      size_t count = size - offset;
      if (command == 0x0c) {
        if (count > PIXEL_CHUNK) count = PIXEL_CHUNK;
        memcpy(dma_pixels,data+offset,count);
        spi_transaction_t transaction{};
        transaction.length=count*8; transaction.tx_buffer=dma_pixels;
        ok=spi_error(spi_device_transmit(device,&transaction));
      } else {
        if (count > CHUNK) count=CHUNK;
        ok=transfer(device,data+offset,nullptr,count);
      }
      offset += count;
    }
    delayMicroseconds(1); digitalWrite(CS, HIGH);
    spi_device_release_bus(device); delayMicroseconds(1);
    if (command == 0x0c) pixel_us += uint32_t(micros()-started);
    return ok;
  }
  bool read_from(spi_device_handle_t target, uint8_t command, uint8_t *data, size_t size) {
    if (!enabled || !data || size > CHUNK - 2) return false;
    uint8_t tx[CHUNK]{}, rx[CHUNK]{};
    tx[0] = command; // tx[1] is the mandatory dummy byte, application note p.19.
    if (!spi_error(spi_device_acquire_bus(target, portMAX_DELAY))) return false;
    digitalWrite(CS, LOW); delayMicroseconds(1);
    bool ok = transfer(target, tx, rx, size + 2);
    delayMicroseconds(1); digitalWrite(CS, HIGH);
    spi_device_release_bus(target); delayMicroseconds(1);
    if (ok) memcpy(data, rx + 2, size);
    return ok;
  }
  bool read(uint8_t command, uint8_t *data, size_t size) { return read_from(device, command, data, size); }
} io;
iris::Tcon<SpiIO> tcon(io);
}
// Legacy numeric telemetry returns 0 if unavailable; read_latch logs the error.
// Supply decisions use the explicit error-bearing sample above, not this wrapper.
uint32_t iris_latch_mv() { return read_latch().mv; }
bool iris_tcon_begin() {
  uint32_t start = millis();
  if (Serial) Serial.printf("IRIS INIT: waiting for DF, timeout %lu ms per attempt\n", (unsigned long)iris::BOOT_TIMEOUT_MS);
  bool ok = tcon.begin();
  if (Serial) Serial.printf("IRIS INIT: %s after %lu ms\n", ok ? "ready" : iris_tcon_error(), (unsigned long)(millis() - start));
  return ok;
}
bool iris_tcon_show(const uint8_t *bgr, size_t length) { return tcon.show(bgr, length); }
bool iris_tcon_show_partial(const uint8_t *bgr, size_t length, uint16_t x, uint16_t y,
                            uint16_t width, uint16_t height, bool fast) {
  return tcon.show_partial(bgr, length, x, y, width, height, fast);
}
bool iris_tcon_show_region(const uint8_t *bgr, size_t length, uint16_t x, uint16_t y,
                           uint16_t width, uint16_t height, bool fast) {
  const uint32_t started=millis(), before_pixels=io.pixel_us;
  const bool ok = tcon.show_region(bgr, length, x, y, width, height, fast);
  if (ok && Serial) Serial.printf("IRIS REGION: M%u confirmed region=%u,%u,%u,%u payload=%lu bytes spi_ms=%lu total_ms=%lu t=%lu\n",
    fast ? 4u : 3u, unsigned(x), unsigned(y), unsigned(width), unsigned(height),
    (unsigned long)(uint32_t(width) * height * 3),
    (unsigned long)(uint32_t(io.pixel_us-before_pixels)/1000),
    (unsigned long)(millis()-started),(unsigned long)millis());
  return ok;
}
bool iris_tcon_show_partial_compact(const uint8_t *bgr, size_t length, uint16_t x, uint16_t y,
                                    uint16_t width, uint16_t height, bool fast) {
  return tcon.show_partial_compact(bgr, length, x, y, width, height, fast);
}
void iris_tcon_sleep() { tcon.sleep(); }
const char *iris_tcon_error() {
  return io.latch_error != ESP_OK ? "TCON latch ADC unavailable" : tcon.error;
}
void iris_tcon_status(uint8_t out[4]) { memcpy(out, tcon.status.bytes, 4); }
#ifdef PHOTO_FRAME_TEST_PATTERN
void iris_tcon_bus_probe() {
  if (tcon.ready) {
    Serial.println("{\"type\":\"tcon_bus\",\"error\":\"controller_initialized\",\"samples\":0}");
    return;
  }
  struct PinSnapshot { int pin; gpio_io_config_t config{}; esp_err_t error = ESP_ERR_INVALID_STATE; };
  PinSnapshot pins[] = {{CS}, {BUS_EN}, {CLK}, {PIN_MISO}, {PIN_MOSI}, {DF}};
  struct Sample {
    int gate = 0, pull = 0, gate_read = -1, cs_before = -1, cs_after = -1;
    int miso_before = -1, miso_after = -1, df = -1;
    uint32_t latch_mv = 0;
    gpio_io_config_t config{};
    gpio_func_in_sel_cfg_reg_t matrix{};
    esp_err_t gate_error = ESP_ERR_INVALID_STATE, pull_error = ESP_ERR_INVALID_STATE, config_error = ESP_ERR_INVALID_STATE;
    esp_err_t spi_error = ESP_ERR_INVALID_STATE;
    uint8_t received[6]{};
  } samples[6];
  constexpr gpio_pull_mode_t pulls[] = {GPIO_PULLDOWN_ONLY, GPIO_PULLUP_ONLY, GPIO_FLOATING};
  const char *pull_names[] = {"down", "up", "none"};
  const uint32_t started = millis();
  size_t count = 0;
  bool acquired = false, powered_phase_skipped = false;
  const char *failure = "";
  esp_err_t acquire_error = ESP_ERR_INVALID_STATE, freq_error = ESP_ERR_INVALID_STATE;
  esp_err_t setup_cs_error = ESP_ERR_INVALID_STATE, setup_gate_error = ESP_ERR_INVALID_STATE;
  int khz = 0;
  // The isolated MCU-side checks are valid on USB alone. Never connect the
  // TCON side without a separate stable supply-latch check below.
  pinMode(BUS_EN, OUTPUT); pinMode(CS, OUTPUT); pinMode(DF, INPUT_PULLDOWN);
  io.enabled = false;
  do {
    // No TCON command is issued: software CS remains high for every clock.
    setup_cs_error = gpio_set_level(gpio_num_t(CS), 1);
    setup_gate_error = gpio_set_level(gpio_num_t(BUS_EN), 0);
    if (setup_cs_error != ESP_OK || setup_gate_error != ESP_OK || !gpio_get_level(gpio_num_t(CS))) {
      failure = "cannot_isolate_or_hold_cs_high"; break;
    }
    if (!io.start_bus()) { failure = "spi_unavailable"; break; }
    for (auto &pin : pins) pin.error = gpio_get_io_config(gpio_num_t(pin.pin), &pin.config);
    freq_error = spi_device_get_actual_freq(io.device, &khz);
    acquire_error = spi_device_acquire_bus(io.device, portMAX_DELAY);
    if (acquire_error != ESP_OK) { failure = "spi_acquire_failed"; break; }
    acquired = true;
    for (int gate = 0; gate <= 1 && !failure[0]; ++gate) {
      if (gate) {
        for (int stable = 0; stable < 10; ++stable) {
          if (iris_latch_mv() < 2500) { powered_phase_skipped = true; break; }
          delay(10);
        }
        if (powered_phase_skipped) break;
      }
      for (int pull = 0; pull < 3; ++pull) {
        const uint32_t latch_mv = iris_latch_mv();
        if (gate && latch_mv < 2500) { failure = "supply_latch_lost"; break; }
        Sample &sample = samples[count++];
        sample.gate = gate; sample.pull = pull;
        sample.latch_mv = latch_mv;
        sample.gate_error = gpio_set_level(gpio_num_t(BUS_EN), gate);
        sample.pull_error = gpio_set_pull_mode(gpio_num_t(PIN_MISO), pulls[pull]);
        delay(1);
        sample.config_error = gpio_get_io_config(gpio_num_t(PIN_MISO), &sample.config);
        sample.matrix.val = GPIO.func_in_sel_cfg[FSPIQ_IN_IDX].val;
        sample.gate_read = gpio_get_level(gpio_num_t(BUS_EN));
        sample.cs_before = gpio_get_level(gpio_num_t(CS));
        sample.miso_before = gpio_get_level(gpio_num_t(PIN_MISO));
        sample.df = gpio_get_level(gpio_num_t(DF));
        if (sample.gate_error != ESP_OK || sample.pull_error != ESP_OK ||
            sample.config_error != ESP_OK || !sample.cs_before || sample.gate_read != gate) {
          failure = "gpio_configuration_or_level_failed"; break;
        }
        uint8_t zeros[sizeof(sample.received)]{};
        spi_transaction_t transaction{};
        transaction.length = sizeof(zeros) * 8;
        transaction.tx_buffer = zeros; transaction.rx_buffer = sample.received;
        sample.spi_error = spi_device_polling_transmit(io.device, &transaction);
        sample.cs_after = gpio_get_level(gpio_num_t(CS));
        sample.miso_after = gpio_get_level(gpio_num_t(PIN_MISO));
        if (sample.spi_error != ESP_OK || !sample.cs_after) {
          failure = "spi_transaction_or_cs_failed"; break;
        }
      }
    }
  } while (false);
  // Restore before printing, including every partial-initialization/failure path.
  esp_err_t restore_cs = gpio_set_level(gpio_num_t(CS), 1);
  esp_err_t restore_gate = gpio_set_level(gpio_num_t(BUS_EN), 0);
  esp_err_t restore_pull = gpio_set_pull_mode(gpio_num_t(PIN_MISO), GPIO_FLOATING);
  io.enabled = false;
  if (acquired) spi_device_release_bus(io.device);
  const uint32_t active_ms = millis() - started;
  for (const auto &pin : pins) {
    const auto &config = pin.config;
    Serial.printf("{\"type\":\"tcon_bus_gpio\",\"pin\":%d,\"config_error\":\"%s\","
                  "\"mux_function\":%lu,\"output_signal\":%lu,\"input_enabled\":%d,\"output_enabled\":%d,"
                  "\"pull_up\":%d,\"pull_down\":%d,\"output_from_peripheral\":%d,\"open_drain\":%d}\n",
      pin.pin, esp_err_to_name(pin.error), (unsigned long)config.fun_sel, (unsigned long)config.sig_out,
      config.ie, config.oe, config.pu, config.pd, config.oe_ctrl_by_periph, config.od);
  }
  for (size_t i = 0; i < count; ++i) {
    const Sample &s = samples[i];
    Serial.printf("{\"type\":\"tcon_bus_sample\",\"bus_en_set\":%d,\"bus_en_read\":%d,\"miso_pull\":\"%s\","
                  "\"cs_before\":%d,\"cs_after\":%d,\"miso_before\":%d,\"miso_after\":%d,\"df\":%d,"
                  "\"latch_mv\":%lu,\"pull_up\":%d,\"pull_down\":%d,\"input_enabled\":%d,\"output_enabled\":%d,"
                  "\"matrix_signal\":%d,\"matrix_pin\":%u,\"matrix_enabled\":%u,\"matrix_inverted\":%u,"
                  "\"gate_error\":\"%s\",\"pull_error\":\"%s\",\"config_error\":\"%s\",\"spi_error\":\"%s\","
                  "\"rx\":[%u,%u,%u,%u,%u,%u]}\n",
      s.gate, s.gate_read, pull_names[s.pull], s.cs_before, s.cs_after, s.miso_before, s.miso_after, s.df,
      (unsigned long)s.latch_mv, s.config.pu, s.config.pd, s.config.ie, s.config.oe,
      FSPIQ_IN_IDX, (unsigned)s.matrix.in_sel, (unsigned)s.matrix.sig_in_sel, (unsigned)s.matrix.in_inv_sel,
      esp_err_to_name(s.gate_error), esp_err_to_name(s.pull_error), esp_err_to_name(s.config_error), esp_err_to_name(s.spi_error),
      s.received[0], s.received[1], s.received[2], s.received[3], s.received[4], s.received[5]);
  }
  Serial.printf("{\"type\":\"tcon_bus\",\"samples\":%u,\"active_ms\":%lu,\"spi_actual_khz\":%d,\"error\":\"%s\","
                "\"powered_phase_skipped\":%s,\"skip_reason\":\"%s\",\"setup_cs_error\":\"%s\",\"setup_gate_error\":\"%s\","
                "\"acquire_error\":\"%s\",\"freq_error\":\"%s\",\"restore_cs_error\":\"%s\","
                "\"restore_gate_error\":\"%s\",\"restore_pull_error\":\"%s\",\"final_cs\":%d,\"final_bus_en\":%d}\n",
    (unsigned)count, (unsigned long)active_ms, khz, failure,
    powered_phase_skipped ? "true" : "false", powered_phase_skipped ? "supply_latch_not_stable" : "",
    esp_err_to_name(setup_cs_error), esp_err_to_name(setup_gate_error), esp_err_to_name(acquire_error), esp_err_to_name(freq_error),
    esp_err_to_name(restore_cs), esp_err_to_name(restore_gate), esp_err_to_name(restore_pull),
    gpio_get_level(gpio_num_t(CS)), gpio_get_level(gpio_num_t(BUS_EN)));
}
bool iris_tcon_write_test(const uint8_t *bgr, size_t length) {
  // Explicit bench command only: a single normal M1 write to diagnose broken
  // receive/DF wiring. Never used at boot or by the AirDrop consumer. Sending
  // pixels alone cannot count as success; retain the normal completion check.
  if (!bgr || length != iris::FRAME_BYTES || !io.enable()) {
    io.disable(); tcon.error = "write test unavailable"; return false;
  }
  tcon.ready = false;
  uint8_t active = 1, spi_input = 2, m1 = 1;
  bool ok = io.write(0x06, &active, 1); io.pause(5);
  ok = ok && io.write(0x08, &spi_input, 1) && io.write(0x07, &m1, 1)
    && io.write(0x09, nullptr, 0);
  uint32_t rows = 0, started = millis();
  while (ok && rows < iris::HEIGHT) {
    if (!io.powered()) { ok = false; break; }
    ok = io.write(0x0c, bgr + rows * iris::ROW_BYTES, iris::ROW_BYTES);
    if (ok) ++rows;
    if ((rows & 7) == 0) io.pause(1);
    if (rows && (rows % 256) == 0 && Serial)
      Serial.printf("IRIS WRITE TEST: %lu/768 rows sent\n", (unsigned long)rows);
  }
  ok = ok && io.write(0x0d, nullptr, 0);
  if (Serial) Serial.printf("IRIS WRITE TEST: transfer %s, %lu rows in %lu ms\n",
    ok ? "sent" : "failed", (unsigned long)rows, (unsigned long)(millis() - started));
  bool confirmed = false;
  iris::Status observed{};
  if (ok) {
    started = millis();
    while (uint32_t(millis() - started) < iris::REFRESH_TIMEOUT_MS && io.powered()) {
      if (io.df()) {
        if (!io.read(0x57, observed.bytes, 4) || observed.fault()) break;
        io.pause(1);
        if (observed.displayed() && !(observed.bytes[0] & 4) && !(observed.bytes[1] & 0x10) && !io.df()) {
          confirmed = true; break;
        }
      }
      io.pause(2);
    }
    if (!confirmed && io.powered()) io.read(0x57, observed.bytes, 4);
  }
  tcon.status = observed;
  io.disable();
  tcon.error = confirmed ? "" : "single write test: TCON completion not confirmed";
  Serial.printf("{\"type\":\"tcon_write_test\",\"rows_sent\":%lu,\"spi_transfer_ok\":%s,"
                "\"completion_confirmed\":%s,\"ints\":[%u,%u,%u,%u]}\n",
    (unsigned long)rows, ok ? "true" : "false", confirmed ? "true" : "false",
    observed.bytes[0], observed.bytes[1], observed.bytes[2], observed.bytes[3]);
  return confirmed;
}
void iris_tcon_probe() {
  // Only diagnose an uninitialized controller. INTS is read-to-clear, so this
  // command must not consume completion events from a live display session.
  if (tcon.ready) { Serial.println("IRIS PROBE: refused while TCON is initialized"); return; }
  if (!io.enable()) {
    io.disable(); Serial.println("IRIS PROBE: supply/latch or SPI unavailable"); return;
  }
  io.pause(300);
  for (int pass = 0; pass < 4; ++pass) {
    uint32_t hz = pass == 1 ? 100000 : 2000000;
    spi_device_handle_t probe = nullptr;
    if (!io.add_device(hz, &probe)) break;
    // Change only weak input bias via IDF, preserving the SPI GPIO matrix.
    // A functioning MISO driver should dominate both weak biases.
    gpio_pull_mode_t pull = pass == 2 ? GPIO_PULLDOWN_ONLY : pass == 3 ? GPIO_PULLUP_ONLY : GPIO_FLOATING;
    gpio_set_pull_mode(gpio_num_t(PIN_MISO), pull); io.pause(1);
    int df_before = digitalRead(DF), gate = digitalRead(BUS_EN);
    uint8_t dc[2]{}, pm = 0, syss = 0, version[4]{}, ints[4]{};
    bool ok = io.read_from(probe, 0x44, dc, 2) && io.read_from(probe, 0x46, &pm, 1)
      && io.read_from(probe, 0x58, &syss, 1) && io.read_from(probe, 0x5d, version, 4)
      && io.read_from(probe, 0x57, ints, 4); // last: clears pending interrupts
    io.pause(1);
    int khz = 0; spi_device_get_actual_freq(probe, &khz);
    spi_bus_remove_device(probe);
    Serial.printf("{\"type\":\"tcon_probe\",\"driver\":\"idf_spi2\",\"spi_actual_khz\":%d,"
                  "\"miso_pull\":\"%s\",\"transaction_ok\":%s,\"bus_en\":%d,"
                  "\"df_before\":%d,\"df_after\":%d,\"dc\":[%u,%u],\"pm\":%u,"
                  "\"syss\":%u,\"parameter_version\":[%u,%u,%u,%u],\"ints\":[%u,%u,%u,%u]}\n",
      khz, pass == 2 ? "down" : pass == 3 ? "up" : "none", ok ? "true" : "false",
      gate, df_before, digitalRead(DF), dc[0], dc[1], pm, syss,
      version[0], version[1], version[2], version[3], ints[0], ints[1], ints[2], ints[3]);
  }
  gpio_set_pull_mode(gpio_num_t(PIN_MISO), GPIO_FLOATING);
  io.disable();
}
#endif
#endif
