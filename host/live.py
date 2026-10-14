"""Bounded, provisional archive reception; files commit only with their own CRC."""
from __future__ import annotations

import struct
import zlib

ARCHIVE_LIMIT = 6 * 1024 * 1024


def archive_images(data: bytes):
    """Walk available odc/newc headers without searching inside file payloads.

    Yields name, received prefix, declared file size. Incomplete headers simply
    wait for more bytes; malformed headers raise. No partial file is persisted.
    """
    pos = 0
    while pos + 6 <= len(data):
        magic = data[pos:pos + 6]
        if magic == b"070707":
            header, base, align = 76, 8, False
            fields = ((18, 6), (59, 6), (65, 11))
        elif magic in (b"070701", b"070702"):
            header, base, align = 110, 16, True
            fields = ((14, 8), (94, 8), (54, 8))
        else:
            raise ValueError("invalid cpio header")
        if pos + header > len(data):
            return
        values = []
        digits = b"01234567" if base == 8 else b"0123456789abcdefABCDEF"
        for offset, count in fields:
            field = data[pos + offset:pos + offset + count]
            if any(c not in digits for c in field):
                raise ValueError("invalid cpio number")
            values.append(int(field, base))
        mode, namesize, size = values
        if not 1 <= namesize <= 4096 or size > ARCHIVE_LIMIT:
            raise ValueError("cpio entry exceeds limits")
        endname = pos + header + namesize
        if endname > len(data):
            return
        rawname = data[pos + header:endname]
        if rawname[-1:] != b"\0":
            raise ValueError("cpio name lacks terminator")
        if rawname == b"TRAILER!!!\0":
            return
        start = (endname + 3) & ~3 if align else endname
        if start + size > ARCHIVE_LIMIT:
            raise ValueError("cpio entry exceeds archive limit")
        name = rawname[:-1].decode("utf-8", errors="replace").replace("\\", "/").split("/")[-1]
        if (mode == 0 or mode & 0o170000 == 0o100000) and not name.startswith("._"):
            prefix = data[start:min(start + size, len(data))]
            yield name, prefix, size
        pos = (start + size + 3) & ~3 if align else start + size


class LiveArchive:
    def __init__(self):
        self.ident = 0
        self.data = bytearray()
        self.valid = False

    def reset(self):
        self.ident = 0
        self.data.clear()
        self.valid = False

    def append(self, payload: bytes):
        if not 8 < len(payload) <= 8200:
            raise ValueError("invalid archive chunk length")
        ident, offset = struct.unpack_from("<II", payload)
        if not ident:
            raise ValueError("invalid transfer id")
        if ident != self.ident:
            self.ident = ident
            self.data.clear()
            self.valid = True
        if not self.valid or offset != len(self.data) or offset + len(payload) - 8 > ARCHIVE_LIMIT:
            self.valid = False
            self.data.clear()
            raise ValueError("archive gap/duplicate/overflow; complete-file fallback required")
        self.data.extend(payload[8:])

    def reference(self, payload: bytes) -> bytes:
        if len(payload) < 19:
            raise ValueError("short file reference")
        ident, offset, length, crc, namesize = struct.unpack_from("<IIIIH", payload)
        if not self.valid or ident != self.ident or offset + length > len(self.data):
            raise ValueError("referenced file is missing bytes")
        if not 1 <= namesize <= 255 or len(payload) != 18 + namesize:
            raise ValueError("invalid reference filename")
        data = bytes(self.data[offset:offset + length])
        if zlib.crc32(data) != crc:
            raise ValueError("file CRC32 mismatch")
        # Preserve the exact UTF-8 name bytes used by the legacy fallback.
        return payload[16:18] + payload[18:] + data
