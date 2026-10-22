import random
import json
import struct
import unittest
import zlib

from protocol import FILE, HEADER, LOG, STATUS, MAGIC, MAX_PAYLOAD, FrameDecoder, encode_frame, file_payload, unpack_file


class ProtocolTests(unittest.TestCase):
    @staticmethod
    def tcon_line():
        return (json.dumps({'firmware':'xiao-c5-iris-tcon-2','output':'iris_spi',
                            'ready':True,'frame':{'enabled':True,'boot':2},
                            'error':'TCON DRAM parity error'})+'\n').encode()

    def test_tcon_status_every_split_and_single_bytes(self):
        line = self.tcon_line()
        for split in range(len(line)+1):
            decoder = FrameDecoder(text_status=True)
            frames = decoder.feed(line[:split]) + decoder.feed(line[split:])
            self.assertEqual([f.kind for f in frames], [STATUS])
            self.assertEqual(json.loads(frames[0].payload)['frame']['boot'], 2)
        decoder = FrameDecoder(text_status=True)
        frames = [f for b in line for f in decoder.feed(bytes([b]))]
        self.assertEqual(len(frames), 1)

    def test_tcon_text_never_parsed_inside_binary_photo(self):
        line = self.tcon_line()
        packet = encode_frame(FILE, 9, file_payload('photo.jpg', b'\n'+line+b'ESPDROP1'))
        decoder = FrameDecoder(text_status=True)
        frames = []
        for byte in line + packet + b'IRIS ERROR: test\n' + line:
            frames.extend(decoder.feed(bytes([byte])))
        self.assertEqual([f.kind for f in frames], [STATUS, FILE, LOG, STATUS])
        self.assertEqual(unpack_file(frames[1].payload)[1], b'\n'+line+b'ESPDROP1')

    def test_tcon_text_bound_invalid_json_reset_and_recovery(self):
        decoder = FrameDecoder(text_status=True)
        self.assertFalse(decoder.feed(b'x'*20000))
        self.assertLessEqual(len(decoder.text_buffer), 8192)
        self.assertFalse(decoder.feed(b'\n{broken}\n{"ready":true}\n'))
        self.assertEqual(len(decoder.feed(self.tcon_line())), 1)
        decoder.feed(self.tcon_line()[:70]); decoder.reset()
        self.assertEqual(len(decoder.feed(self.tcon_line())), 1)
        self.assertFalse(FrameDecoder().feed(self.tcon_line()))

    def test_every_single_split(self):
        packet = encode_frame(FILE, 7, file_payload("照片.jpg", b"abc\x00\xff"))
        for split in range(len(packet) + 1):
            decoder = FrameDecoder()
            frames = decoder.feed(packet[:split]) + decoder.feed(packet[split:])
            self.assertEqual(len(frames), 1)
            self.assertEqual(unpack_file(frames[0].payload), ("照片.jpg", b"abc\x00\xff"))

    def test_large_file_random_chunks_and_boot_logs(self):
        data = bytes(range(256)) * 12288  # 3 MiB, including all binary byte values
        packet = b"ROM boot log\r\n" + encode_frame(FILE, 1, file_payload("IMG.JPG", data))
        packet += encode_frame(LOG, 2, b"done")
        rng = random.Random(42)
        decoder, frames, offset = FrameDecoder(), [], 0
        while offset < len(packet):
            size = rng.randint(1, 16384)
            frames.extend(decoder.feed(packet[offset:offset + size]))
            offset += size
        self.assertEqual(len(frames), 2)
        self.assertEqual(unpack_file(frames[0].payload)[1], data)
        self.assertEqual(frames[1].payload, b"done")

    def test_corrupt_body_not_delivered(self):
        bad = bytearray(encode_frame(FILE, 1, file_payload("x.jpg", b"photo")))
        bad[-1] ^= 1
        decoder = FrameDecoder()
        frames = decoder.feed(bad + encode_frame(LOG, 2, b"recovered"))
        self.assertEqual([f.sequence for f in frames], [2])
        self.assertIn("NOT saved", decoder.errors[0])

    def test_corrupt_header_resynchronises(self):
        bad = bytearray(encode_frame(FILE, 1, file_payload("x", b"abc")))
        bad[16] ^= 1
        decoder = FrameDecoder()
        frames = decoder.feed(bad + encode_frame(LOG, 2, b"ok"))
        self.assertEqual([f.sequence for f in frames], [2])

    def test_oversize_header_rejected_before_buffering(self):
        prefix = struct.pack("<8sBBHIII", MAGIC, 1, FILE, 0, 1, MAX_PAYLOAD + 1, 0)
        packet = prefix + struct.pack("<I", zlib.crc32(prefix))
        decoder = FrameDecoder()
        self.assertEqual(decoder.feed(packet), [])
        self.assertLess(len(decoder.buffer), HEADER.size)

    def test_disconnect_discards_partial_file(self):
        packet = encode_frame(FILE, 1, file_payload("x", b"a" * 1000))
        decoder = FrameDecoder()
        self.assertEqual(decoder.feed(packet[:100]), [])
        decoder.reset()
        frames = decoder.feed(encode_frame(LOG, 2, b"new boot"))
        self.assertEqual([f.sequence for f in frames], [2])

    def test_invalid_filename(self):
        for payload in (b"", b"\x00\x00", b"\xff\xffx", b"\x05\x00a"):
            with self.assertRaises(ValueError):
                unpack_file(payload)

    def test_short_write_preserves_next_frame_inside_claimed_body(self):
        broken = encode_frame(FILE, 1, file_payload("x", b"a" * 60))[:HEADER.size + 8]
        next_frame = encode_frame(FILE, 2, file_payload("good.jpg", b"b" * 100))
        decoder = FrameDecoder()
        frames = decoder.feed(broken + next_frame)
        self.assertEqual([f.sequence for f in frames], [2])
        self.assertEqual(unpack_file(frames[0].payload)[1], b"b" * 100)

    def test_status_trickle_cannot_keep_truncated_frame_alive(self):
        packet = encode_frame(FILE, 1, file_payload("lost", b"a" * 100000))
        decoder = FrameDecoder()
        self.assertEqual(decoder.feed(packet[:100], now=0), [])
        for second in range(2, 30, 2):
            self.assertEqual(decoder.feed(encode_frame(LOG, second, b"status"), now=second), [])
        frames = decoder.feed(encode_frame(LOG, 30, b"recovered"), now=30)
        self.assertEqual([f.sequence for f in frames], [30])
        self.assertTrue(any("exceeded 30 s" in error for error in decoder.errors))

    def test_magic_inside_valid_payload(self):
        payload = file_payload("x", MAGIC * 10)
        frames = FrameDecoder().feed(encode_frame(FILE, 1, payload))
        self.assertEqual(frames[0].payload, payload)


if __name__ == "__main__":
    unittest.main()
