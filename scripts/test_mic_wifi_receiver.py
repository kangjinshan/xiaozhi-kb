#!/usr/bin/env python3

import argparse
import struct

from mic_wifi_receiver import (
    AUDIO_HEADER,
    CHANNELS,
    FRAMES_PER_PACKET,
    PCM_BITS,
    PCM_BYTES,
    PROTOCOL_VERSION,
    SAMPLE_RATE,
    SequenceTracker,
    parse_broadcast_addresses,
    parse_audio_packet,
    requested_input_name,
    verify_output_input_loopback,
)


class FakeRawStream:
    def __init__(self, loopback: bool) -> None:
        self.loopback = loopback
        self.last_write = bytes(PCM_BYTES)

    def __enter__(self):
        return self

    def __exit__(self, *_args) -> None:
        return None

    def write(self, pcm: bytes) -> None:
        self.last_write = pcm

    def read(self, _frames: int):
        return (self.last_write if self.loopback else bytes(PCM_BYTES), False)


class FakeSoundDevice:
    def __init__(self, loopback: bool) -> None:
        self.loopback = loopback

    def RawStream(self, **_kwargs):
        return FakeRawStream(self.loopback)


def packet(sequence: int, timestamp: int = 0) -> bytes:
    return AUDIO_HEADER.pack(
        b"XMIC",
        PROTOCOL_VERSION,
        2,
        AUDIO_HEADER.size,
        sequence,
        timestamp,
        SAMPLE_RATE,
        FRAMES_PER_PACKET,
        CHANNELS,
        PCM_BITS,
    ) + bytes(PCM_BYTES)


def main() -> None:
    parsed = parse_audio_packet(packet(7, 2240))
    assert parsed.sequence == 7
    assert parsed.timestamp_frames == 2240
    assert len(parsed.pcm) == PCM_BYTES

    bad = bytearray(packet(1))
    struct.pack_into("<I", bad, 16, 48000)
    try:
        parse_audio_packet(bytes(bad))
    except ValueError:
        pass
    else:
        raise AssertionError("wrong sample rate accepted")

    tracker = SequenceTracker()
    assert tracker.accept(0xFFFFFFFE) == 0
    assert tracker.accept(0xFFFFFFFF) == 0
    assert tracker.accept(1) == 1
    assert tracker.missing == 1
    assert tracker.accept(1) is None
    assert tracker.accept(0) is None
    assert tracker.late_or_duplicate == 2

    assert parse_broadcast_addresses(
        "inet 192.168.3.2 netmask 0xffffff00 broadcast 192.168.3.255\n"
        "inet 10.0.0.2/24 brd 10.0.0.255 scope global"
    ) == ["192.168.3.255", "10.0.0.255"]

    verify_output_input_loopback(FakeSoundDevice(True), "input", "output")
    try:
        verify_output_input_loopback(FakeSoundDevice(False), "input", "output")
    except RuntimeError as error:
        assert "not an output-to-input loopback" in str(error)
    else:
        raise AssertionError("silent non-loopback device was accepted")

    args = argparse.Namespace(
        no_switch_input=False,
        switch_input=None,
        play_to="BlackHole 2ch",
    )
    # Automatic switching is macOS-only; explicit switching is deterministic.
    args.switch_input = "BlackHole 2ch"
    assert requested_input_name(args) == "BlackHole 2ch"
    args.no_switch_input = True
    assert requested_input_name(args) is None


if __name__ == "__main__":
    main()
