#!/usr/bin/env python3
"""Unit tests for the isolated WiFiS3 RESULT_ERROR compatibility overlay."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT_DIR = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT_DIR / "tools" / "prepare_wifis3_result_error_overlay.py"
SPEC = importlib.util.spec_from_file_location("wifis3_result_error_overlay", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
OVERLAY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OVERLAY)


class WiFiS3ResultErrorOverlayTests(unittest.TestCase):
    def test_patch_replaces_only_the_known_typo(self) -> None:
        source = (
            "#define RESULT_OK       OK_TOKEN TERM_TOKEN\n"
            "#define RESULT_ERROR    OK_TOKEN TERM_TOKEN\n"
            "if(restart && state == at_parse_state_t::Completed) {\n"
        )
        patched = OVERLAY.patch_modem_source(source)
        self.assertIn(
            "#define RESULT_ERROR    ERROR_TOKEN TERM_TOKEN",
            patched,
        )
        self.assertNotIn(
            "#define RESULT_ERROR    OK_TOKEN TERM_TOKEN",
            patched,
        )
        self.assertIn(
            "state == at_parse_state_t::Completed && res != Error",
            patched,
        )

    def test_patch_refuses_unrecognized_or_prepatched_input(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "expected broken parser"):
            OVERLAY.patch_modem_source("#define RESULT_ERROR OTHER\n")
        with self.assertRaisesRegex(RuntimeError, "already contains"):
            OVERLAY.patch_modem_source(
                "#define RESULT_ERROR    ERROR_TOKEN TERM_TOKEN\n"
                "if(restart && state == at_parse_state_t::Completed) {\n"
            )

    def test_overlay_is_pinned_to_the_observed_core_source(self) -> None:
        self.assertEqual(
            OVERLAY.MODEM_SOURCE_SHA256,
            "2c2aa923ea176cf723932e55f5afedf9af25de4b7ba738595f068bc0b2caad26",
        )
        self.assertEqual(OVERLAY.ARDUINO_CORE_VERSION, "1.6.0")


if __name__ == "__main__":
    unittest.main()
