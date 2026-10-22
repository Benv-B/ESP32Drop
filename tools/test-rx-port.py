"""Test the actual RX staging/worker functions with a deterministic RTOS boundary."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
PORT = ROOT / 'ESP32Drop/src/awdl/port/awdl_port_esp32.cpp'
HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "awdl/core/awdl_ring.h"
#include "awdl/core/awdl_diag.h"
#define CONFIG_IDF_TARGET_ESP32C5 1
#define SNAP_LEN 1600
/* FRAME_DEFINITION */
static RawFrame g_ring[RING_SLOTS];
static AwdlRing g_ring_cur;
static AwdlDiag g_diag;
static void *g_rx_ready = (void *)1;
static bool g_awdl_stop, g_proc_stopped, pending;
static bool wake_after_publish = true;
static unsigned gives, takes, waits, failures;
static const unsigned portMAX_DELAY = ~0u;
static std::vector<unsigned> processed;
static uint8_t payload[SNAP_LEN + 32];
static unsigned stop_after;
static void (*on_wait)();
struct TaskExit {};
static void vTaskDelete(void *) { throw TaskExit{}; }
static int xSemaphoreGive(void *) {
  // Wake must never expose an uncommitted or partially copied frame.
  if (!g_awdl_stop) {
    int i = awdl_ring_peek(&g_ring_cur);
    wake_after_publish &= i >= 0 && g_ring[i].data[0] == 0xa5;
  }
  ++gives; pending = true; return 1;
}
static int xSemaphoreTake(void *, unsigned timeout) {
  ++takes;
  if (!pending && timeout == portMAX_DELAY) {
    ++waits;
    if (on_wait) on_wait();
    else { ++failures; throw TaskExit{}; } // unexpected indefinite sleep
  }
  bool had = pending; pending = false; return had;
}
static void process_frame(const RawFrame *f) {
  processed.push_back(f->rx_ts);
  if (processed.size() == stop_after) g_awdl_stop = true;
}
/* PORT_FUNCTIONS */
static void reset() {
  awdl_ring_init(&g_ring_cur, RING_SLOTS);
  g_diag = {}; g_awdl_stop = g_proc_stopped = pending = false;
  gives = takes = waits = 0; wake_after_publish = true;
  processed.clear(); stop_after = ~0u; on_wait = nullptr;
  memset(payload, 0xa5, sizeof payload);
}
static void run_worker() {
  try { proc_task(nullptr); } catch (const TaskExit &) {}
}
static void check(const char *name, bool pass) {
  printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
  if (!pass) ++failures;
}
static void arriving_at_wait() {
  on_wait = nullptr;
  stage_rx_frame(payload, 100, 77, 1234, -40);
}
static void stopping_at_wait() {
  g_awdl_stop = true; xSemaphoreGive(g_rx_ready); on_wait = nullptr;
}
int main() {
  reset();
  for (unsigned i = 0; i < RING_SLOTS - 1; ++i)
    stage_rx_frame(payload, 100, i, 1000 + i, -40);
  stage_rx_frame(payload, 100, 999, 0, -40);
  check("burst fills usable capacity; overflow is counted without overwriting",
        awdl_ring_used(&g_ring_cur) == RING_SLOTS - 1 &&
        g_ring_cur.drops == 1 && g_diag.rx_ring_peak == RING_SLOTS - 1);
  check("only committed frames notify and notifications coalesce",
        gives == RING_SLOTS - 1 && pending && wake_after_publish);
  stop_after = RING_SLOTS - 1; run_worker();
  bool ordered = processed.size() == RING_SLOTS - 1;
  for (unsigned i = 0; i < processed.size(); ++i) ordered &= processed[i] == i;
  check("worker drains burst in FIFO order without an idle wait",
        ordered && takes == 0 && awdl_ring_used(&g_ring_cur) == 0);
  check("stop is acknowledged after releasing the processed slot", g_proc_stopped);

  reset(); stage_rx_frame(payload, sizeof payload, 10, 1234, -42);
  const RawFrame &f = g_ring[0];
  check("snapshot bounds and metadata survive staging",
        f.len == SNAP_LEN && f.rx_ts == 10 && f.now_us == 1234 && f.rssi == -42 &&
        !memcmp(f.data, payload, SNAP_LEN));

  reset(); stop_after = 1; on_wait = arriving_at_wait; run_worker();
  check("arrival between empty check and wait is retained and processed",
        waits == 1 && processed.size() == 1 && processed[0] == 77 && g_proc_stopped);

  reset(); pending = true; stop_after = 1; on_wait = arriving_at_wait; run_worker();
  check("stale wake does not replace checking the ring or lose the next arrival",
        takes == 2 && waits == 1 && processed.size() == 1 && g_proc_stopped);

  reset(); on_wait = stopping_at_wait; run_worker();
  check("shutdown wakes an idle worker without requiring a radio frame",
        waits == 1 && g_proc_stopped && processed.empty());

  reset();
  for (unsigned i = 0; i < 4; ++i) stage_rx_frame(payload, 100, i, i, -40);
  stop_after = 1; run_worker();
  check("shutdown under backlog stops between frames, not with a reserved slot",
        g_proc_stopped && processed.size() == 1 && awdl_ring_used(&g_ring_cur) == 3);
  return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default='clang++')
    args = parser.parse_args()
    source = PORT.read_text(encoding='utf-8')
    functions = []
    for name in ('stage_rx_frame', 'proc_task'):
        found = re.findall(r'^static void ' + name + r'\([^)]*\) \{.*?^\}', source, re.M | re.S)
        if len(found) != 1:
            raise RuntimeError('Expected one definition: ' + name)
        functions.append(found[0])
    frames = re.findall(r'^struct RawFrame \{.*?^\};', source, re.M | re.S)
    if len(frames) != 1:
        raise RuntimeError('Expected one RawFrame definition')
    out = ROOT / '.build-host-tests'
    out.mkdir(exist_ok=True)
    test = out / 'rx_port.cpp'
    test.write_text(HARNESS.replace('/* FRAME_DEFINITION */', frames[0])
                    .replace('/* PORT_FUNCTIONS */', '\n'.join(functions)), encoding='utf-8')
    exe = out / 'rx_port.exe'
    for slots in (16, 32):
        print('RX worker with', slots, 'ring slots', flush=True)
        subprocess.run([args.cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                        f'-DRING_SLOTS={slots}', '-I', str(ROOT / 'ESP32Drop/src'),
                        str(test), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
