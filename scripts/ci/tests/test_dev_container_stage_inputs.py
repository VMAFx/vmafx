#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Regression tests for the dev/Containerfile stage-input contract (ADR-1343)."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CHECKER = ROOT / "scripts/ci/check-dev-container-stage-inputs.py"
SPEC = importlib.util.spec_from_file_location("check_dev_container_stage_inputs", CHECKER)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)

REQUIREMENTS_COPY = "COPY --chown=vmaf:vmaf requirements/ /build/vmaf/requirements/\n"


class StageInputContract(unittest.TestCase):
    def setUp(self) -> None:
        self.containerfile = (ROOT / "dev/Containerfile").read_text(encoding="utf-8")

    def test_current_containerfile_passes(self) -> None:
        self.assertEqual(MODULE.check(self.containerfile), [])

    def test_missing_requirements_copy_fails(self) -> None:
        self.assertIn(REQUIREMENTS_COPY, self.containerfile)
        text = self.containerfile.replace(REQUIREMENTS_COPY, "", 1)
        problems = MODULE.check(text)
        self.assertTrue(any("package-build.txt" in p and "no COPY" in p for p in problems))

    def test_requirement_file_missing_from_repository_fails(self) -> None:
        text = self.containerfile.replace(
            "requirements/locks/package-build.txt", "requirements/locks/absent-lock.txt", 1
        )
        problems = MODULE.check(text)
        self.assertTrue(any("absent-lock.txt does not exist" in p for p in problems))

    def test_missing_copy_source_fails(self) -> None:
        text = self.containerfile.replace(
            REQUIREMENTS_COPY, REQUIREMENTS_COPY.replace("requirements/ ", "no-such-dir/ ", 1), 1
        )
        problems = MODULE.check(text)
        self.assertTrue(any("COPY source no-such-dir/ does not exist" in p for p in problems))

    def test_parent_stage_copy_satisfies_child_stage(self) -> None:
        text = (
            "FROM scratch AS base\nWORKDIR /build/vmaf\nCOPY requirements/ ./requirements/\n"
            "FROM base AS child\n"
            "RUN pip install --require-hashes -r /build/vmaf/requirements/locks/package-build.txt\n"
        )
        self.assertEqual(MODULE.check(text), [])

    def test_sibling_stage_copy_does_not_count(self) -> None:
        text = (
            "FROM scratch AS base\n"
            "FROM base AS provider\nCOPY requirements/ /build/vmaf/requirements/\n"
            "FROM base AS consumer\n"
            "RUN pip install -r /build/vmaf/requirements/locks/package-build.txt\n"
        )
        self.assertTrue(any("no COPY" in p for p in MODULE.check(text)))

    def test_paths_outside_build_root_and_non_pip_reads_are_ignored(self) -> None:
        text = (
            "FROM scratch AS base\n"
            "RUN pip install -r /tmp/requirements.txt && cp -r /build/vmaf/absent /tmp/x\n"
        )
        self.assertEqual(MODULE.check(text), [])


if __name__ == "__main__":
    unittest.main()
