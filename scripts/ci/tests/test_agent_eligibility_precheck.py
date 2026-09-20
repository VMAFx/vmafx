# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Fail-closed controls for the agent eligibility precheck."""

from __future__ import annotations

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import CommandFailed, CommandResult  # noqa: E402
from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

SCRIPT = ROOT / "scripts/ci/agent-eligibility-precheck.py"
PYTHON = str(Path(sys.executable).resolve(strict=True))
SPEC = importlib.util.spec_from_file_location(
    "agent_eligibility_precheck", ROOT / "scripts/ci/agent-eligibility-precheck.py"
)
assert SPEC is not None and SPEC.loader is not None
PRECHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PRECHECK)


class _MissingBacklogItem:
    @staticmethod
    def get(_backlog_id: str) -> None:
        return None


class AgentEligibilityPrecheckTests(unittest.TestCase):
    def test_explicit_offline_task_tag_passes(self) -> None:
        code = PRECHECK.main(["--task-tag", "unit-scope", "--skip-gh-search", "--skip-active-scan"])
        self.assertEqual(code, 0)

    def test_missing_backlog_row_blocks_dispatch(self) -> None:
        self.assertFalse(PRECHECK.check_backlog_row_open("T-NOT-THERE", _MissingBacklogItem()))

    def test_missing_gh_blocks_active_branch_check(self) -> None:
        with mock.patch.object(PRECHECK.shutil, "which", return_value=None):
            code = PRECHECK.main(["--task-tag", "unit-scope", "--skip-gh-search"])
        self.assertEqual(code, 1)

    def test_failed_gh_query_blocks_merged_pr_check(self) -> None:
        tracker = mock.Mock()
        tracker.search_prs.side_effect = CommandFailed(
            CommandResult(argv=("gh",), returncode=7, stdout="", stderr="")
        )
        with mock.patch.object(PRECHECK.shutil, "which", return_value="/usr/bin/gh"):
            self.assertFalse(PRECHECK.check_no_merged_pr("T3-9", tracker))

    def test_scope_token_does_not_match_a_longer_backlog_id(self) -> None:
        with TemporaryDirectory() as temp_name:
            task = Path(temp_name) / "task.output"
            task.write_text("work on T3-90", encoding="utf-8")
            self.assertTrue(
                PRECHECK.check_no_active_agent("T3-9", tasks_glob=str(task), open_branches=[])
            )

    def test_unreadable_task_blocks_dispatch(self) -> None:
        task = Path("unreadable.output")
        with (
            mock.patch.object(PRECHECK, "_glob_paths", return_value=[task]),
            mock.patch.object(Path, "read_text", side_effect=OSError("denied")),
        ):
            self.assertFalse(
                PRECHECK.check_no_active_agent("T3-9", tasks_glob="ignored", open_branches=[])
            )


class AgentEligibilityPrecheckCliTests(unittest.TestCase):
    """End-to-end runs of the CLI against a `gh` that exits non-zero.

    The in-process tests above patch the tracker; these drive the real
    argv path, so they also cover the ``sys.path`` bootstrap and the
    exception type the bounded-subprocess wrapper actually raises.

    A failed `gh` query must BLOCK dispatch. The precheck exists to stop a
    second agent starting work that is already in flight, so a lookup that
    could not run is not evidence that nothing is in flight -- reporting it
    as clear is the gate-that-did-not-run HISS-18 forbids.
    """

    def run_precheck(self, *arguments: str) -> TextCommandResult:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            fake_gh = fake_bin / "gh"
            fake_gh.write_text("#!/bin/sh\nexit 7\n")
            fake_gh.chmod(0o755)
            environment = dict(os.environ)
            environment["PATH"] = os.pathsep.join((str(fake_bin), environment.get("PATH", "")))
            return run_command(
                [PYTHON, str(SCRIPT), *arguments],
                allowed_executables=(PYTHON,),
                cwd=root,
                env=environment,
                capture_output=True,
                text=True,
                timeout_seconds=10,
            )

    def test_merged_pr_lookup_failure_blocks_dispatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            backlog = Path(temporary) / "BACKLOG.md"
            backlog.write_text("| **T9-99** | pending fixture |\n")
            result = self.run_precheck(
                "--backlog-id",
                "T9-99",
                "--backlog-path",
                str(backlog),
                "--skip-active-scan",
            )

        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("agent-eligibility: merged-PR check failed", result.stderr)
        self.assertIn("gh search exited 7", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_open_branch_lookup_failure_blocks_dispatch(self) -> None:
        result = self.run_precheck(
            "--task-tag",
            "offline-fixture",
            "--harness-tasks-glob",
            "/definitely/missing/*.output",
        )

        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("agent-eligibility: open-branch check failed", result.stderr)
        self.assertIn("gh branch listing exited 7", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
