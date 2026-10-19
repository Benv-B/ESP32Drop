"""Capture RevCBringup diagnostics without resetting the board; send explicit commands.

This uses the standalone test firmware's text protocol, NOT SerialPhotoBridge's binary
protocol. The firmware is identified before any command is sent. Close other monitors.
"""
import argparse
import datetime
import json
from pathlib import Path
import time
import serial
from serial.tools import list_ports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="auto")
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--command", nargs="*", default=[], choices=[
        "STATUS", "NFC", "LAST", "ARM30", "DISARM", "PWROFF", "QUIET", "MONITOR", "HELP"])
    args = parser.parse_args()
    if args.seconds < 3:
        parser.error("--seconds must be >=3")
    port = args.port
    if port == "auto":
        ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x303A, 0x1001)]
        if len(ports) != 1:
            raise RuntimeError(f"Expected one XIAO USB device; candidates={ports}; use --port")
        port = ports[0]
    root = Path(__file__).resolve().parents[1]
    path = root / "logs" / ("revc-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".jsonl")
    path.parent.mkdir(exist_ok=True)
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    connection.dtr = False
    connection.rts = False
    connection.port = port
    connection.open()
    identified = False
    latest = None
    rf_states = set()
    status_count = 0
    previous_state = None
    pending_line = bytearray()
    started = time.monotonic()
    try:
        with path.open("w", encoding="utf-8") as log:
            # Wake periodic reporting even if QUIET was used previously: do not send
            # commands blindly, so reset the test firmware manually if it stays silent.
            while time.monotonic() - started < args.seconds:
                pending_line.extend(connection.readline())
                if len(pending_line) > 65536:
                    raise RuntimeError("Unterminated/foreign serial data exceeds 64 KiB")
                if not pending_line.endswith(b"\n"):
                    continue  # serial timeout can split a JSON line in the middle
                line = pending_line.decode("utf-8", "replace").strip()
                pending_line.clear()
                if not line:
                    continue
                try:
                    event = json.loads(line)
                except (ValueError, TypeError):
                    event = {"type": "text", "message": line}
                log.write(json.dumps({"elapsed_s": round(time.monotonic() - started, 3),
                                      "event": event}, ensure_ascii=True) + "\n")
                log.flush()
                if isinstance(event, dict) and event.get("firmware") == "revc-bringup-1":
                    latest = event
                    status_count += 1
                    rf_states.add(event.get("rf_present"))
                    state = (event.get("nfc_ok"), event.get("rf_present"), event.get("off_requested"))
                    if state != previous_state:
                        print(json.dumps(event), flush=True)
                        previous_state = state
                    if not identified:
                        identified = True
                        for command in args.command:
                            connection.write((command + "\n").encode("ascii"))
                elif identified:
                    print(json.dumps(event, ensure_ascii=True), flush=True)
    finally:
        connection.close()
    if not identified:
        raise RuntimeError(f"No RevCBringup status; no commands sent. Capture: {path}")
    print(json.dumps({"capture": str(path), "status_count": status_count,
                      "rf_present_observed": True in rf_states,
                      "rf_absent_observed": False in rf_states,
                      "rf_unknown_observed": None in rf_states,
                      "last_status": latest}, ensure_ascii=True), flush=True)


if __name__ == "__main__":
    main()
