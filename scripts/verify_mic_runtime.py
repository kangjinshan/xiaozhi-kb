#!/usr/bin/env python3
"""Safely verify XiaoZhi Mic boot logs without resetting the device."""

from __future__ import annotations

import argparse
import glob
import sys
import time


REQUIRED_MARKERS = {
    "task": "Mic task created stack=40960 bytes",
    "stereo_codec": "ES7210 stereo input: MIC1=L MIC2=R",
    "shared_ns": "ESP-SR shared-mid stereo NS enabled",
    "realtime_ns": "mode=C6 realtime 10ms+held",
    "udp": "real-time PCM receiver discovery listening on UDP 45830",
    "ready": "microphone DSP shared-mid-ns=enabled Wi-Fi=starting",
    "wifi": "Wi-Fi connected:",
    "realtime": "capture realtime 1.00x",
}

FATAL_MARKERS = (
    "Guru Meditation",
    "task_wdt: Task watchdog got triggered",
    "spi_hal_setup_trans",
    "Stack protection fault",
    "assert failed:",
    "Rebooting...",
)


class MicRuntimeEvidence:
    def __init__(self) -> None:
        self.seen = {name: False for name in REQUIRED_MARKERS}
        self.fatal_lines: list[str] = []

    def consume(self, line: str) -> None:
        for name, marker in REQUIRED_MARKERS.items():
            if marker in line:
                self.seen[name] = True
        if any(marker in line for marker in FATAL_MARKERS):
            self.fatal_lines.append(line.strip())

    @property
    def missing(self) -> list[str]:
        return [name for name, found in self.seen.items() if not found]

    @property
    def passed(self) -> bool:
        return not self.missing and not self.fatal_lines


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port-glob", default="/dev/cu.usbmodem*")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=20.0)
    parser.add_argument("--verbose", action="store_true", help="print all serial lines")
    return parser.parse_args()


def find_port(pattern: str) -> str | None:
    ports = sorted(glob.glob(pattern))
    return ports[0] if ports else None


def main() -> int:
    args = parse_args()
    port = find_port(args.port_glob)
    if port is None:
        print(f"No serial port matches {args.port_glob}", file=sys.stderr)
        return 2
    try:
        import serial  # type: ignore
    except ImportError:
        print("pyserial is required: pip install pyserial", file=sys.stderr)
        return 2

    connection = serial.Serial(port=None, baudrate=args.baud, timeout=0.1)
    connection.dtr = True
    connection.rts = False
    connection.port = port
    connection.open()

    evidence = MicRuntimeEvidence()
    deadline = time.monotonic() + args.duration
    pending = bytearray()
    try:
        while time.monotonic() < deadline:
            chunk = connection.read(4096)
            if not chunk:
                continue
            pending.extend(chunk)
            while b"\n" in pending:
                raw, _, remainder = pending.partition(b"\n")
                pending = bytearray(remainder)
                line = raw.decode("utf-8", errors="replace")
                evidence.consume(line)
                if args.verbose or any(
                    marker in line for marker in REQUIRED_MARKERS.values()
                ):
                    print(line.strip())
    finally:
        connection.close()

    print(
        f"seen={','.join(name for name, found in evidence.seen.items() if found)} "
        f"missing={','.join(evidence.missing) or 'none'} "
        f"fatals={len(evidence.fatal_lines)}"
    )
    if evidence.fatal_lines:
        for line in evidence.fatal_lines:
            print(line, file=sys.stderr)
    if evidence.passed:
        print(
            "PASS: Mic stereo codec, shared ESP-SR NS, Wi-Fi UDP and "
            "real-time capture initialized"
        )
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
