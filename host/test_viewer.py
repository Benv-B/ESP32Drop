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
from unittest.mock import Mock

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
    def test_tcon_status_controls_and_specific_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), 'TEST', 115200)
            reader = SerialReader(gallery)
            reader.connection = Mock(is_open=True)
            status = {'output':'iris_spi','firmware':'xiao-c5-iris-tcon-2',
                      'error':'TCON DRAM parity error','file_error':'output chunk not acknowledged',
                      'frame':{'enabled':True,'boot':1,'state':'retry'}}
            reader.handle_status(status)
            self.assertEqual(gallery.board, status)
            reader.connection.write.assert_not_called()  # no binary LIVE/SAVE subscription
            self.assertTrue(any('DRAM parity' in line for line in gallery.snapshot()['logs']))
            with self.assertRaises(RuntimeError): reader.self_test()
            reader.frame_control('hold')
            reader.connection.write.assert_called_once_with(b'FRAME HOLD\n')
            epoch = reader.delivery_epoch
            reader.handle_status(dict(status, frame={'enabled':True,'boot':2}))
            self.assertEqual(reader.delivery_epoch, epoch+1)

    def test_frame_controls_are_whitelisted_and_require_integrated_firmware(self):
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            reader = SerialReader(gallery)
            reader.connection = Mock(is_open=True)
            with self.assertRaises(RuntimeError):
                reader.frame_control("off")
            gallery.board = {"frame": {"enabled": True}}
            for action in ("hold", "auto", "off", "wake"):
                reader.frame_control(action)
                reader.connection.write.assert_called_with(f"FRAME {action.upper()}\n".encode())
            with self.assertRaises(ValueError):
                reader.frame_control("off\nTEST")
            reader.connection.is_open = False
            with self.assertRaises(RuntimeError):
                reader.frame_control("off")

    def test_self_test_refused_during_receive_pending_save_and_off(self):
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            reader = SerialReader(gallery)
            reader.connection = Mock(is_open=True)
            for state in ({"rx_active": True}, {"delivery_pending": True},
                          {"frame": {"off_requested": True}}):
                gallery.board = state
                with self.assertRaises(RuntimeError):
                    reader.self_test()
            reader.connection.write.assert_not_called()

    def test_frame_http_controls_and_cross_origin_rejection(self):
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            gallery.board = {"frame": {"enabled": True}}
            reader = SerialReader(gallery)
            reader.connection = Mock(is_open=True)
            server = ThreadingHTTPServer(("127.0.0.1", 0), handler_factory(gallery, reader))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            connection = HTTPConnection("127.0.0.1", server.server_port, timeout=2)
            try:
                for path, origin, expected in (
                    ("/api/frame/off", "http://foreign.invalid", 403),
                    ("/api/frame/anything", None, 404),
                    ("/api/frame/hold", f"http://127.0.0.1:{server.server_port}", 200)):
                    connection.request("POST", path, headers={"Origin": origin} if origin else {})
                    response = connection.getresponse()
                    self.assertEqual(response.status, expected)
                    response.read()
                reader.connection.write.assert_called_once_with(b"FRAME HOLD\n")
            finally:
                connection.close(); server.shutdown(); server.server_close(); thread.join(timeout=2)

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

    def test_legacy_console_encoding_does_not_fail_saved_file(self):
        with tempfile.TemporaryDirectory() as tmp, io.BytesIO() as raw:
            console = io.TextIOWrapper(raw, encoding="cp1252", errors="strict")
            gallery = Gallery(Path(tmp), "TEST", 921600)
            with patch("viewer.sys.stdout", console):
                gallery.save(1, file_payload("照片.txt", b"hello"))
            entry = gallery.files[0]
            self.assertEqual(entry["name"], "照片.txt")
            self.assertEqual(gallery.paths[entry["id"]][0].read_bytes(), b"hello")
            self.assertTrue(any("照片.txt" in line for line in gallery.logs))
            self.assertIn(b"\\u7167\\u7247.txt", raw.getvalue())
            console.detach()

    def test_broken_console_does_not_fail_saved_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            gallery = Gallery(Path(tmp), "TEST", 921600)
            with patch("viewer.sys.stdout", io.StringIO()) as console:
                with patch.object(console, "write", side_effect=BrokenPipeError("closed console")):
                    gallery.save(1, file_payload("photo.txt", b"hello"))
            self.assertEqual(len(gallery.files), 1)
            self.assertEqual(gallery.paths[gallery.files[0]["id"]][0].read_bytes(), b"hello")

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
