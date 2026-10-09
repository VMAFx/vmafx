#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/ci/tidy-drift-issue.sh opens or updates exactly one issue (ADR-2796)."""

from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "tidy-drift-issue.sh"

# A stand-in for `gh`: it appends its arguments to $STUB_LOG and answers the three
# calls the script makes from $STUB_JOBS (failed job names) and $STUB_EXISTING (an
# open issue number, or empty).
STUB = """#!/usr/bin/env bash
printf '%s\\n' "$*" >> "$STUB_LOG"
case "$1 $2" in
  "api repos/"*) printf '%s' "$STUB_JOBS" ;;
  "issue list") printf '%s' "$STUB_EXISTING" ;;
  "issue create" | "issue comment") ;;
  *) echo "unexpected: $*" >&2; exit 9 ;;
esac
"""


class TidyDriftIssue(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.log = root / "calls.log"
        self.stub = root / "gh"
        self.stub.write_text(STUB, encoding="utf-8")
        self.stub.chmod(self.stub.stat().st_mode | stat.S_IXUSR)

    def run_script(self, jobs: str, existing: str, *args: str) -> subprocess.CompletedProcess[str]:
        env = {
            **os.environ,
            "TIDY_DRIFT_GH": str(self.stub),
            "STUB_LOG": str(self.log),
            "STUB_JOBS": jobs,
            "STUB_EXISTING": existing,
        }
        return subprocess.run(  # noqa: S603 - fixed argv: bash, the script under test, test args
            ["bash", str(SCRIPT), *(args or ("VMAFx/vmafx", "12345"))],  # noqa: S607
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

    def calls(self) -> list[str]:
        return self.log.read_text(encoding="utf-8").splitlines() if self.log.exists() else []

    def test_creates_the_issue_when_none_is_open(self) -> None:
        result = self.run_script("Tidy Lane (cuda)\nTidy Lane (hip)", "")
        self.assertEqual(result.returncode, 0, result.stderr)
        creates = [c for c in self.calls() if c.startswith("issue create")]
        self.assertEqual(len(creates), 1)
        self.assertIn("Tidy ratchet drift on master", creates[0])
        self.assertIn("- Tidy Lane (hip)", self.log.read_text(encoding="utf-8"))
        self.assertFalse([c for c in self.calls() if c.startswith("issue comment")])

    def test_comments_on_the_open_issue_instead_of_opening_another(self) -> None:
        result = self.run_script("Tidy Lane (sycl)", "77")
        self.assertEqual(result.returncode, 0, result.stderr)
        comments = [c for c in self.calls() if c.startswith("issue comment 77")]
        self.assertEqual(len(comments), 1)
        self.assertFalse([c for c in self.calls() if c.startswith("issue create")])

    def test_refuses_a_run_with_no_failed_job_and_writes_nothing(self) -> None:
        result = self.run_script("", "")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no failed job", result.stderr)
        self.assertFalse([c for c in self.calls() if c.startswith("issue ")])

    def test_rejects_malformed_arguments(self) -> None:
        for args in (("VMAFx",), ("VMAFx/vmafx", "12x"), ("VMAFx/vmafx",), ()):
            with self.subTest(args=args):
                result = subprocess.run(  # noqa: S603 - fixed argv: bash, the script under test
                    ["bash", str(SCRIPT), *args],  # noqa: S607
                    env={**os.environ, "TIDY_DRIFT_GH": str(self.stub)},
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 2)

    def test_a_failed_api_read_fails_closed(self) -> None:
        failing = Path(self.tmp.name) / "gh-fail"
        failing.write_text("#!/usr/bin/env bash\nexit 4\n", encoding="utf-8")
        failing.chmod(failing.stat().st_mode | stat.S_IXUSR)
        result = subprocess.run(  # noqa: S603 - fixed argv: bash, the script under test
            ["bash", str(SCRIPT), "VMAFx/vmafx", "1"],  # noqa: S607
            env={**os.environ, "TIDY_DRIFT_GH": str(failing)},
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot read the jobs", result.stderr)


if __name__ == "__main__":
    unittest.main()
