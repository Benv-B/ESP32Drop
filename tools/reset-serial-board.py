"""Reset this CH340 board: DTR controls EN, RTS controls GPIO0 (swapped).

Does not write flash or eFuses. Close viewer/other serial clients before running.
"""
import argparse
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--mode", choices=("download", "run"), required=True)
    args = parser.parse_args()
    port = serial.Serial(baudrate=115200, timeout=0.2)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    try:
        port.rts = False
        port.dtr = True  # EN low
        time.sleep(0.2)
        if args.mode == "download":
            port.rts = True  # GPIO0 low before releasing EN
        port.dtr = False  # EN high
        time.sleep(0.5)
        port.rts = False
        print(f"Requested {args.mode} on {args.port} using swapped DTR/RTS wiring")
    finally:
        port.close()


if __name__ == "__main__":
    main()
