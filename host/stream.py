"""Acknowledged disk spool for archives larger than the microcontroller's RAM."""
from __future__ import annotations

import mmap
import os
import struct
import tempfile
import zlib

LIMIT = 128 * 1024 * 1024


def complete_entries(data):
    """Validate the entire cpio before publishing any file. Never extract paths."""
    pos, entries = 0, []
    while pos + 6 <= len(data):
        magic = data[pos:pos + 6]
        if magic == b"070707":
            header, base, align, fields = 76, 8, False, ((18, 6), (59, 6), (65, 11))
        elif magic in (b"070701", b"070702"):
            header, base, align, fields = 110, 16, True, ((14, 8), (94, 8), (54, 8))
        else:
            raise ValueError("invalid cpio header")
        if pos + header > len(data):
            raise ValueError("truncated cpio header")
        values = []
        digits = b"01234567" if base == 8 else b"0123456789abcdefABCDEF"
        for off, n in fields:
            raw = data[pos + off:pos + off + n]
            if any(c not in digits for c in raw):
                raise ValueError("invalid cpio number")
            values.append(int(raw, base))
        mode, namesize, size = values
        if not 1 <= namesize <= 4096 or size > LIMIT:
            raise ValueError("cpio entry exceeds limit")
        endname = pos + header + namesize
        if endname > len(data):
            raise ValueError("truncated cpio name")
        rawname = data[pos + header:endname]
        if rawname[-1:] != b"\0" or b"\0" in rawname[:-1]:
            raise ValueError("invalid cpio name")
        if rawname == b"TRAILER!!!\0":
            if size or not entries or any(data[endname:]):
                raise ValueError("invalid cpio trailer")
            return entries
        start = (endname + 3) & ~3 if align else endname
        if start + size > len(data):
            raise ValueError("truncated cpio file")
        if magic == b"070702":
            expected = int(data[pos + 102:pos + 110], 16)
            if sum(data[start:start + size]) & 0xffffffff != expected:
                raise ValueError("cpio checksum mismatch")
        name = rawname[:-1].decode("utf-8", errors="replace").replace("\\", "/").split("/")[-1]
        if mode & 0o170000 in (0, 0o100000) and name and not name.startswith("._"):
            entries.append((name, start, size))
            if len(entries) > 4096:
                raise ValueError("too many archive files")
        end = start + size
        pos = min((end + 3) & ~3, len(data)) if align else end
    # Apple also sends archives ending exactly after the last entry. The board
    # issues COMMIT only after a complete HTTP/DvZip body, never on a partial record.
    if pos == len(data) and entries:
        return entries
    raise ValueError("truncated or empty cpio archive")


class ArchiveStream:
    def __init__(self, directory):
        self.directory = directory
        self.file = None
        self.ident = self.length = self.crc = 0
        self.committed = None
        self.saved = set()

    def reset(self):
        if self.file:
            self.file.close()
        self.file = None
        self.ident = self.length = self.crc = 0
        self.committed = None
        self.saved.clear()

    def append(self, payload):
        if not 9 <= len(payload) <= 8200:
            raise ValueError("invalid stream chunk")
        ident, offset = struct.unpack_from("<II", payload)
        chunk = payload[8:]
        if not ident or offset + len(chunk) > LIMIT:
            raise ValueError("stream exceeds limit")
        if ident != self.ident:
            if offset:
                raise ValueError("missing stream start")
            self.reset()
            self.ident = ident
            self.file = tempfile.TemporaryFile(dir=self.directory)
        if self.file is None or self.committed:
            raise ValueError("stream already closed")
        if offset < self.length:
            self.file.seek(offset)
            if offset + len(chunk) > self.length or self.file.read(len(chunk)) != chunk:
                raise ValueError("conflicting stream retry")
            return  # ACK was lost; do not write or count the chunk again.
        if offset != self.length:
            raise ValueError("missing stream chunk")
        self.file.seek(offset)
        self.file.write(chunk)
        self.crc = zlib.crc32(chunk, self.crc)
        self.length += len(chunk)

    def commit(self, payload, save):
        if len(payload) != 12:
            raise ValueError("invalid stream commit")
        ident, length, crc = struct.unpack("<III", payload)
        if self.committed and self.committed[:3] == (ident, length, crc):
            return self.committed[3]
        if self.file is None or (ident, length, crc) != (self.ident, self.length, self.crc):
            raise ValueError("stream length/CRC mismatch")
        self.file.flush()
        os.fsync(self.file.fileno())
        with mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            entries = complete_entries(data)
            for name, start, size in entries:
                if start not in self.saved:
                    save(name, data[start:start + size])
                    self.saved.add(start)
        self.committed = (ident, length, crc, len(entries))
        self.file.close()
        self.file = None
        return len(entries)
