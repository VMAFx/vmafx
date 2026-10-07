#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A matrix leg's gate judges its own leg, not the matrix aggregate.

``needs.<matrix job>.result`` is the aggregate of every leg, so when several
gate jobs read it, one failing leg turns every gate red and the required
checks name the wrong legs. ``scripts/ci/gate_leg_result.py`` reads the run's
jobs and judges the one named job. The workflow contract below fails on any
workflow where two or more gates read one matrix job's aggregate result.

``GATE_WORKFLOWS_DIR`` points the contract at another workflow directory, for
example a checkout of master from before the fix.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]  # PyYAML ships no stubs in the hook env, as in ci_router.py

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import gate_leg_result as glr

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = Path(os.environ.get("GATE_WORKFLOWS_DIR", ROOT / ".github" / "workflows"))


def job(name: str, conclusion: str | None) -> dict[str, Any]:
    return {"name": name, "conclusion": conclusion}


RUN = [
    job("Linux Intel LLVM work", "success"),
    job("macOS Clang+Metal work", "success"),
    job("Windows MSVC+CUDA (full) work", "failure"),
    job("Plan build impact", "success"),
]


class Judge(unittest.TestCase):
    def test_another_leg_failing_passes_this_leg(self) -> None:
        self.assertIsNone(glr.judge("success", "true", "macOS Clang+Metal work", RUN))
        self.assertIsNone(glr.judge("success", "true", "Linux Intel LLVM work", RUN))

    def test_own_leg_failing_fails(self) -> None:
        reason = glr.judge("success", "true", "Windows MSVC+CUDA (full) work", RUN)
        self.assertIn("failure", reason or "")

    def test_own_leg_absent_fails(self) -> None:
        self.assertIsNotNone(glr.judge("success", "true", "Nonexistent work", RUN))

    def test_own_leg_unfinished_fails(self) -> None:
        self.assertIsNotNone(glr.judge("success", "true", "A work", [job("A work", None)]))

    def test_own_leg_cancelled_fails(self) -> None:
        self.assertIsNotNone(glr.judge("success", "true", "A work", [job("A work", "cancelled")]))

    def test_ambiguous_leg_fails(self) -> None:
        jobs = [job("A work", "success"), job("A work", "success")]
        self.assertIsNotNone(glr.judge("success", "true", "A work", jobs))

    def test_not_selected_and_skipped_passes(self) -> None:
        self.assertIsNone(glr.judge("success", "false", "A work", [job("A work", "skipped")]))
        self.assertIsNone(glr.judge("success", "false", "A work", []))

    def test_not_selected_but_ran_fails(self) -> None:
        self.assertIsNotNone(glr.judge("success", "false", "A work", [job("A work", "success")]))

    def test_plan_failure_fails(self) -> None:
        self.assertIsNotNone(glr.judge("failure", "false", "A work", []))
        self.assertIsNotNone(glr.judge("skipped", "true", "A work", [job("A work", "success")]))

    def test_bad_selected_fails(self) -> None:
        self.assertIsNotNone(glr.judge("success", "", "A work", [job("A work", "success")]))

    def test_paginated_documents_are_joined(self) -> None:
        text = json.dumps({"jobs": [job("A work", "success")]}) + "\n"
        text += json.dumps({"jobs": [job("B work", "failure")]})
        self.assertEqual([j["name"] for j in glr.parse_jobs(text)], ["A work", "B work"])

    def test_garbage_is_an_error_exit(self) -> None:
        directory = Path(tempfile.mkdtemp(prefix="gate-leg-"))
        self.addCleanup(shutil.rmtree, directory, True)
        path = directory / "garbage.json"
        path.write_text("not json", encoding="utf-8")
        os.environ.update(PLAN_RESULT="success", SELECTED="true")
        self.assertEqual(glr.main(["--job", "A work", "--jobs-file", str(path)]), 1)


def aggregate_readers(workflow: dict[str, Any]) -> dict[str, list[str]]:
    """Map each matrix job to the gate jobs that read its aggregate result."""
    jobs = workflow.get("jobs", {})
    matrix_jobs = {n for n, j in jobs.items() if "matrix" in (j.get("strategy") or {})}
    readers: dict[str, list[str]] = {}
    for name, body in jobs.items():
        text = json.dumps(body.get("steps", []))
        for matrix in matrix_jobs:
            if re.search(rf"needs\.{re.escape(matrix)}\.result", text):
                readers.setdefault(matrix, []).append(name)
    return readers


class WorkflowContract(unittest.TestCase):
    def test_no_two_gates_share_one_matrix_aggregate(self) -> None:
        offenders = []
        for path in sorted(WORKFLOWS.glob("*.yml")):
            workflow = yaml.safe_load(path.read_text(encoding="utf-8"))
            for matrix, gates in aggregate_readers(workflow).items():
                if len(gates) > 1:
                    offenders.append(f"{path.name}: {sorted(gates)} read needs.{matrix}.result")
        self.assertEqual(offenders, [])

    def test_every_gate_script_call_names_a_leg_of_the_workflow(self) -> None:
        for path in sorted(WORKFLOWS.glob("*.yml")):
            text = path.read_text(encoding="utf-8")
            for leg in re.findall(r'gate_leg_result\.py --job "([^"]+)"', text):
                self.assertTrue(leg.endswith(" work"), (path.name, leg))
                check = leg[: -len(" work")]
                self.assertRegex(text, rf"check_name: {re.escape(check)}\n", (path.name, leg))

    def test_gates_using_the_script_can_read_the_run(self) -> None:
        for path in sorted(WORKFLOWS.glob("*.yml")):
            workflow = yaml.safe_load(path.read_text(encoding="utf-8"))
            for name, body in workflow.get("jobs", {}).items():
                if "gate_leg_result.py" not in json.dumps(body.get("steps", [])):
                    continue
                self.assertEqual(
                    (body.get("permissions") or {}).get("actions"), "read", (path.name, name)
                )


if __name__ == "__main__":
    unittest.main()
