#!/usr/bin/env python3
"""Fail when XiaoZhi crashes during startup or Wi-Fi scanning.

The script listens with the board's known-safe USB line state. It never
resets or flashes the device.
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
    parser.add_argument("--duration", type=float, default=8)
    args = parser.parse_args()

    ports = sorted(glob.glob(args.port_glob))
    if not ports:
        print(f"FAIL: no serial port matched {args.port_glob!r}")
        return 2

    output = capture(ports[0], args.baud, args.duration)
    stack_faults = output.count("Stack protection fault")
    xiaozhi_boots = output.count("main: boot -> xiaozhi")
    sd_log_frames = output.count("SdCardLogVprintf")
    print(
        f"xiaozhi_boots={xiaozhi_boots} stack_faults={stack_faults} "
        f"sd_log_frames={sd_log_frames}"
    )
    if stack_faults or xiaozhi_boots > 1 or sd_log_frames:
        print("FAIL: XiaoZhi startup is not stable")
        return 1
    print("PASS: no XiaoZhi startup crash observed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
