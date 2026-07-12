#!/usr/bin/env python3
"""Verify one complete Recorder → Agent → SD → playback serial flow safely."""

from __future__ import annotations

import argparse
import glob
import time
from typing import NamedTuple


class RuntimeResult(NamedTuple):
    ok: bool
    missing: tuple[str, ...]
    failures: tuple[str, ...]


MILESTONES = {
    "wifi_connected": "Agent voice Wi-Fi connected",
    "wss_ready": "Agent voice WSS ready",
    "user_wav_stored": "Agent user WAV stored: /sdcard/agent/",
    "turn_accepted": "Agent accepted turn:",
    "assistant_wav_stored": "Agent reply stored: /sdcard/agent/",
    "playback_started": "Agent reply playback start: /sdcard/agent/",
}

FATAL_MARKERS = {
    "spi2_assertion": "assert failed: spi_hal_setup_trans",
    "stack_protection": "Stack protection fault",
    "guru_meditation": "Guru Meditation Error",
    "hash_failure": "reply integrity check failed",
    "event_queue_overflow": "network event queue overflow",
}


def evaluate_log(output: str) -> RuntimeResult:
    missing = tuple(
        name for name, marker in MILESTONES.items() if marker not in output
    )
    failures = [
        name for name, marker in FATAL_MARKERS.items() if marker in output
    ]
    if output.count("recorder app starting") > 1:
        failures.append("reboot_loop")
    result_failures = tuple(failures)
    return RuntimeResult(not missing and not result_failures, missing, result_failures)


def capture(port: str, baud: int, duration_s: float) -> str:
    import serial

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
    parser.add_argument("--duration", type=float, default=30)
    args = parser.parse_args()

    ports = sorted(glob.glob(args.port_glob))
    if not ports:
        print(f"FAIL: no serial port matched {args.port_glob!r}")
        return 2

    duration = max(30.0, args.duration)
    result = evaluate_log(capture(ports[0], args.baud, duration))
    if result.missing:
        print("missing=" + ",".join(result.missing))
    if result.failures:
        print("failures=" + ",".join(result.failures))
    if not result.ok:
        print("FAIL: complete Agent voice runtime flow was not observed")
        return 1
    print("PASS: Agent voice reply was stored and playback started")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
