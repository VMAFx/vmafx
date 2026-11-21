#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shape of .github/workflows/python-lock-refresh.yml (ADR-3560).

Positive: the path filter equals the manifest inputs read through the lock checker's loader;
the workflow is Renovate-only, read-only by default, pinned by SHA, capped, serialised per ref,
pushes through the shared script and calls the lock checker's own write mode.
Negative: planted copies lose a path, gain a pattern, carry an expression in ``run:``, use an
unpinned action, push with a force flag, write a second generator or lose the guard step; each
is reported.
Boundary: both lock refresh workflows commit and push only through
scripts/ci/lock_refresh_publish.py.
"""

from __future__ import annotations

import copy
import re
import sys
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import check_python_dependency_locks as lock_checker  # noqa: E402

WORKFLOWS = ROOT / ".github" / "workflows"
PYTHON_WORKFLOW = WORKFLOWS / "python-lock-refresh.yml"
NEO_WORKFLOW = WORKFLOWS / "intel-neo-lock-refresh.yml"
SHA_RE = re.compile(r"^[^@\s]+@[0-9a-f]{40}$")
FORCE_RE = re.compile(r"(?:^|\s)(?:--force\S*|-f|\+HEAD\S*)(?:\s|$)")
GENERATOR_RE = re.compile(r"pip-compile|uv\s+pip\s+compile|uv\s+lock")


def load(path: Path) -> dict[str, Any]:
    """Parse a workflow; YAML 1.1 reads the ``on`` key as ``True``, so fold it back."""
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    assert isinstance(data, dict)
    if True in data:
        data["on"] = data.pop(True)
    return data


def steps_of(workflow: dict[str, Any]) -> list[dict[str, Any]]:
    """The steps of the workflow's only job."""
    (job,) = workflow["jobs"].values()
    return list(job["steps"])


def run_texts(workflow: dict[str, Any]) -> list[str]:
    """Every ``run:`` script of the workflow."""
    return [step["run"] for step in steps_of(workflow) if "run" in step]


def shape_findings(workflow: dict[str, Any], inputs: list[str]) -> list[str]:
    """Return what deviates from the required shape; an empty list is a pass."""
    found: list[str] = []
    push = workflow["on"]["push"]
    if push.get("branches") != ["renovate/**"]:
        found.append("branches must be exactly renovate/**")
    paths = push.get("paths", [])
    if sorted(paths) != sorted(inputs) or len(set(paths)) != len(paths):
        found.append("path filter is not the manifest input set")
    if workflow.get("permissions") != {"contents": "read"}:
        found.append("top-level permissions must be contents: read")
    group = workflow.get("concurrency", {}).get("group", "")
    if "github.ref" not in group or workflow["concurrency"].get("cancel-in-progress") is not False:
        found.append("concurrency must be per ref without cancel-in-progress")
    (job,) = workflow["jobs"].values()
    if not isinstance(job.get("timeout-minutes"), int):
        found.append("job needs a literal timeout-minutes")
    found += [
        f"unpinned action {step['uses']}"
        for step in steps_of(workflow)
        if "uses" in step and not SHA_RE.match(step["uses"])
    ]
    texts = run_texts(workflow)
    found += ["expression inside run:" for text in texts if "${{" in text]
    found += ["force push" for text in texts if FORCE_RE.search(text)]
    found += ["second generator" for text in texts if GENERATOR_RE.search(text)]
    found += guard_order_findings(texts)
    return found


def guard_order_findings(texts: list[str]) -> list[str]:
    """The guard runs before the write mode, which runs before the publish."""
    joined = [
        next((i for i, t in enumerate(texts) if marker in t), -1)
        for marker in (
            "lock_refresh_publish.py refuse-non-renovate",
            "check_python_dependency_locks.py write",
            "lock_refresh_publish.py publish",
        )
    ]
    if -1 in joined or joined != sorted(joined):
        return ["guard, write and publish steps are missing or out of order"]
    return []


class PythonLockRefreshWorkflowTest(unittest.TestCase):
    """The checked-in workflow and planted variants of it."""

    def setUp(self) -> None:
        """Load the workflow and the manifest input set."""
        self.workflow = load(PYTHON_WORKFLOW)
        self.inputs = lock_checker.manifest_inputs(ROOT)

    def planted(self) -> dict[str, Any]:
        """A deep copy to plant a defect in."""
        return copy.deepcopy(self.workflow)

    def test_checked_in_workflow_has_the_required_shape(self) -> None:
        """Positive: no finding, and the filter is the manifest input set."""
        self.assertEqual(shape_findings(self.workflow, self.inputs), [])
        paths = self.workflow["on"]["push"]["paths"]
        self.assertEqual(sorted(paths), sorted(self.inputs))
        self.assertGreater(len(paths), 20)

    def test_publish_step_reads_outputs_from_the_manifest_and_names_the_bot_commit(self) -> None:
        """Positive: outputs come from the checker, the subject is the DCO subject."""
        text = "\n".join(run_texts(self.workflow))
        self.assertIn("check_python_dependency_locks.py outputs", text)
        self.assertIn("--check-strays-when-unchanged", text)
        self.assertIn('-- "${outputs[@]}"', text)
        self.assertNotIn("requirements/locks/*.txt", text)
        dco = (ROOT / "scripts" / "ci" / "check-dco.py").read_text(encoding="utf-8")
        subject = re.search(r'PYTHON_LOCK_SUBJECT = "([^"]+)"', dco)
        assert subject is not None
        self.assertIn(f'--subject "{subject.group(1)}"', text)

    def test_checkout_keeps_no_credentials_and_the_token_comes_last(self) -> None:
        """Positive: checkout drops credentials; the token is minted after the regeneration."""
        steps = steps_of(self.workflow)
        checkout = next(s for s in steps if s.get("uses", "").startswith("actions/checkout@"))
        self.assertIs(checkout["with"]["persist-credentials"], False)
        names = [s.get("name", "") for s in steps]
        mint = next(i for i, n in enumerate(names) if n.startswith("Mint"))
        write = next(i for i, s in enumerate(steps) if "write" in s.get("run", ""))
        self.assertLess(write, mint)

    def test_planted_missing_path_is_reported(self) -> None:
        """Negative: dropping one manifest input from the filter is reported."""
        wf = self.planted()
        wf["on"]["push"]["paths"].pop()
        self.assertIn("path filter is not the manifest input set", shape_findings(wf, self.inputs))

    def test_planted_pattern_or_duplicate_path_is_reported(self) -> None:
        """Negative: a glob in place of an input, and a duplicated input, are reported."""
        wf = self.planted()
        wf["on"]["push"]["paths"][0] = "requirements/locks/*.in"
        self.assertIn("path filter is not the manifest input set", shape_findings(wf, self.inputs))
        wf = self.planted()
        wf["on"]["push"]["paths"].append(wf["on"]["push"]["paths"][0])
        self.assertIn("path filter is not the manifest input set", shape_findings(wf, self.inputs))

    def test_planted_run_defects_are_reported(self) -> None:
        """Negative: an expression, a force flag and a second generator in ``run:``."""
        cases = {
            "expression inside run:": 'echo "${{ github.ref }}"',
            "force push": "git push --force origin HEAD",
            "second generator": "uv pip compile requirements/locks/build.in",
        }
        for finding, line in cases.items():
            with self.subTest(finding=finding):
                wf = self.planted()
                steps_of(wf)[-1]["run"] += f"\n{line}\n"
                self.assertIn(finding, shape_findings(wf, self.inputs))

    def test_planted_unpinned_action_and_missing_guard_are_reported(self) -> None:
        """Negative: a tag-pinned action and a removed guard step are reported."""
        wf = self.planted()
        steps_of(wf)[0]["uses"] = "actions/checkout@v7"
        self.assertIn("unpinned action actions/checkout@v7", shape_findings(wf, self.inputs))
        wf = self.planted()
        steps = steps_of(wf)
        wf["jobs"]["refresh-locks"]["steps"] = [
            s for s in steps if "refuse" not in s.get("run", "")
        ]
        self.assertIn(
            "guard, write and publish steps are missing or out of order",
            shape_findings(wf, self.inputs),
        )

    def test_boundary_both_lock_workflows_publish_through_the_shared_script(self) -> None:
        """Boundary: neither workflow runs git commit or git push itself."""
        for path in (PYTHON_WORKFLOW, NEO_WORKFLOW):
            with self.subTest(workflow=path.name):
                text = "\n".join(run_texts(load(path)))
                self.assertIn("lock_refresh_publish.py publish", text)
                self.assertIn("lock_refresh_publish.py refuse-non-renovate", text)
                self.assertNotRegex(text, r"git\s+(?:-c\s+\S+\s+)*(?:commit|push)\b")


if __name__ == "__main__":
    unittest.main()
