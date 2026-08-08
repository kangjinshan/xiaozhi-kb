#!/usr/bin/env python3

from macos_audio_input import AudioInputDevice, fourcc, resolve_input_device


def main() -> None:
    assert fourcc("dIn ") == 0x64496E20
    devices = [
        AudioInputDevice(10, "MacBook Pro Microphone", 1),
        AudioInputDevice(20, "BlackHole 2ch", 2),
        AudioInputDevice(30, "BlackHole 16ch", 16),
    ]
    assert resolve_input_device(devices, "BlackHole 2ch").object_id == 20
    assert resolve_input_device(devices, "microphone").object_id == 10
    try:
        resolve_input_device(devices, "BlackHole")
    except ValueError as error:
        assert "ambiguous" in str(error)
    else:
        raise AssertionError("ambiguous input name was accepted")
    try:
        resolve_input_device(devices, "missing")
    except ValueError as error:
        assert "available" in str(error)
    else:
        raise AssertionError("missing input name was accepted")


if __name__ == "__main__":
    main()
