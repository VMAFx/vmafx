#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Dev Container features stay on exact releases or digests (issue 3096)."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import check_devcontainer_feature_pins as check  # noqa: E402


class Findings(unittest.TestCase):
    def test_exact_releases_pass(self) -> None:
        config = {
            "features": {
                "ghcr.io/devcontainers/features/common-utils:2.7.1": {},
                "ghcr.io/devcontainers/features/nix:1.4.1": {},
            }
        }
        self.assertEqual(check.findings(config), [])

    def test_a_major_tag_is_refused(self) -> None:
        config = {"features": {"ghcr.io/devcontainers/features/rust:1": {}}}
        self.assertIn("not an exact release", check.findings(config)[0])

    def test_a_digest_is_exact(self) -> None:
        digest = "sha256:" + "0" * 64
        config = {"features": {f"ghcr.io/devcontainers/features/rust@{digest}": {}}}
        self.assertEqual(check.findings(config), [])

    def test_a_missing_version_is_refused(self) -> None:
        config = {"features": {"ghcr.io/devcontainers/features/nix": {}}}
        self.assertIn("no version", check.findings(config)[0])

    def test_features_must_be_a_non_empty_object(self) -> None:
        self.assertIn("missing", check.findings({})[0])
        self.assertIn("empty", check.findings({"features": {}})[0])


class RepositoryConfig(unittest.TestCase):
    def test_the_checked_in_devcontainer_uses_exact_releases(self) -> None:
        config = json.loads((ROOT / ".devcontainer/devcontainer.json").read_text(encoding="utf-8"))
        self.assertEqual(check.findings(config), [])


if __name__ == "__main__":
    unittest.main()
