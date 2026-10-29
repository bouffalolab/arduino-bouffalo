#!/usr/bin/env python3
"""Static contract checks for the project-controlled Bouffalo SDK source lock."""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
LOCK_PATH = ROOT_DIR / "third_party" / "bouffalo_sdk.lock.json"
GITMODULES_PATH = ROOT_DIR / ".gitmodules"


class BouffaloSdkSourceLockTest(unittest.TestCase):
    def setUp(self) -> None:
        self.lock = json.loads(LOCK_PATH.read_text(encoding="utf-8"))

    def test_root_submodule_is_pinned_to_remote_sdk(self) -> None:
        gitmodules = GITMODULES_PATH.read_text(encoding="utf-8")
        self.assertIn('[submodule "third_party/bouffalo_sdk"]', gitmodules)
        self.assertIn("path = third_party/bouffalo_sdk", gitmodules)
        self.assertIn(
            "url = ssh://gerrit.bouffalolab.com:29418/bouffalo/bouffalo_sdk.git",
            gitmodules,
        )
        self.assertEqual(self.lock["path"], "third_party/bouffalo_sdk")
        self.assertEqual(
            self.lock["source_url"],
            "ssh://gerrit.bouffalolab.com:29418/bouffalo/bouffalo_sdk.git",
        )
        self.assertEqual(
            self.lock["commit"],
            "240ff49ba0189ebeba92e6d5c42533f63d90a161",
        )

    def test_required_bridge_submodules_are_locked(self) -> None:
        required = self.lock["required_submodules"]
        self.assertIsInstance(required, dict)
        expected = {
            "components/crypto/mbedtls/mbedtls_v3",
            "components/crypto/mbedtls/mbedtls_v3/framework",
            "components/fs",
            "components/graphics",
            "components/net/lwip/lwip",
            "components/usb/cherryusb",
            "components/wireless/bluetooth",
            "components/wireless/macsw",
            "components/wireless/wifi6",
            "drivers/lhal",
            "drivers/soc/bl616cl/phyrf",
            "drivers/soc/bl616cl/std",
            "drivers/sys",
            "tools/bflb_tools",
        }
        self.assertTrue(expected.issubset(set(required)))
        self.assertEqual(len(required), 15)
        framework_url = "https://github.com/Mbed-TLS/mbedtls-framework"
        for path, record in required.items():
            self.assertNotIn("..", path)
            record = required[path]
            self.assertRegex(record["commit"], r"^[0-9a-f]{40}$")
            if path == "components/crypto/mbedtls/mbedtls_v3/framework":
                self.assertEqual(record["url"], framework_url)
            else:
                self.assertRegex(
                    record["url"],
                    r"^ssh://gerrit\.bouffalolab\.com:29418/.+\.git$",
                )

    def test_sparse_checkout_excludes_unrelated_sdk_trees(self) -> None:
        sparse_paths = self.lock["bootstrap"]["sparse_paths"]
        self.assertIn("components", sparse_paths)
        self.assertIn("drivers", sparse_paths)
        self.assertIn("bsp", sparse_paths)
        self.assertIn("examples/wifi", sparse_paths)
        self.assertIn("tools/byai", sparse_paths)
        self.assertIn("tools/cmake", sparse_paths)
        self.assertNotIn("tests", sparse_paths)
        self.assertNotIn("chiptest", sparse_paths)
        self.assertNotIn("examples", sparse_paths)
        self.assertEqual(
            self.lock["bootstrap"]["command"],
            "python3 tools/prepare_bouffalo_sdk_source.py --init",
        )

    def test_lock_contains_no_machine_local_source_path(self) -> None:
        lock_text = LOCK_PATH.read_text(encoding="utf-8")
        self.assertNotIn("/home/", lock_text)
        self.assertNotIn("\\\\", lock_text)
        self.assertIsNotNone(
            re.fullmatch(
                r"[0-9a-f]{40}",
                self.lock["commit"],
            )
        )


if __name__ == "__main__":
    unittest.main()
