#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for scripts/ci/lint-dashboards.sh (ADR-2349).

The linter is replaced by a stand-in through DASHBOARD_LINTER, so the cases
run offline; the go-ci workflow runs the pinned release binary.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "ci" / "lint-dashboards.sh"
BASH = shutil.which("bash") or "/bin/bash"

# A stand-in linter: fails a file whose name contains "bad", records its argv.
FAKE = """#!/bin/sh
echo "$@" >> "$FAKE_LOG"
case "$3" in *bad*) echo "error: not clean"; exit 1;; esac
exit 0
"""


class LintDashboardsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.fake = self.dir / "dashboard-linter"
        self.fake.write_text(FAKE, encoding="utf-8")
        self.fake.chmod(0o700)
        self.log = self.dir / "calls.log"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_script(self, *args: str) -> subprocess.CompletedProcess[str]:
        env = {**os.environ, "DASHBOARD_LINTER": str(self.fake), "FAKE_LOG": str(self.log)}
        return subprocess.run(  # noqa: S603 -- fixed bash executable and script argv
            [BASH, str(SCRIPT), *args],
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout=60,
        )

    def test_every_shipped_dashboard_is_linted_strictly(self) -> None:
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.log.read_text(encoding="utf-8").splitlines()
        shipped = sorted((ROOT / "deploy" / "grafana" / "dashboards").glob("*.json"))
        self.assertEqual(len(calls), len(shipped))
        self.assertTrue(all(c.startswith("lint --strict ") for c in calls), calls)
        self.assertIn("not the pinned", result.stderr)

    def test_a_finding_fails_the_run_and_the_others_still_run(self) -> None:
        good, bad = self.dir / "good.json", self.dir / "bad.json"
        good.write_text("{}", encoding="utf-8")
        bad.write_text("{}", encoding="utf-8")
        result = self.run_script(str(bad), str(good))
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL", result.stderr)
        self.assertEqual(len(self.log.read_text(encoding="utf-8").splitlines()), 2)

    def test_the_pins_are_present(self) -> None:
        text = (ROOT / "build-config.env").read_text(encoding="utf-8")
        self.assertRegex(text, r'(?m)^DASHBOARD_LINTER_VERSION="v\d+\.\d+\.\d+"$')
        self.assertRegex(text, r'(?m)^DASHBOARD_LINTER_LINUX_AMD64_SHA256="[0-9a-f]{64}"$')


if __name__ == "__main__":
    unittest.main()
