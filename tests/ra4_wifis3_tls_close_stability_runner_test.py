#!/usr/bin/env python3
"""Reject incomplete TLS cycles and sustained loss in idle heap samples."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from run_ra4_wifis3_tls_graceful_close_gate import validate_result


def transcript(rounds=12, heap_loss=0):
    lines = []
    for cycle in range(1, rounds + 1):
        prefix = "RA4_TLS_GRACEFUL_CLOSE:"
        lines.append(prefix + "CYCLE_START=" + str(cycle))
        lines.extend(prefix + marker for marker in (
            "FIRST_HANDSHAKE_PASS", "FIRST_ECHO_PASS", "CONNECTED_AFTER_CLOSE=0",
            "AVAILABLE_AFTER_CLOSE=0", "READ_AFTER_CLOSE=-1", "WRITE_AFTER_CLOSE=0",
            "RECOVERY_HANDSHAKE_PASS", "RECOVERY_ECHO_PASS", "CYCLE_ELAPSED_MS=500",
            "CLOSE_NOTIFY_PASS" if cycle % 3 == 1 else "BARE_FIN_PASS",
        ))
        if cycle % 3 != 1:
            lines.append(prefix + "TAIL_DATA_PASS")
        for stage in ("FIRST", "RECOVERY", "IDLE"):
            values = [0 if stage == "IDLE" else 6, 0, 16384, 0, 0, 10000, 200,
                      51200, 30720, 0, 0, 0, 0, 51200, 30720]
            if cycle > rounds - 3:
                values[-1] -= heap_loss
            lines.append(prefix + stage + "_DIAG=" + ",".join(map(str, values)))
        lines.append(prefix + "CYCLE_PASS=" + str(cycle))
    lines.extend((prefix + "HANDSHAKES_PASS=" + str(rounds * 2), prefix + "RESULT=PASS"))
    return "\n".join(lines) + "\n"


class StabilityRunnerTest(unittest.TestCase):
    def test_stable_and_allowance(self):
        self.assertIn("TLS_CLOSE_IDLE_WRAM_BASELINE_FINAL_LOSS_BYTES=30720,30720,0",
                      validate_result(transcript(), "mixed", 12))
        validate_result(transcript(heap_loss=2048), "mixed", 12)
        validate_result(transcript().replace("\n", "\r\n"), "mixed", 12)

    def test_extended_diagnostics_keep_original_heap_fields(self):
        import re
        extended = re.sub(r"(_DIAG=[^\n]+)",
                          lambda match: match[0] + "," + ",".join(["0"] * 29), transcript())
        self.assertIn("TLS_CLOSE_IDLE_WRAM_BASELINE_FINAL_LOSS_BYTES=30720,30720,0",
                      validate_result(extended, "mixed", 12))

    def test_heap_loss_fails(self):
        with self.assertRaisesRegex(RuntimeError, "lost 3072 bytes"):
            validate_result(transcript(heap_loss=3072), "mixed", 12)

    def test_flow_suffix_is_versioned_and_keeps_heap_checks(self):
        import re
        def with_flow(text, version=1, length=18):
            return re.sub(r"(_DIAG=[^\n]+)", lambda match: match[0] + "," +
                          ",".join(map(str, [0]*29 + [version] + [0]*(length-1))), text)
        self.assertIn("TLS_CLOSE_IDLE_WRAM_BASELINE_FINAL_LOSS_BYTES=30720,30720,0",
                      validate_result(with_flow(transcript()), "mixed", 12))
        with self.assertRaisesRegex(RuntimeError, "lost 3072 bytes"):
            validate_result(with_flow(transcript(heap_loss=3072)), "mixed", 12)
        for version, length in ((0, 18), (2, 18), (1, 17), (1, 19)):
            with self.assertRaisesRegex(RuntimeError, "resource diagnostic"):
                validate_result(with_flow(transcript(), version, length), "mixed", 12)

    def test_missing_or_duplicate_cycle_fails(self):
        for replacement in ("", "RA4_TLS_GRACEFUL_CLOSE:CYCLE_PASS=11"):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                validate_result(transcript().replace("RA4_TLS_GRACEFUL_CLOSE:CYCLE_PASS=12", replacement), "mixed", 12)

    def test_missing_middle_echo_fails(self):
        with self.assertRaisesRegex(RuntimeError, "cycle 1 missing"):
            validate_result(transcript().replace("RA4_TLS_GRACEFUL_CLOSE:RECOVERY_ECHO_PASS\n", "", 1), "mixed", 12)

    def test_malformed_diagnostic_fails(self):
        with self.assertRaises(RuntimeError):
            validate_result(transcript().replace("IDLE_DIAG=0,0", "IDLE_DIAG=0,invalid", 1), "mixed", 12)

    def test_wrong_handshake_total_fails(self):
        with self.assertRaisesRegex(RuntimeError, "handshake total"):
            validate_result(transcript().replace("HANDSHAKES_PASS=24", "HANDSHAKES_PASS=2"), "mixed", 12)


if __name__ == "__main__":
    unittest.main()
