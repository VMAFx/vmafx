#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the lane brief requirements in ``.config/lane-brief.yaml`` (ADR-3553).

The lane launcher (``agy-run.sh`` in the praetor repository) reads that file from the
job's worktree and refuses a brief that lacks a listed line. The shape tests always run.
The replay tests run the launcher's own reader, ``brief_stage.py``, against planted
briefs. The reader is found through ``LANE_BRIEF_STAGE_PY`` or ``LANE_TOOLS_DIR``, the
same variables the launcher honours; without either the replay tests skip and say why.
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from scripts.ci import run_affected_suites as runner  # noqa: E402
from scripts.ci import suite_registry as registry_lib  # noqa: E402
from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

CONFIG = REPO / ".config" / "lane-brief.yaml"
ALLOWED_KEYS = frozenset({"required", "required_by_stage", "credential_stub_tests"})
STAGES = frozenset({"impl", "fix", "review", "audit", "research"})
REVIEW_CLAUSES = ("Write nothing in the worktree", "return: final message only")
MAX_ENTRIES = 32
MAX_ENTRY_CHARS = 200
READER_TIMEOUT_SECONDS = 60
ROUTER_STUB = "tiers:\n  test:\n    target_tasks: [review, feature_implementation]\n"
SKIP_REASON = (
    "brief_stage.py is absent: set LANE_BRIEF_STAGE_PY or LANE_TOOLS_DIR to the praetor "
    "tools/lanes reader (CI has no praetor clone); alternate coverage: the shape tests in "
    "this file and the host replay in the pull request evidence"
)

# The PR 3470 review brief shape: it asked for a report file in the worktree, so the lane
# guard ended the job with rc 79.
UNSAFE_REVIEW_BRIEF = (
    "task: review\n"
    "order: planted-review-without-clause\n"
    "\n"
    "Review the pull request head. Write the report to lane-report.md in the worktree\n"
    "and keep scratch files under lane-scratch/.\n"
)
SAFE_REVIEW_BRIEF = (
    "task: review\n"
    "order: planted-review-with-clause\n"
    "\n"
    "- Read only. Write nothing in the worktree: no tracked edit, no untracked file.\n"
    "return: final message only. Put the report in the final message.\n"
)
IMPL_BRIEF = (
    "task: feature_implementation\n"
    "order: planted-impl\n"
    "\n"
    "Change the parser and add a test; the dispatcher commits.\n"
)
NO_TASK_BRIEF = "order: planted-no-task\n\nReview the pull request head.\n"


def load_config(path: Path = CONFIG) -> dict[str, Any]:
    """Parse the config file; the reader accepts a mapping at the top level only."""
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise TypeError(f"{path}: top level must be a mapping")
    return data


def find_reader() -> Path | None:
    """Locate brief_stage.py the way the launcher does: explicit file first, then tools dir."""
    explicit = os.environ.get("LANE_BRIEF_STAGE_PY", "")
    if explicit and Path(explicit).is_file():
        return Path(explicit)
    tools_dir = os.environ.get("LANE_TOOLS_DIR", "")
    if tools_dir and (Path(tools_dir) / "brief_stage.py").is_file():
        return Path(tools_dir) / "brief_stage.py"
    return None


class LaneBriefConfigShapeTest(unittest.TestCase):
    """The file parses and stays inside the reader's contract."""

    def test_keys_are_reader_keys(self) -> None:
        data = load_config()
        self.assertTrue(set(data) <= ALLOWED_KEYS, sorted(set(data) - ALLOWED_KEYS))

    def test_review_stage_holds_both_clauses(self) -> None:
        review = load_config()["required_by_stage"]["review"]
        for clause in REVIEW_CLAUSES:
            self.assertIn(clause, review)

    def test_stage_lists_fit_reader_limits(self) -> None:
        by_stage = load_config()["required_by_stage"]
        self.assertTrue(set(by_stage) <= STAGES, sorted(set(by_stage) - STAGES))
        for stage, entries in by_stage.items():
            self.assertIsInstance(entries, list, stage)
            self.assertLessEqual(len(entries), MAX_ENTRIES, stage)
            for entry in entries:
                self.assertIsInstance(entry, str, stage)
                self.assertTrue(0 < len(entry) <= MAX_ENTRY_CHARS, entry)

    def test_config_change_runs_this_file(self) -> None:
        # A change to the config alone must select the suite that holds this file.
        registry = registry_lib.load_registry(REPO)
        affected = runner.affected_suites(registry, [".config/lane-brief.yaml"])
        self.assertIn("tooling", affected)
        this_file = Path(__file__).resolve().relative_to(REPO).as_posix()
        self.assertEqual(registry_lib.suite_files(registry, [this_file], "tooling"), [this_file])

    def test_unrelated_config_change_selects_no_suite(self) -> None:
        registry = registry_lib.load_registry(REPO)
        self.assertEqual(runner.affected_suites(registry, [".config/no-such-file.yaml"]), {})

    def test_shape_check_rejects_a_list_at_top_level(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "lane-brief.yaml"
            bad.write_text("- review\n", encoding="utf-8")
            with self.assertRaises(TypeError):
                load_config(bad)


@unittest.skipIf(find_reader() is None, SKIP_REASON)
class LaneBriefReplayTest(unittest.TestCase):
    """Planted briefs through the launcher's reader against this repository's config."""

    def setUp(self) -> None:
        reader = find_reader()
        if reader is None:
            self.skipTest(SKIP_REASON)
        self.reader = reader
        self.serial = 0
        self.tmp = Path(tempfile.mkdtemp(prefix="lane-brief-test-"))
        self.router = self.tmp / "routing.yaml"
        self.router.write_text(ROUTER_STUB, encoding="utf-8")

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def worktree(self, config_text: str) -> Path:
        """A scratch worktree holding only the config under test."""
        self.serial += 1
        root = self.tmp / f"wt{self.serial}"
        (root / ".config").mkdir(parents=True)
        (root / ".config" / "lane-brief.yaml").write_text(config_text, encoding="utf-8")
        return root

    def required(self, brief_text: str, worktree: Path) -> TextCommandResult:
        self.serial += 1
        brief = self.tmp / f"brief{self.serial}.md"
        brief.write_text(brief_text, encoding="utf-8")
        env = dict(os.environ, LANE_ROUTER_CONFIG=str(self.router))
        return run_command(
            [sys.executable, "-B", str(self.reader), "required", str(brief), str(worktree)],
            allowed_executables=(sys.executable,),
            capture_output=True,
            text=True,
            env=env,
            timeout_seconds=READER_TIMEOUT_SECONDS,
            check=False,
        )

    def repo_worktree(self) -> Path:
        return self.worktree(CONFIG.read_text(encoding="utf-8"))

    def test_review_brief_without_clause_is_refused(self) -> None:
        result = self.required(UNSAFE_REVIEW_BRIEF, self.repo_worktree())
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        for clause in REVIEW_CLAUSES:
            self.assertIn(clause, result.stdout)

    def test_review_brief_with_clause_passes(self) -> None:
        result = self.required(SAFE_REVIEW_BRIEF, self.repo_worktree())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_impl_brief_needs_no_review_clause(self) -> None:
        result = self.required(IMPL_BRIEF, self.repo_worktree())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_brief_without_task_line_is_invalid(self) -> None:
        result = self.required(NO_TASK_BRIEF, self.repo_worktree())
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("cannot resolve brief stage", result.stdout)

    def test_planted_brief_passes_without_the_review_clause(self) -> None:
        # Mutation: drop the review stage and the unsafe brief launches again, so the
        # refusal above comes from this file and not from the reader alone.
        data = load_config()
        del data["required_by_stage"]["review"]
        mutant = self.worktree(yaml.safe_dump(data, sort_keys=True))
        result = self.required(UNSAFE_REVIEW_BRIEF, mutant)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
