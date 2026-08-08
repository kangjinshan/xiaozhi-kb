#!/usr/bin/env python3
"""Use XiaoZhi Mic as a real-time stereo microphone over Wi-Fi.

The receiver sends a small UDP discovery heartbeat and immediately consumes
the device's 20 ms PCM16 stereo packets. Use ``--play-to`` with BlackHole or
another virtual loopback output to expose the stream as a system microphone.
Writing a WAV is optional and is never part of the live transport path.
"""

from __future__ import annotations

import argparse
from array import array
from dataclasses import dataclass
import math
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time
import wave
import re


PORT = 45830
PROTOCOL_VERSION = 1
LISTEN_REQUEST = struct.Struct("<4sBBH").pack(
    b"XMIC", PROTOCOL_VERSION, 1, 0
)
AUDIO_HEADER = struct.Struct("<4sBBHIIIHBB")
AUDIO_TYPE = 2
SAMPLE_RATE = 16000
CHANNELS = 2
PCM_BITS = 16
FRAMES_PER_PACKET = 320
PCM_BYTES = FRAMES_PER_PACKET * CHANNELS * (PCM_BITS // 8)
SILENCE = bytes(PCM_BYTES)


@dataclass(frozen=True)
class AudioPacket:
    sequence: int
    timestamp_frames: int
    pcm: bytes


def parse_audio_packet(datagram: bytes) -> AudioPacket:
    if len(datagram) != AUDIO_HEADER.size + PCM_BYTES:
        raise ValueError(f"wrong datagram size: {len(datagram)}")
    (
        magic,
        version,
        packet_type,
        header_bytes,
        sequence,
        timestamp_frames,
        sample_rate,
        frames,
        channels,
        pcm_bits,
    ) = AUDIO_HEADER.unpack_from(datagram)
    expected = (
        b"XMIC",
        PROTOCOL_VERSION,
        AUDIO_TYPE,
        AUDIO_HEADER.size,
        SAMPLE_RATE,
        FRAMES_PER_PACKET,
        CHANNELS,
        PCM_BITS,
    )
    actual = (
        magic,
        version,
        packet_type,
        header_bytes,
        sample_rate,
        frames,
        channels,
        pcm_bits,
    )
    if actual != expected:
        raise ValueError(f"unsupported XiaoZhi Mic packet format: {actual}")
    return AudioPacket(
        sequence=sequence,
        timestamp_frames=timestamp_frames,
        pcm=datagram[AUDIO_HEADER.size :],
    )


class SequenceTracker:
    def __init__(self) -> None:
        self.last_sequence: int | None = None
        self.received = 0
        self.missing = 0
        self.late_or_duplicate = 0

    def accept(self, sequence: int) -> int | None:
        """Return forward missing count, or None for old/duplicate packets."""
        if self.last_sequence is None:
            self.last_sequence = sequence
            self.received += 1
            return 0
        advance = (sequence - self.last_sequence) & 0xFFFFFFFF
        if advance == 0 or advance >= 0x80000000:
            self.late_or_duplicate += 1
            return None
        missing = advance - 1
        self.missing += missing
        self.last_sequence = sequence
        self.received += 1
        return missing


def parse_broadcast_addresses(interface_text: str) -> list[str]:
    addresses = re.findall(
        r"\b(?:broadcast|brd)\s+(\d{1,3}(?:\.\d{1,3}){3})\b",
        interface_text,
    )
    return list(dict.fromkeys(addresses))


def automatic_broadcast_addresses() -> list[str]:
    addresses = ["255.255.255.255"]
    for command in (["ifconfig"], ["ip", "-o", "-4", "addr", "show"]):
        try:
            result = subprocess.run(
                command, check=False, capture_output=True, text=True
            )
        except OSError:
            continue
        addresses.extend(parse_broadcast_addresses(result.stdout))
    return list(dict.fromkeys(addresses))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--device-ip",
        help="device IP or broadcast address; default probes all local broadcasts",
    )
    parser.add_argument(
        "--port", type=int, default=PORT, help="device UDP port"
    )
    parser.add_argument(
        "--bind", default="0.0.0.0", help="local interface address"
    )
    parser.add_argument(
        "--play-to",
        help="PortAudio output name/index, normally a BlackHole device",
    )
    parser.add_argument(
        "--switch-input",
        nargs="?",
        const="",
        metavar="NAME",
        help=(
            "on macOS, make this virtual device the default microphone after "
            "the first live packet; without NAME, reuse --play-to"
        ),
    )
    parser.add_argument(
        "--no-switch-input",
        action="store_true",
        help="do not automatically select --play-to as the macOS microphone",
    )
    parser.add_argument(
        "--keep-input",
        action="store_true",
        help="do not restore the previous macOS input when live audio stops",
    )
    parser.add_argument(
        "--input-release-timeout",
        type=float,
        default=2.0,
        help="seconds without live packets before restoring the previous input",
    )
    parser.add_argument(
        "--wav",
        type=Path,
        help="optional live backup WAV; does not affect real-time output",
    )
    parser.add_argument(
        "--stdout", action="store_true", help="also write raw PCM16LE to stdout"
    )
    parser.add_argument(
        "--duration", type=float, default=0, help="seconds; 0 runs until Ctrl-C"
    )
    parser.add_argument(
        "--list-devices", action="store_true", help="list PortAudio devices"
    )
    return parser.parse_args()


def load_sounddevice(required: bool):
    if not required:
        return None
    try:
        import sounddevice  # type: ignore
    except ImportError as error:
        raise RuntimeError(
            "PortAudio output requires: pip install sounddevice"
        ) from error
    return sounddevice


def open_playback(sounddevice, target: str | None):
    if target is None:
        return None
    device: str | int = int(target) if target.isdecimal() else target
    try:
        playback = sounddevice.RawOutputStream(
            samplerate=SAMPLE_RATE,
            blocksize=FRAMES_PER_PACKET,
            channels=CHANNELS,
            dtype="int16",
            device=device,
            latency="low",
        )
        playback.start()
        return playback
    except Exception as error:
        raise RuntimeError(
            f"cannot open real-time output device {target!r}: {error}"
        ) from error


def verify_output_input_loopback(
    sounddevice, input_target: str, output_target: str
) -> None:
    """Fail early if writing the virtual output cannot reach its input."""
    frames = FRAMES_PER_PACKET
    captured = [[], []]
    phase = 0
    try:
        with sounddevice.RawStream(
            samplerate=SAMPLE_RATE,
            blocksize=frames,
            channels=(CHANNELS, CHANNELS),
            dtype="int16",
            device=(input_target, output_target),
            latency="low",
        ) as stream:
            for block in range(30):
                samples = array("h")
                for frame in range(frames):
                    value = 6000 if ((phase + frame) // 8) % 2 == 0 else -6000
                    samples.extend((value, -value))
                phase += frames
                if sys.byteorder != "little":
                    samples.byteswap()
                stream.write(samples.tobytes())
                raw, _ = stream.read(frames)
                if block < 6:
                    continue
                received = array("h")
                received.frombytes(bytes(raw))
                if sys.byteorder != "little":
                    received.byteswap()
                captured[0].extend(received[0::2])
                captured[1].extend(received[1::2])
    except Exception as error:
        raise RuntimeError(
            f"cannot probe virtual audio loopback {output_target!r} -> "
            f"{input_target!r}: {error}"
        ) from error

    rms = [
        math.sqrt(sum(sample * sample for sample in channel) / len(channel))
        if channel
        else 0.0
        for channel in captured
    ]
    if min(rms) < 500.0:
        raise RuntimeError(
            f"{output_target!r} is not an output-to-input loopback device "
            f"(probe RMS L={rms[0]:.1f}, R={rms[1]:.1f}). Install/use "
            "BlackHole 2ch or another real virtual loopback."
        )


def requested_input_name(args: argparse.Namespace) -> str | None:
    if args.no_switch_input:
        return None
    if args.switch_input is None:
        if sys.platform != "darwin" or args.play_to is None:
            return None
        name = args.play_to
    else:
        name = args.switch_input or args.play_to
    if not name:
        raise RuntimeError("--switch-input requires NAME or --play-to NAME")
    if args.play_to is None:
        raise RuntimeError("--switch-input also requires --play-to")
    if str(name).isdecimal():
        raise RuntimeError(
            "--switch-input needs a CoreAudio device name, not a PortAudio index"
        )
    return str(name)


def prepare_input_switch(args: argparse.Namespace):
    name = requested_input_name(args)
    if name is None:
        return None
    try:
        from macos_audio_input import MacOSAudioInput, TemporaryDefaultInput
    except ImportError as error:
        raise RuntimeError(
            "macOS input switching requires scripts/macos_audio_input.py"
        ) from error
    result = TemporaryDefaultInput(MacOSAudioInput(), name)
    if result.target.input_channels < CHANNELS:
        raise RuntimeError(
            f"{result.target.name!r} has only {result.target.input_channels} "
            "input channel(s); XiaoZhi Wi-Fi Mic requires stereo loopback"
        )
    return result


def receive(args: argparse.Namespace) -> None:
    if args.input_release_timeout <= 0:
        raise RuntimeError("--input-release-timeout must be greater than zero")
    sounddevice = load_sounddevice(args.play_to is not None)
    input_switch = prepare_input_switch(args)
    playback = None
    wav_file = None
    udp = None
    tracker = SequenceTracker()
    malformed = 0
    inserted_silence = 0
    started = time.monotonic()
    first_audio_at: float | None = None
    last_audio_at: float | None = None
    last_report_at = started
    next_heartbeat_at = 0.0

    def output(pcm: bytes) -> None:
        if playback is not None:
            playback.write(pcm)
        if wav_file is not None:
            wav_file.writeframesraw(pcm)
        if args.stdout:
            sys.stdout.buffer.write(pcm)
            sys.stdout.buffer.flush()

    try:
        if input_switch is not None:
            verify_output_input_loopback(
                sounddevice, input_switch.target.name, args.play_to
            )
        playback = open_playback(sounddevice, args.play_to)
        if args.wav is not None:
            args.wav.parent.mkdir(parents=True, exist_ok=True)
            wav_file = wave.open(str(args.wav), "wb")
            wav_file.setnchannels(CHANNELS)
            wav_file.setsampwidth(PCM_BITS // 8)
            wav_file.setframerate(SAMPLE_RATE)

        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256 * 1024)
        udp.bind((args.bind, 0))
        udp.settimeout(0.2)
        target_addresses = (
            [args.device_ip] if args.device_ip else automatic_broadcast_addresses()
        )
        destinations = [(address, args.port) for address in target_addresses]
        print(
            "Discovering XiaoZhi Mic at "
            + ", ".join(f"{host}:{port}" for host, port in destinations)
            + "; "
            f"local UDP port {udp.getsockname()[1]}",
            file=sys.stderr,
        )
        while args.duration <= 0 or time.monotonic() - started < args.duration:
            now = time.monotonic()
            if now >= next_heartbeat_at:
                for destination in destinations:
                    try:
                        udp.sendto(LISTEN_REQUEST, destination)
                    except OSError:
                        # One inactive interface must not prevent discovery on
                        # another local subnet.
                        pass
                next_heartbeat_at = now + 1.0
            try:
                datagram, source = udp.recvfrom(2048)
            except socket.timeout:
                now = time.monotonic()
                if (
                    input_switch is not None
                    and input_switch.switched
                    and not args.keep_input
                    and last_audio_at is not None
                    and now - last_audio_at >= args.input_release_timeout
                ):
                    restored = input_switch.restore()
                    if restored is not None:
                        print(
                            "Wi-Fi Mic inactive; macOS default microphone "
                            f"restored -> {restored.name}",
                            file=sys.stderr,
                        )
                    tracker = SequenceTracker()
                continue
            try:
                packet = parse_audio_packet(datagram)
            except ValueError:
                malformed += 1
                continue
            now = time.monotonic()
            last_audio_at = now
            if first_audio_at is None:
                first_audio_at = now
                print(f"Live audio from {source[0]}:{source[1]}", file=sys.stderr)
            if input_switch is not None and not input_switch.switched:
                selected = input_switch.activate()
                print(
                    f"macOS default microphone -> {selected.name}",
                    file=sys.stderr,
                )
            missing = tracker.accept(packet.sequence)
            if missing is None:
                continue
            # Preserve real-time duration across a short LAN loss. A long jump
            # is counted but not replayed as seconds of stale silence.
            silence_packets = min(missing, 25)
            for _ in range(silence_packets):
                output(SILENCE)
            inserted_silence += silence_packets
            output(packet.pcm)

            now = time.monotonic()
            if now - last_report_at >= 5.0:
                audio_seconds = tracker.received * 0.020
                wall_seconds = now - first_audio_at if first_audio_at else 0.0
                realtime = audio_seconds / wall_seconds if wall_seconds > 0 else 0
                print(
                    f"live={audio_seconds:.2f}s realtime={realtime:.2f}x "
                    f"missing={tracker.missing} malformed={malformed} "
                    f"late={tracker.late_or_duplicate}",
                    file=sys.stderr,
                )
                last_report_at = now
    except KeyboardInterrupt:
        pass
    finally:
        if udp is not None:
            udp.close()
        if wav_file is not None:
            wav_file.close()
        if playback is not None:
            playback.stop()
            playback.close()
        if input_switch is not None and not args.keep_input:
            restored = input_switch.restore()
            if restored is not None:
                print(
                    f"macOS default microphone restored -> {restored.name}",
                    file=sys.stderr,
                )

    elapsed = time.monotonic() - first_audio_at if first_audio_at else 0.0
    print(
        f"Stopped: received={tracker.received} ({tracker.received * 0.02:.2f}s) "
        f"wall={elapsed:.2f}s missing={tracker.missing} "
        f"inserted_silence={inserted_silence} malformed={malformed}",
        file=sys.stderr,
    )


def main() -> int:
    args = parse_args()
    try:
        if args.list_devices:
            sounddevice = load_sounddevice(True)
            print(sounddevice.query_devices())
            return 0
        if args.stdout and sys.stdout.isatty():
            raise RuntimeError("refusing to write raw PCM to a terminal")
        receive(args)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
