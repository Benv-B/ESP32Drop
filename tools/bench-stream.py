"""Exercise the C5 record pump -> USB -> durable disk with 7 MiB of synthetic data.

Stop the viewer first. This does not measure Wi-Fi/AirDrop. Drop one ACK on purpose
and verify the retried chunk neither corrupts nor duplicates the saved original.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
from viewer import Gallery, SerialReader


class FaultReader(SerialReader):
    dropped = False
    count = 0
    def answer(self, word, sequence, epoch=None, files=None):
        if word == "SAVED" and files == 0:
            self.count += 1
            if self.count == 10:
                self.dropped = True
                self.gallery.log("TEST: deliberately dropping one chunk ACK")
                return
        return super().answer(word, sequence, epoch, files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM8")
    args = parser.parse_args()
    gallery = Gallery(Path(__file__).resolve().parents[1] / "logs" / "stream-bench", args.port, 921600)
    reader = FaultReader(gallery); reader.start()
    try:
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline and not gallery.snapshot()["board"]:
            time.sleep(0.1)
        board = gallery.snapshot()["board"]
        if (not board.get("ready") or board.get("bridge_protocol") != 1
                or not board.get("archive_stream_enabled")
                or board.get("rx_active") or board.get("delivery_pending")):
            raise RuntimeError(f"Expected idle C5 stream firmware: {board}")
        started = time.monotonic()
        with reader.write_lock:
            reader.connection.write(b"STREAMTEST\n")
        floor = board["psram_free"]
        while time.monotonic() - started < 90:
            state = gallery.snapshot()
            if state["board"].get("file_errors", 0) > board.get("file_errors", 0):
                raise RuntimeError(json.dumps(state["board"]))
            floor = min(floor, state["board"].get("psram_free", floor))
            files = [f for f in state["files"] if f["name"] == "bridge_stream_selftest.bin"]
            if files:
                path, _ = gallery.paths[files[0]["id"]]
                actual = hashlib.sha256(path.read_bytes()).hexdigest()
                expected = hashlib.sha256(bytes(range(256)) * (7 * 4096)).hexdigest()
                if len(files) != 1 or files[0]["size"] != 7 * 1024 * 1024 or actual != expected or not reader.dropped:
                    raise AssertionError("synthetic stream content/retry mismatch")
                elapsed = time.monotonic() - started
                print(json.dumps({"verified": True, "synthetic": True, "bytes": files[0]["size"],
                    "sha256": actual, "seconds": round(elapsed, 3), "kib_s": round(7168 / elapsed, 1),
                    "lost_ack_recovered": reader.dropped, "psram_free_min": floor}), flush=True)
                time.sleep(2.5)  # consume the device's completion/status before closing
                return
            time.sleep(0.1)
        raise TimeoutError("7 MiB stream test did not finish")
    finally:
        reader.stop()


if __name__ == "__main__": main()
