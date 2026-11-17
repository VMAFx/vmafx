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

For the merge-gate workflows (`required-aggregator.yml` and every workflow in
its `editedRerunWorkflows` array), a pull-request event that does not move the
head (`edited`, `reopened`, `ready_for_review`) must not cancel a running run
of the same head, no later same-head event may replace a pending `edited` run
of a non-aggregator gate workflow, and `jobs.aggregator` must not skip on the
event action (ADR-3557); only `synchronize` cancels the superseded run.
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
    ("docs-render-pr.yml", "workflow"): (
        "one render branch and pull request per repository (ADR-3373, ADR-3552); "
        "cancel-in-progress is false so one run finishes and one pending run "
        "renders the latest master tip"
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

# Workflow -> why a pull request run of it carries no workflow-level block that
# supersedes the older run of the same pull request.
UNSUPERSEDED: dict[str, str] = {
    "docs.yml": (
        "per-job blocks instead (ADR-1294, ADR-1297): the build jobs supersede, the "
        "Pages deploy job must never be cancelled mid-write"
    ),
    "pr-label-lanes.yml": (
        "each called lane keeps its own group, and the same group at both levels "
        "would deadlock the caller and the callee"
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


STAMP_T1 = "2026-11-17T06:00:00Z"
STAMP_T2 = "2026-11-17T06:05:00Z"


def pr_push(sha: str, updated_at: str = STAMP_T1) -> dict[str, str]:
    return {
        "github.ref": "refs/pull/7/merge",
        "github.sha": sha,
        "github.event_name": "pull_request",
        "github.event.action": "synchronize",
        "github.workflow": "W",
        "github.event.pull_request.number": "7",
        "github.event.pull_request.updated_at": updated_at,
        "github.event.number": "7",
        "github.run_id": f"run-{sha}",
    }


def pr_event(
    sha: str, action: str, updated_at: str = STAMP_T1, run_id: str | None = None
) -> dict[str, str]:
    ctx = {**pr_push(sha, updated_at), "github.event.action": action}
    if run_id is not None:
        ctx["github.run_id"] = run_id
    return ctx


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


PR_EVENTS = ("pull_request", "pull_request_target", "merge_group")


def unsuperseded_findings(
    workflows: dict[str, dict[Any, Any]], listed: dict[str, str]
) -> list[str]:
    """Every pull-request workflow cancels its older run, or says why it cannot."""
    problems: list[str] = []
    for name, workflow in sorted(workflows.items()):
        if not any(event in triggers(workflow) for event in PR_EVENTS):
            if name in listed:
                problems.append(f"{name} is listed as unsuperseded but has no PR trigger")
            continue
        block = concurrency_blocks(workflow).get("workflow")
        superseding = False
        if block is not None:
            block = normalise(block)
            same = render(block["group"], pr_push("a" * 40)) == render(
                block["group"], pr_push("b" * 40)
            )
            cancel = render(block["cancel-in-progress"], pr_push("a" * 40))
            superseding = same and cancel in (True, "true")
        if name in listed:
            if superseding:
                problems.append(f"{name} is listed as unsuperseded but supersedes PR runs")
        elif not superseding:
            problems.append(
                f"{name} has no workflow-level concurrency block that cancels the older "
                "run of the same pull request"
            )
    for name in sorted(set(listed) - set(workflows)):
        problems.append(f"{name} is listed as unsuperseded but does not exist")
    return problems


EDITED_RERUN_ARRAY_RE = re.compile(r"\beditedRerunWorkflows\s*=\s*\[(.*?)\]", re.S)
QUOTED_PATH_RE = re.compile(r"""['"]([^'"]+)['"]""")
DEFAULT_PR_TYPES = ("opened", "synchronize", "reopened")
EXEMPT_SAME_HEAD_TYPES = frozenset({"synchronize", "opened", "closed"})


def _aggregator_scripts(workflows: dict[str, dict[Any, Any]]) -> list[str]:
    aggregator = workflows.get("required-aggregator.yml")
    jobs = aggregator.get("jobs") if isinstance(aggregator, dict) else None
    job = jobs.get("aggregator") if isinstance(jobs, dict) else None
    steps = job.get("steps") if isinstance(job, dict) else None
    if not isinstance(steps, list):
        return []
    scripts: list[str] = []
    for step in steps:
        with_map = step.get("with") if isinstance(step, dict) else None
        script = with_map.get("script") if isinstance(with_map, dict) else None
        if isinstance(script, str):
            scripts.append(script)
    return scripts


def gate_workflows(workflows: dict[str, dict[Any, Any]]) -> tuple[set[str], list[str]]:
    """Return merge-gate workflow basenames and any editedRerunWorkflows parse findings."""
    gates: set[str] = {"required-aggregator.yml"}
    found_paths: list[str] = []
    for script in _aggregator_scripts(workflows):
        for match in EDITED_RERUN_ARRAY_RE.finditer(script):
            uncommented = re.sub(
                r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.S)
            )
            found_paths.extend(
                Path(raw).name for raw in QUOTED_PATH_RE.findall(uncommented) if Path(raw).name
            )
    if not found_paths:
        return (
            gates,
            ["required-aggregator.yml [aggregator] missing or empty editedRerunWorkflows array"],
        )
    gates.update(found_paths)
    return gates, []


def pull_request_types(workflow: dict[Any, Any]) -> list[str]:
    trig = triggers(workflow)
    if "pull_request" not in trig:
        return []
    pr_on = trig["pull_request"]
    raw = pr_on.get("types") if isinstance(pr_on, dict) else None
    if raw is None:
        return list(DEFAULT_PR_TYPES)
    return [raw] if isinstance(raw, str) else [str(item) for item in raw]


def _cancels_running_group(
    block: dict[str, Any], sha: str, action: str, running_groups: set[Any]
) -> bool:
    for stamp in (STAMP_T1, STAMP_T2):
        event_ctx = pr_event(sha, action, stamp, run_id="run-2")
        if render(block["group"], event_ctx) not in running_groups:
            continue
        if render(block.get("cancel-in-progress", False), event_ctx) in (True, "true"):
            return True
    return False


def _block_same_head_findings(
    name: str, scope: str, block: dict[str, Any], pr_types: list[str]
) -> list[str]:
    sha_a = "a" * 40
    running_groups = {
        render(block["group"], pr_event(sha_a, running, STAMP_T1, run_id="run-1"))
        for running in pr_types
    }
    checked_types = [t for t in pr_types if t not in EXEMPT_SAME_HEAD_TYPES]
    found: list[str] = []
    for action in checked_types:
        if _cancels_running_group(block, sha_a, action, running_groups):
            found.append(
                f"{name} [{scope}] a same-head {action} run cancels the running run "
                "of the same head"
            )
    return found


def same_head_findings(workflows: dict[str, dict[Any, Any]]) -> list[str]:
    """A non-synchronize PR event must never cancel a running merge-gate run of the same head."""
    gates, problems = gate_workflows(workflows)
    for name in sorted(gates):
        workflow = workflows.get(name)
        if workflow is None:
            problems.append(f"{name} is listed as a gate workflow but does not exist")
            continue
        pr_types = pull_request_types(workflow)
        for scope, raw in concurrency_blocks(workflow).items():
            problems.extend(_block_same_head_findings(name, scope, normalise(raw), pr_types))
    return problems


def _block_pending_replacement_findings(
    name: str, scope: str, block: dict[str, Any], pr_types: list[str]
) -> list[str]:
    sha_a = "a" * 40
    edited_group = render(block["group"], pr_event(sha_a, "edited", STAMP_T1, run_id="run-edit-1"))
    colliding: list[str] = []
    for action in sorted(set(pr_types)):
        later_ctx = pr_event(sha_a, action, STAMP_T2, run_id="run-edit-2")
        same_sec_ctx = pr_event(sha_a, action, STAMP_T1, run_id="run-edit-2")
        if render(block["group"], later_ctx) == edited_group:
            colliding.append(action)
        elif render(block["group"], same_sec_ctx) == edited_group:
            colliding.append(f"same-second {action}")
    if not colliding:
        return []
    return [
        f"{name} [{scope}] an edited run shares its concurrency group with a later "
        f"same-head {', '.join(colliding)} run, which replaces it while pending"
    ]


def pending_replacement_findings(workflows: dict[str, dict[Any, Any]]) -> list[str]:
    """No later same-head PR event may replace a pending edited gate run outside the aggregator."""
    gates, problems = gate_workflows(workflows)
    for name in sorted(gates - {"required-aggregator.yml"}):
        workflow = workflows.get(name)
        if workflow is None:
            problems.append(f"{name} is listed as a gate workflow but does not exist")
            continue
        pr_types = pull_request_types(workflow)
        if "edited" not in pr_types:
            continue
        for scope, raw in concurrency_blocks(workflow).items():
            problems.extend(
                _block_pending_replacement_findings(name, scope, normalise(raw), pr_types)
            )
    return problems


def skip_findings(workflows: dict[str, dict[Any, Any]]) -> list[str]:
    """Skipping jobs.aggregator or its steps on an action passes branch protection."""
    aggregator = workflows.get("required-aggregator.yml")
    jobs = aggregator.get("jobs") if isinstance(aggregator, dict) else None
    job = jobs.get("aggregator") if isinstance(jobs, dict) else None
    if not isinstance(job, dict):
        return ["required-aggregator.yml [aggregator] missing aggregator job"]
    problems: list[str] = []
    if "if" in job:
        problems.append(
            f"required-aggregator.yml [aggregator] job has if: {job['if']!r} "
            "(a skipped required context counts as passing)"
        )
    for idx, step in enumerate(job.get("steps") or []):
        if not isinstance(step, dict) or "if" not in step:
            continue
        cond = str(step["if"])
        if "github.event.action" in cond or "edited" in cond:
            label = step.get("name") or f"step {idx}"
            problems.append(
                f"required-aggregator.yml [aggregator:{label}] step has if: {cond!r} "
                "(a skipped required context counts as passing)"
            )
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
GATE_PR_ON: dict[str, Any] = {
    "pull_request": {"types": ["opened", "edited", "synchronize", "reopened", "ready_for_review"]}
}
AGGREGATOR_ON: dict[str, Any] = {**GATE_PR_ON, "push": {"branches": ["master"]}}
AGGREGATOR_NEW: dict[str, Any] = {
    "group": f"required-aggregator-${{{{ github.workflow }}}}-{FIXED}",
    "cancel-in-progress": (
        "${{ github.event_name == 'pull_request' && github.event.action == 'synchronize' }}"
    ),
}
RULES_NEW: dict[str, Any] = {
    "group": (
        "rule-enforcement-${{ github.event.pull_request.number }}-"
        "${{ github.event.action == 'edited' && github.run_id || 'head' }}"
    ),
    "cancel-in-progress": "${{ github.event.action == 'synchronize' }}",
}
GATE_SCRIPT = "const editedRerunWorkflows = [\n  '.github/workflows/rule-enforcement.yml',\n];"


def aggregator_jobs(
    script: str | None = GATE_SCRIPT,
    *,
    job_if: str | None = None,
    step_if: str | None = None,
) -> dict[str, Any]:
    steps: list[dict[str, Any]] = []
    if step_if is not None:
        steps.append({"name": "guard", "if": step_if, "run": "true"})
    if script is not None:
        steps.append({"name": "poll", "with": {"script": script}})
    job: dict[str, Any] = {"steps": steps}
    if job_if is not None:
        job["if"] = job_if
    return {"aggregator": job}


def gate_pair(
    *,
    aggregator: dict[Any, Any] | None = None,
    rules: dict[Any, Any] | None = None,
) -> dict[str, dict[Any, Any]]:
    return {
        "required-aggregator.yml": (
            aggregator
            if aggregator is not None
            else wf(AGGREGATOR_ON, AGGREGATOR_NEW, aggregator_jobs())
        ),
        "rule-enforcement.yml": rules if rules is not None else wf(GATE_PR_ON, RULES_NEW),
    }


class RepositoryWorkflows(unittest.TestCase):
    """Positive: the checked-in workflows keep master runs apart."""

    def test_no_master_push_cancels_an_earlier_master_run(self) -> None:
        self.assertEqual(master_findings(load_all(), SERIALISED), [])

    def test_pull_requests_still_supersede_older_runs(self) -> None:
        self.assertEqual(pull_request_findings(load_all(), SERIALISED), [])

    def test_every_pull_request_workflow_supersedes_older_runs(self) -> None:
        self.assertEqual(unsuperseded_findings(load_all(), UNSUPERSEDED), [])

    def test_aggregator_is_sha_scoped_on_master(self) -> None:
        block = load_all()["required-aggregator.yml"]["concurrency"]
        self.assertNotEqual(
            render(block["group"], master_push("a" * 40)),
            render(block["group"], master_push("b" * 40)),
        )
        self.assertFalse(render(block["cancel-in-progress"], master_push("a" * 40)))

    def test_no_same_head_event_cancels_a_gate_workflow(self) -> None:
        self.assertEqual(same_head_findings(load_all()), [])

    def test_no_later_same_head_event_replaces_a_pending_edited_gate_run(self) -> None:
        self.assertEqual(pending_replacement_findings(load_all()), [])

    def test_aggregator_never_skips_on_pull_request_action(self) -> None:
        self.assertEqual(skip_findings(load_all()), [])

    def test_synchronize_still_cancels_stale_gate_workflow_runs(self) -> None:
        workflows = load_all()
        gates, findings = gate_workflows(workflows)
        self.assertEqual(findings, [])
        self.assertEqual(gates, {"required-aggregator.yml", "rule-enforcement.yml"})
        for name in sorted(gates):
            block = normalise(workflows[name]["concurrency"])
            self.assertEqual(
                render(block["group"], pr_event("a" * 40, "synchronize")),
                render(block["group"], pr_event("b" * 40, "synchronize")),
                name,
            )
            self.assertIs(
                render(block["cancel-in-progress"], pr_event("b" * 40, "synchronize")),
                True,
                name,
            )


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

    def test_negative_pr_workflow_without_block_is_refused(self) -> None:
        flow = wf({"pull_request": None})
        self.assertEqual(len(unsuperseded_findings({"x.yml": flow}, {})), 1)

    def test_negative_pr_workflow_without_cancel_is_refused(self) -> None:
        block = {"group": NEW["group"], "cancel-in-progress": False}
        flow = wf({"pull_request": None}, block)
        self.assertEqual(len(unsuperseded_findings({"x.yml": flow}, {})), 1)

    def test_positive_pr_workflow_with_superseding_block(self) -> None:
        flow = wf({"pull_request": None, "push": None}, NEW)
        self.assertEqual(unsuperseded_findings({"x.yml": flow}, {}), [])

    def test_boundary_listed_pr_workflow_without_block(self) -> None:
        flow = wf({"pull_request": None})
        self.assertEqual(unsuperseded_findings({"x.yml": flow}, {"x.yml": "why"}), [])

    def test_boundary_schedule_only_needs_no_block(self) -> None:
        flow = wf({"schedule": [{"cron": "0 0 * * *"}]})
        self.assertEqual(unsuperseded_findings({"x.yml": flow}, {}), [])

    def test_stale_unsuperseded_entries_are_refused(self) -> None:
        superseding = wf({"pull_request": None}, NEW)
        self.assertEqual(len(unsuperseded_findings({"x.yml": superseding}, {"x.yml": "y"})), 1)
        self.assertEqual(len(unsuperseded_findings({}, {"gone.yml": "y"})), 1)

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

    def test_negative_same_head_old_block_on_edited_gate_workflow_is_refused(self) -> None:
        agg = wf(
            {"pull_request": {"types": ["opened", "edited", "synchronize"]}},
            NEW,
            aggregator_jobs(),
        )
        self.assertEqual(
            same_head_findings(gate_pair(aggregator=agg)),
            [
                "required-aggregator.yml [workflow] a same-head edited run cancels the "
                "running run of the same head"
            ],
        )
        rules = wf({"pull_request": {"types": ["opened", "edited", "synchronize"]}}, OLD)
        self.assertEqual(
            same_head_findings(gate_pair(rules=rules)),
            [
                "rule-enforcement.yml [workflow] a same-head edited run cancels the "
                "running run of the same head"
            ],
        )

    def test_positive_same_head_synchronize_only_expression_is_accepted(self) -> None:
        self.assertEqual(same_head_findings(gate_pair()), [])

    def test_boundary_same_head_absent_types_evaluates_reopened(self) -> None:
        rules = wf({"pull_request": None}, OLD)
        self.assertEqual(
            same_head_findings(gate_pair(rules=rules)),
            [
                "rule-enforcement.yml [workflow] a same-head reopened run cancels the "
                "running run of the same head"
            ],
        )

    def test_boundary_same_head_opened_and_closed_are_exempt(self) -> None:
        rules = wf({"pull_request": {"types": ["opened", "closed", "synchronize"]}}, OLD)
        self.assertEqual(same_head_findings(gate_pair(rules=rules)), [])

    def test_negative_same_head_job_level_block_is_refused(self) -> None:
        rules = wf(
            {"pull_request": {"types": ["edited"]}},
            RULES_NEW,
            {"gates": {"concurrency": OLD}},
        )
        self.assertEqual(
            same_head_findings(gate_pair(rules=rules)),
            [
                "rule-enforcement.yml [gates] a same-head edited run cancels the "
                "running run of the same head"
            ],
        )

    def test_negative_missing_or_empty_edited_rerun_workflows_is_refused(self) -> None:
        for bad_script in (None, "const required = [];", "const editedRerunWorkflows = [];"):
            agg = wf(AGGREGATOR_ON, AGGREGATOR_NEW, aggregator_jobs(script=bad_script))
            _, problems = gate_workflows({"required-aggregator.yml": agg})
            self.assertEqual(len(problems), 1)
            self.assertEqual(len(same_head_findings({"required-aggregator.yml": agg})), 1)

    def test_negative_aggregator_job_if_skipping_edited_is_refused(self) -> None:
        agg = wf(
            AGGREGATOR_ON,
            AGGREGATOR_NEW,
            aggregator_jobs(job_if="github.event.action != 'edited'"),
        )
        found = skip_findings(gate_pair(aggregator=agg))
        self.assertEqual(len(found), 1)
        self.assertIn("required-aggregator.yml [aggregator]", found[0])

    def test_negative_aggregator_step_if_mentioning_edited_or_action_is_refused(self) -> None:
        for step_if in (
            "github.event.action != 'edited'",
            "github.head_ref != 'edited'",
            "github.event.action == 'synchronize'",
        ):
            agg = wf(AGGREGATOR_ON, AGGREGATOR_NEW, aggregator_jobs(step_if=step_if))
            self.assertEqual(len(skip_findings(gate_pair(aggregator=agg))), 1)

    def test_boundary_aggregator_draft_step_if_and_non_gate_workflow_are_accepted(self) -> None:
        agg = wf(
            AGGREGATOR_ON,
            AGGREGATOR_NEW,
            aggregator_jobs(
                step_if="github.event_name == 'pull_request' && github.event.pull_request.draft == true"
            ),
        )
        pair = {
            **gate_pair(aggregator=agg),
            "advisory.yml": wf({"pull_request": {"types": ["edited"]}}, OLD),
        }
        self.assertEqual(skip_findings(pair), [])
        self.assertEqual(same_head_findings(pair), [])

    def test_negative_shared_pr_group_replaces_pending_edited_rules_run(self) -> None:
        rules = wf(
            GATE_PR_ON,
            {
                "group": "rule-enforcement-${{ github.event.pull_request.number }}",
                "cancel-in-progress": "${{ github.event.action == 'synchronize' }}",
            },
        )
        self.assertEqual(
            pending_replacement_findings(gate_pair(rules=rules)),
            [
                "rule-enforcement.yml [workflow] an edited run shares its concurrency group "
                "with a later same-head edited, opened, ready_for_review, reopened, synchronize "
                "run, which replaces it while pending"
            ],
        )

    def test_positive_per_edit_group_isolates_pending_edited_rules_run(self) -> None:
        pair = gate_pair()
        self.assertEqual(pending_replacement_findings(pair), [])
        self.assertEqual(same_head_findings(pair), [])

    def test_positive_run_id_per_edit_group_separates_same_second_edits(self) -> None:
        sha_a = "a" * 40
        edit_one = pr_event(sha_a, "edited", STAMP_T1, run_id="run-edit-1")
        edit_two = pr_event(sha_a, "edited", STAMP_T1, run_id="run-edit-2")
        self.assertNotEqual(
            render(RULES_NEW["group"], edit_one),
            render(RULES_NEW["group"], edit_two),
        )
        pair = gate_pair()
        self.assertEqual(pending_replacement_findings(pair), [])
        self.assertEqual(same_head_findings(pair), [])

    def test_negative_updated_at_per_edit_group_collides_on_same_second_edits(self) -> None:
        rules = wf(
            GATE_PR_ON,
            {
                "group": (
                    "rule-enforcement-${{ github.event.pull_request.number }}-"
                    "${{ github.event.action == 'edited' && "
                    "github.event.pull_request.updated_at || 'head' }}"
                ),
                "cancel-in-progress": "${{ github.event.action == 'synchronize' }}",
            },
        )
        self.assertEqual(
            pending_replacement_findings(gate_pair(rules=rules)),
            [
                "rule-enforcement.yml [workflow] an edited run shares its concurrency group "
                "with a later same-head same-second edited run, which replaces it while pending"
            ],
        )

    def test_negative_action_keyed_group_cancels_repeated_same_head_events(self) -> None:
        group = "rules-${{ github.event.pull_request.number }}-${{ github.event.action }}"
        cancelling = wf(GATE_PR_ON, {"group": group, "cancel-in-progress": True})
        self.assertEqual(
            same_head_findings(gate_pair(rules=cancelling)),
            [
                "rule-enforcement.yml [workflow] a same-head edited run cancels the "
                "running run of the same head",
                "rule-enforcement.yml [workflow] a same-head reopened run cancels the "
                "running run of the same head",
                "rule-enforcement.yml [workflow] a same-head ready_for_review run cancels "
                "the running run of the same head",
            ],
        )
        queued = wf(
            GATE_PR_ON,
            {"group": group, "cancel-in-progress": "${{ github.event.action == 'synchronize' }}"},
        )
        self.assertEqual(same_head_findings(gate_pair(rules=queued)), [])

    def test_negative_per_edit_group_with_unconditional_cancel_flags_head_group_actions(
        self,
    ) -> None:
        rules = wf(GATE_PR_ON, {"group": RULES_NEW["group"], "cancel-in-progress": True})
        self.assertEqual(
            same_head_findings(gate_pair(rules=rules)),
            [
                "rule-enforcement.yml [workflow] a same-head reopened run cancels the "
                "running run of the same head",
                "rule-enforcement.yml [workflow] a same-head ready_for_review run cancels "
                "the running run of the same head",
            ],
        )

    def test_boundary_gate_workflow_without_edited_type_needs_no_per_edit_group(self) -> None:
        rules = wf(
            {"pull_request": {"types": ["opened", "synchronize", "reopened"]}},
            {
                "group": "rule-enforcement-${{ github.event.pull_request.number }}",
                "cancel-in-progress": "${{ github.event.action == 'synchronize' }}",
            },
        )
        self.assertEqual(pending_replacement_findings(gate_pair(rules=rules)), [])


if __name__ == "__main__":
    unittest.main()
