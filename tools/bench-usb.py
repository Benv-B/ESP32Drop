"""Check 1 MiB from the real S3 USB port. Stop the viewer before running.

No files are sent over Wi-Fi. CRC, byte contents and repeated transfers are
checked independently of the firmware; output is JSON for repeatable measurements.
"""
import argparse
import json
from pathlib import Path
import sys
import time
import zlib

import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
from protocol import FILE, LOG, STATUS, FrameDecoder, unpack_file
from viewer import detect_usb_port


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="auto")
    parser.add_argument("--repeat", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.repeat <= 20:
        parser.error("--repeat must be 1..20")
    port = detect_usb_port().device if args.port == "auto" else args.port
    connection = serial.Serial(baudrate=921600, timeout=0.2, write_timeout=2)
    connection.dtr = False
    connection.rts = False
    connection.port = port
    expected = bytes(range(256)) * 4096
    with connection:
        connection.write(b"\n")
        decoder = FrameDecoder()
        # Verify firmware/idle before occupying the USB application task.
        deadline = time.monotonic() + 8
        status = None
        while time.monotonic() < deadline and status is None:
            for frame in decoder.feed(connection.read(max(1, connection.in_waiting))):
                if frame.kind == STATUS:
                    status = json.loads(frame.payload)
        if not status or status.get("firmware") not in ("xiao-dual-1", "xiao-dual-2") or status.get("rx_active"):
            raise RuntimeError(f"Expected idle xiao-dual firmware; got {status}")
        print(json.dumps({"board": status}), flush=True)
        for run in range(1, args.repeat + 1):
            start = time.monotonic()
            first_byte = None
            connection.write(b"BENCH\n")
            complete = False
            while time.monotonic() - start < 20 and not complete:
                data = connection.read(max(1, min(65536, connection.in_waiting)))
                arrived = time.monotonic()
                frames = decoder.feed(data)
                if first_byte is None and decoder.expected_bytes > 1048576:
                    first_byte = arrived
                if decoder.errors:
                    raise RuntimeError(list(decoder.errors))
                for frame in frames:
                    if frame.kind == LOG and b"BENCH busy" in frame.payload:
                        raise RuntimeError(frame.payload.decode())
                    if frame.kind != FILE:
                        continue
                    name, payload = unpack_file(frame.payload)
                    if name != "bridge_usb_benchmark.bin" or payload != expected:
                        raise RuntimeError("USB benchmark content mismatch")
                    elapsed = arrived - (first_byte if first_byte is not None else start)
                    print(json.dumps({"run": run, "bytes": len(payload), "crc32": f"{zlib.crc32(payload):08x}",
                                      "usb_s": round(elapsed, 4), "usb_kib_s": round(1024 / elapsed, 1),
                                      "command_to_verified_s": round(time.monotonic() - start, 4),
                                      "verified": True}), flush=True)
                    complete = True
            if not complete:
                raise TimeoutError("USB benchmark did not complete within 20 s")


if __name__ == "__main__":
    main()
