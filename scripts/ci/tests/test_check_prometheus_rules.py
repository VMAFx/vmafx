#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for scripts/ci/check-prometheus-rules.sh (ADR-2349).

promtool is replaced by a stand-in through PROMTOOL, so the cases run
offline; the go-ci workflow runs the pinned release.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "ci" / "check-prometheus-rules.sh"
BASH = shutil.which("bash") or "/bin/bash"

# A stand-in promtool: records its argv and working directory, fails the
# subcommand named in FAKE_FAIL.
FAKE = """#!/bin/sh
echo "$PWD $*" >> "$FAKE_LOG"
[ "$1 $2" = "$FAKE_FAIL" ] && exit 1
exit 0
"""


class CheckPrometheusRulesTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.fake = self.dir / "promtool"
        self.fake.write_text(FAKE, encoding="utf-8")
        self.fake.chmod(0o700)
        self.log = self.dir / "calls.log"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_script(self, fail: str = "") -> subprocess.CompletedProcess[str]:
        env = {
            **os.environ,
            "PROMTOOL": str(self.fake),
            "FAKE_LOG": str(self.log),
            "FAKE_FAIL": fail,
        }
        return subprocess.run(  # noqa: S603 -- fixed bash executable and script argv
            [BASH, str(SCRIPT)], env=env, capture_output=True, text=True, check=False, timeout=60
        )

    def test_checks_the_rule_file_and_runs_its_tests_from_its_directory(self) -> None:
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.log.read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(calls), 2, calls)
        self.assertTrue(
            calls[0].endswith("check rules " + str(ROOT / "deploy/prometheus/vmafx-rules.yaml")),
            calls,
        )
        self.assertTrue(
            calls[1].startswith(
                str(ROOT / "deploy/prometheus") + " test rules vmafx-rules.test.yaml"
            ),
            calls,
        )

    def test_a_failing_check_fails_the_run_and_the_tests_still_run(self) -> None:
        result = self.run_script(fail="check rules")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(len(self.log.read_text(encoding="utf-8").splitlines()), 2)

    def test_a_failing_unit_test_fails_the_run(self) -> None:
        self.assertEqual(self.run_script(fail="test rules").returncode, 1)


if __name__ == "__main__":
    unittest.main()
