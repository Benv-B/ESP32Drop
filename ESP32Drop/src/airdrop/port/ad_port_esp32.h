/* ad_port_esp32.h -- the AirDrop layer's entry points on the ESP32 backend.
 *
 * Everything here runs over a link the AWDL layer owns. There is no configuration for
 * that link and none is offered: AirDrop is not a protocol you can point somewhere else.
 *
 * THE CALLBACK RULE. ad_poll() is the only thing that runs user code, and it runs it on
 * the CALLER's task. Nothing in this header can put your code on the TLS task or the
 * frame task, which is why there is no "onReceive fires from wherever" option -- the
 * invariants those tasks carry are not ones a library can ask a user to honour.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../core/ad_status.h"
#include "../core/ad_stream_error.h"
#include "../core/ad_dnssd.h"   /* AIRDROP_PORT */
#include "../core/ad_cpio.h"    /* AD_FILE_* and the archive walker */


/* One received file, valid ONLY for the duration of the callback. The bytes live in a
   PSRAM block that ad_poll() releases once the whole archive has been handed over -- copy
   what you need, do not keep the pointer.
   
   Each file is delivered once unless the callback explicitly requests a retry with
   ad_file_retry_later(). A transfer is a cpio archive and may hold several; you get each
   one. The library does not choose between them, and in particular it does not filter by
   type -- `type` is sniffed from the leading bytes as a convenience, and AD_FILE_OTHER is
   delivered exactly like the rest. (The code this replaces returned the FIRST JPEG or PNG
   and discarded the archive, so a PDF, a text file, or the second of two photos vanished
   and the transfer was reported as "no displayable image".) */
struct AdFile {
  const uint8_t *data;
  uint32_t       len;
  const char    *name;      /* basename, NUL-terminated                                */
  const char    *path;      /* as the sender wrote it, e.g. "./photo.png"              */
  int            type;      /* AD_FILE_* -- from the bytes, never from the name        */
  uint32_t       index;     /* 0-based position among the files in THIS transfer       */
  uint32_t       count;     /* how many files this transfer carries                    */
  uint32_t       transfer_id;    /* optional live archive identity, 0 if unavailable */
  uint32_t       archive_offset; /* file data offset inside that archive */
};

/* Optional live decoded-archive tap. Enable BEFORE ad_begin(). The TLS task only
 * publishes immutable prefixes; callers copy at most 8192 bytes per read. No user
 * code or USB I/O runs on the TLS task, and no extra image-sized buffer is allocated.
 * State: 0 unavailable, 1 receiving, 2 validated archive, 3 aborted. A new id resets
 * the requested offset to zero. Partial bytes are provisional, never a saved file.
 * Successful data stays readable until ad_poll() finishes the file callbacks. */
struct AdPreview {
  uint32_t transfer_id, offset, length, available;
  uint8_t state;
};
bool ad_preview_enable(void);
void ad_preview_read(uint32_t transfer_id, uint32_t offset, uint8_t *dst,
                     uint32_t capacity, struct AdPreview *out);

/* Optional bounded archive relay. Enable before ad_begin(), instead of preview.
 * The receive task decodes one record and waits for the consumer's ACK; it never
 * invokes application code. Poll on the application task; USB and SPI consumers
 * use the same DATA/COMMIT contract. Kind values also match host/protocol.py.
 * COMMIT may be acknowledged only after validating cpio and completing the
 * consumer's output contract (durable files, or confirmed display completion).
 * Timeout/disconnect fails the upload before its HTTP success response. */
enum { AD_STREAM_DATA = 8, AD_STREAM_COMMIT = 9, AD_STREAM_CHUNK_SIZE = 8192 };
struct AdStreamPacket {
  uint32_t token, transfer_id, offset, length, crc;
  uint8_t kind;
  uint8_t data[AD_STREAM_CHUNK_SIZE];
};
bool ad_stream_enable(void);
bool ad_stream_read(struct AdStreamPacket *out);
void ad_stream_ack(uint32_t token, bool accepted, uint32_t files);
/* Copies the reason; caller storage need not outlive this call. Late tokens are ignored. */
void ad_stream_reject(uint32_t token, enum AdStreamError error, const char *message);
/* Explicit diagnostic: 7 MiB synthetic archive through the real pump/consumer queues.
 * No radio transfer is measured. Refuses while an upload/delivery is pending. */
bool ad_stream_self_test(void);

/* Return false if you could not use the file. That is counted and reported through
   AdStatus.file_err / file_errmsg, so a badge that receives a format it cannot draw says
   so in its diagnostics instead of dropping the transfer silently. */
typedef bool (*ad_file_cb_t)(void *ctx, const struct AdFile *f);

void ad_on_file(ad_file_cb_t cb, void *ctx);

/* Inside a file callback ONLY: request another attempt, then return false.
 * The archive stays in PSRAM until delivery succeeds; ad_poll retries after 1 s.
 * Earlier accepted files are skipped. New uploads get 503 while it is retained.
 * No callback pointer may be kept by the application; reconnect the consumer to
 * resume. Power loss still loses PSRAM. A false return without this call is a
 * permanent refusal, preserving the original callback contract. */
void ad_file_retry_later(void);

/* Starts the TLS listener. AWDL must already be up: this binds to the link-local address
 * the AWDL netif owns.
 *
 * `name` is what appears on the sender's share sheet. It is UTF-8, so "リビングのバッジ"
 * and "badge 🐱" work -- the plist carries non-ASCII as UTF-16BE. It is measured, not
 * assumed: the tile reads ReceiverComputerName out of the /Discover response, established
 * by giving three candidate fields three different values and looking.
 *
 * Returns 0; -1 if the listener task could not be created; -2 if the name could not be
 * encoded (malformed UTF-8, or longer than the format's one-byte length). A name that
 * cannot be encoded is refused rather than truncated: it is something a user typed. */
/* `max_receive` bounds the DECODED archive this device will accept, in bytes, and with it
 * the PSRAM the library takes while a transfer is running. Pass 0 for the library's own
 * default of 6 MiB (RX_MAX_AUTO in ad_port_esp32.cpp). PSRAM only ever lowers that ceiling,
 * never raises it in the default buffered mode. With ad_stream_enable(), only a
 * 256 KiB record is retained and max_receive bounds total decoded stream bytes;
 * the consumer is responsible for validation and durable storage before COMMIT.
 *
 * WHY IT IS YOURS TO SET. Decoding happens while the body arrives, so the library holds
 * one block about the size of the finished archive for the length of a transfer. Left to
 * itself it takes nearly all of PSRAM -- which is right for a badge that does nothing else
 * and wrong for a sketch that wants a framebuffer, a sound sample or a log of its own.
 * Whoever wrote the sketch knows which; the library does not.
 *
 * A transfer larger than this is refused with HTTP 413 to the sender, so the Mac says the
 * transfer failed instead of appearing to succeed. That is a policy limit and it is
 * reported as one; running out of memory below the limit is a different fact and answers
 * 507. ad_stats_read() publishes the effective ceiling, which may be lower than you asked
 * for if PSRAM could not supply it.
 *
 * Sizes worth knowing, all measured: a 2.4 MB photo decodes to a 2,354,688-byte archive;
 * the largest free PSRAM block on an ESP32-S3R8 with this firmware is 7,864,308 B, and once
 * the residue buffer and RX_OUT_HEADROOM are taken out there is room for about 7.1 MB of
 * decoded archive. That is the most an explicit max_receive can actually get. */
int  ad_begin(const char *name, uint32_t max_receive);

/* Fires pending callbacks on the calling task. Cheap when there is nothing to do.
   Blocking inside a callback costs nothing on the air: the transmit cadence is owned by
   the AWDL layer's own task and does not depend on this being called promptly. */
void ad_poll(void);

/* True once ad_begin() has claimed this device for receiving. */
bool ad_role_is_receiver(void);

/* Power control: close admission only between complete TLS/jobs and with no
 * retained archive. Once closed, restart the MCU to receive again. Wait for
 * quiesced before awdl_end(); the listener then owns no sockets/netif work. */
bool ad_shutdown_busy(void);
bool ad_shutdown_if_idle(void);
bool ad_shutdown_quiesced(void);

void ad_stats_read(struct AdStatus *out);

/* The SMALLEST free stack the TLS listener task has ever had, in bytes, or 0 before it
 * runs. The task is created with 32,768; a real transfer peaks at 17,348 bytes used, so
 * roughly 15 KB of that is headroom. The number matters because 32,768 CONTIGUOUS bytes of
 * internal RAM is the largest single allocation this library makes, out of the same heap
 * mbedTLS is competing for -- an ad_begin() that returns -1 failed to find that block, not
 * to find enough stack. Whether the reserve can be cut is then a measurement rather than an
 * argument.
 *
 * Sampled on demand; call at a low rate, not for every received packet. */
uint32_t ad_listen_stack_min_free(void);

/* --- diagnostics ----------------------------------------------------------------
 *
 * The library never writes to Serial. It stages one line at a time into a static queue and
 * you decide where they go -- which is the only arrangement that works, because these
 * lines are produced on the TLS task and a user-supplied sink would run there too. There
 * is no setLogSink() for that reason.
 *
 * Copies ONE whole line (NUL-terminated, keeping its own trailing newline) into dst and
 * returns its length, or 0 when the queue is empty. Call it in a loop:
 *
 *     char line[200];
 *     while (ad_diag_read_line(line, sizeof(line))) Serial.print(line);
 *
 * Lines are dropped rather than blocking a protocol task when the queue is full;
 * ad_diag_dropped() is the count, and a non-zero value means you are draining too slowly,
 * not that anything is wrong on the air. */
size_t   ad_diag_read_line(char *dst, size_t cap);
uint32_t ad_diag_dropped(void);

#ifdef AIRDROP_BODY_KEEP
/* --- raw-body probe (NOT part of the library's interface) ------------------------
 *
 * Only under -DAIRDROP_BODY_KEEP, which tools/build-bodykeep.sh sets. In that build the
 * /Upload body is NOT parsed: it is held so the sketch can write it out verbatim, because
 * a captured stream answers every question about the format at once and offline, while a
 * device-side instrument answers one question per physical AirDrop.
 *
 * ad_body_peek() returns the length and lends the pointer -- the library still owns the
 * memory. Call ad_body_release() when finished with it; nothing else frees it, so a
 * caller that forgets holds 2.35 MB of PSRAM and drops the next transfer. */
uint32_t ad_body_peek(const uint8_t **p);
void     ad_body_release(void);
#endif
