import io
import json
import tempfile
import threading
import unittest
from http.client import HTTPConnection
from http.server import ThreadingHTTPServer
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import serial

from PIL import Image

from protocol import file_payload
from viewer import Gallery, SerialReader, detect_usb_port, handler_factory


class UsbPortTests(unittest.TestCase):
    @staticmethod
    def port(device, serial_number="xiao", vid=0x303A, pid=0x1001):
        return SimpleNamespace(device=device, serial_number=serial_number, vid=vid, pid=pid)

    @patch("viewer.list_ports.comports")
    def test_detect_native_usb_not_uart(self, ports):
        ports.return_value = [self.port("COM1", vid=None, pid=None), self.port("COM7")]
        self.assertEqual(detect_usb_port().device, "COM7")

    @patch("viewer.list_ports.comports")
    def test_missing_or_ambiguous_requires_explicit_port(self, ports):
        for candidates in ([], [self.port("COM7"), self.port("COM8", "other")]):
            ports.return_value = candidates
            with self.assertRaises(serial.SerialException):
                detect_usb_port()

    @patch("viewer.list_ports.comports")
    def test_reenumeration_keeps_device_identity(self, ports):
        ports.return_value = [self.port("COM9"), self.port("COM7", "other")]
        self.assertEqual(detect_usb_port("xiao").device, "COM9")
        with self.assertRaises(serial.SerialException):
            detect_usb_port("missing")


class ViewerTests(unittest.TestCase):
    def test_save_original_preview_and_safe_paths(self):
        image = Image.new("RGB", (20, 10), (20, 120, 200))
        output = io.BytesIO()
        image.save(output, "PNG")
        data = output.getvalue()
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            gallery.save(12, file_payload("../../照片.png", data))
            entry = gallery.snapshot()["files"][0]
            original, preview = gallery.paths[entry["id"]]
            self.assertEqual(original.parent, Path(tmp).resolve())
            self.assertEqual(original.read_bytes(), data)
            self.assertEqual(entry["name"], "照片.png")
            self.assertTrue(entry["preview"])
            assert preview is not None
            with Image.open(preview) as result:
                self.assertEqual(result.size, (20, 10))

    def test_nonimage_still_saved(self):
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            gallery.save(1, file_payload("test.txt", b"hello"))
            entry = gallery.snapshot()["files"][0]
            self.assertFalse(entry["preview"])
            self.assertEqual(gallery.paths[entry["id"]][0].read_bytes(), b"hello")

    def test_http_preview_download_and_no_path_traversal(self):
        image = Image.new("RGB", (4, 4), "blue")
        output = io.BytesIO()
        image.save(output, "JPEG")
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            gallery.save(1, file_payload("x.jpg", output.getvalue()))
            reader = SerialReader(gallery)  # deliberately not started: HTTP-only test
            server = ThreadingHTTPServer(("127.0.0.1", 0), handler_factory(gallery, reader))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            connection = HTTPConnection("127.0.0.1", server.server_port, timeout=2)
            try:
                connection.request("GET", "/api/state")
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                entry = json.loads(response.read())["files"][0]
                connection.request("GET", "/preview/" + entry["id"])
                response = connection.getresponse()
                self.assertEqual(response.getheader("Content-Type"), "image/jpeg")
                self.assertTrue(response.read().startswith(b"\xff\xd8"))
                connection.request("GET", "/files/" + entry["id"])
                response = connection.getresponse()
                self.assertEqual(response.read(), output.getvalue())
                self.assertIn("attachment", response.getheader("Content-Disposition") or "")
                connection.request("GET", "/files/../../README.md")
                response = connection.getresponse()
                self.assertEqual(response.status, 404)
                response.read()
            finally:
                connection.close()
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
