#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/ci/scheduled-failure-issue.sh opens or updates a tracking issue for scheduled failures."""

from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "scheduled-failure-issue.sh"

STUB = """#!/usr/bin/env bash
printf '%s\\n' "$*" >> "$STUB_LOG"
case "$1 $2" in
  "api repos/"*)
    reads=$(grep -c '^api repos/' "$STUB_LOG")
    if [ "$reads" -gt "${STUB_EMPTY_READS:-0}" ]; then printf '%s' "$STUB_JOBS"; fi ;;
  "issue list") printf '%s' "$STUB_EXISTING" ;;
  "issue create" | "issue comment") ;;
  *) echo "unexpected: $*" >&2; exit 9 ;;
esac
"""


class ScheduledFailureIssueTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.log = root / "calls.log"
        self.stub = root / "gh"
        self.stub.write_text(STUB, encoding="utf-8")
        self.stub.chmod(self.stub.stat().st_mode | stat.S_IXUSR)

    def run_script(
        self,
        jobs: str,
        existing: str,
        *args: str,
        env_override: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        env = {
            **os.environ,
            "SCHEDULED_FAILURE_GH": str(self.stub),
            "STUB_LOG": str(self.log),
            "STUB_JOBS": jobs,
            "STUB_EXISTING": existing,
            "SCHEDULED_FAILURE_WAIT": "0",
        }
        if env_override:
            env.update(env_override)
        default_args = (
            "VMAFx/vmafx",
            "12345",
            "Upstream watcher failed on schedule",
            "See docs/development/upstream-watchers.md.",
        )
        return subprocess.run(  # noqa: S603 - fixed argv: bash, the script under test
            ["bash", str(SCRIPT), *(args or default_args)],  # noqa: S607
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

    def calls(self) -> list[str]:
        return self.log.read_text(encoding="utf-8").splitlines() if self.log.exists() else []

    def test_creates_the_issue_when_none_is_open(self) -> None:
        result = self.run_script("ffmpeg-av1-videotoolbox\nnetflix-unported", "")
        self.assertEqual(result.returncode, 0, result.stderr)
        creates = [c for c in self.calls() if c.startswith("issue create")]
        self.assertEqual(len(creates), 1)
        self.assertIn("Upstream watcher failed on schedule", creates[0])
        body_log = self.log.read_text(encoding="utf-8")
        self.assertIn("- ffmpeg-av1-videotoolbox", body_log)
        self.assertIn("- netflix-unported", body_log)
        self.assertIn("See docs/development/upstream-watchers.md.", body_log)
        self.assertFalse([c for c in self.calls() if c.startswith("issue comment")])

    def test_comments_on_the_open_issue_instead_of_opening_another(self) -> None:
        result = self.run_script("netflix-unported", "88")
        self.assertEqual(result.returncode, 0, result.stderr)
        comments = [c for c in self.calls() if c.startswith("issue comment 88")]
        self.assertEqual(len(comments), 1)
        self.assertFalse([c for c in self.calls() if c.startswith("issue create")])

    def api_reads(self) -> int:
        return len([c for c in self.calls() if c.startswith("api repos/")])

    def test_a_run_with_no_failed_job_exits_zero_with_a_notice(self) -> None:
        result = self.run_script("", "")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("no failed job", result.stdout)
        self.assertEqual(self.api_reads(), 6)
        self.assertFalse([c for c in self.calls() if c.startswith("issue ")])

    def test_a_failed_job_the_api_lists_late_is_still_reported(self) -> None:
        result = self.run_script("netflix-unported", "", env_override={"STUB_EMPTY_READS": "2"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.api_reads(), 3)
        self.assertEqual(len([c for c in self.calls() if c.startswith("issue create")]), 1)
        self.assertNotIn("no failed job", result.stdout)

    def test_custom_title_appears(self) -> None:
        custom_title = "Custom Scheduled Task Failed"
        result = self.run_script(
            "job-a",
            "",
            "VMAFx/vmafx",
            "999",
            custom_title,
            "Remediation hint",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        creates = [c for c in self.calls() if c.startswith("issue create")]
        self.assertEqual(len(creates), 1)
        self.assertIn(custom_title, creates[0])

    def test_rejects_malformed_arguments(self) -> None:
        test_cases = (
            ("VMAFx", "123", "Title", "Hint"),
            ("VMAFx/vmafx", "12x", "Title", "Hint"),
            ("VMAFx/vmafx", "123", "", "Hint"),
            ("VMAFx/vmafx", "123", "Title", ""),
            ("VMAFx/vmafx", "123"),
            ("VMAFx/vmafx",),
            (),
        )
        for args in test_cases:
            with self.subTest(args=args):
                result = subprocess.run(  # noqa: S603 - fixed argv: bash, the script under test
                    ["bash", str(SCRIPT), *args],  # noqa: S607
                    env={**os.environ, "SCHEDULED_FAILURE_GH": str(self.stub)},
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 2)

    def test_env_fallback_to_tidy_drift_gh(self) -> None:
        # When SCHEDULED_FAILURE_GH is unset, TIDY_DRIFT_GH should be used
        env = {
            **os.environ,
            "SCHEDULED_FAILURE_GH": "",
            "TIDY_DRIFT_GH": str(self.stub),
            "STUB_LOG": str(self.log),
            "STUB_JOBS": "job-1",
            "STUB_EXISTING": "",
        }
        result = subprocess.run(  # noqa: S603 - fixed argv: bash, the script under test
            [  # noqa: S607
                "bash",
                str(SCRIPT),
                "VMAFx/vmafx",
                "123",
                "Title",
                "Hint",
            ],
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(any(c.startswith("issue create") for c in self.calls()))

    def test_a_failed_api_read_fails_closed(self) -> None:
        failing = Path(self.tmp.name) / "gh-fail"
        failing.write_text("#!/usr/bin/env bash\nexit 4\n", encoding="utf-8")
        failing.chmod(failing.stat().st_mode | stat.S_IXUSR)
        result = subprocess.run(  # noqa: S603 - fixed argv: bash, the script under test
            [  # noqa: S607
                "bash",
                str(SCRIPT),
                "VMAFx/vmafx",
                "1",
                "Title",
                "Hint",
            ],
            env={**os.environ, "SCHEDULED_FAILURE_GH": str(failing)},
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot read the jobs", result.stderr)


if __name__ == "__main__":
    unittest.main()
