"""Receive CRC-checked files from SerialPhotoBridge and show them on localhost."""
from __future__ import annotations

import argparse
import io
import json
import queue
import threading
import time
import uuid
import warnings
import webbrowser
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, urlsplit

import serial
from PIL import Image, ImageOps

from protocol import FILE, LOG, STATUS, FrameDecoder, unpack_file

try:
    from pillow_heif import register_heif_opener
    register_heif_opener()
except ImportError:
    pass  # JPEG/PNG still work; HEIC is saved, but needs pillow-heif for its preview.

Image.MAX_IMAGE_PIXELS = 32_000_000
warnings.simplefilter("error", Image.DecompressionBombWarning)


class Gallery:
    def __init__(self, output: Path, port: str, baud: int):
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()
        self.connected = False
        self.port, self.baud = port, baud
        self.board: dict = {}
        self.files: list[dict] = []
        self.paths: dict[str, tuple[Path, Path | None]] = {}
        self.logs: deque[str] = deque(maxlen=80)
        self.serial_progress: tuple[int, int] = (0, 0)

    def log(self, text: str):
        line = f"{datetime.now():%H:%M:%S} {text.strip()}"
        with self.lock:
            self.logs.append(line)
        print(line, flush=True)

    def snapshot(self):
        with self.lock:
            return {"connected": self.connected, "port": self.port, "baud": self.baud,
                    "board": dict(self.board), "files": list(self.files),
                    "logs": list(self.logs), "serial_progress": self.serial_progress}

    def save(self, sequence: int, payload: bytes):
        name, data = unpack_file(payload)
        # Sender-provided names are labels only, never filesystem paths.
        name = Path(name.replace("\\", "/")).name
        name = "".join(c for c in name if ord(c) >= 32)[:255] or "unnamed"
        suffix = Path(name).suffix.lower()
        if suffix not in {".jpg", ".jpeg", ".png", ".heic", ".heif", ".gif", ".bmp",
                          ".webp", ".tif", ".tiff", ".mov", ".mp4", ".pdf"}:
            suffix = ".bin"
        ident = uuid.uuid4().hex
        stem = f"{datetime.now():%Y%m%d_%H%M%S}_{sequence}_{ident[:8]}"
        original = self.output / (stem + suffix)
        with original.open("xb") as file:
            file.write(data)
        preview = self.output / (stem + ".preview.jpg")
        preview_error = ""
        try:
            with Image.open(io.BytesIO(data)) as source:
                image = ImageOps.exif_transpose(source)
                image.thumbnail((1920, 1920))
                image.convert("RGB").save(preview, "JPEG", quality=90)
        except Exception as exc:
            # The verified original stays available, even for videos/unsupported HEIC.
            preview_error = str(exc)
            preview.unlink(missing_ok=True)
            preview = None
        entry = {"id": ident, "name": name, "size": len(data), "sequence": sequence,
                 "time": datetime.now().isoformat(timespec="seconds"),
                 "preview": preview is not None, "preview_error": preview_error,
                 "self_test": name == "bridge_selftest.jpg"}
        with self.lock:
            self.paths[ident] = (original, preview)
            self.files.insert(0, entry)
            self.files[:] = self.files[:100]
        self.log(f"Saved {name}: {len(data):,} bytes, serial CRC32 OK -> {original}")
        if preview_error:
            self.log(f"Preview unavailable (original saved): {preview_error}")


class SerialReader(threading.Thread):
    def __init__(self, gallery: Gallery):
        super().__init__(daemon=True)
        self.gallery = gallery
        self.stop_event = threading.Event()
        self.write_lock = threading.Lock()
        self.connection: serial.Serial | None = None
        self.file_queue: queue.Queue = queue.Queue(maxsize=8)
        self.file_worker = threading.Thread(target=self.process_files, daemon=True)

    def process_files(self):
        while not self.stop_event.is_set():
            try:
                frame = self.file_queue.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                self.gallery.save(frame.sequence, frame.payload)
            except Exception as exc:
                self.gallery.log(f"File could not be saved: {exc}")
            finally:
                self.file_queue.task_done()

    def self_test(self):
        with self.write_lock:
            if self.connection is None or not self.connection.is_open:
                raise RuntimeError("serial port is not connected")
            self.connection.write(b"TEST\n")
        self.gallery.log("Requested synthetic JPEG test (this is NOT an iPhone transfer)")

    def run(self):
        self.file_worker.start()
        while not self.stop_event.is_set():
            decoder = FrameDecoder()
            try:
                connection = serial.Serial(baudrate=self.gallery.baud, timeout=0.2,
                                           write_timeout=2, rtscts=False, dsrdtr=False)
                # Set before open to avoid deliberately resetting the board.
                connection.dtr = False
                connection.rts = False
                connection.port = self.gallery.port
                connection.open()
                with self.write_lock:
                    self.connection = connection
                with self.gallery.lock:
                    self.gallery.connected = True
                    self.gallery.board = {}
                self.gallery.log(f"Connected {self.gallery.port} @ {self.gallery.baud}")
                last_data = time.monotonic()
                while not self.stop_event.is_set():
                    data = connection.read(16384)
                    if data:
                        last_data = time.monotonic()
                        for frame in decoder.feed(data):
                            if frame.kind == FILE:
                                try:
                                    self.file_queue.put_nowait(frame)
                                except queue.Full:
                                    self.gallery.log("File queue full; frame dropped, resend from iPhone")
                            elif frame.kind == LOG:
                                self.gallery.log(frame.payload.decode("utf-8", errors="replace"))
                            elif frame.kind == STATUS:
                                try:
                                    status = json.loads(frame.payload)
                                    if isinstance(status, dict):
                                        with self.gallery.lock:
                                            self.gallery.board = status
                                except (ValueError, UnicodeDecodeError):
                                    self.gallery.log("Invalid board status JSON")
                    elif decoder.expected_bytes and time.monotonic() - last_data > 10:
                        decoder.reset()
                    with self.gallery.lock:
                        self.gallery.serial_progress = (len(decoder.buffer), decoder.expected_bytes)
                    while decoder.errors:
                        self.gallery.log(decoder.errors.popleft())
            except (serial.SerialException, OSError) as exc:
                self.gallery.log(f"Serial disconnected/unavailable: {exc}; retrying in 2 seconds")
            finally:
                with self.write_lock:
                    if self.connection is not None:
                        self.connection.close()
                    self.connection = None
                with self.gallery.lock:
                    self.gallery.connected = False
                    self.gallery.board = {}
                    self.gallery.serial_progress = (0, 0)
            self.stop_event.wait(2)

    def stop(self):
        self.stop_event.set()
        self.join(timeout=3)
        self.file_worker.join(timeout=3)


def handler_factory(gallery: Gallery, reader: SerialReader):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format: str, *args: object) -> None:
            pass

        def reply(self, body: bytes, content_type: str, code: int = 200, extra=None):
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'")
            for key, value in (extra or {}).items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urlsplit(self.path).path
            if path == "/":
                self.reply(Path(__file__).with_name("index.html").read_bytes(), "text/html; charset=utf-8")
            elif path == "/api/state":
                self.reply(json.dumps(gallery.snapshot(), ensure_ascii=False).encode(), "application/json; charset=utf-8")
            elif path.startswith(("/preview/", "/files/")):
                group, ident = path.strip("/").split("/", 1)
                with gallery.lock:
                    paths = gallery.paths.get(ident)
                    entry = next((item for item in gallery.files if item["id"] == ident), None)
                file = paths[1 if group == "preview" else 0] if paths else None
                if file is None or not file.is_file():
                    self.reply(b"Not found", "text/plain", 404)
                    return
                extra = {}
                if group == "files":
                    name = entry["name"] if entry else file.name
                    extra["Content-Disposition"] = "attachment; filename*=UTF-8''" + quote(name, safe="")
                self.reply(file.read_bytes(), "image/jpeg" if group == "preview" else "application/octet-stream", extra=extra)
            else:
                self.reply(b"Not found", "text/plain", 404)

        def do_POST(self):
            # Do not allow a foreign web page to send commands to a local device.
            expected_origin = f"http://{self.headers.get('Host', '')}"
            if self.headers.get("Origin") not in (None, expected_origin):
                self.reply(b"Forbidden", "text/plain", 403)
                return
            if self.path != "/api/self-test":
                self.reply(b"Not found", "text/plain", 404)
                return
            try:
                reader.self_test()
                self.reply(b'{"ok": true}', "application/json")
            except Exception as exc:
                self.reply(json.dumps({"error": str(exc)}).encode(), "application/json", 503)

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="board UART, e.g. COM3")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--http-port", type=int, default=8765)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "received")
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()
    gallery = Gallery(args.output, args.port, args.baud)
    reader = SerialReader(gallery)
    server = ThreadingHTTPServer(("127.0.0.1", args.http_port), handler_factory(gallery, reader))
    reader.start()
    url = f"http://127.0.0.1:{args.http_port}"
    gallery.log(f"Viewer {url} | originals: {gallery.output}")
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        reader.stop()


if __name__ == "__main__":
    main()
