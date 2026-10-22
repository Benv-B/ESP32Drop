"""Compile the actual port completion/gate functions against a fake Wi-Fi queue.

Only SDK/RTOS boundaries are stubbed. Extracting these small functions avoids
duplicating the completion policy, or having to emulate the entire ESP32 port.
"""
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
#define AWDL_NETIF_INFLIGHT_MAX 1
#ifndef AWDL_NETIF_MTU
#define AWDL_NETIF_MTU 1280
#endif
#define AWDL_TX_HEADROOM 40
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
static const uint8_t NETIF_TX_RETRY_SLOTS = 64, NETIF_TX_RETRY_MAX = 7;
struct NetifTxFrame { uint16_t len; uint8_t buf[AWDL_NETIF_MTU + AWDL_TX_HEADROOM]; };
struct esp_80211_tx_info_t { uint8_t *data; uint8_t data_len; int tx_status; };
static const int WIFI_SEND_SUCCESS = 0, pdTRUE = 1;
static volatile uint8_t g_netif_inflight;
static volatile uint16_t g_netif_inflight_seq[64], g_retry_seq[64];
static volatile uint8_t g_netif_inflight_used[64], g_retry_n[64];
static void *g_netif_txq = (void *)1, *g_cadence_task = (void *)1;
static NetifTxFrame retry_storage, queued_frame;
static NetifTxFrame *g_retry_frame = &retry_storage;
static bool queue_full;
static unsigned queued, notified;
struct {
  unsigned netif_inflight, netif_inflight_max;
  unsigned txdone_mcast_ok, txdone_mcast_fail, txdone_ucast_ok, txdone_ucast_fail;
  unsigned txdone_action_ok, txdone_action_fail;
  unsigned netif_retry_exhausted, netif_retry_drop, netif_retry_queued;
} g_diag;
static void xTaskNotifyGive(void *) { ++notified; }
static int xQueueSendToFront(void *, const NetifTxFrame *f, int) {
  if (queue_full) return 0;
  queued_frame = *f; ++queued; return pdTRUE;
}
/* PORT_FUNCTIONS */
static uint8_t frame[AWDL_NETIF_MTU + AWDL_TX_HEADROOM];
static void reset(bool multicast, uint16_t seq = 5) {
  g_netif_inflight = 0;
  memset((void *)g_netif_inflight_used, 0, sizeof g_netif_inflight_used);
  memset((void *)g_netif_inflight_seq, 0, sizeof g_netif_inflight_seq);
  memset((void *)g_retry_seq, 0, sizeof g_retry_seq);
  memset((void *)g_retry_n, 0, sizeof g_retry_n);
  memset(&g_diag, 0, sizeof g_diag); memset(frame, 0, sizeof frame);
  frame[0] = 0x08; frame[4] = multicast ? 0x33 : 0x02;
  frame[32] = 3; frame[33] = 4; frame[34] = uint8_t(seq); frame[35] = uint8_t(seq >> 8);
  frame[40] = 0x60; frame[45] = 20;
  g_retry_seq[seq & 63] = seq; queued = notified = 0; queue_full = false;
  g_netif_txq = (void *)1; g_retry_frame = &retry_storage;
}
static void complete(bool ok) {
  esp_80211_tx_info_t info = {frame, 100, ok ? WIFI_SEND_SUCCESS : 1};
  raw_tx_done_cb(&info);
}
static int failures;
static void check(const char *name, bool ok) {
  printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++failures;
}
int main() {
  for (bool ok : {true, false}) {
    reset(true); netif_inflight_begin(5); complete(ok);
    check(ok ? "multicast success releases sole TX slot" : "multicast failure releases sole TX slot",
          netif_inflight_room() && !g_netif_inflight && !queued && notified == 1 &&
          g_diag.txdone_ucast_ok == 0 && g_diag.txdone_ucast_fail == 0);
  }
  reset(false); netif_inflight_begin(5); complete(true);
  check("unicast success releases without retry", netif_inflight_room() && !queued);
  reset(false); netif_inflight_begin(5); complete(false);
  check("unicast failure retains exact frame for bounded retry",
        netif_inflight_room() && queued == 1 && queued_frame.len == 100 &&
        !memcmp(queued_frame.buf, frame, 100) && g_retry_n[5] == 1);
  complete(false);
  check("duplicate completion cannot enqueue another retry", queued == 1 && g_retry_n[5] == 1);
  reset(false); netif_inflight_begin(5);
  frame[44] = uint8_t((AWDL_NETIF_MTU - 40) >> 8);
  frame[45] = uint8_t(AWDL_NETIF_MTU - 40); complete(false);
  check("full MTU retry copies the IPv6 length, not the 8-bit callback length",
        queued == 1 && queued_frame.len == sizeof frame &&
        !memcmp(queued_frame.buf, frame, sizeof frame));
  reset(false, 69); netif_inflight_begin(5); complete(false);
  check("unowned tap/aliased sequence cannot release active slot", g_netif_inflight == 1 && !queued);
  reset(false); netif_inflight_begin(5); g_retry_n[5] = 7; complete(false);
  check("retry exhaustion still releases slot", netif_inflight_room() && !queued && g_diag.netif_retry_exhausted == 1);
  reset(false); netif_inflight_begin(5); queue_full = true; complete(false);
  check("retry queue overflow still releases slot", netif_inflight_room() && !queued && g_diag.netif_retry_drop == 1);
  reset(false); netif_inflight_begin(5); frame[0] = 0xd0; complete(true);
  check("action completion cannot release data slot", g_netif_inflight == 1 && !queued && g_diag.txdone_action_ok == 1);
  reset(false); netif_inflight_begin(5); netif_inflight_done(5);
  check("synchronous injection refusal releases slot", netif_inflight_room());
  return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default='clang++')
    args = parser.parse_args()
    source = PORT.read_text(encoding='utf-8')
    functions = []
    for name in ('netif_inflight_room', 'netif_inflight_begin', 'netif_inflight_done', 'raw_tx_done_cb'):
        matches = re.findall(r'^static (?:bool|void) ' + name + r'\([^)]*\) \{.*?^\}', source, re.M | re.S)
        if len(matches) != 1:
            raise RuntimeError('Expected one function definition: ' + name)
        functions.append(matches[0])
    out = ROOT / '.build-host-tests'
    out.mkdir(exist_ok=True)
    test = out / 'netif_tx_completion.cpp'
    test.write_text('#include <initializer_list>\n' + HARNESS.replace('/* PORT_FUNCTIONS */', '\n'.join(functions)), encoding='utf-8')
    exe = out / 'netif_tx_completion.exe'
    for mtu in (1280, 1460):
        print('Testing port TX completion at MTU', mtu, flush=True)
        subprocess.run([args.cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                        f'-DAWDL_NETIF_MTU={mtu}', str(test), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
