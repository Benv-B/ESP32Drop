"""Receive CRC-checked files from SerialPhotoBridge and show them on localhost."""
from __future__ import annotations

import argparse
import io
import json
import hashlib
import os
import queue
import struct
import sys
import threading
import time
import uuid
import warnings
import webbrowser
from collections import deque, OrderedDict
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, urlsplit

import serial
from serial.tools import list_ports
from PIL import Image, ImageOps, ImageFile

from protocol import ABORT, ARCHIVE, FILE_REF, FILE_DATA, FILE, LOG, STATUS, Frame, FrameDecoder, unpack_file
from live import LiveArchive, archive_images

try:
    from pillow_heif import register_heif_opener
    register_heif_opener()
except ImportError:
    pass  # JPEG/PNG still work; HEIC is saved, but needs pillow-heif for its preview.

Image.MAX_IMAGE_PIXELS = 32_000_000
warnings.simplefilter("error", Image.DecompressionBombWarning)
# Pillow's truncated-image flag is process-global. Every decode, including final
# strict verification, uses this lock so a provisional preview cannot weaken it.
IMAGE_LOCK = threading.Lock()


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
        self.live: dict = {"state": "idle", "revision": 0}
        self.live_jpeg: bytes | None = None
        self.live_generation = 0

    def log(self, text: str):
        line = f"{datetime.now():%H:%M:%S} {text.strip()}"
        with self.lock:
            self.logs.append(line)
        # Redirected Windows output can use a legacy code page. Diagnostics must
        # never turn a successful disk commit into MISS (and a duplicate retry).
        try:
            encoding = getattr(sys.stdout, "encoding", None) or "utf-8"
            print(line.encode(encoding, errors="backslashreplace").decode(encoding), flush=True)
        except (OSError, UnicodeError):
            pass  # A closed/broken console does not affect the web log or delivery.

    def snapshot(self):
        with self.lock:
            return {"connected": self.connected, "port": self.port, "baud": self.baud,
                    "board": dict(self.board), "files": list(self.files),
                    "logs": list(self.logs), "serial_progress": self.serial_progress,
                    "live": dict(self.live)}

    def clear_live(self, state: str):
        with self.lock:
            self.live_generation += 1
            self.live = {"state": state, "revision": self.live.get("revision", 0) + 1}
            self.live_jpeg = None

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
        partial = original.with_suffix(original.suffix + ".part")
        try:
            with partial.open("xb") as file:
                file.write(data)
                file.flush()
                os.fsync(file.fileno())
            partial.replace(original)
        except Exception:
            partial.unlink(missing_ok=True)
            raise
        preview = self.output / (stem + ".preview.jpg")
        preview_error = ""
        try:
            with IMAGE_LOCK, Image.open(io.BytesIO(data)) as source:
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
                 "self_test": name == "bridge_selftest.jpg",
                 "source": "replay" if self.board.get("transport") == "replay" else "usb"}
        with self.lock:
            self.paths[ident] = (original, preview)
            self.files.insert(0, entry)
            self.files[:] = self.files[:100]
            if self.live.get("state") == "verified":
                self.live["state"] = "saved"
        self.log(f"Saved {name}: {len(data):,} bytes, serial CRC32 OK -> {original}")
        if preview_error:
            self.log(f"Preview unavailable (original saved): {preview_error}")


def detect_usb_port(serial_number: str | None = None):
    """Only select native ESP32-S3 Hardware CDC, never an arbitrary COM port."""
    ports = [p for p in list_ports.comports()
             if (p.vid, p.pid) == (0x303A, 0x1001)
             and (serial_number is None or p.serial_number == serial_number)]
    if len(ports) != 1:
        raise serial.SerialException(
            f"Expected one ESP32 USB CDC device, found {len(ports)}; "
            "connect the board or select --port COMx explicitly")
    return ports[0]


class SerialReader(threading.Thread):
    def __init__(self, gallery: Gallery):
        super().__init__(daemon=True)
        self.gallery = gallery
        self.requested_port = gallery.port
        self.usb_serial_number: str | None = None
        self.stop_event = threading.Event()
        self.write_lock = threading.Lock()
        self.connection: serial.Serial | None = None
        self.file_queue: queue.Queue = queue.Queue(maxsize=8)
        self.file_worker = threading.Thread(target=self.process_files, daemon=True)
        self.live_archive = LiveArchive()
        self.preview_queue: queue.Queue = queue.Queue(maxsize=1)
        self.preview_worker = threading.Thread(target=self.process_previews, daemon=True)
        self.last_preview = 0.0
        self.live_started = 0.0
        self.last_reference: bytes | None = None
        self.last_reference_key: bytes | None = None
        self.last_uptime: int | None = None
        self.delivery_lock = threading.Lock()
        self.deliveries = OrderedDict()
        self.delivery_epoch = 0

    def answer(self, word: str, sequence: int, epoch: int | None = None):
        try:
            with self.write_lock:
                if self.connection is not None and (epoch is None or epoch == self.delivery_epoch):
                    self.connection.write(f"{word} {sequence}\n".encode("ascii"))
        except (serial.SerialException, OSError) as exc:
            self.gallery.log(f"USB acknowledgement unavailable; receiver can retry: {exc}")

    def queue_delivery(self, frame: Frame, identity: tuple):
        unpack_file(frame.payload)  # validate before accepting into the worker queue
        key = (self.delivery_epoch,) + identity
        digest = hashlib.sha256(frame.payload).digest()
        saved = False
        with self.delivery_lock:
            existing = self.deliveries.get(key)
            if existing is not None:
                if existing["digest"] != digest:
                    raise ValueError("same delivery identity has different file contents")
                saved = existing["saved"]
                if not saved:
                    existing["sequences"].add(frame.sequence)
            else:
                self.deliveries[key] = {"digest": digest, "saved": False, "sequences": {frame.sequence}}
                self.gallery.clear_live("verified")
                try:
                    self.file_queue.put_nowait((key, frame))
                except queue.Full:
                    del self.deliveries[key]
                    raise
        if saved:
            self.answer("SAVED", frame.sequence, key[0])

    def handle_file_frame(self, frame: Frame):
        try:
            if frame.kind == FILE_DATA:
                if len(frame.payload) < 11:
                    raise ValueError("short identified file frame")
                ident, offset = struct.unpack_from("<II", frame.payload)
                identity = ("archive", ident, offset) if ident else ("sequence", frame.sequence)
                decoded = Frame(FILE, frame.sequence, frame.payload[8:])
            else:
                decoded = frame
                if frame.payload == self.last_reference and self.last_reference_key:
                    ident, offset = struct.unpack_from("<II", self.last_reference_key)
                    identity = ("archive", ident, offset)
                else:
                    identity = ("sequence", frame.sequence)
            self.last_reference = self.last_reference_key = None
            self.queue_delivery(decoded, identity)
        except (ValueError, queue.Full) as exc:
            self.answer("MISS", frame.sequence)
            self.gallery.log(f"File delivery not accepted; retry needed: {exc}")

    def process_previews(self):
        while not self.stop_event.is_set():
            try:
                generation, data = self.preview_queue.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                self.render_preview(generation, data)
            except Exception:
                pass  # Partial headers/unsupported formats wait; never stop USB reading.
            finally:
                self.preview_queue.task_done()

    def render_preview(self, generation: int, data: bytes):
        candidate = None
        for name, prefix, total in archive_images(data):
            if not prefix:
                continue
            candidate = (name, prefix, total)
            if len(prefix) < total:
                break
        if candidate is None:
            return
        name, prefix, total = candidate
        jpeg = None
        note = "此格式等待完整文件后解码"
        # Pillow can decode available JPEG scans and PNG rows provisionally.
        # This flag is scoped to the worker lock; final files still decode strictly.
        preview_format = ("JPEG" if prefix.startswith(b"\xff\xd8\xff") else
                          "PNG" if prefix.startswith(b"\x89PNG\r\n\x1a\n") else None)
        if preview_format:
            try:
                with IMAGE_LOCK:
                    previous = ImageFile.LOAD_TRUNCATED_IMAGES
                    try:
                        ImageFile.LOAD_TRUNCATED_IMAGES = True
                        with Image.open(io.BytesIO(prefix)) as source:
                            source.draft("RGB", (960, 960))
                            image = ImageOps.exif_transpose(source)
                            image.thumbnail((960, 960))
                            output = io.BytesIO()
                            image.convert("RGB").save(output, "JPEG", quality=80)
                            jpeg = output.getvalue()
                        note = f"临时 {preview_format} 预览：黑块/灰块可能是尚未收到的区域，尚未确认完整"
                    finally:
                        ImageFile.LOAD_TRUNCATED_IMAGES = previous
            except (OSError, ValueError, SyntaxError):
                note = f"已收到 {preview_format}，等待更多可解码数据"
        with self.gallery.lock:
            if generation != self.gallery.live_generation or self.gallery.live.get("state") != "receiving":
                return
            if self.gallery.live.get("name") != name:
                self.gallery.live_jpeg = None
                self.gallery.live["preview"] = False
            self.gallery.live.update(name=name, received=len(prefix), total=total, note=note)
            if jpeg:
                self.gallery.live_jpeg = jpeg
                self.gallery.live["revision"] += 1
                self.gallery.live["preview"] = True

    def handle_live_frame(self, frame: Frame):
        if frame.kind == ARCHIVE:
            old_id = self.live_archive.ident
            try:
                self.live_archive.append(frame.payload)
            except ValueError as exc:
                if old_id != self.live_archive.ident or self.gallery.snapshot()["live"]["state"] != "fallback":
                    self.gallery.log(str(exc))
                self.gallery.clear_live("fallback")
                return
            now = time.monotonic()
            if self.live_archive.ident != old_id:
                self.gallery.clear_live("receiving")
                self.live_started = now
                self.last_preview = 0
                self.last_reference = None
                self.last_reference_key = None
            with self.gallery.lock:
                elapsed = max(now - self.live_started, 0.001)
                self.gallery.live.update(archive_bytes=len(self.live_archive.data),
                                         elapsed_s=round(elapsed, 2),
                                         bytes_per_s=round(len(self.live_archive.data) / elapsed))
                generation = self.gallery.live_generation
            if now - self.last_preview >= 0.6:
                self.last_preview = now
                try:
                    self.preview_queue.get_nowait()
                    self.preview_queue.task_done()
                except queue.Empty:
                    pass
                self.preview_queue.put_nowait((generation, bytes(self.live_archive.data)))
        elif frame.kind == FILE_REF:
            try:
                payload = self.live_archive.reference(frame.payload)
                ident, offset = struct.unpack_from("<II", frame.payload)
                self.queue_delivery(Frame(FILE, frame.sequence, payload), ("archive", ident, offset))
                self.last_reference = payload
                self.last_reference_key = frame.payload
                answer = "HAVE"
            except (ValueError, queue.Full) as exc:
                self.gallery.log(f"Requesting full file fallback: {exc}")
                self.gallery.clear_live("fallback")
                answer = "MISS"
            self.answer(answer, frame.sequence)
        elif frame.kind == ABORT:
            if len(frame.payload) == 4 and struct.unpack("<I", frame.payload)[0] == self.live_archive.ident:
                self.live_archive.reset()
                self.gallery.clear_live("aborted")

    def process_files(self):
        while not self.stop_event.is_set():
            try:
                key, frame = self.file_queue.get(timeout=0.5)
            except queue.Empty:
                continue
            try:
                self.gallery.save(frame.sequence, frame.payload)
                with self.delivery_lock:
                    item = self.deliveries[key]
                    item["saved"] = True
                    sequences = item["sequences"]
                    item["sequences"] = set()
                    # Retain small digests, not large image buffers, for lost-ACK retries.
                    while len(self.deliveries) > 128:
                        oldest = next((k for k, v in self.deliveries.items() if v["saved"]), None)
                        if oldest is None:
                            break
                        del self.deliveries[oldest]
                for sequence in sequences:
                    self.answer("SAVED", sequence, key[0])
            except Exception as exc:
                with self.delivery_lock:
                    item = self.deliveries.pop(key, None)
                for sequence in item["sequences"] if item else (frame.sequence,):
                    self.answer("MISS", sequence, key[0])
                self.gallery.log(f"File could not be saved: {exc}")
            finally:
                self.file_queue.task_done()

    def self_test(self):
        with self.write_lock:
            if self.connection is None or not self.connection.is_open:
                raise RuntimeError("serial port is not connected")
            self.connection.write(b"TEST\n")
        self.gallery.log("Requested synthetic JPEG test (this is NOT an iPhone transfer)")

    def handle_status(self, status: dict):
        uptime = status.get("uptime_ms", 0)
        if self.last_uptime is not None and uptime < self.last_uptime:
            self.live_archive.reset()
            self.last_reference = self.last_reference_key = None
            self.gallery.clear_live("idle")
            with self.write_lock:
                self.delivery_epoch += 1
        self.last_uptime = uptime
        # HWCDC can briefly report disconnected without the COM port closing.
        # Firmware then clears LIVE. Renew on every status, not only at boot;
        # LIVE is idempotent and must never rewind an archive already in flight.
        with self.write_lock:
            if self.connection is not None:
                self.connection.write(b"LIVE\nSAVE\n")
        with self.gallery.lock:
            self.gallery.board = status

    def run(self):
        self.file_worker.start()
        self.preview_worker.start()
        while not self.stop_event.is_set():
            decoder = FrameDecoder()
            try:
                connection = serial.Serial(baudrate=self.gallery.baud, timeout=0.2,
                                           write_timeout=2, rtscts=False, dsrdtr=False)
                # Set before open to avoid deliberately resetting the board.
                connection.dtr = False
                connection.rts = False
                port = self.requested_port
                if port.lower() == "auto":
                    device = detect_usb_port(self.usb_serial_number)
                    port = device.device
                    self.usb_serial_number = device.serial_number
                connection.port = port
                connection.open()
                with self.write_lock:
                    self.connection = connection
                    self.delivery_epoch += 1
                with self.gallery.lock:
                    self.gallery.connected = True
                    self.gallery.port = port
                    self.gallery.board = {}
                # Empty command establishes HWCDC host activity without resetting it.
                connection.write(b"\nLIVE\nSAVE\n")
                self.gallery.log(f"Connected {self.gallery.port} @ {self.gallery.baud}")
                last_data = time.monotonic()
                self.last_uptime = None
                while not self.stop_event.is_set():
                    data = connection.read(max(1, min(65536, connection.in_waiting)))
                    if data:
                        last_data = time.monotonic()
                        for frame in decoder.feed(data):
                            if frame.kind in (ARCHIVE, FILE_REF, ABORT):
                                self.handle_live_frame(frame)
                            elif frame.kind in (FILE, FILE_DATA):
                                self.handle_file_frame(frame)
                            elif frame.kind == LOG:
                                self.gallery.log(frame.payload.decode("utf-8", errors="replace"))
                            elif frame.kind == STATUS:
                                try:
                                    status = json.loads(frame.payload)
                                    if isinstance(status, dict):
                                        self.handle_status(status)
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
                self.live_archive.reset()
                self.last_reference = None
                self.last_reference_key = None
                self.gallery.clear_live("disconnected")
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
        self.preview_worker.join(timeout=3)


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
            elif path == "/live-preview.jpg":
                with gallery.lock:
                    jpeg = gallery.live_jpeg
                self.reply(jpeg or b"No preview", "image/jpeg" if jpeg else "text/plain", 200 if jpeg else 404)
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
    parser.add_argument("--port", default="auto", help="native USB CDC port, e.g. COM7; default: auto")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--http-port", type=int, default=8765)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "received")
    parser.add_argument("--no-browser", action="store_true")
    parser.add_argument("--replay", type=Path, help="paced LOCAL replay of an image; no board used")
    parser.add_argument("--replay-kib", type=float, default=48, help="local replay KiB/s (default 48)")
    args = parser.parse_args()
    if not 0 < args.replay_kib <= 10000:
        parser.error("--replay-kib must be in (0, 10000]")
    gallery = Gallery(args.output, args.port, args.baud)
    if args.replay:
        from replay import ReplayReader
        reader = ReplayReader(gallery, args.replay, args.replay_kib)
    else:
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
    # replay imports SerialReader from this module; keep one shared Pillow lock.
    sys.modules.setdefault("viewer", sys.modules[__name__])
    main()
