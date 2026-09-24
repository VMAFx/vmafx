# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regression tests for REUSE 3.3 license and copyright compliance (BUG-003)."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

import reuse.lint
import reuse.project

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402


class ReuseComplianceTests(unittest.TestCase):
    """Ensure repository maintains 100% REUSE 3.3 specification compliance."""

    project: reuse.project.Project

    @classmethod
    def setUpClass(cls) -> None:
        """Load the effective metadata once for provenance assertions."""
        cls.project = reuse.project.Project.from_directory(ROOT)

    def assert_provenance(self, path: str, licenses: set[str], copyright_holders: set[str]) -> None:
        """Assert the effective REUSE record, not merely TOML source text."""
        records = self.project.reuse_info_of(path)
        self.assertEqual(len(records), 1, f"{path}: expected one effective REUSE record")
        record = records[0]
        self.assertEqual({str(item) for item in record.spdx_expressions}, licenses, path)
        holders = {item.name for item in record.copyright_notices}
        self.assertTrue(
            copyright_holders <= holders,
            f"{path}: missing holders {copyright_holders - holders}; got {holders}",
        )

    def test_reuse_toml_exists_and_valid(self) -> None:
        """REUSE.toml must exist at repo root and declare version 1."""
        reuse_toml = ROOT / "REUSE.toml"
        self.assertTrue(reuse_toml.is_file(), "REUSE.toml must exist at root")
        content = reuse_toml.read_text(encoding="utf-8")
        self.assertIn("version = 1", content)
        self.assertIn("[[annotations]]", content)

    def test_reuse_lint_cli_passes(self) -> None:
        """'reuse lint' CLI command must exit with code 0 and report compliance."""
        result = run_command(
            ["reuse", "lint"],
            allowed_executables=["reuse"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout_seconds=30.0,
            max_output_bytes=2 * 1024 * 1024,
        )
        self.assertEqual(
            result.returncode,
            0,
            f"'reuse lint' failed (exit {result.returncode}):\n{result.stdout}\n{result.stderr}",
        )
        self.assertIn("compliant with version 3.3 of the REUSE Specification", result.stdout)
        self.assertIn("Bad licenses: 0", result.stdout)
        self.assertIn("Missing licenses: 0", result.stdout)
        self.assertIn("Unused licenses: 0", result.stdout)
        self.assertIn("Invalid SPDX License Expressions: 0", result.stdout)

    def test_reuse_report_programmatic_invariants(self) -> None:
        """Programmatic inspection must report zero missing licenses/copyrights."""
        report = reuse.lint.ProjectReport.generate(self.project)

        self.assertEqual(len(report.files_without_copyright), 0, "No files may lack copyright")
        self.assertEqual(len(report.files_without_licenses), 0, "No files may lack licensing info")
        self.assertEqual(
            len(report.invalid_spdx_expressions), 0, "No invalid SPDX expressions allowed"
        )
        self.assertEqual(
            len(report.unused_licenses), 0, "All licenses in LICENSES/ must be referenced"
        )
        self.assertEqual(
            len(report.missing_licenses), 0, "All declared licenses must exist in LICENSES/"
        )
        self.assertEqual(len(report.bad_licenses), 0, "No bad licenses allowed")

    def test_provenance_overrides_prevent_silent_relicensing(self) -> None:
        """Coverage metadata must not relabel inherited or no-CLA contributions."""
        cases = {
            # Root and renamed Netflix/vmaf descendants remain BSD.
            "README.md": ({"BSD-2-Clause-Patent"}, {"Netflix, Inc.", "Lusoris"}),
            "docs/usage/python.md": (
                {"BSD-2-Clause-Patent"},
                {"Netflix, Inc. and VMAF contributors", "Lusoris"},
            ),
            # Binary upstream references retain their named authors.
            "docs/reference/papers/CAMBI_PCS2021.pdf": (
                {"BSD-2-Clause-Patent"},
                {"Pulkit Tandon", "Mariana Afonso", "Joel Sole", "Lukáš Krasula"},
            ),
            # No-CLA contributions and append-only aggregate files remain BSD.
            "docs/adr/1122-vmaf-v1-model-port.md": (
                {"BSD-2-Clause-Patent"},
                {"Dmitry Popovich", "Lusoris"},
            ),
            "docs/rebase-notes.md": (
                {"BSD-2-Clause-Patent"},
                {"Christopher Degawa", "Dmitry Popovich", "Kyle Swanson", "Lusoris"},
            ),
            "docs/adr/0398-mytestcase-migration-partial-port.md": (
                {"BSD-2-Clause-Patent"},
                {"christosb", "Lusoris"},
            ),
            # FFmpeg derivatives follow the exact source units they patch.
            "ffmpeg-patches/0001-libvmaf-add-tiny-model-option.patch": (
                {"LGPL-2.1-or-later"},
                {"the FFmpeg developers", "Lusoris"},
            ),
            "ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch": (
                {"LGPL-2.1-or-later"},
                {"the FFmpeg developers", "Lawrence Curtis", "Lusoris"},
            ),
            "ffmpeg-patches/0019-ffmpeg-eliminate-gcc-14-build-diagnostics.patch": (
                {"LGPL-2.1-or-later AND GPL-2.0-or-later"},
                {"the FFmpeg developers", "Lusoris"},
            ),
        }
        for path, (licenses, holders) in cases.items():
            with self.subTest(path=path):
                self.assert_provenance(path, licenses, holders)


if __name__ == "__main__":
    unittest.main()
