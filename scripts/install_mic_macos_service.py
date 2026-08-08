#!/usr/bin/env python3
"""Install the XiaoZhi Wi-Fi Mic receiver as a macOS LaunchAgent."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import venv


SERVICE_LABEL = "com.xiaozhi.wifi-mic"
DEFAULT_AUDIO_DEVICE = "BlackHole 2ch"
SOURCE_FILES = ("mic_wifi_receiver.py", "macos_audio_input.py")


def service_paths(home: Path) -> tuple[Path, Path]:
    app_dir = home / "Library" / "Application Support" / "XiaoZhi Mic"
    plist = home / "Library" / "LaunchAgents" / f"{SERVICE_LABEL}.plist"
    return app_dir, plist


def launch_agent_config(app_dir: Path, audio_device: str) -> dict:
    python = app_dir / "venv" / "bin" / "python"
    receiver = app_dir / "mic_wifi_receiver.py"
    return {
        "Label": SERVICE_LABEL,
        "ProgramArguments": [
            str(python),
            str(receiver),
            "--play-to",
            audio_device,
            "--input-release-timeout",
            "2",
        ],
        "WorkingDirectory": str(app_dir),
        "RunAtLoad": True,
        "KeepAlive": True,
        "ProcessType": "Interactive",
        "EnvironmentVariables": {
            "PYTHONUNBUFFERED": "1",
            "PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
        },
        "StandardOutPath": "/private/tmp/xiaozhi-wifi-mic.log",
        "StandardErrorPath": "/private/tmp/xiaozhi-wifi-mic-error.log",
    }


def run(
    command: list[str], *, check: bool = True, cwd: Path | None = None
) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=check, text=True, cwd=cwd)


def bootout(plist_path: Path) -> None:
    subprocess.run(
        ["launchctl", "bootout", launch_domain(), str(plist_path)],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def launch_domain() -> str:
    return f"gui/{os.getuid()}"


def validate_platform() -> None:
    if sys.platform != "darwin":
        raise RuntimeError("the background receiver installer supports macOS only")


def validate_source_files(script_dir: Path) -> None:
    missing = [name for name in SOURCE_FILES if not (script_dir / name).is_file()]
    if missing:
        raise RuntimeError("missing receiver source: " + ", ".join(missing))


def validate_audio_device(python: Path, app_dir: Path, audio_device: str) -> None:
    code = """
import sounddevice
from macos_audio_input import MacOSAudioInput, resolve_input_device
from mic_wifi_receiver import verify_output_input_loopback

name = __import__('sys').argv[1]
core_audio = MacOSAudioInput()
target = resolve_input_device(core_audio.input_devices(), name)
if target.input_channels < 2:
    raise RuntimeError(f'{target.name!r} does not provide stereo input')
outputs = [d for d in sounddevice.query_devices() if d['max_output_channels'] >= 2]
if not any(d['name'].casefold() == name.casefold() for d in outputs):
    raise RuntimeError(f'{name!r} does not provide stereo output')
verify_output_input_loopback(sounddevice, target.name, name)
print(f'Validated stereo output-to-input loopback: {target.name}')
"""
    run([str(python), "-c", code, audio_device], check=True, cwd=app_dir)


def install(audio_device: str) -> None:
    validate_platform()
    home = Path.home().resolve()
    script_dir = Path(__file__).resolve().parent
    validate_source_files(script_dir)
    app_dir, plist_path = service_paths(home)
    app_dir.mkdir(parents=True, exist_ok=True)
    plist_path.parent.mkdir(parents=True, exist_ok=True)
    for name in SOURCE_FILES:
        shutil.copy2(script_dir / name, app_dir / name)

    environment = app_dir / "venv"
    if not (environment / "bin" / "python").exists():
        venv.EnvBuilder(with_pip=True).create(environment)
    python = environment / "bin" / "python"
    run([str(python), "-m", "pip", "install", "--quiet", "sounddevice>=0.5,<1"])
    validate_audio_device(python, app_dir, audio_device)

    config = launch_agent_config(app_dir, audio_device)
    temporary = plist_path.with_suffix(".plist.tmp")
    with temporary.open("wb") as handle:
        plistlib.dump(config, handle, sort_keys=True)
    os.replace(temporary, plist_path)

    domain = launch_domain()
    bootout(plist_path)
    run(["launchctl", "bootstrap", domain, str(plist_path)])
    run(["launchctl", "kickstart", "-k", f"{domain}/{SERVICE_LABEL}"])
    print(f"Installed and started {SERVICE_LABEL}")
    print("Entering Mic on the device will now switch macOS to", audio_device)


def uninstall() -> None:
    validate_platform()
    home = Path.home().resolve()
    app_dir, plist_path = service_paths(home)
    bootout(plist_path)
    if plist_path.exists():
        plist_path.unlink()
    if app_dir.exists():
        shutil.rmtree(app_dir)
    print(f"Uninstalled {SERVICE_LABEL}")


def status() -> int:
    validate_platform()
    result = subprocess.run(
        ["launchctl", "print", f"{launch_domain()}/{SERVICE_LABEL}"],
        check=False,
        text=True,
        capture_output=True,
    )
    if result.returncode == 0:
        print(result.stdout, end="")
    else:
        print(f"{SERVICE_LABEL} is not running")
    return 0 if result.returncode == 0 else 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    install_parser = subparsers.add_parser("install")
    install_parser.add_argument(
        "--audio-device", default=DEFAULT_AUDIO_DEVICE
    )
    subparsers.add_parser("uninstall")
    subparsers.add_parser("status")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.command == "install":
            install(args.audio_device)
            return 0
        if args.command == "uninstall":
            uninstall()
            return 0
        return status()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
