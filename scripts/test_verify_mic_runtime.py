import importlib.util
from pathlib import Path
import sys
import unittest


MODULE_PATH = Path(__file__).with_name("verify_mic_runtime.py")
SPEC = importlib.util.spec_from_file_location("verify_mic_runtime", MODULE_PATH)
verify_mic_runtime = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = verify_mic_runtime
SPEC.loader.exec_module(verify_mic_runtime)


class MicRuntimeEvidenceTest(unittest.TestCase):
    def test_complete_healthy_boot_passes(self):
        evidence = verify_mic_runtime.MicRuntimeEvidence()
        for marker in verify_mic_runtime.REQUIRED_MARKERS.values():
            evidence.consume(f"I (100) mic: {marker}")
        self.assertTrue(evidence.passed)
        self.assertEqual(evidence.missing, [])

    def test_missing_shared_ns_fails(self):
        evidence = verify_mic_runtime.MicRuntimeEvidence()
        for name, marker in verify_mic_runtime.REQUIRED_MARKERS.items():
            if name != "shared_ns":
                evidence.consume(marker)
        self.assertFalse(evidence.passed)
        self.assertEqual(evidence.missing, ["shared_ns"])

    def test_fatal_marker_fails(self):
        evidence = verify_mic_runtime.MicRuntimeEvidence()
        for marker in verify_mic_runtime.REQUIRED_MARKERS.values():
            evidence.consume(marker)
        evidence.consume("Guru Meditation Error: Core 0 panic'ed")
        self.assertFalse(evidence.passed)
        self.assertEqual(len(evidence.fatal_lines), 1)

    def test_task_watchdog_marker_fails(self):
        evidence = verify_mic_runtime.MicRuntimeEvidence()
        for marker in verify_mic_runtime.REQUIRED_MARKERS.values():
            evidence.consume(marker)
        evidence.consume(
            "E (121054) task_wdt: Task watchdog got triggered."
        )
        self.assertFalse(evidence.passed)
        self.assertEqual(len(evidence.fatal_lines), 1)


if __name__ == "__main__":
    unittest.main()
