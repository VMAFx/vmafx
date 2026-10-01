#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Regression tests for dev/Containerfile reference binary build flags (ADR-1317)."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CONTAINERFILE_PATH = ROOT / "dev/Containerfile"


def validate_reference_build_flags(containerfile_content: str) -> list[str]:
    """Validate that dev/Containerfile does not enable -march=native on the reference binary."""
    errors: list[str] = []

    # Find the meson setup block in libvmaf-build stage
    match = re.search(
        r"(meson\s+setup\s+core/build\s+core\b.*?)(?=\n\s*(?:USER|\Z|RUN\s+--mount|FROM))",
        containerfile_content,
        re.DOTALL,
    )
    if not match:
        errors.append("Could not locate 'meson setup core/build core' in Containerfile")
        return errors

    meson_block = match.group(1)

    if "-march=native" in meson_block:
        errors.append(
            "Found '-march=native' in dev/Containerfile reference binary build flags. "
            "Under Intel icx, -march=native enables FMA contraction causing CPU extractor "
            "and SpEED scores to drift from default builds (ADR-1317)."
        )

    # Check for rationale citation
    if "ADR-1317" not in containerfile_content:
        errors.append(
            "dev/Containerfile must cite ADR-1317 explaining the floating-point contraction isolation"
        )

    if "T-DEV-IMAGE-ICX-NATIVE-FMA-DRIFT-2026-09-30" not in containerfile_content:
        errors.append("dev/Containerfile must cite T-DEV-IMAGE-ICX-NATIVE-FMA-DRIFT-2026-09-30")

    return errors


class DevContainerReferenceBuildFlagsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.containerfile = CONTAINERFILE_PATH.read_text(encoding="utf-8")

    def test_current_containerfile_passes(self) -> None:
        errors = validate_reference_build_flags(self.containerfile)
        self.assertEqual(errors, [])

    def test_march_native_detected_and_rejected(self) -> None:
        tampered = self.containerfile.replace(
            "--prefix=/usr/local \\",
            '--prefix=/usr/local \\\n        -Dc_args="-march=native" \\',
        )
        errors = validate_reference_build_flags(tampered)
        self.assertTrue(any("-march=native" in err for err in errors))

    def test_missing_meson_setup_detected(self) -> None:
        tampered = self.containerfile.replace(
            "meson setup core/build core", "meson setup other/build core"
        )
        errors = validate_reference_build_flags(tampered)
        self.assertTrue(any("Could not locate" in err for err in errors))

    def test_missing_adr_citation_detected(self) -> None:
        tampered = self.containerfile.replace("ADR-1317", "ADR-XXXX")
        errors = validate_reference_build_flags(tampered)
        self.assertTrue(any("ADR-1317" in err for err in errors))


if __name__ == "__main__":
    unittest.main()
