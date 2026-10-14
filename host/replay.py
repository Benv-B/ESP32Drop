"""Local paced replay through the real framing, archive and preview consumers.

This exercises PC software only; it does not measure AirDrop, USB or an e-paper panel.
"""
from __future__ import annotations

import struct
import zlib
from pathlib import Path

from live import ARCHIVE_LIMIT
from protocol import ARCHIVE, FILE_REF, FrameDecoder, encode_frame
from viewer import SerialReader


def make_archive(name: str, data: bytes) -> tuple[bytes, int]:
    encoded = ("./" + name).encode("utf-8") + b"\0"
    if len(name.encode("utf-8")) > 255:
        raise ValueError("filename too long")
    header = ("070707" + "000000" * 2 + f"{0o100644:06o}" + "000000" * 4
              + "00000000000" + f"{len(encoded):06o}{len(data):011o}").encode("ascii")
    archive = header + encoded + data
    if len(archive) > ARCHIVE_LIMIT:
        raise ValueError("replay exceeds 6 MiB archive limit")
    return archive, len(header) + len(encoded)


class ReplayReader(SerialReader):
    def __init__(self, gallery, path: Path, kib_per_second: float):
        super().__init__(gallery)
        self.path = path
        self.rate = kib_per_second * 1024

    def self_test(self):
        raise RuntimeError("本地回放没有连接 ESP；请在 USB 模式使用自检")

    def run(self):
        self.file_worker.start()
        self.preview_worker.start()
        try:
            if self.path.stat().st_size > ARCHIVE_LIMIT:
                raise ValueError("replay file exceeds limit")
            data = self.path.read_bytes()
            archive, offset = make_archive(self.path.name, data)
            with self.gallery.lock:
                self.gallery.board = {"transport": "replay", "rx_active": True}
            self.gallery.log("LOCAL REPLAY: PC software only, not AirDrop/USB/e-paper timing")
            decoder = FrameDecoder()
            seq = 0
            if self.stop_event.wait(2):
                return
            for pos in range(0, len(archive), 8192):
                chunk = archive[pos:pos + 8192]
                if self.stop_event.wait(len(chunk) / self.rate):
                    return
                seq += 1
                wire = encode_frame(ARCHIVE, seq, struct.pack("<II", 1, pos) + chunk)
                for start in range(0, len(wire), 997):
                    for frame in decoder.feed(wire[start:start + 997]):
                        self.handle_live_frame(frame)
            name = self.path.name.encode("utf-8")
            ref = struct.pack("<IIIIH", 1, offset, len(data), zlib.crc32(data), len(name)) + name
            for frame in decoder.feed(encode_frame(FILE_REF, seq + 1, ref)):
                self.handle_live_frame(frame)
            with self.gallery.lock:
                self.gallery.board["rx_active"] = False
            self.gallery.log("LOCAL REPLAY complete; file queued for saving")
        except Exception as exc:
            self.gallery.clear_live("aborted")
            self.gallery.log(f"Replay failed: {exc}")
