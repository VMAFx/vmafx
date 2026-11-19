#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Dev Container features resolve through the generated lock (issue 3096)."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import check_devcontainer_feature_pins as check  # noqa: E402

DIGEST = "sha256:" + "0" * 64


def config(*references: str) -> dict[str, object]:
    return {"features": {reference: {} for reference in references}}


def record(reference: str, version: str = "2.7.1") -> dict[str, object]:
    base = reference.split("@", 1)[0].rsplit(":", 1)[0]
    return {
        reference: {
            "version": version,
            "resolved": f"{base}@{DIGEST}",
            "integrity": DIGEST,
        }
    }


class Findings(unittest.TestCase):
    def test_exact_lock_records_pass(self) -> None:
        reference = "ghcr.io/devcontainers/features/common-utils:2"
        self.assertEqual(check.findings(config(reference), {"features": record(reference)}), [])

    def test_a_missing_record_is_refused(self) -> None:
        reference = "ghcr.io/devcontainers/features/rust:1"
        problems = check.findings(config(reference), {"features": {}})
        self.assertIn("has no devcontainer-lock.json record", problems[0])

    def test_a_record_without_a_digest_is_refused(self) -> None:
        reference = "ghcr.io/devcontainers/features/nix:1"
        lock = record(reference)
        lock[reference]["resolved"] = "ghcr.io/devcontainers/features/nix:1.4.1"
        self.assertIn(
            "not an @sha256 digest", check.findings(config(reference), {"features": lock})[0]
        )

    def test_a_major_only_version_is_refused(self) -> None:
        reference = "ghcr.io/devcontainers/features/rust:1"
        self.assertIn(
            "not an exact release",
            check.findings(config(reference), {"features": record(reference, "1")})[0],
        )

    def test_a_record_for_another_feature_is_refused(self) -> None:
        reference = "ghcr.io/devcontainers/features/rust:1"
        lock = record(reference)
        lock[reference]["resolved"] = "ghcr.io/devcontainers/features/nix@" + DIGEST
        self.assertIn("names", check.findings(config(reference), {"features": lock})[0])

    def test_features_must_be_a_non_empty_object(self) -> None:
        self.assertIn("missing", check.findings({}, {"features": {}})[0])
        self.assertIn("empty", check.findings({"features": {}}, {"features": {}})[0])


class RepositoryFiles(unittest.TestCase):
    def test_the_checked_in_devcontainer_lock_resolves_every_feature(self) -> None:
        config = json.loads((ROOT / ".devcontainer/devcontainer.json").read_text(encoding="utf-8"))
        lock = json.loads(
            (ROOT / ".devcontainer/devcontainer-lock.json").read_text(encoding="utf-8")
        )
        self.assertEqual(check.findings(config, lock), [])


if __name__ == "__main__":
    unittest.main()
