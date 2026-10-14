import io
import random
import struct
import tempfile
import unittest
import zlib
from unittest.mock import patch
from pathlib import Path

from PIL import Image, ImageFile

from live import ARCHIVE_LIMIT, LiveArchive, archive_images
from protocol import ABORT, ARCHIVE, FILE_DATA, FILE_REF, Frame, FrameDecoder, encode_frame, file_payload, unpack_file
from replay import make_archive
from viewer import Gallery, SerialReader


def reference(data, offset, name="photo.jpg", ident=1):
    encoded = name.encode()
    return struct.pack("<IIIIH", ident, offset, len(data), zlib.crc32(data), len(encoded)) + encoded


def chunks(archive, ident=1):
    for pos in range(0, len(archive), 8192):
        yield struct.pack("<II", ident, pos) + archive[pos:pos + 8192]


class LiveTests(unittest.TestCase):
    def test_large_stream_packet_splits_and_file_crc(self):
        data = bytes(range(256)) * 12288
        archive, offset = make_archive("photo.jpg", data)
        live, decoder = LiveArchive(), FrameDecoder()
        for sequence, chunk in enumerate(chunks(archive)):
            wire = encode_frame(ARCHIVE, sequence, chunk)
            for start in range(0, len(wire), 127):
                for frame in decoder.feed(wire[start:start + 127]):
                    live.append(frame.payload)
        self.assertEqual(unpack_file(live.reference(reference(data, offset))), ("photo.jpg", data))
        live.data[offset + 10] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC"):
            live.reference(reference(data, offset))

    def test_missing_duplicate_overflow_and_recovery(self):
        for offset in (0, 4, ARCHIVE_LIMIT):
            live = LiveArchive()
            live.append(struct.pack("<II", 1, 0) + b"abc")
            with self.assertRaises(ValueError):
                live.append(struct.pack("<II", 1, offset) + b"d")
            self.assertFalse(live.valid)
            live.append(struct.pack("<II", 2, 0) + b"new")
            self.assertTrue(live.valid)
            self.assertEqual(live.data, b"new")

    def test_corrupt_chunk_forces_reference_fallback(self):
        live, decoder = LiveArchive(), FrameDecoder()
        archive, offset = make_archive("photo.jpg", b"x" * 16000)
        packets = list(chunks(archive))
        wire = bytearray(encode_frame(ARCHIVE, 1, packets[0]))
        wire[-1] ^= 1
        self.assertEqual(decoder.feed(wire), [])
        with self.assertRaises(ValueError):
            live.append(packets[1])
        with self.assertRaises(ValueError):
            live.reference(reference(b"x" * 16000, offset))

    def test_odc_every_prefix_and_multiple_files(self):
        data = b"jpeg bytes"
        archive, offset = make_archive("photo.jpg", data)
        for n in range(len(archive) + 1):
            entries = list(archive_images(archive[:n]))
            if n < offset:
                self.assertEqual(entries, [])
            else:
                self.assertEqual(entries, [("photo.jpg", data[:n - offset], len(data))])
        sidecar, _ = make_archive("._photo.jpg", b"metadata")
        second, _ = make_archive("second.png", b"png")
        self.assertEqual([x[0] for x in archive_images(sidecar + archive + second)],
                         ["photo.jpg", "second.png"])

    def test_newc_alignment_and_malformed_headers(self):
        name, data = b"a.jpg\0", b"12345"
        fields = [0, 0o100644, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0]
        header = b"070701" + b"".join(f"{v:08x}".encode() for v in fields)
        archive = header + name
        archive += b"\0" * (-len(archive) % 4)
        archive += data
        self.assertEqual(list(archive_images(archive)), [("a.jpg", data, len(data))])
        for bad in (b"xxxxxx", archive[:14] + b"-0000001" + archive[22:]):
            with self.assertRaises(ValueError):
                list(archive_images(bad))

    def test_partial_jpeg_before_commit_and_strict_flag_restored(self):
        pixels = random.Random(5).randbytes(640 * 480 * 3)
        output = io.BytesIO()
        Image.frombytes("RGB", (640, 480), pixels).save(output, "JPEG", quality=90)
        data = output.getvalue()
        archive, offset = make_archive("photo.jpg", data)
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            reader = SerialReader(gallery)
            gallery.clear_live("receiving")
            generation = gallery.live_generation
            reader.render_preview(generation, archive[:offset + len(data) // 2])
            state = gallery.snapshot()
            self.assertTrue(state["live"]["preview"])
            self.assertLess(state["live"]["received"], state["live"]["total"])
            self.assertEqual(state["files"], [])
            self.assertEqual(list(Path(tmp).iterdir()), [])
            self.assertFalse(ImageFile.LOAD_TRUNCATED_IMAGES)
            gallery.clear_live("aborted")
            reader.render_preview(generation, archive)
            self.assertIsNone(gallery.live_jpeg)  # stale worker must not resurrect image

    def test_commit_ack_abort_and_missing_reference(self):
        data = b"original bytes"
        archive, offset = make_archive("photo.jpg", data)
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            reader = SerialReader(gallery)
            reader.connection = io.BytesIO()
            for seq, payload in enumerate(chunks(archive)):
                reader.handle_live_frame(Frame(ARCHIVE, seq, payload))
            ref = Frame(FILE_REF, 8, reference(data, offset))
            reader.handle_live_frame(ref)
            self.assertEqual(reader.connection.getvalue(), b"HAVE 8\n")
            _, frame = reader.file_queue.get_nowait()
            self.assertEqual(unpack_file(frame.payload)[1], data)
            reader.handle_live_frame(ref)  # ACK lost: do not save same reference twice
            self.assertTrue(reader.file_queue.empty())
            reader.handle_live_frame(Frame(ABORT, 9, struct.pack("<I", 1)))
            self.assertFalse(reader.live_archive.valid)
            self.assertEqual(gallery.snapshot()["live"]["state"], "aborted")
            reader.handle_live_frame(ref)
            self.assertTrue(reader.connection.getvalue().endswith(b"MISS 8\n"))

    def test_png_renders_before_complete_and_keeps_final_decode_strict(self):
        pixels = random.Random(15).randbytes(128 * 192 * 3)
        output = io.BytesIO()
        Image.frombytes("RGB", (128, 192), pixels).save(output, "PNG")
        data = output.getvalue()
        archive, offset = make_archive("screenshot.png", data)
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            reader = SerialReader(gallery)
            gallery.clear_live("receiving")
            generation = gallery.live_generation
            reader.render_preview(generation, archive[:offset + len(data) // 3])
            first = gallery.live_jpeg
            self.assertIsNotNone(first)
            self.assertLess(gallery.live["received"], gallery.live["total"])
            reader.render_preview(generation, archive[:offset + 2 * len(data) // 3])
            self.assertNotEqual(first, gallery.live_jpeg)
            self.assertEqual(gallery.files, [])
            self.assertFalse(ImageFile.LOAD_TRUNCATED_IMAGES)
            with Image.open(io.BytesIO(data[:len(data) // 2])) as incomplete:
                with self.assertRaises(OSError):
                    incomplete.load()
            gallery.clear_live("aborted")
            reader.render_preview(generation, archive)
            self.assertIsNone(gallery.live_jpeg)

    def test_live_renewal_after_usb_dip_preserves_inflight_prefix(self):
        with tempfile.TemporaryDirectory() as tmp:
            reader = SerialReader(Gallery(Path(tmp), "TEST", 921600))
            reader.connection = io.BytesIO()
            reader.handle_live_frame(Frame(ARCHIVE, 1, struct.pack("<II", 1, 0) + b"abc"))
            for uptime, enabled in ((2000, True), (4000, False), (6000, True)):
                reader.handle_status({"uptime_ms": uptime, "live_enabled": enabled})
                self.assertEqual(reader.live_archive.data, b"abc")
            self.assertEqual(reader.connection.getvalue(), b"LIVE\nSAVE\n" * 3)
            reader.handle_status({"uptime_ms": 100, "live_enabled": False})
            self.assertFalse(reader.live_archive.valid)
            self.assertEqual(reader.gallery.live["state"], "idle")

    def test_saved_ack_follows_disk_commit_and_retry_does_not_duplicate(self):
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            reader = SerialReader(gallery)
            reader.connection = io.BytesIO()
            original_save = gallery.save
            def checked_save(sequence, payload):
                self.assertNotIn(b"SAVED", reader.connection.getvalue())
                original_save(sequence, payload)
            reader.file_worker.start()
            try:
                payload = struct.pack("<II", 42, 100) + file_payload("test.txt", b"original")
                with patch.object(gallery, "save", side_effect=checked_save):
                    reader.handle_file_frame(Frame(FILE_DATA, 20, payload))
                    reader.file_queue.join()
                self.assertEqual(reader.connection.getvalue(), b"SAVED 20\n")
                self.assertEqual(len(gallery.files), 1)
                self.assertEqual(gallery.paths[gallery.files[0]["id"]][0].read_bytes(), b"original")
                reader.handle_file_frame(Frame(FILE_DATA, 21, payload))
                self.assertTrue(reader.file_queue.empty())
                self.assertTrue(reader.connection.getvalue().endswith(b"SAVED 21\n"))
                self.assertEqual(len(gallery.files), 1)
                reader.handle_file_frame(Frame(FILE_DATA, 22, payload[:-1] + b"X"))
                self.assertTrue(reader.connection.getvalue().endswith(b"MISS 22\n"))
            finally:
                reader.stop_event.set()
                reader.file_worker.join(2)

    def test_disk_failure_requests_retry_and_removes_partial_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            reader = SerialReader(gallery)
            reader.connection = io.BytesIO()
            reader.file_worker.start()
            try:
                payload = struct.pack("<II", 42, 100) + file_payload("test.txt", b"original")
                with patch("viewer.os.fsync", side_effect=OSError("synthetic disk failure")):
                    reader.handle_file_frame(Frame(FILE_DATA, 20, payload))
                    reader.file_queue.join()
                self.assertEqual(reader.connection.getvalue(), b"MISS 20\n")
                self.assertEqual(list(Path(tmp).iterdir()), [])
                self.assertEqual(gallery.files, [])
                reader.handle_file_frame(Frame(FILE_DATA, 21, payload))
                reader.file_queue.join()
                self.assertTrue(reader.connection.getvalue().endswith(b"SAVED 21\n"))
                self.assertEqual(len(gallery.files), 1)
            finally:
                reader.stop_event.set()
                reader.file_worker.join(2)

    def test_unsupported_format_reports_bytes_without_fake_pixels(self):
        archive, _ = make_archive("photo.heic", b"\0\0\0\x18ftypheic" + b"x" * 500)
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            reader = SerialReader(gallery)
            gallery.clear_live("receiving")
            reader.render_preview(gallery.live_generation, archive[:-100])
            self.assertIsNone(gallery.live_jpeg)
            self.assertGreater(gallery.snapshot()["live"]["received"], 0)


if __name__ == "__main__":
    unittest.main()
