#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Verify the tester fixture commit mirrors build-config.env's VMAF_RESOURCE_COMMIT.

Dockerfile ``ARG`` defaults and workflow-level ``env`` blocks cannot source
``build-config.env`` at parse time, so the tester image and both tester-bundle
workflows repeat the commit. ``check_vmaf_resource_commit()`` fails on a missing,
duplicated or differing mirror.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "workflow_versions", ROOT / "scripts/ci/check-workflow-versions.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

CHECK = MODULE.check_vmaf_resource_commit
WANT = "c0ab6adbd7e41bb354f14686ed08500530622bc3"


class VmafResourceCommitSingleSource(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name)
        self.write("docker/Dockerfile.tester", f"ARG VMAF_RESOURCE_COMMIT={WANT}\n")
        self.write(".github/workflows/macos-tester-bundle.yml", f"  VMAF_RESOURCE_COMMIT: {WANT}\n")
        self.write(
            ".github/workflows/windows-tester-bundle.yml",
            f"  VMAF_RESOURCE_COMMIT: {WANT}\n",
        )

    def write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_matching_mirrors_pass(self) -> None:
        self.assertEqual(CHECK(self.repo, WANT), [])

    def test_repository_mirrors_match_config(self) -> None:
        want = MODULE.load_config(ROOT).get("VMAF_RESOURCE_COMMIT")
        self.assertEqual(CHECK(ROOT, want), [])

    def test_drifted_mirror_fails(self) -> None:
        self.write("docker/Dockerfile.tester", "ARG VMAF_RESOURCE_COMMIT=deadbeef\n")
        problems = CHECK(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn("docker/Dockerfile.tester", problems[0])
        self.assertIn("deadbeef", problems[0])

    def test_missing_mirror_fails(self) -> None:
        (self.repo / ".github/workflows/macos-tester-bundle.yml").unlink()
        problems = CHECK(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn("macos-tester-bundle.yml", problems[0])

    def test_duplicate_mirror_fails(self) -> None:
        self.write(
            "docker/Dockerfile.tester",
            f"ARG VMAF_RESOURCE_COMMIT={WANT}\nARG VMAF_RESOURCE_COMMIT={WANT}\n",
        )
        problems = CHECK(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn("exactly once", problems[0])

    def test_missing_config_value_fails(self) -> None:
        self.assertEqual(
            CHECK(self.repo, None), ["build-config.env defines no VMAF_RESOURCE_COMMIT"]
        )


if __name__ == "__main__":
    unittest.main()
