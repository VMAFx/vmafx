#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for repository targeting in upstream-watcher and related workflows."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]  # PyYAML ships no stubs

WORKFLOWS_DIR = Path(__file__).resolve().parents[3] / ".github" / "workflows"


PY_CALL = r'\[\s*["\']gh["\']\s*,\s*["\'](?:issue|pr|label|release)["\'][^\]]*\]'
SH_CALL = r"(?:^|[\s;&|(`])gh\s+(?:issue|pr|label|release)\b([^;&|\n`]*)"
HELPERS = ("ensure_label.sh", "scheduled-failure-issue.sh")


def _step_runs(job: dict[str, object]) -> list[str]:
    steps = job.get("steps")
    if not isinstance(steps, list):
        return []
    runs = [step.get("run") for step in steps if isinstance(step, dict)]
    return [run for run in runs if isinstance(run, str)]


def _calls_gh(run: str) -> bool:
    if any(helper in run for helper in HELPERS):
        return True
    return bool(re.search(PY_CALL, run) or re.search(SH_CALL, run))


def _check_calls_name_repo(run: str) -> None:
    for match in re.finditer(PY_CALL, run):
        if not re.search(r"""["']--repo["']""", match.group(0)):
            raise ValueError(f"Python gh call missing '--repo': {match.group(0)}")
    for match in re.finditer(SH_CALL, re.sub(r"\\\s*\n", " ", run)):
        if "--repo" not in match.group(1):
            raise ValueError(f"Shell gh call missing '--repo': {match.group(0)}")


def check_workflow_gh_repo(text: str) -> None:
    """Refuse a job that calls gh without job-level GH_REPO, or a call without --repo."""
    jobs = yaml.safe_load(text)["jobs"]
    for job_name, job in jobs.items():
        runs = [run for run in _step_runs(job) if _calls_gh(run)]
        env = job.get("env")
        if runs and (not isinstance(env, dict) or "GH_REPO" not in env):
            raise ValueError(f"Job '{job_name}' calls gh without GH_REPO in its env")
        for run in runs:
            _check_calls_name_repo(run)


class UpstreamWatcherGhRepoTest(unittest.TestCase):
    def test_step_with_gh_issue_create_and_no_repo_refused(self) -> None:
        workflow = """
jobs:
  alert:
    runs-on: ubuntu-26.04
    env:
      GH_REPO: ${{ github.repository }}
    steps:
      - name: Open issue
        run: |
          gh issue create --title "Broken" --body "Details"
"""
        with self.assertRaises(ValueError) as ctx:
            check_workflow_gh_repo(workflow)
        self.assertIn("Shell gh call missing '--repo'", str(ctx.exception))

    def test_no_gh_repo_in_env_refused(self) -> None:
        workflow = """
jobs:
  alert:
    runs-on: ubuntu-26.04
    env:
      GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
    steps:
      - name: Open issue
        run: |
          gh issue create --repo "$GITHUB_REPOSITORY" --title "Broken"
"""
        with self.assertRaises(ValueError) as ctx:
            check_workflow_gh_repo(workflow)
        self.assertIn("calls gh without GH_REPO in its env", str(ctx.exception))

    def test_python_list_form_without_repo_refused(self) -> None:
        workflow = """
jobs:
  alert:
    runs-on: ubuntu-26.04
    env:
      GH_REPO: ${{ github.repository }}
    steps:
      - name: Open issue Python
        run: |
          python3 - <<'EOF'
          cmd = ["gh", "issue", "create", "--title", "Bug"]
          EOF
"""
        with self.assertRaises(ValueError) as ctx:
            check_workflow_gh_repo(workflow)
        self.assertIn("Python gh call missing '--repo'", str(ctx.exception))

    def test_valid_synthetic_workflow_passes(self) -> None:
        workflow = """
jobs:
  alert:
    runs-on: ubuntu-26.04
    env:
      GH_REPO: ${{ github.repository }}
    steps:
      - name: Shell step
        run: |
          gh issue list --repo "$GITHUB_REPOSITORY" --state open
          gh issue create --repo "$GITHUB_REPOSITORY" --title "T" --body "B"
      - name: Python step
        run: |
          cmd = [
              "gh", "issue", "create",
              "--repo", os.environ["GITHUB_REPOSITORY"],
              "--title", "T",
          ]
"""
        check_workflow_gh_repo(workflow)

    def test_real_workflow_passes(self) -> None:
        workflow_path = WORKFLOWS_DIR / "upstream-watcher.yml"
        text = workflow_path.read_text(encoding="utf-8")
        check_workflow_gh_repo(text)

    def test_qualifying_workflows_pass(self) -> None:
        files = sorted(WORKFLOWS_DIR.glob("*.yml")) + sorted(WORKFLOWS_DIR.glob("*.yaml"))
        checked = 0
        for path in files:
            text = path.read_text(encoding="utf-8")
            has_upstream_remote = bool(re.search(r"git\s+remote\s+add\b.*?\bupstream\b", text))
            is_upstream_watcher = path.name == "upstream-watcher.yml"
            if has_upstream_remote or is_upstream_watcher:
                check_workflow_gh_repo(text)
                checked += 1
        self.assertGreater(checked, 0)


if __name__ == "__main__":
    unittest.main()
