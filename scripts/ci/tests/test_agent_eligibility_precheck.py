# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Fail-closed controls for the agent eligibility precheck."""

from __future__ import annotations

import importlib.util
import subprocess
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
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
        tracker.search_prs.side_effect = subprocess.CalledProcessError(7, ["gh"])
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


if __name__ == "__main__":
    unittest.main()
