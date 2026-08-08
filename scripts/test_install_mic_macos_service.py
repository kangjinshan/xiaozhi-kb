#!/usr/bin/env python3

from pathlib import Path

from install_mic_macos_service import (
    DEFAULT_AUDIO_DEVICE,
    SERVICE_LABEL,
    launch_agent_config,
    service_paths,
)


def main() -> None:
    home = Path("/Users/tester")
    app_dir, plist = service_paths(home)
    assert app_dir == Path("/Users/tester/Library/Application Support/XiaoZhi Mic")
    assert plist == Path(
        "/Users/tester/Library/LaunchAgents/com.xiaozhi.wifi-mic.plist"
    )
    config = launch_agent_config(app_dir, DEFAULT_AUDIO_DEVICE)
    assert config["Label"] == SERVICE_LABEL
    assert config["RunAtLoad"] is True
    assert config["KeepAlive"] is True
    arguments = config["ProgramArguments"]
    assert arguments[-3:] == [DEFAULT_AUDIO_DEVICE, "--input-release-timeout", "2"]
    assert str(app_dir / "mic_wifi_receiver.py") in arguments


if __name__ == "__main__":
    main()
