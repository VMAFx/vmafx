#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A push to master must never cancel the run of an earlier master commit.

The merge train lands master commits every few minutes and keeps one sentinel
commit's runs alive itself. A workflow whose concurrency group is the ref and
whose `cancel-in-progress` is true cancels the previous commit's push run on
every landing, so no master commit gets a complete verdict. This test reads
every workflow that a push to master triggers and evaluates its concurrency
blocks (workflow level and job level) for two master pushes with different
SHAs, and for two pushes to one pull request:

* master: the two groups must differ (the SHA is in the group), so one push
  cannot cancel or evict the other, unless the block is listed in
  `SERIALISED` with a reason;
* pull request: the groups must be equal and `cancel-in-progress` true, so a
  newer push still supersedes an older one (the policy before this change).

A `SERIALISED` entry must name a block that exists, in a workflow that a push
to master triggers, and that really shares one group between master SHAs; a
stale entry fails like a missing one.
"""

from __future__ import annotations

import fnmatch
import re
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github" / "workflows"

# (workflow file, "workflow" or job id) -> why master runs share one group.
SERIALISED: dict[tuple[str, str], str] = {
    ("dev-container-publish.yml", "workflow"): (
        "publishes the dev container image to the registry; two publishes must not "
        "interleave, and a pending publish replaced by a newer one is fine (latest wins)"
    ),
    ("docs.yml", "deploy"): (
        "one Pages deployment per repository (ADR-1294); cancelling one mid-write is worse"
    ),
    ("release-please.yml", "workflow"): (
        "a second release-please run would open a duplicate release PR for the same push"
    ),
    ("scorecard.yml", "workflow"): (
        "publishes the Scorecard attestation; the score describes the repository state, "
        "not one commit, so a superseded pending run loses nothing"
    ),
}

TOKEN = re.compile(r"\s*(\|\||&&|==|!=|'[^']*'|[A-Za-z_][A-Za-z0-9_.]*|\()")


def evaluate(expr: str, ctx: dict[str, str]) -> Any:
    """Evaluate the subset of the Actions expression language the workflows use."""
    tokens: list[str] = []
    pos = 0
    expr = expr.strip()
    while pos < len(expr):
        match = TOKEN.match(expr, pos)
        if not match:
            raise ValueError(f"cannot tokenise {expr!r} at {pos}")
        tokens.append(match.group(1))
        pos = match.end()

    def atom(i: int) -> tuple[Any, int]:
        tok = tokens[i]
        if tok.startswith("'"):
            return tok[1:-1], i + 1
        if tok in ("true", "false"):
            return tok == "true", i + 1
        return ctx.get(tok, ""), i + 1

    def compare(i: int) -> tuple[Any, int]:
        left, i = atom(i)
        if i < len(tokens) and tokens[i] in ("==", "!="):
            op = tokens[i]
            right, i = atom(i + 1)
            return (left == right) if op == "==" else (left != right), i
        return left, i

    def conj(i: int) -> tuple[Any, int]:
        left, i = compare(i)
        while i < len(tokens) and tokens[i] == "&&":
            right, i = compare(i + 1)
            left = right if left else left
        return left, i

    def disj(i: int) -> tuple[Any, int]:
        left, i = conj(i)
        while i < len(tokens) and tokens[i] == "||":
            right, i = conj(i + 1)
            left = left if left else right
        return left, i

    value, end = disj(0)
    if end != len(tokens):
        raise ValueError(f"trailing tokens in {expr!r}")
    return value


def render(template: Any, ctx: dict[str, str]) -> Any:
    """Expand `${{ ... }}` in a string; a bare boolean passes through."""
    if isinstance(template, bool):
        return template
    whole = re.fullmatch(r"\s*\$\{\{((?:(?!\}\}).)*)\}\}\s*", str(template), re.S)
    if whole:
        return evaluate(whole.group(1), ctx)
    return re.sub(
        r"\$\{\{(.*?)\}\}", lambda m: str(evaluate(m.group(1), ctx)), str(template), flags=re.S
    )


def master_push(sha: str) -> dict[str, str]:
    return {
        "github.ref": "refs/heads/master",
        "github.sha": sha,
        "github.event_name": "push",
        "github.workflow": "W",
        "github.run_id": f"run-{sha}",
    }


def pr_push(sha: str) -> dict[str, str]:
    return {
        "github.ref": "refs/pull/7/merge",
        "github.sha": sha,
        "github.event_name": "pull_request",
        "github.workflow": "W",
        "github.event.pull_request.number": "7",
        "github.event.number": "7",
        "github.run_id": f"run-{sha}",
    }


def triggers(workflow: dict[Any, Any]) -> dict[str, Any]:
    on = workflow.get(True, workflow.get("on"))
    if isinstance(on, str):
        return {on: None}
    if isinstance(on, list):
        return dict.fromkeys(on)
    return dict(on or {})


def pushes_to_master(workflow: dict[Any, Any]) -> bool:
    push = triggers(workflow).get("push", False)
    if push is False:
        return False
    branches = (push or {}).get("branches")
    if (push or {}).get("tags") and not branches:
        return False
    return branches is None or any(fnmatch.fnmatch("master", b) for b in branches)


def concurrency_blocks(workflow: dict[Any, Any]) -> dict[str, dict[str, Any]]:
    blocks: dict[str, dict[str, Any]] = {}
    if workflow.get("concurrency"):
        blocks["workflow"] = workflow["concurrency"]
    for job, body in (workflow.get("jobs") or {}).items():
        if isinstance(body, dict) and body.get("concurrency"):
            blocks[job] = body["concurrency"]
    return blocks


def normalise(block: Any) -> dict[str, Any]:
    return {"group": block, "cancel-in-progress": False} if isinstance(block, str) else block


def master_findings(
    workflows: dict[str, dict[Any, Any]], serialised: dict[tuple[str, str], str]
) -> list[str]:
    """Every way a push-to-master workflow can cancel or evict an earlier master run."""
    problems: list[str] = []
    seen: set[tuple[str, str]] = set()
    for name, workflow in sorted(workflows.items()):
        if not pushes_to_master(workflow):
            continue
        for scope, raw in concurrency_blocks(workflow).items():
            block = normalise(raw)
            shared = render(block["group"], master_push("a" * 40)) == render(
                block["group"], master_push("b" * 40)
            )
            if (name, scope) in serialised:
                seen.add((name, scope))
                if not shared:
                    problems.append(f"{name} [{scope}] is listed as serialised but has a SHA group")
            elif shared:
                problems.append(
                    f"{name} [{scope}] shares one group between master pushes "
                    f"({block['group']!r}): put github.sha in it for refs/heads/master"
                )
    for key in sorted(set(serialised) - seen):
        problems.append(
            f"{key[0]} [{key[1]}] is listed as serialised but is not a push-to-master block"
        )
    return problems


def pull_request_findings(
    workflows: dict[str, dict[Any, Any]], serialised: dict[tuple[str, str], str]
) -> list[str]:
    """A newer push to one pull request must still cancel the older run."""
    problems: list[str] = []
    for name, workflow in sorted(workflows.items()):
        if not pushes_to_master(workflow) or "pull_request" not in triggers(workflow):
            continue
        for scope, raw in concurrency_blocks(workflow).items():
            if (name, scope) in serialised:
                continue
            block = normalise(raw)
            same = render(block["group"], pr_push("a" * 40)) == render(
                block["group"], pr_push("b" * 40)
            )
            cancel = render(block["cancel-in-progress"], pr_push("a" * 40))
            if not (same and cancel in (True, "true")):
                problems.append(f"{name} [{scope}] no longer supersedes an older PR run")
    return problems


def load_all() -> dict[str, dict[Any, Any]]:
    return {
        path.name: yaml.safe_load(path.read_text(encoding="utf-8"))
        for path in sorted(WORKFLOWS.glob("*.yml"))
    }


def wf(on: Any, concurrency: Any = None, jobs: Any = None) -> dict[Any, Any]:
    out: dict[Any, Any] = {True: on}
    if concurrency is not None:
        out["concurrency"] = concurrency
    if jobs is not None:
        out["jobs"] = jobs
    return out


FIXED = "${{ github.ref == 'refs/heads/master' && github.sha || github.ref }}"
OLD = {"group": "x-${{ github.workflow }}-${{ github.ref }}", "cancel-in-progress": True}
NEW = {
    "group": f"x-${{{{ github.workflow }}}}-{FIXED}",
    "cancel-in-progress": "${{ github.ref != 'refs/heads/master' }}",
}


class RepositoryWorkflows(unittest.TestCase):
    """Positive: the checked-in workflows keep master runs apart."""

    def test_no_master_push_cancels_an_earlier_master_run(self) -> None:
        self.assertEqual(master_findings(load_all(), SERIALISED), [])

    def test_pull_requests_still_supersede_older_runs(self) -> None:
        self.assertEqual(pull_request_findings(load_all(), SERIALISED), [])

    def test_aggregator_is_sha_scoped_on_master(self) -> None:
        block = load_all()["required-aggregator.yml"]["concurrency"]
        self.assertNotEqual(
            render(block["group"], master_push("a" * 40)),
            render(block["group"], master_push("b" * 40)),
        )
        self.assertFalse(render(block["cancel-in-progress"], master_push("a" * 40)))


class Contract(unittest.TestCase):
    """Negative and boundary cases on synthetic workflows."""

    def run_master(
        self, workflow: dict[Any, Any], serialised: dict[tuple[str, str], str] | None = None
    ) -> list[str]:
        return master_findings({"x.yml": workflow}, serialised or {})

    def test_negative_ref_group_with_cancel_is_refused(self) -> None:
        found = self.run_master(wf({"push": {"branches": ["master"]}}, OLD))
        self.assertEqual(len(found), 1)
        self.assertIn("x.yml [workflow]", found[0])

    def test_negative_ref_group_without_cancel_is_refused(self) -> None:
        block = {"group": OLD["group"], "cancel-in-progress": False}
        self.assertEqual(len(self.run_master(wf("push", block))), 1)

    def test_negative_job_level_block_is_refused(self) -> None:
        jobs = {"build": {"concurrency": OLD}}
        self.assertEqual(len(self.run_master(wf({"push": None}, None, jobs))), 1)

    def test_positive_sha_group_is_accepted(self) -> None:
        self.assertEqual(self.run_master(wf({"push": {"branches": ["master"]}}, NEW)), [])

    def test_positive_string_form_block_with_sha_is_accepted(self) -> None:
        self.assertEqual(self.run_master(wf("push", f"g-{FIXED}")), [])

    def test_boundary_other_branch_push_is_not_a_master_push(self) -> None:
        self.assertEqual(self.run_master(wf({"push": {"branches": ["release/*"]}}, OLD)), [])

    def test_boundary_tag_only_push_is_not_a_master_push(self) -> None:
        self.assertEqual(self.run_master(wf({"push": {"tags": ["v*"]}}, OLD)), [])

    def test_boundary_branch_glob_matching_master_counts(self) -> None:
        self.assertEqual(len(self.run_master(wf({"push": {"branches": ["m*"]}}, OLD))), 1)

    def test_boundary_no_concurrency_block_is_fine(self) -> None:
        self.assertEqual(self.run_master(wf({"push": None})), [])

    def test_boundary_schedule_only_workflow_is_out_of_scope(self) -> None:
        self.assertEqual(self.run_master(wf({"schedule": [{"cron": "0 0 * * *"}]}, OLD)), [])

    def test_serialised_entry_with_reason_is_accepted(self) -> None:
        listed = {("x.yml", "workflow"): "publishes"}
        self.assertEqual(self.run_master(wf({"push": None}, OLD), listed), [])

    def test_stale_entry_for_a_sha_scoped_block_is_refused(self) -> None:
        listed = {("x.yml", "workflow"): "publishes"}
        found = self.run_master(wf({"push": None}, NEW), listed)
        self.assertEqual(len(found), 1)
        self.assertIn("SHA group", found[0])

    def test_stale_entry_for_a_missing_workflow_is_refused(self) -> None:
        found = master_findings({}, {("gone.yml", "workflow"): "x"})
        self.assertEqual(len(found), 1)

    def test_every_serialised_entry_has_a_reason(self) -> None:
        for reason in SERIALISED.values():
            self.assertGreater(len(reason.split()), 3)

    def test_pull_request_cancel_removed_is_refused(self) -> None:
        block = {"group": NEW["group"], "cancel-in-progress": False}
        flow = wf({"push": None, "pull_request": None}, block)
        self.assertEqual(len(pull_request_findings({"x.yml": flow}, {})), 1)

    def test_pull_request_sha_group_is_refused(self) -> None:
        block = {"group": "x-${{ github.sha }}", "cancel-in-progress": True}
        flow = wf({"push": None, "pull_request": None}, block)
        self.assertEqual(len(pull_request_findings({"x.yml": flow}, {})), 1)

    def test_pull_request_policy_kept_by_the_fix(self) -> None:
        flow = wf({"push": None, "pull_request": None}, NEW)
        self.assertEqual(pull_request_findings({"x.yml": flow}, {}), [])

    def test_evaluator_short_circuit_semantics(self) -> None:
        ctx = {"a": "1", "b": ""}
        self.assertEqual(evaluate("b && 'x' || 'y'", ctx), "y")
        self.assertEqual(evaluate("a && 'x' || 'y'", ctx), "x")
        self.assertIs(evaluate("a != 'z'", ctx), True)

    def test_render_keeps_two_expressions_apart(self) -> None:
        group = "${{ github.workflow }}-${{ github.event.number || github.run_id }}"
        self.assertEqual(render(group, pr_push("a" * 40)), "W-7")
        self.assertEqual(render(group, master_push("a" * 40)), f"W-run-{'a' * 40}")

    def test_positive_run_id_group_is_per_run_on_master(self) -> None:
        flow = wf(
            {"push": {"branches": ["master"]}, "pull_request": None},
            {
                "group": "${{ github.workflow }}-${{ github.event.number || github.run_id }}",
                "cancel-in-progress": "${{ github.event_name == 'pull_request' }}",
            },
        )
        self.assertEqual(master_findings({"x.yml": flow}, {}), [])
        self.assertEqual(pull_request_findings({"x.yml": flow}, {}), [])


if __name__ == "__main__":
    unittest.main()
