#ifdef PHOTO_FRAME_RX_BENCH
#if defined(PHOTO_FRAME_TCON)
#error "Receive benchmark and display output are mutually exclusive"
#endif
#include <Arduino.h>
#include <ESP32Drop.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <esp_wifi.h>
#include "bridge_app.h"
#include "rx_bench_consumer.h"
#include "rx_bench_history.h"
#ifdef PHOTO_FRAME_TCP_RX_PROFILE
extern "C" unsigned photo_frame_tcp_window(void);
extern "C" unsigned photo_frame_tcp_mailbox(void);
#else
static unsigned photo_frame_tcp_window() { return CONFIG_LWIP_TCP_WND_DEFAULT; }
static unsigned photo_frame_tcp_mailbox() { return CONFIG_LWIP_TCP_RECVMBOX_SIZE; }
#endif

namespace {
RxBenchConsumer sink;
AdStreamPacket *packet = nullptr;
bool ready = false;
const char *boot_error = "";
char boot_error_detail[80];
uint32_t consumer_us = 0;
RxBenchHistory *history = nullptr;
uint64_t history_next = 0, history_end = 0;

void request_history() {
  AdStatus current{};
  if (ready) ad_stats_read(&current);
  if (current.rx_active) { Serial.println("RX TRACE BUSY: upload in progress"); return; }
  if (!history) { Serial.println("RX TRACE unavailable: allocation failed"); return; }
  history_next = history->oldest();
  history_end = history->written;
  Serial.printf("RX TRACE BEGIN count=%u; volatile, cleared on reboot\n",
                unsigned(history_end - history_next));
  if (history_next == history_end) Serial.println("RX TRACE END");
}

void drain_history() {
  if (history_next == history_end || !history) return;
  AdStatus current{};
  if (ready) ad_stats_read(&current);
  if (!Serial || current.rx_active) {
    history_next = history_end;
    if (Serial) Serial.println("RX TRACE paused for upload; request TRACE again after upload");
    return;
  }
  // Keep the loop available to the stream consumer even when a dump was requested.
  for (int i = 0; i < 2 && history_next < history_end; ++i, ++history_next) {
    const char *line = history->read(history_next);
    Serial.print("RX TRACE: ");
    Serial.println(line ? line : "[record overwritten]");
  }
  if (history_next == history_end) Serial.println("RX TRACE END");
}

void status() {
  if (!Serial) return;
  AdStatus ad{}; AwdlStatus link{}; AwdlDiag radio{};
  if (ready) { ad_stats_read(&ad); awdl_status_read(&link); awdl_diag_read(&radio,false); }
  uint8_t channel = 0; wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
  if (ready && esp_wifi_get_channel(&channel,&secondary) != ESP_OK) channel = 0;
  for (char *p=ad.file_errmsg;*p;++p) if (*p=='"'||*p=='\\'||(unsigned char)*p<32) *p='?';
  Serial.printf("{\"firmware\":\"xiao-c5-rx-bench-7\",\"output\":\"receive_benchmark\","
    "\"name\":\"Photo Frame RX Test\",\"ready\":%s,\"uptime_ms\":%lu,\"state\":\"%s\","
    "\"error\":\"%s\",\"file_error\":\"%s\",\"file_err\":%lu,\"upload_ok\":%lu,"
    "\"rx_bytes\":%lu,\"rx_elapsed_ms\":%lu,\"archive_bytes\":%lu,\"jpeg_bytes\":%lu,\"jpeg_crc32\":\"%08lx\","
    "\"validated\":%lu,\"consumer_us\":%lu,\"inflate_us\":%lu,\"rx_active\":%s,"
    "\"saved\":false,\"displayed\":0,\"tcp_rx_window\":%u,\"tcp_rx_mailbox\":%u,\"network\":{\"radio_channel\":%u,\"have_master\":%s,"
    "\"locked\":%s,\"peers\":%ld,\"ch_mask\":%u,\"mif_sent\":%lu,\"mif_age_ms\":%lu,"
    "\"query_seen\":%lu,\"resp_tx\":%lu,\"announce_tx\":%lu,\"announce_errors\":%lu,"
    "\"tcp_accept\":%lu,\"tls_ok\":%lu,\"discover_ok\":%lu,\"ask_ok\":%lu,\"desync\":%lu,"
    "\"syn_airdrop\":%lu,\"tcp_tous\":%lu,\"syn_pair1\":%lu,\"syn_pair2\":%lu,"
    "\"netif_mtu\":%u,\"netif_mtu_error\":%ld,\"netif_tx_sent\":%lu,\"netif_tx_errors\":%lu,\"txq_drop\":%lu,\"txq_capacity\":%u,\"txq_peak\":%u,"
    "\"tx_done_ok\":%lu,\"tx_done_fail\":%lu,\"retry_queued\":%lu,\"retry_exhausted\":%lu,"
    "\"inflight\":%u,\"rx_to_us\":%lu,\"rx_matched\":%lu,\"rx_retry\":%lu,"
    "\"rx_duplicates\":%lu,\"ring_drop\":%lu,\"ring_capacity\":%lu,\"ring_peak\":%lu,\"rx_queue_us_max\":%lu,"
    "\"proc_us_max\":%lu,\"tx_call_us_max\":%lu,\"mif_late\":%lu,\"internal_free\":%lu,\"internal_largest\":%lu}}\n",
    ready?"true":"false",(unsigned long)millis(),!ready?"error":ad.rx_active?"receiving":ad.file_errmsg[0]?"error":ad.upload_ok?"received":"waiting",
    boot_error,ad.file_errmsg,(unsigned long)ad.file_err,(unsigned long)ad.upload_ok,
    (unsigned long)ad.rx_bytes,(unsigned long)ad.rx_elapsed_ms,(unsigned long)sink.archive.used,
    (unsigned long)sink.jpeg_bytes,(unsigned long)sink.jpeg_crc32,(unsigned long)sink.validated,(unsigned long)consumer_us,
    (unsigned long)ad.decode_us,ad.rx_active?"true":"false",photo_frame_tcp_window(),photo_frame_tcp_mailbox(),unsigned(channel),link.have_master?"true":"false",
    link.locked?"true":"false",(long)link.peers,unsigned(link.ch6_mask),(unsigned long)link.mif_sent,(unsigned long)link.ms_since_mif,
    (unsigned long)ad.query_seen,(unsigned long)ad.resp_tx,(unsigned long)ad.announce_tx,(unsigned long)ad.announce_errors,
    (unsigned long)ad.tcp_accept,(unsigned long)ad.tls_ok,(unsigned long)ad.discover_ok,(unsigned long)ad.ask_ok,(unsigned long)ad.desync,
    (unsigned long)ad.syn_airdrop,(unsigned long)ad.tcp_tous,(unsigned long)ad.syn_pair1,(unsigned long)ad.syn_pair2,
    unsigned(radio.netif_mtu_set),(long)radio.netif_mtu_err,
    (unsigned long)radio.netif_tx_sent,(unsigned long)radio.netif_tx_err,(unsigned long)radio.netif_txq_drop,
    unsigned(AWDL_NETIF_TX_QUEUE_DEPTH),unsigned(radio.txq_depth_max),
    (unsigned long)radio.txdone_ucast_ok,(unsigned long)radio.txdone_ucast_fail,(unsigned long)radio.netif_retry_queued,(unsigned long)radio.netif_retry_exhausted,
    unsigned(radio.netif_inflight),(unsigned long)radio.rx_to_us,(unsigned long)radio.rx_to_us_matched,(unsigned long)radio.rx_to_us_retry,
    (unsigned long)radio.netif_rx_duplicates,(unsigned long)radio.ring_drop,
    (unsigned long)radio.rx_ring_capacity,(unsigned long)radio.rx_ring_peak,(unsigned long)radio.rx_queue_us_max,
    (unsigned long)radio.proc_max_us,(unsigned long)radio.netif_tx_call_us_max,(unsigned long)link.mif_late,
    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
    (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
}
}

void bridge_setup() {
  Serial.begin(115200);
  history = static_cast<RxBenchHistory *>(heap_caps_malloc(sizeof(RxBenchHistory),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if (history) history->init();
  sink.archive.capacity=iris::ARCHIVE_CAP;
  sink.archive.data=(uint8_t *)heap_caps_malloc(iris::ARCHIVE_CAP,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  packet=(AdStreamPacket *)heap_caps_malloc(sizeof(AdStreamPacket),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if (!packet || !sink.archive.data || !ad_stream_enable()) { boot_error="benchmark allocation failed"; return; }
  const esp_err_t awdl_error = awdl_begin();
  if (awdl_error!=ESP_OK) {
    snprintf(boot_error_detail,sizeof(boot_error_detail),"AWDL initialization failed: %s (%d)",
             esp_err_to_name(awdl_error),int(awdl_error));
    boot_error=boot_error_detail;
    return;
  }
  if (ad_begin("Photo Frame RX Test",iris::ARCHIVE_CAP)!=0) { boot_error="AirDrop initialization failed"; return; }
  ready=true;
}

void bridge_loop() {
  if (ready && ad_stream_read(packet)) {
    if (packet->kind==AD_STREAM_DATA && packet->offset==0) consumer_us=0;
    const uint32_t started=micros();
    const bool ok=sink.consume(*packet,esp_rom_crc32_le);
    consumer_us+=micros()-started;
    if (ok) ad_stream_ack(packet->token,true,packet->kind==AD_STREAM_COMMIT?1:0);
    else ad_stream_reject(packet->token,AD_STREAM_ERROR_INVALID,sink.error);
    if (packet->kind==AD_STREAM_COMMIT && Serial)
      Serial.printf("RX BENCH: archive_valid=%d bytes=%lu jpeg=%lu consumer_us=%lu; RAM only, not saved/displayed\n",
        ok,(unsigned long)sink.archive.used,(unsigned long)sink.jpeg_bytes,(unsigned long)consumer_us);
  }
  // Read-only diagnostics; no latch, NFC, SPI, or display initialization.
  static char command[24]; static size_t used=0;
  while (Serial.available()) {
    char c=Serial.read();
    if (c=='\n') {
      command[used]=0;
      if (!strcmp(command,"STATUS")) status();
      else if (!strcmp(command,"TRACE")) request_history();
      used=0;
    }
    else if(c!='\r' && used+1<sizeof(command)) command[used++]=c;
  }
  char line[200];
  for(int i=0;i<8&&ad_diag_read_line(line,sizeof(line));++i) {
    // Retain timestamped connection/request events even without a serial reader.
    // Exclude high-volume packet and mDNS traces so handshakes survive idle time.
    if (history && !strncmp(line,"EV ",3)) history->push(line);
    if(Serial)Serial.print(line);
  }
  for(int i=0;i<4&&awdl_diag_read_line(line,sizeof(line));++i) if(Serial)Serial.print(line);
  drain_history();
  static uint32_t last=0;
  if(millis()-last>=2000) { last=millis();status(); }
  delay(1);
}
#endif
