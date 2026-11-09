#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""`make coverage-check` must hand coverage-check.sh a gcovr JSON summary.

The target once built an lcov `.info` file and passed it to a script that reads
gcovr's `--json-summary`; the gate could never pass. Two things are held here:
the recipe (read from `make -n`, no build) and the script's refusal of an input
that is not a gcovr summary.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any, ClassVar

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "ci" / "coverage-check.sh"
MAKE = shutil.which("make")
BASH = shutil.which("bash")
SUBPROCESS_TIMEOUT_S = 60
SCRIPT_TIMEOUT_S = 60


def _summary(overall: float, dnn: float) -> dict[str, Any]:
    return {
        "line_percent": overall,
        "files": [
            {
                "filename": "core/src/dnn/dnn_api.c",
                "line_percent": dnn,
                "line_total": 100,
                "line_covered": int(dnn),
            },
        ],
    }


class CoverageRecipe(unittest.TestCase):
    dry: ClassVar[str]

    @classmethod
    def setUpClass(cls) -> None:
        """Capture make coverage-check dry-run output across test cases in this class."""
        if MAKE is None:
            raise unittest.SkipTest("make not installed")
        cls.dry = subprocess.run(  # noqa: S603 -- fixed make argv, no shell
            [MAKE, "-n", "-C", str(ROOT), "coverage-check"],
            capture_output=True,
            text=True,
            check=True,
            timeout=SUBPROCESS_TIMEOUT_S,
        ).stdout

    def test_recipe_uses_gcovr_json_summary(self) -> None:
        self.assertIn("gcovr", self.dry)
        self.assertIn("--json-summary", self.dry)

    def test_recipe_has_no_lcov(self) -> None:
        self.assertNotIn("lcov", self.dry)
        self.assertNotIn(".info", self.dry)

    def test_check_argument_is_the_json_summary(self) -> None:
        line = next((ln for ln in self.dry.splitlines() if "coverage-check.sh" in ln), "")
        self.assertIn("build-coverage/coverage.json", line)
        summary = self.dry.split("--json-summary", 1)[1].split()[0]
        self.assertEqual(summary, "build-coverage/coverage.json")


class CoverageScript(unittest.TestCase):
    def _run(self, text: str, suffix: str) -> subprocess.CompletedProcess[str]:
        """Run the coverage-check script against temporary coverage output file content."""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / f"cov{suffix}"
            path.write_text(text, encoding="utf-8")
            return subprocess.run(  # noqa: S603 -- fixed bash argv, no shell
                [BASH or "bash", str(SCRIPT), str(path), "37", "85"],
                capture_output=True,
                text=True,
                check=False,
                timeout=SCRIPT_TIMEOUT_S,
            )

    def test_passes_a_gcovr_summary_above_the_floors(self) -> None:
        r = self._run(json.dumps(_summary(50.0, 90.0)), ".json")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_fails_a_summary_below_the_overall_floor(self) -> None:
        r = self._run(json.dumps(_summary(20.0, 90.0)), ".json")
        self.assertEqual(r.returncode, 1)

    def test_fails_a_critical_file_below_the_floor(self) -> None:
        r = self._run(json.dumps(_summary(50.0, 10.0)), ".json")
        self.assertEqual(r.returncode, 1)

    def test_refuses_an_lcov_info_file_by_name(self) -> None:
        lcov = "TN:\nSF:core/src/dnn/dnn_api.c\nDA:1,1\nLF:1\nLH:1\nend_of_record\n"
        r = self._run(lcov, ".info")
        self.assertEqual(r.returncode, 2)
        self.assertIn("not a gcovr --json-summary file", r.stderr)
        self.assertNotIn("Traceback", r.stderr)


if __name__ == "__main__":
    unittest.main()
