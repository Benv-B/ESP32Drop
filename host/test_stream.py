import hashlib
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from protocol import FrameDecoder, STREAM_DATA, STREAM_COMMIT, encode_frame
from stream import ArchiveStream, complete_entries, LIMIT


def odc(name, data, mode=0o100644):
    encoded = name.encode() + b"\0"
    h = ("070707" + "000000" * 2 + f"{mode:06o}" + "000000" * 4 +
         "00000000000" + f"{len(encoded):06o}{len(data):011o}").encode()
    return h + encoded + data


class StreamTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.s = ArchiveStream(Path(self.tmp.name))

    def tearDown(self):
        self.s.reset()
        self.tmp.cleanup()

    def receive(self, data):
        for off in range(0, len(data), 8192):
            self.s.append(struct.pack("<II", 7, off) + data[off:off + 8192])
        return struct.pack("<III", 7, len(data), zlib.crc32(data))

    def test_live_photo_pair_above_ram_with_lost_acks(self):
        still, motion = bytes(range(256)) * 8192, bytes(range(255, -1, -1)) * 24576
        archive = odc("./IMG.HEIC", still) + odc("./IMG.MOV", motion) + odc("TRAILER!!!", b"")
        decoder = FrameDecoder()
        for seq, off in enumerate(range(0, len(archive), 8192)):
            packet = struct.pack("<II", 7, off) + archive[off:off + 8192]
            wire = encode_frame(STREAM_DATA, seq, packet)
            if seq == 5:  # A damaged USB frame must never reach the spool.
                corrupt = bytearray(wire); corrupt[-1] ^= 1
                self.assertEqual(decoder.feed(corrupt), [])
            for p in range(0, len(wire), 1009):
                for f in decoder.feed(wire[p:p + 1009]):
                    self.s.append(f.payload)
            if seq % 29 == 0:
                self.s.append(packet)  # lost ACK, identical retry
        saved = []
        commit = struct.pack("<III", 7, len(archive), zlib.crc32(archive))
        save = lambda name, data: saved.append((name, hashlib.sha256(data).digest()))
        self.assertEqual(self.s.commit(commit, save), 2)
        self.assertEqual(self.s.commit(commit, save), 2)  # lost final ACK
        self.assertEqual(saved, [("IMG.HEIC", hashlib.sha256(still).digest()),
                                 ("IMG.MOV", hashlib.sha256(motion).digest())])
        self.assertIsNone(self.s.file)

    def test_missing_conflicting_and_oversize_chunks(self):
        self.s.append(struct.pack("<II", 7, 0) + b"abc")
        for off, data in ((5, b"x"), (0, b"xyz"), (LIMIT, b"x")):
            with self.assertRaises(ValueError):
                self.s.append(struct.pack("<II", 7, off) + data)
        self.assertEqual(self.s.length, 3)

    def test_crc_and_truncation_never_save(self):
        data = odc("x.mov", b"x" * 10000)
        for damaged in (data[:-1], data + b"0707", b"", odc("TRAILER!!!", b"")):
            self.s.reset()
            commit = self.receive(damaged)
            with self.assertRaises(ValueError):
                self.s.commit(commit, lambda *_: self.fail("saved incomplete archive"))
        self.s.reset(); commit = self.receive(data)
        corrupt = commit[:-1] + bytes([commit[-1] ^ 1])
        with self.assertRaisesRegex(ValueError, "CRC"):
            self.s.commit(corrupt, lambda *_: self.fail("saved corrupt archive"))

    def test_save_failure_retries_only_unsaved_files(self):
        data = odc("a.heic", b"a") + odc("b.mov", b"b")
        commit = self.receive(data); saved = []
        def save(name, data):
            if name == "b.mov": raise OSError("disk full")
            saved.append(name)
        with self.assertRaises(OSError): self.s.commit(commit, save)
        self.assertEqual(self.s.commit(commit, lambda name, _: saved.append(name)), 2)
        self.assertEqual(saved, ["a.heic", "b.mov"])

    def test_exact_boundary_and_unsafe_entry_types(self):
        data = odc("../same.jpg", b"yes") + odc("link", b"target", 0o120777)
        entries = complete_entries(data)
        self.assertEqual([(n, data[o:o+s]) for n, o, s in entries], [("same.jpg", b"yes")])


if __name__ == "__main__": unittest.main()
