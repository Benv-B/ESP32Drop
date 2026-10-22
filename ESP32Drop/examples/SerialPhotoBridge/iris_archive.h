#pragma once
#include <stdint.h>
#include <string.h>
#include <airdrop/core/ad_cpio.h>

namespace iris {
// A single static photo per transfer, and the ceiling that follows from the hardware.
// The archive is the DECODED cpio (the JPEG plus ~600 B of envelope) and has to be held
// whole: the final CRC covers every byte, the cpio walk needs the entries, and the JPEG
// decoder reads out of it. That, not policy, is what bounds it:
//
//   8 MiB PSRAM - 2.25 MiB framebuffer (1024 x 768 x 3) - 320 KiB DvZip residue
//              - 256 KiB streamed decode block - 2 x 8 KiB stream packets
//              - 26 KiB frame ring                ~= 5.1 MiB left before any slack
//
// 4 MiB leaves about 1 MiB for the decoder, fragmentation and the sketch, and is what
// this board was measured with. Raising it is one constant; the reason it is NOT derived
// from free PSRAM at boot is that a cap which moves between boots turns "this photo does
// not fit" into a coin flip, and the failure it buys is an allocation failure in the
// middle of a transfer instead of a refusal at the start.
//
// What the sender sees above it: HTTP 413, and "file over 4 MiB cap" on the badge status.
// HEIC / multi-photo transfers are refused separately at commit(), because this panel's
// decoder takes baseline JPEG only.
constexpr uint32_t ARCHIVE_CAP = 4u * 1024u * 1024u;

struct ArchivePreview {
  enum class State : uint8_t { Waiting, Jpeg, Unsupported, Invalid, Multiple };
  State state = State::Waiting;
  const uint8_t *jpeg = nullptr;
  uint32_t available = 0, total = 0, archive_offset = 0, transfer_id = 0;
  const char *name = nullptr;
  uint32_t name_length = 0;
  // Only the entry's declared bytes have arrived. This is NEVER a successful
  // COMMIT/CRC or proof that another file will not follow later in the archive.
  bool complete = false;
  uint32_t rejected_size = 0, rejected_offset = 0;
  const char *error = "";
};

struct Archive {
  uint8_t *data = nullptr;
  uint32_t capacity = 0, id = 0, used = 0, crc = 0;
  bool active = false;
  const char *error = "";
  using Crc = uint32_t (*)(uint32_t, const uint8_t *, uint32_t);
  bool append(uint32_t transfer, uint32_t offset, const uint8_t *src, uint32_t n, Crc update) {
    if (!active || transfer != id) {
      active = false;
      if (offset != 0) { error = "archive first offset is not zero"; return false; }
      id = transfer; used = crc = 0; error = ""; active = true;
    }
    if (!data || !src || !n) { error = "archive buffer/transfer empty"; active = false; return false; }
    if (offset != used) { error = "archive offset mismatch"; active = false; return false; }
    /* One message per reason. This was a shared "size/offset mismatch", which read the
       same whether the sender desynchronised or the photo is simply larger than
       ARCHIVE_CAP -- opposite things to tell a user, and the second one is recoverable. */
    if (used > capacity || n > capacity - used) {
      error = "file over archive cap"; active = false; return false;
    }
    memcpy(data + used, src, n); used += n; crc = update(crc, src, n); return true;
  }
  bool commit(uint32_t transfer, uint32_t length, uint32_t expected_crc,
              const uint8_t *&jpeg, uint32_t &jpeg_size) {
    jpeg = nullptr; jpeg_size = 0;
    bool valid = active && id == transfer && used == length && crc == expected_crc;
    active = false;
    if (!valid) { error = "archive commit/CRC mismatch"; return false; }
    AdCpioIter it; AdCpioEntry entry;
    ad_cpio_begin(&it, data, used);
    uint32_t photos = 0;
    while (ad_cpio_next(&it, &entry)) {
      if (!ad_cpio_is_regular(entry.mode)) continue;
      if (ad_file_is_sidecar(&entry)) continue;
      if (ad_cpio_sniff(entry.data, entry.len) != AD_FILE_JPEG) {
        error = "send one JPEG; HEIC/PNG/Live Photo video are unsupported"; return false;
      }
      ++photos; jpeg = entry.data; jpeg_size = entry.len;
    }
    if (it.truncated || photos != 1) {
      error = it.truncated ? "archive truncated" : "send exactly one JPEG per transfer"; return false;
    }
    error = ""; return true;
  }

  // Provisional contiguous JPEG bytes during an active transfer. All pointers
  // borrow this archive's buffer; appending the same transfer keeps the prefix
  // intact, but starting another transfer may overwrite it. Call on the same
  // consumer task as append(), never concurrently with an append/new transfer.
  // The caller must separately validate baseline JPEG headers, dimensions and
  // decoder limits; three magic bytes do not mean the JPEG is decoder-ready.
  // No filename, payload, checksum or eventual archive success is trusted here.
  // Scans headers only, without allocation, and leaves append/commit untouched.
  ArchivePreview preview() const {
    using State = ArchivePreview::State;
    ArchivePreview out; out.transfer_id = id;
    if (!active) return out;
    auto fail = [this](State state, const char *message) {
      ArchivePreview result; result.transfer_id = id;
      result.state = state; result.error = message; return result;
    };
    const uint32_t limit = capacity < ARCHIVE_CAP ? capacity : ARCHIVE_CAP;
    if (!used) return out;
    if (!data || used > limit) return fail(State::Invalid, "invalid preview buffer");
    uint32_t pos = 0;
    while (pos < used) {
      if (used - pos < 6) return out;
      const uint8_t *header = data + pos;
      uint32_t hdr, mode, namesize, filesize;
      bool aligned;
      if (!memcmp(header, "070707", 6)) {
        hdr = 76; aligned = false;
        if (used - pos < hdr) return out;
        if (!preview_number(header + 18, 6, 8, mode) ||
            !preview_number(header + 59, 6, 8, namesize) ||
            !preview_number(header + 65, 11, 8, filesize))
          return fail(State::Invalid, "invalid odc preview header");
      } else if (!memcmp(header, "070701", 6) || !memcmp(header, "070702", 6)) {
        hdr = 110; aligned = true;
        if (used - pos < hdr) return out;
        if (!preview_number(header + 14, 8, 16, mode) ||
            !preview_number(header + 54, 8, 16, filesize) ||
            !preview_number(header + 94, 8, 16, namesize))
          return fail(State::Invalid, "invalid newc preview header");
      } else return fail(State::Invalid, "unknown preview archive format");

      const uint32_t name_offset = pos + hdr; // hdr is within already received data
      if (!namesize || namesize > limit - name_offset)
        return fail(State::Invalid, "preview name exceeds archive limit");
      // A preview does not need to handle megabytes of path data repeatedly.
      // Final COMMIT remains the authority and retains its existing behavior.
      if (namesize > 4096) return fail(State::Unsupported, "preview name exceeds 4096 bytes");
      if (namesize > used - name_offset) return out;
      const char *name = reinterpret_cast<const char *>(data + name_offset);
      if (name[namesize-1] != 0 || memchr(name, 0, namesize-1))
        return fail(State::Invalid, "preview name is not terminated correctly");
      if (namesize == 11 && !memcmp(name, "TRAILER!!!", 10)) {
        if (filesize) return fail(State::Invalid, "preview trailer contains data");
        return out;
      }
      uint32_t offset = name_offset + namesize;
      if (aligned) {
        const uint32_t padding = (4 - (offset & 3u)) & 3u;
        if (padding > limit - offset) return fail(State::Invalid, "preview name padding exceeds archive limit");
        offset += padding;
      }
      if (filesize > limit - offset) {
        auto rejected = fail(State::Invalid, "preview file exceeds archive limit");
        rejected.rejected_size = filesize; rejected.rejected_offset = offset;
        return rejected;
      }
      if (offset > used) return out;
      const uint32_t received = used - offset;
      const uint32_t available = received < filesize ? received : filesize;

      if (ad_cpio_is_regular(mode)) {
        const uint8_t *payload = data + offset;
        const bool jpeg = available >= 3 && payload[0] == 0xff &&
                          payload[1] == 0xd8 && payload[2] == 0xff;
        const bool sidecar = available >= 4 && payload[0] == 0 && payload[1] == 5 &&
                             payload[2] == 0x16 && payload[3] == 7;
        if (jpeg) {
          if (out.jpeg) return fail(State::Multiple, "multiple JPEGs in preview archive");
          out.state = State::Jpeg; out.jpeg = payload;
          out.available = available; out.total = filesize; out.archive_offset = offset;
          out.name = name; out.name_length = namesize - 1; out.complete = available == filesize;
        } else if (!sidecar) {
          // Wait for enough actual bytes to distinguish JPEG/AppleDouble; the
          // ._ filename convention alone never authorizes ignoring a payload.
          if (available < 4 && available < filesize) return out;
          return fail(State::Unsupported, "preview requires one JPEG and metadata only");
        }
      }

      if (available < filesize) return out;
      pos = offset + filesize;
      if (aligned) {
        const uint32_t padding = (4 - (pos & 3u)) & 3u;
        if (padding > used - pos) return out; // unreceived (or final omitted) tail padding
        pos += padding;
      }
    }
    return out;
  }

 private:
  // odc's eleven octal digits can represent 33 bits; reject overflow instead of
  // wrapping an attacker-controlled file size into a small buffer offset.
  static bool preview_number(const uint8_t *p, unsigned count, uint32_t base, uint32_t &value) {
    value = 0;
    for (unsigned i = 0; i < count; ++i) {
      uint32_t digit;
      if (p[i] >= '0' && p[i] <= '9') digit = p[i] - '0';
      else if (p[i] >= 'a' && p[i] <= 'f') digit = p[i] - 'a' + 10;
      else if (p[i] >= 'A' && p[i] <= 'F') digit = p[i] - 'A' + 10;
      else return false;
      if (digit >= base || value > (UINT32_MAX - digit) / base) return false;
      value = value * base + digit;
    }
    return true;
  }
};
}
