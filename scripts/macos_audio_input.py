#!/usr/bin/env python3
"""Read and change the macOS default audio input through CoreAudio."""

from __future__ import annotations

from dataclasses import dataclass
import ctypes
from ctypes import byref
import sys
from typing import Sequence


CORE_AUDIO_PATH = "/System/Library/Frameworks/CoreAudio.framework/CoreAudio"
CORE_FOUNDATION_PATH = (
    "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"
)
SYSTEM_AUDIO_OBJECT = 1
CORE_AUDIO_OK = 0
CF_STRING_ENCODING_UTF8 = 0x08000100


def fourcc(value: str) -> int:
    if len(value) != 4 or not value.isascii():
        raise ValueError("CoreAudio fourcc must contain four ASCII characters")
    return int.from_bytes(value.encode("ascii"), "big")


class AudioObjectPropertyAddress(ctypes.Structure):
    _fields_ = [
        ("selector", ctypes.c_uint32),
        ("scope", ctypes.c_uint32),
        ("element", ctypes.c_uint32),
    ]


class AudioBuffer(ctypes.Structure):
    _fields_ = [
        ("channels", ctypes.c_uint32),
        ("data_byte_size", ctypes.c_uint32),
        ("data", ctypes.c_void_p),
    ]


@dataclass(frozen=True)
class AudioInputDevice:
    object_id: int
    name: str
    input_channels: int


def resolve_input_device(
    devices: Sequence[AudioInputDevice], query: str
) -> AudioInputDevice:
    normalized = query.strip().casefold()
    if not normalized:
        raise ValueError("input device name is empty")
    exact = [item for item in devices if item.name.casefold() == normalized]
    if len(exact) == 1:
        return exact[0]
    partial = [item for item in devices if normalized in item.name.casefold()]
    if len(partial) == 1:
        return partial[0]
    if not partial:
        available = ", ".join(item.name for item in devices) or "none"
        raise ValueError(
            f"no macOS input device matches {query!r}; available: {available}"
        )
    raise ValueError(
        f"input device name {query!r} is ambiguous: "
        + ", ".join(item.name for item in partial)
    )


class MacOSAudioInput:
    def __init__(self) -> None:
        if sys.platform != "darwin":
            raise RuntimeError("default-input switching is supported only on macOS")
        self._audio = ctypes.cdll.LoadLibrary(CORE_AUDIO_PATH)
        self._foundation = ctypes.cdll.LoadLibrary(CORE_FOUNDATION_PATH)
        self._configure_signatures()

    def _configure_signatures(self) -> None:
        address_pointer = ctypes.POINTER(AudioObjectPropertyAddress)
        self._audio.AudioObjectGetPropertyDataSize.argtypes = [
            ctypes.c_uint32,
            address_pointer,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self._audio.AudioObjectGetPropertyDataSize.restype = ctypes.c_int32
        self._audio.AudioObjectGetPropertyData.argtypes = [
            ctypes.c_uint32,
            address_pointer,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_void_p,
        ]
        self._audio.AudioObjectGetPropertyData.restype = ctypes.c_int32
        self._audio.AudioObjectSetPropertyData.argtypes = [
            ctypes.c_uint32,
            address_pointer,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_void_p,
        ]
        self._audio.AudioObjectSetPropertyData.restype = ctypes.c_int32
        self._foundation.CFStringGetCString.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_long,
            ctypes.c_uint32,
        ]
        self._foundation.CFStringGetCString.restype = ctypes.c_bool

    @staticmethod
    def _address(selector: str, scope: str = "glob") -> AudioObjectPropertyAddress:
        return AudioObjectPropertyAddress(fourcc(selector), fourcc(scope), 0)

    def _check(self, status: int, operation: str) -> None:
        if status != CORE_AUDIO_OK:
            unsigned = ctypes.c_uint32(status).value
            raw = unsigned.to_bytes(4, "big")
            code = raw.decode("ascii", "replace")
            raise RuntimeError(
                f"CoreAudio {operation} failed: status={status} ({code!r})"
            )

    def _property_size(
        self, object_id: int, address: AudioObjectPropertyAddress
    ) -> int:
        size = ctypes.c_uint32()
        status = self._audio.AudioObjectGetPropertyDataSize(
            object_id, byref(address), 0, None, byref(size)
        )
        self._check(status, "get property size")
        return size.value

    def _device_ids(self) -> list[int]:
        address = self._address("dev#")
        size = self._property_size(SYSTEM_AUDIO_OBJECT, address)
        if size == 0:
            return []
        count = size // ctypes.sizeof(ctypes.c_uint32)
        values = (ctypes.c_uint32 * count)()
        data_size = ctypes.c_uint32(size)
        status = self._audio.AudioObjectGetPropertyData(
            SYSTEM_AUDIO_OBJECT,
            byref(address),
            0,
            None,
            byref(data_size),
            values,
        )
        self._check(status, "list devices")
        return list(values)

    def _device_name(self, object_id: int) -> str:
        address = self._address("lnam")
        value = ctypes.c_void_p()
        size = ctypes.c_uint32(ctypes.sizeof(value))
        status = self._audio.AudioObjectGetPropertyData(
            object_id, byref(address), 0, None, byref(size), byref(value)
        )
        self._check(status, "read device name")
        if not value.value:
            return f"CoreAudio device {object_id}"
        buffer = ctypes.create_string_buffer(1024)
        converted = self._foundation.CFStringGetCString(
            value, buffer, len(buffer), CF_STRING_ENCODING_UTF8
        )
        if not converted:
            return f"CoreAudio device {object_id}"
        return buffer.value.decode("utf-8", "replace")

    def _input_channels(self, object_id: int) -> int:
        address = self._address("slay", "inpt")
        size = self._property_size(object_id, address)
        raw = ctypes.create_string_buffer(size)
        data_size = ctypes.c_uint32(size)
        status = self._audio.AudioObjectGetPropertyData(
            object_id, byref(address), 0, None, byref(data_size), raw
        )
        self._check(status, "read input stream configuration")
        if size < ctypes.sizeof(ctypes.c_uint32):
            return 0
        buffer_count = ctypes.c_uint32.from_buffer(raw).value
        # AudioBufferList aligns its first AudioBuffer to pointer alignment.
        offset = (ctypes.sizeof(ctypes.c_uint32) + ctypes.alignment(AudioBuffer) - 1)
        offset -= offset % ctypes.alignment(AudioBuffer)
        available = max(0, (size - offset) // ctypes.sizeof(AudioBuffer))
        channels = 0
        for index in range(min(buffer_count, available)):
            item = AudioBuffer.from_buffer(
                raw, offset + index * ctypes.sizeof(AudioBuffer)
            )
            channels += item.channels
        return channels

    def input_devices(self) -> list[AudioInputDevice]:
        devices: list[AudioInputDevice] = []
        for object_id in self._device_ids():
            channels = self._input_channels(object_id)
            if channels > 0:
                devices.append(
                    AudioInputDevice(object_id, self._device_name(object_id), channels)
                )
        return devices

    def default_input_id(self) -> int:
        address = self._address("dIn ")
        value = ctypes.c_uint32()
        size = ctypes.c_uint32(ctypes.sizeof(value))
        status = self._audio.AudioObjectGetPropertyData(
            SYSTEM_AUDIO_OBJECT,
            byref(address),
            0,
            None,
            byref(size),
            byref(value),
        )
        self._check(status, "read default input")
        return value.value

    def device_for_id(self, object_id: int) -> AudioInputDevice:
        for device in self.input_devices():
            if device.object_id == object_id:
                return device
        raise ValueError(f"CoreAudio input device {object_id} is unavailable")

    def set_default_input_id(self, object_id: int) -> None:
        device = self.device_for_id(object_id)
        if device.input_channels <= 0:
            raise ValueError(f"{device.name!r} has no input channels")
        address = self._address("dIn ")
        value = ctypes.c_uint32(object_id)
        status = self._audio.AudioObjectSetPropertyData(
            SYSTEM_AUDIO_OBJECT,
            byref(address),
            0,
            None,
            ctypes.sizeof(value),
            byref(value),
        )
        self._check(status, "set default input")
        if self.default_input_id() != object_id:
            raise RuntimeError("macOS did not retain the requested default input")

    def set_default_input(self, name: str) -> AudioInputDevice:
        device = resolve_input_device(self.input_devices(), name)
        self.set_default_input_id(device.object_id)
        return device


class TemporaryDefaultInput:
    """Switch to a live virtual input, restoring the old input on exit."""

    def __init__(self, audio: MacOSAudioInput, target_name: str) -> None:
        self.audio = audio
        self.target = resolve_input_device(audio.input_devices(), target_name)
        self.previous_id: int | None = None
        self.switched = False

    def activate(self) -> AudioInputDevice:
        if self.switched:
            return self.target
        self.previous_id = self.audio.default_input_id()
        self.audio.set_default_input_id(self.target.object_id)
        self.switched = True
        return self.target

    def restore(self) -> AudioInputDevice | None:
        if not self.switched or self.previous_id is None:
            return None
        # Respect a manual change made while the receiver was running.
        if self.audio.default_input_id() != self.target.object_id:
            self.switched = False
            return None
        previous = self.audio.device_for_id(self.previous_id)
        self.audio.set_default_input_id(previous.object_id)
        self.switched = False
        return previous
