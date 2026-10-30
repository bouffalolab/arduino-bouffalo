#!/usr/bin/env python3
"""Unit tests for AP-outage reconnect gate support helpers."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import stat
import tempfile
import unittest


ROOT_DIR = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT_DIR / "tools" / "sta_reconnect_gate_support.py"
SPEC = importlib.util.spec_from_file_location("sta_reconnect_gate_support", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
SUPPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUPPORT)


class StaReconnectGateSupportTests(unittest.TestCase):
    def test_status_helpers_deduplicate_and_format(self) -> None:
        statuses: list[int] = []
        SUPPORT.append_status_if_changed(statuses, 2)
        SUPPORT.append_status_if_changed(statuses, 2)
        SUPPORT.append_status_if_changed(statuses, 3)
        self.assertEqual(statuses, [2, 3])
        self.assertEqual(SUPPORT.status_sequence_text(statuses), "2,3")

    def test_outage_requires_disconnect_without_reconnect(self) -> None:
        SUPPORT.require_outage_statuses([3, 0])
        with self.assertRaisesRegex(RuntimeError, "did not produce"):
            SUPPORT.require_outage_statuses([3])
        with self.assertRaisesRegex(RuntimeError, "before AP recovery"):
            SUPPORT.require_outage_statuses([3, 0, 3])

    def test_observe_statuses_keeps_deduplicated_sequence(self) -> None:
        values = iter([3, 3, 0])
        clock = iter([1.0, 1.0, 1.1, 1.2, 2.1]).__next__
        observed = SUPPORT.observe_statuses(
            lambda: next(values),
            duration_seconds=1.0,
            poll_interval_seconds=0.01,
            monotonic=clock,
            sleep=lambda _: None,
        )
        self.assertEqual(observed, [3, 0])

    def test_hook_requires_regular_executable_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hook"
            path.write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
            with self.assertRaisesRegex(RuntimeError, "not executable"):
                SUPPORT.require_hook(str(path), "outage")
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
            self.assertEqual(SUPPORT.require_hook(str(path), "outage"), path)
        with self.assertRaisesRegex(RuntimeError, "must be provided"):
            SUPPORT.require_hook(None, "recovery")

    def test_hook_does_not_receive_sta_credentials(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hook"
            output = Path(directory) / "output"
            path.write_text(
                "#!/bin/sh\n"
                f"printf '%s:%s:%s' \"$BL616CL_STA_SSID\" \"$BL616CL_STA_PASSWORD\" "
                f"\"$UNO_R4_STA_RECONNECT_PHASE\" > '{output}'\n",
                encoding="ascii",
            )
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
            old_ssid = os.environ.get("BL616CL_STA_SSID")
            old_password = os.environ.get("BL616CL_STA_PASSWORD")
            try:
                os.environ["BL616CL_STA_SSID"] = "fixture-ssid"
                os.environ["BL616CL_STA_PASSWORD"] = "fixture-password"
                SUPPORT.run_hook(path, "outage", 1.0)
            finally:
                if old_ssid is None:
                    os.environ.pop("BL616CL_STA_SSID", None)
                else:
                    os.environ["BL616CL_STA_SSID"] = old_ssid
                if old_password is None:
                    os.environ.pop("BL616CL_STA_PASSWORD", None)
                else:
                    os.environ["BL616CL_STA_PASSWORD"] = old_password
            self.assertEqual(output.read_text(encoding="ascii"), "::outage")


if __name__ == "__main__":
    unittest.main()
