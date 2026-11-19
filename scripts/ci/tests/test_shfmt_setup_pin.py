#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Ubuntu setup selects a pinned shfmt asset for each architecture (issue 3101)."""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/setup/ubuntu.sh"


class ShfmtSetup(unittest.TestCase):
    def setUp(self) -> None:
        self.text = SCRIPT.read_text(encoding="utf-8")

    def test_every_asset_has_a_digest(self) -> None:
        for digest in (
            "fb096c5d1ac6beabbdbaa2874d025badb03ee07929f0c9ff67563ce8c75398b1",
            "32d92acaa5cd8abb29fc49dac123dc412442d5713967819d8af2c29f1b3857c7",
            "f6c23b95971bcc5ebf86411bccefb79f1e99f8efe7c722d980a3f244838b1398",
            "5f9a2c46249dab66d7767df676083ea6a3313831ca9f4bc29e4c21af87bd4f5e",
        ):
            self.assertIn(digest, self.text)

    def test_url_uses_the_selected_asset(self) -> None:
        self.assertIn("shfmt_${SHFMT_VERSION}_linux_${SHFMT_ARCH}", self.text)
        self.assertNotIn("shfmt_${SHFMT_VERSION}_linux_amd64", self.text)

    def test_digest_is_verified_before_install(self) -> None:
        self.assertLess(self.text.index("sha256sum"), self.text.index("install -m 0755"))

    def test_architecture_case_covers_all_assets(self) -> None:
        for arch in ("amd64", "arm64", "arm", "386"):
            self.assertIn(f'SHFMT_ARCH="{arch}"', self.text)


if __name__ == "__main__":
    unittest.main()
