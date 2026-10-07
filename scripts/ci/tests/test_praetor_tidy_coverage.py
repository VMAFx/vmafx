#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/ci/praetor_tidy_coverage.py renders praetor's lane list and exceptions, and refuses a stale one.

Each case builds a throwaway git repository. The planted defects: a unit a lane measures that the
lane file lacks, an exception the manifest block lacks, and a block edited by hand; ``--check``
must fail each, and ``--write`` must repair it. A HISS-11 entry of the repository list reaches the
block with the expiry cap; an entry of a rule praetor does not read stays out.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/ci/praetor_tidy_coverage.py"
GIT = shutil.which("git") or "git"
CLEAN_ENV = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
TOML = '[[exception]]\npath = "src/win32.c"\nreason = "Windows only"\nexpires = 2027-06-30\n'


class Repo:
    def __init__(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        subprocess.run(  # noqa: S603 -- fixed argv
            [GIT, "init", "-q", str(self.root)], check=True, timeout=30, env=CLEAN_ENV
        )
        (self.root / "scripts/ci").mkdir(parents=True)
        (self.root / "scripts/ci/tidy-baseline-cpu.json").write_text(
            json.dumps({"measured_sources": ["src/a.c"]})
        )
        (self.root / ".config/lint-exceptions.d").mkdir(parents=True)
        (self.root / ".config/lint-exceptions.d/clang-tidy-coverage.toml").write_text(TOML)
        (self.root / ".standards.yaml").write_text("version: 1\n")
        for unit in ("src/a.c", "src/win32.c", "scripts/ci/exact_twins.d/adm.hip"):
            path = self.root / unit
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("int x;\n")
        subprocess.run(  # noqa: S603 -- fixed argv
            [GIT, "-C", str(self.root), "add", "-A"], check=True, timeout=30, env=CLEAN_ENV
        )

    def run(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 -- fixed argv
            [sys.executable, str(SCRIPT), "--root", str(self.root), *args],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
            env=CLEAN_ENV,
        )

    def close(self) -> None:
        self.dir.cleanup()


class PraetorTidyCoverageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = Repo()
        self.addCleanup(self.repo.close)

    def test_a_fresh_repository_is_stale_until_written(self) -> None:
        self.assertEqual(self.repo.run("--check").returncode, 1)
        self.assertEqual(self.repo.run("--write").returncode, 0)
        self.assertEqual(self.repo.run("--check").returncode, 0)

    def test_the_rendering_has_the_lane_list_and_one_entry_per_unexcused_unit(self) -> None:
        self.repo.run("--write")
        lanes = (self.repo.root / ".config/clang-tidy/measured-sources.txt").read_text()
        self.assertEqual(lanes, "src/a.c\n")
        manifest = (self.repo.root / ".standards.yaml").read_text()
        self.assertIn('path: "src/win32.c"', manifest)
        self.assertIn('path: "scripts/ci/exact_twins.d/adm.hip"', manifest)
        self.assertNotIn('path: "src/a.c"', manifest)
        # the declared date is past praetor's 90-day window, so it is cut to the cap
        self.assertNotIn("2027-06-30", manifest)
        self.assertIn('expires: "2027-01-04"', manifest)

    def test_a_hand_edited_block_is_refused(self) -> None:
        self.repo.run("--write")
        manifest = self.repo.root / ".standards.yaml"
        manifest.write_text(manifest.read_text().replace("Windows only", "edited by hand"))
        result = self.repo.run("--check")
        self.assertEqual(result.returncode, 1)
        self.assertIn(".standards.yaml is stale", result.stdout)

    def test_a_new_measured_unit_makes_the_lane_file_stale(self) -> None:
        self.repo.run("--write")
        (self.repo.root / "scripts/ci/tidy-baseline-cpu.json").write_text(
            json.dumps({"measured_sources": ["src/a.c", "src/b.c"]})
        )
        result = self.repo.run("--check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("measured-sources.txt is stale", result.stdout)

    def test_a_declared_praetor_rule_entry_is_rendered_with_the_cap(self) -> None:
        workflow = self.repo.root / ".github/workflows/publish.yml"
        workflow.parent.mkdir(parents=True)
        workflow.write_text("on: push\n")
        (self.repo.root / ".config/lint-exceptions.d/HISS-11.toml").write_text(
            '[[exception]]\npath = ".github/workflows/publish.yml"\n'
            'reason = "provenance at Level 2"\nexpires = 2027-03-31\n'
        )
        self.repo.run("--write")
        manifest = (self.repo.root / ".standards.yaml").read_text()
        self.assertIn(
            '  - rule: HISS-11\n    path: ".github/workflows/publish.yml"\n'
            '    reason: "provenance at Level 2"\n    expires: "2027-01-04"\n',
            manifest,
        )

    def test_an_entry_of_another_rule_stays_out_of_the_block(self) -> None:
        (self.repo.root / ".config/lint-exceptions.d/black.toml").write_text(
            '[[exception]]\npath = "src/a.c"\nreason = "formatter"\nexpires = 2027-06-30\n'
        )
        self.repo.run("--write")
        manifest = (self.repo.root / ".standards.yaml").read_text()
        self.assertNotIn("rule: black", manifest)
        self.assertNotIn('path: "src/a.c"', manifest)

    def test_write_keeps_the_text_around_the_block(self) -> None:
        manifest = self.repo.root / ".standards.yaml"
        manifest.write_text("version: 1\nprofiles:\n  - p\n")
        self.repo.run("--write")
        self.repo.run("--write")
        text = manifest.read_text()
        self.assertTrue(text.startswith("version: 1\nprofiles:\n  - p\n"))
        self.assertEqual(text.count("# BEGIN generated"), 1)


if __name__ == "__main__":
    unittest.main()
