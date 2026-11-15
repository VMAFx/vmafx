#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the lane brief requirements in ``.config/lane-brief.yaml`` (ADR-3553).

The lane launcher (``agy-run.sh`` in the praetor repository) reads that file from the
job's worktree and refuses a brief that lacks a listed line. The shape tests always run.
The replay tests run the launcher's own reader, ``brief_stage.py``, against planted
briefs. The reader is found through ``LANE_BRIEF_STAGE_PY`` or ``LANE_TOOLS_DIR``, the
same variables the launcher honours. Without either the replay tests skip and say why; a
variable that names a missing file fails them instead, so a wrong path cannot pass as a skip.
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

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
MAX_STAGES = 8
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
FILLER = "".join(f"note {n}\n" for n in range(1, 20))  # 19 lines before the task line


def load_config(path: Path = CONFIG) -> dict[str, Any]:
    """Parse the config file; the reader accepts a mapping at the top level only."""
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise TypeError(f"{path}: top level must be a mapping")
    return data


def entry_errors(entries: object, key: str) -> list[str]:
    """The reader's list contract: a list of at most 32 strings of 1 to 200 characters."""
    if not isinstance(entries, list):
        return [f"{key} must be a list"]
    if len(entries) > MAX_ENTRIES:
        return [f"{key} holds more than {MAX_ENTRIES} entries"]
    return [
        f"{key} entry {index} must be a string of 1 to {MAX_ENTRY_CHARS} characters"
        for index, entry in enumerate(entries)
        if not isinstance(entry, str) or not 0 < len(entry) <= MAX_ENTRY_CHARS
    ]


def shape_errors(data: object) -> list[str]:
    """What the reader refuses with rc 2 (load_lane_brief, required_for_stage); empty = valid."""
    if not isinstance(data, dict):
        return ["top level must be a mapping"]
    errors = [f"unknown top-level key {key}" for key in sorted(set(data) - ALLOWED_KEYS)]
    if "required" in data:
        errors += entry_errors(data["required"], "required")
    by_stage = data.get("required_by_stage", {})
    if not isinstance(by_stage, dict):
        return [*errors, "required_by_stage must be a mapping"]
    errors += [f"unknown stage {stage}" for stage in sorted(set(by_stage) - STAGES)]
    for stage, entries in list(by_stage.items())[:MAX_STAGES]:  # HISS-02 bound
        errors += entry_errors(entries, f"required_by_stage[{stage}]")
    return errors


def review_config(entries: list[str]) -> str:
    """A config text whose review stage lists ``entries``."""
    return str(yaml.safe_dump({"required_by_stage": {"review": entries}}, sort_keys=True))


def limit_entries(count: int, chars: int) -> list[str]:
    """``count`` distinct entries of ``chars`` characters each (``chars`` >= 2)."""
    return [f"{index:02d}".ljust(chars, "x") for index in range(count)]


def find_reader() -> Path | None:
    """Locate brief_stage.py through the launcher's variables: explicit file, then tools dir.

    A set variable wins even when its path is missing, so the replay tests fail on it; only
    when neither variable is set (or both are empty) is there no reader to replay against.
    """
    explicit = os.environ.get("LANE_BRIEF_STAGE_PY", "")
    if explicit:
        return Path(explicit)
    tools_dir = os.environ.get("LANE_TOOLS_DIR", "")
    if tools_dir:
        return Path(tools_dir) / "brief_stage.py"
    return None


class LaneBriefConfigShapeTest(unittest.TestCase):
    """The file parses and stays inside the reader's contract."""

    def test_repo_config_fits_reader_contract(self) -> None:
        self.assertEqual(shape_errors(load_config()), [])

    def test_review_stage_holds_both_clauses(self) -> None:
        review = load_config()["required_by_stage"]["review"]
        for clause in REVIEW_CLAUSES:
            self.assertIn(clause, review)

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

    def test_sibling_of_the_config_selects_no_suite(self) -> None:
        # The routing entry is an exact path, not a prefix.
        registry = registry_lib.load_registry(REPO)
        siblings = [".config/lane-brief.yaml.orig", ".config/lane-brief.yml"]
        self.assertEqual(runner.affected_suites(registry, siblings), {})

    def test_shape_check_rejects_a_list_at_top_level(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bad = Path(tmp) / "lane-brief.yaml"
            bad.write_text("- review\n", encoding="utf-8", newline="\n")
            with self.assertRaises(TypeError):
                load_config(bad)


class LaneBriefShapeBoundaryTest(unittest.TestCase):
    """The shape check accepts each reader limit and refuses one step past it."""

    def test_limits_are_inclusive(self) -> None:
        for entries in (limit_entries(MAX_ENTRIES, MAX_ENTRY_CHARS), ["x"]):
            data = {"required_by_stage": {"review": entries}}
            self.assertEqual(shape_errors(data), [], len(entries))
        self.assertEqual(shape_errors({}), [])

    def test_one_past_each_limit_is_an_error(self) -> None:
        cases = (
            limit_entries(MAX_ENTRIES + 1, 10),
            limit_entries(1, MAX_ENTRY_CHARS + 1),
            [""],
            [5],
        )
        for entries in cases:
            data = {"required_by_stage": {"review": entries}}
            self.assertNotEqual(shape_errors(data), [], entries[:1])

    def test_unknown_key_stage_or_type_is_an_error(self) -> None:
        cases: tuple[object, ...] = (
            {"requires": ["x"]},
            {"required_by_stage": {"reviews": ["x"]}},
            {"required_by_stage": None},
            {"required": "x"},
            ["review"],
        )
        for data in cases:
            self.assertNotEqual(shape_errors(data), [], data)


class ReaderLookupTest(unittest.TestCase):
    """find_reader skips only when no variable names a reader."""

    def lookup(self, **env: str) -> Path | None:
        with mock.patch.dict(os.environ, env, clear=True):
            return find_reader()

    def test_no_variable_means_no_reader(self) -> None:
        self.assertIsNone(self.lookup())

    def test_empty_variables_count_as_unset(self) -> None:
        self.assertIsNone(self.lookup(LANE_BRIEF_STAGE_PY="", LANE_TOOLS_DIR=""))

    def test_missing_explicit_path_is_kept_so_replay_fails(self) -> None:
        missing = "/nonexistent/lane-tools/brief_stage.py"
        self.assertEqual(self.lookup(LANE_BRIEF_STAGE_PY=missing), Path(missing))

    def test_explicit_file_wins_over_tools_dir(self) -> None:
        found = self.lookup(LANE_BRIEF_STAGE_PY="/a/brief_stage.py", LANE_TOOLS_DIR="/b")
        self.assertEqual(found, Path("/a/brief_stage.py"))
        self.assertEqual(self.lookup(LANE_TOOLS_DIR="/b"), Path("/b/brief_stage.py"))


@unittest.skipIf(find_reader() is None, SKIP_REASON)
class LaneBriefReplayTest(unittest.TestCase):
    """Planted briefs through the launcher's reader against this repository's config."""

    def setUp(self) -> None:
        reader = find_reader()
        if reader is None:
            self.skipTest(SKIP_REASON)
        if not reader.is_file():
            self.fail(f"{reader}: named by LANE_BRIEF_STAGE_PY or LANE_TOOLS_DIR, not a file")
        self.reader = reader
        self.serial = 0
        self.tmp = Path(tempfile.mkdtemp(prefix="lane-brief-test-"))
        self.router = self.tmp / "routing.yaml"
        self.router.write_text(ROUTER_STUB, encoding="utf-8", newline="\n")

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def worktree(self, config_text: str) -> Path:
        """A scratch worktree holding only the config under test."""
        self.serial += 1
        root = self.tmp / f"wt{self.serial}"
        (root / ".config").mkdir(parents=True)
        (root / ".config" / "lane-brief.yaml").write_text(
            config_text, encoding="utf-8", newline="\n"
        )
        return root

    def required(self, brief_text: str, worktree: Path) -> TextCommandResult:
        self.serial += 1
        brief = self.tmp / f"brief{self.serial}.md"
        brief.write_text(brief_text, encoding="utf-8", newline="\n")
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

    def test_brief_with_one_clause_names_only_the_other(self) -> None:
        brief = "task: review\n\nWrite nothing in the worktree.\n"
        result = self.required(brief, self.repo_worktree())
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "return: final message only")

    def test_task_line_counts_on_line_20_only(self) -> None:
        on_20 = self.required(FILLER + UNSAFE_REVIEW_BRIEF, self.repo_worktree())
        self.assertEqual(on_20.returncode, 1, on_20.stdout + on_20.stderr)
        on_21 = self.required(FILLER + "note 20\n" + UNSAFE_REVIEW_BRIEF, self.repo_worktree())
        self.assertEqual(on_21.returncode, 2, on_21.stdout + on_21.stderr)
        self.assertIn("cannot resolve brief stage", on_21.stdout)

    def test_reader_accepts_the_limits_the_shape_check_accepts(self) -> None:
        entries = limit_entries(MAX_ENTRIES, MAX_ENTRY_CHARS)
        result = self.required(SAFE_REVIEW_BRIEF, self.worktree(review_config(entries)))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), MAX_ENTRIES)

    def test_reader_refuses_one_past_each_limit(self) -> None:
        cases = (
            (limit_entries(MAX_ENTRIES + 1, 10), "at most 32 entries"),
            (limit_entries(1, MAX_ENTRY_CHARS + 1), "up to 200 characters"),
        )
        for entries, reason in cases:
            result = self.required(SAFE_REVIEW_BRIEF, self.worktree(review_config(entries)))
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn(reason, result.stdout)

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
