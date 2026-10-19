# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Planted-case tests for the candidate leg check (ADR-2198).

Every case feeds ``check_candidate_legs.check()`` the answers a forge would give for a
synthetic candidate. The red cases are the failures this check exists for: a leg that
is skipped because its inputs did not change, a leg that failed, and a dispatch that
built some other source.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import sys
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

_spec = importlib.util.spec_from_file_location(
    "check_candidate_legs", ROOT / "scripts/release/check-candidate-legs.py"
)
assert _spec is not None and _spec.loader is not None
legs_check = importlib.util.module_from_spec(_spec)
sys.modules["check_candidate_legs"] = legs_check
_spec.loader.exec_module(legs_check)

SHA = "a" * 40
OTHER = "b" * 40
REPO = "VMAFx/vmafx"
LEGS = json.loads((ROOT / "scripts/release/candidate-legs.json").read_text("utf-8"))["workflows"]
WINDOWS = next(entry for entry in LEGS if entry["file"] == "windows-tester-bundle.yml")


def run(run_id: int, event: str, head: str, title: str = "t", created: str = "2026-10-07") -> Any:
    return {
        "id": run_id,
        "event": event,
        "status": "completed",
        "head_sha": head,
        "display_title": title,
        "created_at": created,
    }


class Forge:
    """A fake of the three endpoints the check reads."""

    def __init__(self, runs: dict[str, list[Any]], jobs: dict[int, dict[str, str]]) -> None:
        self.runs, self.jobs = runs, jobs

    def __call__(self, path: str) -> object:
        if "/runs?" in path:
            filename = re.search(r"workflows/([^/]+)/runs", path).group(1)  # type: ignore[union-attr]
            rows = self.runs.get(filename, [])
            if "head_sha=" in path:
                rows = [r for r in rows if r["head_sha"] == SHA]
            elif "event=workflow_dispatch" in path:
                rows = [r for r in rows if r["event"] == "workflow_dispatch"]
            return {"workflow_runs": rows if "page=1" in path else []}
        run_id = int(re.search(r"runs/(\d+)/jobs", path).group(1))  # type: ignore[union-attr]
        names = self.jobs.get(run_id, {})
        rows = [{"name": n, "conclusion": c} for n, c in names.items()]
        return {"jobs": rows if "page=1" in path else []}


def all_green(entry: dict[str, Any], state: str = "success") -> dict[str, str]:
    return dict.fromkeys(entry["jobs"], state)


def only_windows(forge: Forge) -> list[str]:
    problems: list[str] = legs_check.check(forge, REPO, [WINDOWS], SHA)
    return problems


class Green(unittest.TestCase):
    def test_every_leg_green_in_a_push_run_of_the_candidate_passes(self) -> None:
        forge = Forge({"windows-tester-bundle.yml": [run(1, "push", SHA)]}, {1: all_green(WINDOWS)})
        self.assertEqual(only_windows(forge), [])

    def test_a_dispatch_titled_with_the_candidate_sha_counts(self) -> None:
        title = f"Windows tester zips, source {SHA}"
        forge = Forge(
            {"windows-tester-bundle.yml": [run(2, "workflow_dispatch", OTHER, title)]},
            {2: all_green(WINDOWS)},
        )
        self.assertEqual(only_windows(forge), [])

    def test_legs_may_come_from_two_runs(self) -> None:
        half = len(WINDOWS["jobs"]) // 2
        first = dict.fromkeys(WINDOWS["jobs"][:half], "success")
        second = dict.fromkeys(WINDOWS["jobs"][half:], "success")
        forge = Forge(
            {"windows-tester-bundle.yml": [run(1, "push", SHA, created="1"), run(2, "push", SHA)]},
            {1: first, 2: second},
        )
        self.assertEqual(only_windows(forge), [])


class Red(unittest.TestCase):
    def test_a_failed_sycl_leg_is_named(self) -> None:
        jobs = all_green(WINDOWS)
        sycl = "Build and test the zip (Windows x64-sycl)"
        jobs[sycl] = "failure"
        forge = Forge({"windows-tester-bundle.yml": [run(1, "push", SHA)]}, {1: jobs})
        problems = only_windows(forge)
        self.assertEqual(len(problems), 1)
        self.assertIn(sycl, problems[0])
        self.assertIn("failure", problems[0])

    def test_a_skipped_leg_is_not_green(self) -> None:
        """A master push skips the legs when no input changed: that proves nothing."""
        forge = Forge(
            {"windows-tester-bundle.yml": [run(1, "push", SHA)]},
            {1: all_green(WINDOWS, "skipped")},
        )
        self.assertEqual(len(only_windows(forge)), len(WINDOWS["jobs"]))

    def test_a_leg_absent_from_the_run_is_not_green(self) -> None:
        jobs = all_green(WINDOWS)
        del jobs["Verify the zip and write its SBOM (Windows x64-sycl)"]
        forge = Forge({"windows-tester-bundle.yml": [run(1, "push", SHA)]}, {1: jobs})
        self.assertEqual(len(only_windows(forge)), 1)

    def test_a_dispatch_for_another_source_proves_nothing(self) -> None:
        for title in ("Publish Windows Tester Bundle", f"Windows tester zips, source {OTHER}"):
            with self.subTest(title=title):
                forge = Forge(
                    {"windows-tester-bundle.yml": [run(2, "workflow_dispatch", SHA, title)]},
                    {2: all_green(WINDOWS)},
                )
                self.assertEqual(len(only_windows(forge)), 1)
                self.assertIn("no completed run", only_windows(forge)[0])

    def test_a_run_of_another_commit_proves_nothing(self) -> None:
        forge = Forge(
            {"windows-tester-bundle.yml": [run(1, "push", OTHER)]}, {1: all_green(WINDOWS)}
        )
        self.assertEqual(len(only_windows(forge)), 1)

    def test_an_unfinished_run_does_not_count(self) -> None:
        pending = run(1, "push", SHA)
        pending["status"] = "in_progress"
        forge = Forge({"windows-tester-bundle.yml": [pending]}, {1: all_green(WINDOWS)})
        self.assertEqual(len(only_windows(forge)), 1)

    def test_the_whole_list_fails_without_any_run(self) -> None:
        problems = legs_check.check(Forge({}, {}), REPO, LEGS, SHA)
        self.assertEqual(len(problems), len(LEGS))

    def test_a_short_sha_is_refused(self) -> None:
        with self.assertRaises(legs_check.LegsError):
            legs_check.check(Forge({}, {}), REPO, LEGS, "abc123")

    def test_a_malformed_answer_is_refused(self) -> None:
        with self.assertRaises(legs_check.LegsError):
            legs_check.check(lambda _path: [], REPO, LEGS, SHA)

    def test_the_command_needs_a_token(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(legs_check.main(["--sha", SHA]), 2)


class Declaration(unittest.TestCase):
    """The list names jobs that exist: a renamed leg must not silently fall out of the cut check."""

    def test_every_listed_job_is_defined_by_its_workflow(self) -> None:
        for entry in LEGS:
            workflow = yaml.safe_load((ROOT / ".github/workflows" / entry["file"]).read_text())
            templates = [str(job.get("name", "")) for job in workflow["jobs"].values()]
            patterns = [
                re.compile(
                    "^" + ".+".join(re.escape(p) for p in re.split(r"\$\{\{[^}]*\}\}", t)) + "$"
                )
                for t in templates
            ]
            for job in entry["jobs"]:
                with self.subTest(workflow=entry["file"], job=job):
                    self.assertTrue(any(p.match(job) for p in patterns), job)

    def test_every_windows_leg_is_listed(self) -> None:
        text = (ROOT / ".github/workflows/windows-tester-bundle.yml").read_text()
        legs = re.search(r"verify_matrix='\{\"name\":\[([^\]]*)\]\}'", text)
        assert legs is not None
        names = re.findall(r'"([^"]+)"', legs.group(1))
        self.assertEqual(sorted(names), ["arm64", "x64", "x64-cuda", "x64-sycl"])
        for name in names:
            self.assertIn(f"Build and test the zip (Windows {name})", WINDOWS["jobs"])
            self.assertIn(f"Verify the zip and write its SBOM (Windows {name})", WINDOWS["jobs"])

    def test_dispatches_name_their_source_in_the_run_title(self) -> None:
        for entry in LEGS:
            text = (ROOT / ".github/workflows" / entry["file"]).read_text()
            with self.subTest(workflow=entry["file"]):
                self.assertRegex(text, r"(?m)^run-name: .*github\.event\.inputs\.ref")


if __name__ == "__main__":
    unittest.main()
