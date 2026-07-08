#!/usr/bin/env python3
"""Verify the XiaoZhi BLE keyboard after a physical cold boot.

This script intentionally does not reset or flash the board. The Waveshare
ESP32-C6 board used for the BLE keyboard can be left in ROM download/stub mode
by automatic DTR/RTS reset sequences, so runtime verification should be done
after a real power cycle.
"""

from __future__ import annotations

import argparse
import asyncio
import glob
import sys
import time

try:
    import serial
except ImportError as exc:  # pragma: no cover - operator environment issue
    raise SystemExit("pyserial is required; use /tmp/ser-venv/bin/python") from exc


DEFAULT_PORT_GLOB = "/dev/cu.usbmodem*"
DEFAULT_BAUD = 115200


def find_port(pattern: str, timeout_s: float) -> str | None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
        time.sleep(0.5)
    return None


def capture_serial(port: str, baud: int, duration_s: float) -> str:
    ser = serial.Serial(port=None, baudrate=baud, timeout=0.1)
    ser.port = port
    # Keep the line state that did not force ROM download in testing.
    ser.dtr = True
    ser.rts = False
    ser.open()
    try:
        start = time.monotonic()
        chunks: list[bytes] = []
        while time.monotonic() - start < duration_s:
            data = ser.read(4096)
            if data:
                chunks.append(data)
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
        return b"".join(chunks).decode("utf-8", "replace")
    finally:
        ser.close()


async def scan_ble(timeout_s: float) -> bool:
    try:
        from bleak import BleakScanner
    except ImportError:
        print("BLE scan skipped: bleak is not installed")
        return False

    async def _discover() -> bool:
        devices = await BleakScanner.discover(timeout=timeout_s, return_adv=True)
        for _addr, (device, adv) in devices.items():
            name = device.name or adv.local_name or ""
            if name == "XiaoZhi KB":
                print(f"BLE found: {name} {device.address}")
                return True
        print(f"BLE scan finished: XiaoZhi KB not found among {len(devices)} devices")
        return False

    try:
        return await asyncio.wait_for(_discover(), timeout_s + 8)
    except Exception as exc:
        print(f"BLE scan failed: {exc}")
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port-glob", default=DEFAULT_PORT_GLOB)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--wait-port", type=float, default=60)
    parser.add_argument("--serial-duration", type=float, default=25)
    parser.add_argument("--skip-ble", action="store_true")
    args = parser.parse_args()

    print("Waiting for USB serial port after physical cold boot...")
    port = find_port(args.port_glob, args.wait_port)
    if port is None:
        print(f"No port matched {args.port_glob!r} within {args.wait_port:g}s")
        return 2

    print(f"Using {port}; listening without reset for {args.serial_duration:g}s")
    serial_text = capture_serial(port, args.baud, args.serial_duration)
    serial_ok = (
        "boot -> keyboard app" in serial_text
        or "keyboard heartbeat" in serial_text
        or "XiaoZhi KB" in serial_text
    )
    print(f"\nserial_runtime_evidence={serial_ok}")

    ble_ok = False
    if not args.skip_ble:
        ble_ok = asyncio.run(scan_ble(8))

    return 0 if serial_ok or ble_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
