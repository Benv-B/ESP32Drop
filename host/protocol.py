"""Binary USB framing shared with SerialPhotoBridge (no third-party dependencies)."""
from __future__ import annotations

import struct
import time
import zlib
from collections import deque
from dataclasses import dataclass

MAGIC = b"ESPDROP1"
VERSION = 1
LOG, FILE, STATUS = 1, 2, 3
ARCHIVE, FILE_REF, ABORT = 4, 5, 6
FILE_DATA = 7  # transfer id + archive offset + legacy file payload; saved ACK required
KINDS = (LOG, FILE, STATUS, ARCHIVE, FILE_REF, ABORT, FILE_DATA)
HEADER = struct.Struct("<8sBBHIIII")
MAX_PAYLOAD = 8 * 1024 * 1024 + 512


@dataclass(frozen=True)
class Frame:
    kind: int
    sequence: int
    payload: bytes


def encode_frame(kind: int, sequence: int, payload: bytes) -> bytes:
    if kind not in KINDS or len(payload) > MAX_PAYLOAD:
        raise ValueError("invalid frame")
    prefix = struct.pack("<8sBBHIII", MAGIC, VERSION, kind, 0,
                         sequence, len(payload), zlib.crc32(payload))
    return prefix + struct.pack("<I", zlib.crc32(prefix)) + payload


def file_payload(name: str, data: bytes) -> bytes:
    encoded = name.encode("utf-8")
    if not 0 < len(encoded) <= 255:
        raise ValueError("filename must be 1..255 UTF-8 bytes")
    return struct.pack("<H", len(encoded)) + encoded + data


def unpack_file(payload: bytes) -> tuple[str, bytes]:
    if len(payload) < 3:
        raise ValueError("missing filename")
    length = struct.unpack_from("<H", payload)[0]
    if not 0 < length <= 255 or 2 + length > len(payload):
        raise ValueError("invalid filename length")
    return payload[2:2 + length].decode("utf-8", errors="replace"), payload[2 + length:]


class FrameDecoder:
    """Resynchronise after boot logs, bad headers, corruption and a board reset.

    Only header-CRC-validated lengths are trusted. A file is delivered ONLY after
    its entire payload passes CRC32. Transport chunk boundaries have no meaning.
    """

    def __init__(self) -> None:
        self.buffer = bytearray()
        self.errors: deque[str] = deque(maxlen=32)
        self.expected_bytes = 0
        self.frame_started: float | None = None

    def reset(self) -> None:
        if self.buffer:
            self.errors.append("partial serial frame discarded (timeout/disconnect)")
        self.buffer.clear()
        self.expected_bytes = 0
        self.frame_started = None

    def feed(self, data: bytes | bytearray, *, now: float | None = None) -> list[Frame]:
        now = time.monotonic() if now is None else now
        # A truncated frame followed by a status every two seconds never reaches
        # the reader's silence timeout. Bound each frame's lifetime as well.
        # Allow at least 30 s, and large fallback files at 64 KiB/s. This is
        # deliberately well below measured native USB speed to tolerate load.
        frame_budget = max(30, self.expected_bytes / 65536)
        if self.frame_started is not None and now - self.frame_started >= frame_budget:
            self.reset()
            self.errors.append(f"serial frame exceeded {frame_budget:g} s; searching for next frame")
        self.buffer.extend(data)
        frames = []
        while True:
            start = self.buffer.find(MAGIC)
            if start < 0:
                # Retain a possible prefix split across two reads, not arbitrary boot logs.
                if len(self.buffer) >= len(MAGIC):
                    del self.buffer[:-(len(MAGIC) - 1)]
                self.expected_bytes = 0
                self.frame_started = None
                break
            if start:
                del self.buffer[:start]
            if len(self.buffer) < HEADER.size:
                break
            _, version, kind, flags, seq, length, crc, header_crc = HEADER.unpack_from(self.buffer)
            limit = MAX_PAYLOAD if kind in (FILE, FILE_DATA) else 8200
            if (version != VERSION or kind not in KINDS or flags != 0
                    or length > limit
                    or zlib.crc32(memoryview(self.buffer)[:HEADER.size - 4]) != header_crc):
                self.errors.append("bad serial header; searching for next frame")
                del self.buffer[0]
                self.expected_bytes = 0
                self.frame_started = None
                continue
            total = HEADER.size + length
            self.expected_bytes = total
            if self.frame_started is None:
                self.frame_started = now
            if len(self.buffer) < total:
                break
            payload = bytes(self.buffer[HEADER.size:total])
            self.expected_bytes = 0
            self.frame_started = None
            if zlib.crc32(payload) != crc:
                self.errors.append(f"CRC32 failed for frame {seq}; file NOT saved")
                # A short write can put the NEXT valid header inside the failed
                # frame's claimed body. Do not discard that recovery point.
                del self.buffer[0]
                continue
            del self.buffer[:total]
            frames.append(Frame(kind, seq, payload))
        return frames
