import importlib.util
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("verify_agent_voice_runtime.py")


def _load_module():
    spec = importlib.util.spec_from_file_location("verify_agent_voice_runtime", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_complete_runtime_log_passes():
    module = _load_module()
    output = "\n".join([
        "Agent voice Wi-Fi connected",
        "Agent voice WSS ready",
        "Agent user WAV stored: /sdcard/agent/20260712/turn-redacted/user.wav",
        "Agent accepted turn: turn-redacted",
        "Agent reply stored: /sdcard/agent/20260712/turn-redacted/assistant.wav",
        "Agent reply playback start: /sdcard/agent/20260712/turn-redacted/assistant.wav",
    ])
    result = module.evaluate_log(output)
    assert result.ok
    assert result.missing == ()
    assert result.failures == ()


def test_missing_milestone_and_fatal_runtime_text_fail():
    module = _load_module()
    result = module.evaluate_log(
        "recorder app starting\n"
        "assert failed: spi_hal_setup_trans\n"
        "recorder app starting\n"
        "reply integrity check failed\n"
    )
    assert not result.ok
    assert "wss_ready" in result.missing
    assert "spi2_assertion" in result.failures
    assert "reboot_loop" in result.failures
    assert "hash_failure" in result.failures
