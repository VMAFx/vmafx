#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pre-Commit CI scope: pull requests check their diff, pushes the tree (HISS-18).

Cases: the decision function (positive, negative, boundary), the real
``lint-and-format.yml`` (the job calls the script and keeps no literal
``--all-files``), the routing of a docs-only pull request (no build matrix leg)
against a master push (full), and a planted regression that restores the
unconditional full-tree run and must be refused. A pull request that changes a
hook input (the config, a local hook's entry script) gets the whole tree: the
file list of #3237, which added ``check-fork-python-subprocess`` and passed on
its diff before the master push failed, is the planted case.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import ci_router as wr
from scripts.ci import precommit_scope

ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = ROOT / ".github" / "workflows" / "lint-and-format.yml"
CONFIG = ROOT / ".github" / "ci-tier.json"
BASE = "a" * 40
OWN_PR = wr.SyntheticEvent(
    "pull_request",
    "synchronize",
    ref="refs/pull/1/merge",
    head_ref="docs/example",
    head_repo="VMAFx/vmafx",
)
MASTER_PUSH = wr.SyntheticEvent("push", ref="refs/heads/master")
FULL_ONLY_LEG = "Ubuntu gcc"
INPUTS = precommit_scope.hook_inputs((ROOT / ".pre-commit-config.yaml").read_text("utf-8"))
# The files #3237 changed that the hooks read (subset of its 30-file list).
PR_3237 = (
    ".pre-commit-config.yaml",
    "scripts/ci/check_fork_python_subprocess.py",
    "python/test/hdr_models_pin_test.py",
)


def precommit_run_lines(workflow_text: str) -> list[str]:
    """The `run:` bodies of the steps of the Pre-Commit job."""
    jobs = yaml.safe_load(workflow_text)["jobs"]
    return [str(s["run"]) for s in jobs["pre-commit"]["steps"] if "run" in s]


def scoped(workflow_text: str) -> bool:
    """True when the Pre-Commit job never runs pre-commit with a literal whole-tree flag."""
    runs = precommit_run_lines(workflow_text)
    calls_script = any("precommit_scope.py" in r for r in runs)
    literal_full = any("pre-commit run" in r and "--all-files" in r for r in runs)
    return calls_script and not literal_full


class ScopeDecision(unittest.TestCase):
    def test_pull_request_uses_the_diff(self) -> None:
        args, reason = precommit_scope.scope("pull_request", BASE, [])
        self.assertEqual(args, ["--from-ref", BASE, "--to-ref", "HEAD"])
        self.assertIn("pull request", reason)

    def test_pull_request_target_uses_the_diff(self) -> None:
        args, _ = precommit_scope.scope("pull_request_target", BASE, [])
        self.assertNotIn("--all-files", args)

    def test_non_pull_request_events_use_the_tree(self) -> None:
        for event in ("push", "schedule", "workflow_dispatch", ""):
            with self.subTest(event=event):
                args, _ = precommit_scope.scope(event, BASE)
                self.assertEqual(args, ["--all-files"])

    def test_missing_or_malformed_base_fails_closed(self) -> None:
        for base in ("", "abc", "g" * 40, BASE + "0", "refs/heads/master"):
            with self.subTest(base=base):
                args, _ = precommit_scope.scope("pull_request", base)
                self.assertEqual(args, ["--all-files"])

    def test_hook_input_change_uses_the_tree(self) -> None:
        args, reason = precommit_scope.scope("pull_request", BASE, PR_3237, INPUTS)
        self.assertEqual(args, ["--all-files"])
        self.assertIn("check_fork_python_subprocess.py", reason)

    def test_entry_script_alone_uses_the_tree(self) -> None:
        changed = ["scripts/ci/check_fork_python_subprocess.py"]
        args, _ = precommit_scope.scope("pull_request", BASE, changed, INPUTS)
        self.assertEqual(args, ["--all-files"])

    def test_other_change_keeps_the_diff(self) -> None:
        changed = ["docs/index.md", "python/test/hdr_models_pin_test.py"]
        args, _ = precommit_scope.scope("pull_request", BASE, changed, INPUTS)
        self.assertEqual(args, ["--from-ref", BASE, "--to-ref", "HEAD"])

    def test_unknown_file_list_fails_closed(self) -> None:
        args, _ = precommit_scope.scope("pull_request", BASE, None, INPUTS)
        self.assertEqual(args, ["--all-files"])

    def test_planted_diff_only_scope_is_refused(self) -> None:
        # The pre-fix decision knew no hook inputs: #3237's list then got the diff only.
        self.assertEqual(
            precommit_scope.scope("pull_request", BASE, PR_3237, ())[0],
            ["--from-ref", BASE, "--to-ref", "HEAD"],
        )

    def test_inputs_name_local_entry_scripts_only(self) -> None:
        self.assertIn(".pre-commit-config.yaml", INPUTS)
        self.assertIn("scripts/ci/check_fork_python_subprocess.py", INPUTS)
        self.assertFalse(any(path.startswith(("python3", "semgrep")) for path in INPUTS))
        planted = (
            "repos:\n- repo: https://example.invalid/x\n  hooks:\n"
            "  - id: x\n    entry: scripts/ci/check_fork_python_subprocess.py\n"
        )
        self.assertEqual(precommit_scope.hook_inputs(planted, ROOT), {".pre-commit-config.yaml"})

    def test_print_mode_runs_nothing(self) -> None:
        self.assertEqual(
            precommit_scope.main(["--event", "push", "--print"]),
            0,
        )


class WorkflowRouting(unittest.TestCase):
    text = WORKFLOW.read_text(encoding="utf-8")

    def test_job_calls_the_scope_script(self) -> None:
        self.assertTrue(scoped(self.text))

    def test_planted_unconditional_full_run_is_refused(self) -> None:
        # The pre-HISS-18 step: whole tree on every event. The check must fail on it.
        old = (
            "jobs:\n  pre-commit:\n    steps:\n"
            "      - run: pre-commit run --show-diff-on-failure --all-files\n"
        )
        self.assertFalse(scoped(old))

    def test_pre_commit_context_still_reports_on_a_pull_request(self) -> None:
        workflows = wr.load_workflows(ROOT / ".github" / "workflows")
        _, runs = wr.simulate(workflows, OWN_PR, CONFIG)
        self.assertIn("Pre-Commit", wr.ran_names(runs))

    def test_docs_only_pull_request_runs_no_build_matrix_leg(self) -> None:
        workflows = wr.load_workflows(ROOT / ".github" / "workflows")
        _, pr_runs = wr.simulate(workflows, OWN_PR, CONFIG)
        _, push_runs = wr.simulate(workflows, MASTER_PUSH, CONFIG)
        self.assertNotIn(FULL_ONLY_LEG, wr.ran_names(pr_runs))
        self.assertIn(FULL_ONLY_LEG, wr.ran_names(push_runs))
        self.assertIn("Pre-Commit", wr.ran_names(push_runs))


if __name__ == "__main__":
    unittest.main()
