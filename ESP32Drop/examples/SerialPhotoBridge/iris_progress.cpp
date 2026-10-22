#ifdef PHOTO_FRAME_TCON
#include <atomic>
#include <driver/gpio.h>
#include <esp_timer.h>
#include "iris_progress.h"

namespace {
// XIAO ESP32-C5 V1.1 D3: +3V3 -> R13 (1.5k) -> LED -> GPIO27.
// Use the physical XIAO pin, not the M5Stamp board's LED alias.
constexpr gpio_num_t LED = GPIO_NUM_27;
std::atomic<IrisProgress> phase{IrisProgress::Off};
std::atomic<uint32_t> started{0};
esp_timer_handle_t timer = nullptr;
void tick(void *) {
  IrisProgress p = phase.load(std::memory_order_acquire);
  uint32_t elapsed = uint32_t(esp_timer_get_time() / 1000) - started.load();
  bool on = false;
  switch (p) {
    case IrisProgress::Waiting: on = elapsed % 3000 < 50; break;
    case IrisProgress::Receiving: on = elapsed % 400 < 200; break;
    case IrisProgress::Processing: on = elapsed % 1000 < 500; break;
    case IrisProgress::Refreshing: on = true; break;
    case IrisProgress::Done: on = elapsed < 800 && elapsed % 400 < 150; break;
    case IrisProgress::Error: on = elapsed % 2000 < 600 && elapsed % 200 < 100; break;
    case IrisProgress::Off: break;
  }
  gpio_set_level(LED, on ? 0 : 1);
}
}
bool iris_progress_begin() {
  if (timer) return true;
  gpio_set_level(LED, 1);
  if (gpio_set_direction(LED, GPIO_MODE_OUTPUT) != ESP_OK) return false;
  esp_timer_create_args_t args{};
  args.callback = tick; args.dispatch_method = ESP_TIMER_TASK; args.name = "iris_led";
  if (esp_timer_create(&args, &timer) != ESP_OK) return false;
  if (esp_timer_start_periodic(timer, 50000) != ESP_OK) {
    esp_timer_delete(timer); timer = nullptr; return false;
  }
  return true;
}
void iris_progress_set(IrisProgress next) {
  if (phase.load() == next) return;
  started.store(uint32_t(esp_timer_get_time() / 1000));
  phase.store(next, std::memory_order_release);
  if (next == IrisProgress::Off) gpio_set_level(LED, 1);
}
#endif
