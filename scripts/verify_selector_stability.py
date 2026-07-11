#!/usr/bin/env python3
"""Fail when the app selector is crashing and rebooting in a loop.

The script only listens to the USB serial port. It deliberately keeps the
known-safe DTR/RTS state and never resets or flashes the board.
"""

from __future__ import annotations

import argparse
import glob
import time

import serial


def capture(port: str, baud: int, duration_s: float) -> str:
    device = serial.Serial(port=None, baudrate=baud, timeout=0.1)
    device.port = port
    device.dtr = True
    device.rts = False
    device.open()
    try:
        deadline = time.monotonic() + duration_s
        chunks: list[bytes] = []
        while time.monotonic() < deadline:
            chunk = device.read(4096)
            if chunk:
                chunks.append(chunk)
        return b"".join(chunks).decode("utf-8", "replace")
    finally:
        device.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port-glob", default="/dev/cu.usbmodem*")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=4)
    args = parser.parse_args()

    ports = sorted(glob.glob(args.port_glob))
    if not ports:
        print(f"FAIL: no serial port matched {args.port_glob!r}")
        return 2

    output = capture(ports[0], args.baud, args.duration)
    crash_count = output.count("assert failed: spi_hal_setup_trans")
    reboot_count = output.count("rst:0xc (SW_CPU)")
    selector_count = output.count("app_selector: boot -> app selector")
    print(
        f"selector_boots={selector_count} crashes={crash_count} "
        f"software_reboots={reboot_count}"
    )
    # Opening the USB serial port immediately after a flash can include reset
    # markers emitted before the selector started. A live loop is identified
    # by the SPI assertion itself or by entering the selector more than once.
    if crash_count or selector_count > 1:
        print("FAIL: app selector is not stable")
        return 1
    print("PASS: no selector reboot loop observed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
