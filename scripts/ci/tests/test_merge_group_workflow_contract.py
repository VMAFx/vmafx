#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fail closed when workflows feeding merge_group lack the merge_group trigger (ADR-3556).

A merge_group workflow's pull_request cost guard must exclude merge_group with !=.
"""

from __future__ import annotations

import re
import shutil
import sys
import tempfile
import unittest
from collections.abc import Mapping
from pathlib import Path
from typing import Any, TypeGuard

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import ci_router as wr  # noqa: E402
from scripts.ci import required_aggregator_harness as harness  # noqa: E402

WORKFLOWS_DIR = ROOT / ".github" / "workflows"
AGGREGATOR_FILE = "required-aggregator.yml"
FORBIDDEN_MERGE_GROUP_WORKFLOWS = (
    "release-dry-run.yml",
    "rule-enforcement.yml",
    "scorecard.yml",
)
PRAETOR_LOCKED_BARE_MERGE_GROUP: dict[str, str] = {
    "praetor-api.yml": (
        "praetor locks praetor-api.yml byte for byte (ADR-1506, ADR-3061); its bare "
        "merge_group: trigger defaults to checks_requested across all branches including master"
    ),
}
LOCAL_WORKFLOW_RE = re.compile(r"^\./\.github/workflows/([^/@]+\.ya?ml)$")
PR_COST_GUARD_RE = re.compile(r"github\.event_name\s*!=\s*'pull_request'(?!\s*\|\|)")
MERGE_GROUP_EXCLUSION_RE = re.compile(r"github\.event_name\s*!=\s*'merge_group'")


def _expand_job_names(job_id: str, job_cfg: dict[str, Any]) -> list[str]:
    """Expand static and matrix-templated check-run names for a single job (ci_router rows)."""
    raw_name = str(job_cfg.get("name", job_id))
    if "matrix." not in raw_name:
        return [raw_name]
    return [wr._expand(raw_name, row) for row in wr._matrix_rows(job_cfg)]


def _callee_contexts(
    wf_name: str,
    caller_id: str,
    caller_names: list[str],
    uses: Any,
    docs: dict[str, dict[str, Any]],
) -> list[tuple[str, str, str]]:
    """Expand contexts produced by a local ``workflow_call`` target."""
    match = LOCAL_WORKFLOW_RE.match(str(uses)) if isinstance(uses, str) else None
    callee_doc = docs.get(match.group(1), {}) if match is not None else {}
    callee_jobs = callee_doc.get("jobs")
    if not isinstance(callee_jobs, dict):
        return []
    triples: list[tuple[str, str, str]] = []
    for c_id, c_cfg in callee_jobs.items():
        if not isinstance(c_cfg, dict):
            continue
        for c_name in _expand_job_names(str(c_id), c_cfg):
            triples.append((c_name, wf_name, caller_id))
            triples.extend((f"{prefix} / {c_name}", wf_name, caller_id) for prefix in caller_names)
    return triples


def _workflow_reported_contexts(
    wf_name: str, doc: dict[str, Any], docs: dict[str, dict[str, Any]]
) -> list[tuple[str, str, str]]:
    """Return ``(context_name, workflow_file, job_id)`` produced by ``wf_name``."""
    jobs = doc.get("jobs")
    if not isinstance(jobs, dict):
        return []
    triples: list[tuple[str, str, str]] = []
    for job_id, job_cfg in jobs.items():
        if not isinstance(job_cfg, dict):
            continue
        jid = str(job_id)
        caller_names = _expand_job_names(jid, job_cfg)
        triples.extend((name, wf_name, jid) for name in caller_names)
        triples.extend(_callee_contexts(wf_name, jid, caller_names, job_cfg.get("uses"), docs))
    return triples


def _aggregator_sets(workflows_dir: Path) -> tuple[set[str], set[str], set[str], list[str]]:
    """Extract ``(required, pull_request_only, merge_group_exempt, errors)`` from aggregator JS."""
    errors: list[str] = []
    script = harness._embedded_script((workflows_dir / AGGREGATOR_FILE).read_text(encoding="utf-8"))
    required = set(harness._required_names(script))
    pr_only = set(harness._javascript_string_list(script, "pullRequestOnly", "pullRequestOnly"))
    if "const mergeGroupExempt = [" not in script:
        errors.append(f"{AGGREGATOR_FILE}: missing mergeGroupExempt list")
        return required, pr_only, set(), errors
    mg_exempt = set(harness._javascript_string_list(script, "mergeGroupExempt", "mergeGroupExempt"))
    if mg_exempt != {"Scorecard Master Gate"}:
        errors.append(
            f"{AGGREGATOR_FILE}: mergeGroupExempt must be ['Scorecard Master Gate'], got {sorted(mg_exempt)}"
        )
    return required, pr_only, mg_exempt, errors


def compute_merge_group_feeding_map(
    workflows_dir: Path = WORKFLOWS_DIR,
) -> tuple[list[tuple[str, str, str]], set[str], list[str]]:
    """Compute ``(owed_triples, pr_only_workflows, errors)`` across ``workflows_dir``."""
    required, pr_only, mg_exempt, errors = _aggregator_sets(workflows_dir)
    owed = required - pr_only - {"Scorecard PR Gate"} - mg_exempt
    docs = wr.load_workflows(workflows_dir)
    triples: list[tuple[str, str, str]] = []
    pr_only_workflows: set[str] = set()
    for wf_name, doc in docs.items():
        if wf_name == AGGREGATOR_FILE or set(wr.triggers_of(doc)) == {"workflow_call"}:
            continue
        reported = _workflow_reported_contexts(wf_name, doc, docs)
        req_in_wf = {ctx for ctx, _, _ in reported if ctx in required}
        if req_in_wf and req_in_wf <= pr_only:
            pr_only_workflows.add(wf_name)
        triples.extend((ctx, wf, jid) for ctx, wf, jid in reported if ctx in owed)
    mapped = {ctx for ctx, _, _ in triples}
    errors.extend(f"unmapped owed merge_group context: {ctx}" for ctx in sorted(owed - mapped))
    return sorted(set(triples)), pr_only_workflows, errors


def _validate_merge_group_trigger(wf_name: str, on_map: dict[str, Any]) -> str | None:
    """Validate ``on.merge_group`` for a workflow that owes ``merge_group`` runs.

    ``ci_router.triggers_of`` turns a bare ``merge_group:`` into ``{}``; GitHub reads both
    the bare key and an empty mapping as default types on all branches.
    """
    if "merge_group" not in on_map:
        return f"{wf_name}: missing merge_group trigger"
    cfg = on_map["merge_group"]
    if wf_name in PRAETOR_LOCKED_BARE_MERGE_GROUP and not cfg:
        return None
    if not isinstance(cfg, dict):
        return f"{wf_name}: merge_group must declare types and branches (got {cfg!r})"
    types = cfg.get("types")
    branches = cfg.get("branches")
    if types != ["checks_requested"]:
        return f"{wf_name}: merge_group.types must be ['checks_requested'], got {types!r}"
    if branches != ["master"]:
        return f"{wf_name}: merge_group.branches must be ['master'], got {branches!r}"
    return None


def _is_unadapted_pr_cost_guard(guard: object) -> TypeGuard[str]:
    """True when guard skips pull_request for cost without excluding merge_group through !=."""
    if not isinstance(guard, str):
        return False
    return bool(PR_COST_GUARD_RE.search(guard)) and not MERGE_GROUP_EXCLUSION_RE.search(guard)


def _job_cost_guard_errors(wf_name: str, job_id: str, job_cfg: Mapping[str, Any]) -> list[str]:
    """Return errors for job or step ``pull_request`` cost guards missing ``merge_group``."""
    candidates: list[tuple[str, object]] = [("if", job_cfg.get("if"))]
    steps = job_cfg.get("steps")
    if isinstance(steps, list):
        for idx, step in enumerate(steps):
            if isinstance(step, Mapping):
                candidates.append((f"steps[{idx}].if", step.get("if")))
    return [
        f"{wf_name}: job {job_id} ({loc}) skips pull_request without merge_group: {guard}"
        for loc, guard in candidates
        if _is_unadapted_pr_cost_guard(guard)
    ]


def check_merge_group_cost_guards(docs: Mapping[str, Mapping[str, Any]]) -> list[str]:
    """Refuse job or step ``pull_request`` cost guards on ``merge_group`` workflows."""
    errors: list[str] = []
    for wf_name in sorted(docs):
        doc = docs[wf_name]
        if "merge_group" not in wr.triggers_of(doc):
            continue
        jobs = doc.get("jobs")
        if not isinstance(jobs, Mapping):
            continue
        for job_id, job_cfg in jobs.items():
            if isinstance(job_cfg, Mapping):
                errors.extend(_job_cost_guard_errors(wf_name, str(job_id), job_cfg))
    return errors


SECRET_RE = re.compile(r"secrets\.(?!GITHUB_TOKEN\b)[A-Za-z_][A-Za-z0-9_]*", re.I)
GITHUB_TOKEN_EXPR = "${{ " + "github.token }}"
PRIVATE_EXPR = "${{ " + "secrets.PRIVATE }}"
ACTION_PATH = ROOT / ".github/actions/runner-probe/action.yml"
PROVENANCE_IF = "github.event_name == 'merge_group'"
PROBE_IF = "github.event_name != 'merge_group' || steps.provenance.outputs.same_repo == 'true'"


def _secret_steps(job: Mapping[str, Any]) -> list[int]:
    steps = job.get("steps", [])
    return (
        [i for i, step in enumerate(steps) if SECRET_RE.search(str(step))]
        if isinstance(steps, list)
        else []
    )


def _guarded_steps(job: Mapping[str, Any], *, composite: bool = False) -> bool:
    steps = job.get("steps", [])
    if not isinstance(steps, list):
        return False
    secret_indices = _secret_steps(job)
    if composite:
        secret_indices = [
            i
            for i, step in enumerate(steps)
            if "inputs.token" in str(step) or SECRET_RE.search(str(step))
        ]
    for i, step in enumerate(steps):
        if not isinstance(step, Mapping):
            continue
        if (
            step.get("id") == "provenance"
            and step.get("if") == PROVENANCE_IF
            and step.get("run") == "python3 scripts/ci/merge_group_provenance.py"
            and step.get("env") == {"GH_TOKEN": GITHUB_TOKEN_EXPR}
            and secret_indices
            and all(i < j for j in secret_indices)
        ):
            return all(steps[j].get("if") == PROBE_IF for j in secret_indices)
    return False


def _guarded_probe(job: Mapping[str, Any], action_path: Path = ACTION_PATH) -> bool:
    steps = job.get("steps", [])
    if not isinstance(steps, list):
        return False
    secret_indices = _secret_steps(job)
    if _guarded_steps(job):
        return True
    try:
        action = yaml.safe_load(action_path.read_text(encoding="utf-8"))
        runs = action["runs"]
        if runs.get("using") != "composite" or not _guarded_steps(runs, composite=True):
            return False
    except OSError, KeyError, TypeError, AttributeError, yaml.YAMLError:
        return False
    return bool(secret_indices) and all(
        isinstance(steps[i], Mapping)
        and steps[i].get("id") == "probe"
        and steps[i].get("uses") == "./.github/actions/runner-probe"
        and SECRET_RE.search(str(steps[i].get("with", {}).get("token", "")))
        for i in secret_indices
    )


def check_merge_group_sensitive_jobs(docs: Mapping[str, Mapping[str, Any]]) -> list[str]:
    """Require a provenance probe before secret use or self-hosted execution."""
    errors = []
    for name, doc in sorted(docs.items()):
        if "merge_group" not in wr.triggers_of(doc):
            continue
        jobs = doc.get("jobs", {})
        if not isinstance(jobs, Mapping):
            continue
        for job_id, job in jobs.items():
            if not isinstance(job, Mapping):
                continue
            secret = bool(SECRET_RE.search(str(job)))
            hosted = "self-hosted" in str(job.get("runs-on", ""))
            condition = str(job.get("if", ""))
            excluded = MERGE_GROUP_EXCLUSION_RE.search(condition) and "||" not in condition
            if not (secret or hosted) or excluded:
                continue
            needs = job.get("needs", [])
            needs = [needs] if isinstance(needs, str) else needs
            guarded = _guarded_probe(job) if secret else False
            if hosted and not secret and isinstance(needs, list):
                guarded = any(
                    isinstance(jobs.get(dep), Mapping)
                    and _guarded_probe(jobs[dep])
                    and f"needs.{dep}.outputs.available == 'true'" in str(job.get("if", ""))
                    for dep in needs
                )
            if not guarded:
                errors.append(f"{name}: job {job_id} lacks merge_group provenance guard")
    return errors


def check_merge_group_workflow_contract(workflows_dir: Path = WORKFLOWS_DIR) -> list[str]:
    """Return all ``merge_group`` trigger contract violations under ``workflows_dir``."""
    triples, pr_only_workflows, errors = compute_merge_group_feeding_map(workflows_dir)
    docs = wr.load_workflows(workflows_dir)
    feeding_workflows = {wf for _, wf, _ in triples}
    for wf_name in sorted(feeding_workflows | {AGGREGATOR_FILE}):
        problem = _validate_merge_group_trigger(wf_name, wr.triggers_of(docs.get(wf_name, {})))
        if problem is not None:
            errors.append(problem)
    for locked_wf in sorted(PRAETOR_LOCKED_BARE_MERGE_GROUP):
        on_map = wr.triggers_of(docs.get(locked_wf, {}))
        if locked_wf not in feeding_workflows or on_map.get("merge_group") != {}:
            errors.append(f"{locked_wf}: stale PRAETOR_LOCKED_BARE_MERGE_GROUP exception")
    forbidden = set(FORBIDDEN_MERGE_GROUP_WORKFLOWS) | pr_only_workflows
    for forbidden_wf in sorted(forbidden):
        if "merge_group" in wr.triggers_of(docs.get(forbidden_wf, {})):
            errors.append(f"{forbidden_wf}: must not declare merge_group trigger")
    errors.extend(check_merge_group_cost_guards(docs))
    return errors


def _mutate_workflow_copy(
    tmp: str, wf_name: str, merge_group_value: Any, *, drop: bool = False
) -> list[str]:
    """Copy ``WORKFLOWS_DIR`` into ``tmp``, mutate ``wf_name``'s trigger and run the check."""
    copied = Path(tmp) / "workflows"
    shutil.copytree(WORKFLOWS_DIR, copied)
    target = copied / wf_name
    doc = yaml.safe_load(target.read_text(encoding="utf-8"))
    on_key = "on" if "on" in doc else True
    if drop:
        doc[on_key].pop("merge_group", None)
    else:
        doc[on_key]["merge_group"] = merge_group_value
    target.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8", newline="\n")
    return check_merge_group_workflow_contract(copied)


def _replace_in_workflow_copy(tmp: str, wf_name: str, old: str, new: str) -> list[str]:
    """Copy ``WORKFLOWS_DIR`` to ``tmp``, replace one substring in ``wf_name``, and check."""
    copied = Path(tmp) / "workflows"
    shutil.copytree(WORKFLOWS_DIR, copied)
    target = copied / wf_name
    text = target.read_text(encoding="utf-8")
    if text.count(old) != 1:
        raise AssertionError(f"{wf_name}: expected 1 match for {old!r}, found {text.count(old)}")
    target.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    return check_merge_group_workflow_contract(copied)


class MergeGroupWorkflowContractTests(unittest.TestCase):
    """Static and planted-mutation tests for the merge_group workflow trigger contract."""

    def test_repository_workflows_satisfy_merge_group_contract(self) -> None:
        """Require merge_group on required-aggregator.yml and every feeding workflow only."""
        self.assertEqual(check_merge_group_workflow_contract(WORKFLOWS_DIR), [])

    def test_sensitive_jobs_have_provenance_guards(self) -> None:
        self.assertEqual(check_merge_group_sensitive_jobs(wr.load_workflows(WORKFLOWS_DIR)), [])

    def test_sycl_sensitive_jobs_have_provenance_guards(self) -> None:
        docs = wr.load_workflows(WORKFLOWS_DIR)
        self.assertEqual(
            check_merge_group_sensitive_jobs({"sycl-parity.yml": docs["sycl-parity.yml"]}), []
        )

    def test_sensitive_job_planted_cases(self) -> None:
        secret = {"env": {"TOKEN": PRIVATE_EXPR}, "if": PROBE_IF}
        provenance = {
            "id": "provenance",
            "if": PROVENANCE_IF,
            "run": "python3 scripts/ci/merge_group_provenance.py",
            "env": {"GH_TOKEN": GITHUB_TOKEN_EXPR},
        }
        doc: dict[str, Any] = {
            "sample.yml": {"on": {"merge_group": {}}, "jobs": {"probe": {"steps": [secret]}}}
        }
        job = doc["sample.yml"]["jobs"]["probe"]
        self.assertTrue(check_merge_group_sensitive_jobs(doc))
        job["steps"] = [secret, provenance]
        self.assertTrue(check_merge_group_sensitive_jobs(doc))
        job["steps"] = [provenance, secret]
        self.assertEqual(check_merge_group_sensitive_jobs(doc), [])
        job["steps"] = [secret]
        job["if"] = "github.event_name != 'merge_group'"
        self.assertEqual(check_merge_group_sensitive_jobs(doc), [])

    def test_composite_guard_planted_cases(self) -> None:
        action = yaml.safe_load(ACTION_PATH.read_text(encoding="utf-8"))
        steps = action["runs"]["steps"]
        job = {
            "steps": [
                {
                    "id": "probe",
                    "uses": "./.github/actions/runner-probe",
                    "with": {"token": PRIVATE_EXPR},
                }
            ]
        }
        self.assertTrue(_guarded_probe(job))
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "action.yml"
            action["runs"]["steps"] = steps[1:]
            copy.write_text(yaml.safe_dump(action), encoding="utf-8")
            self.assertFalse(_guarded_probe(job, copy))
            action["runs"]["steps"] = list(reversed(steps))
            copy.write_text(yaml.safe_dump(action), encoding="utf-8")
            self.assertFalse(_guarded_probe(job, copy))

    def test_missing_merge_group_trigger_on_feeding_workflow_fails_closed(self) -> None:
        """Fail when a feeding workflow lacks its merge_group trigger."""
        with tempfile.TemporaryDirectory() as tmp:
            errors = _mutate_workflow_copy(tmp, "build.yml", None, drop=True)
        self.assertIn("build.yml: missing merge_group trigger", errors)

    def test_wrong_merge_group_types_or_branches_fails_closed(self) -> None:
        """Fail when a feeding workflow declares bare merge_group or wrong branches/types."""
        with tempfile.TemporaryDirectory() as tmp:
            bad = {"types": ["checks_requested"], "branches": ["main"]}
            errors = _mutate_workflow_copy(tmp, "go-ci.yml", bad)
        self.assertTrue(any(err.startswith("go-ci.yml: merge_group.branches") for err in errors))

    def test_forbidden_workflows_gaining_merge_group_fail_closed(self) -> None:
        """Fail when release-dry-run.yml, scorecard.yml or rule-enforcement.yml gains merge_group."""
        valid = {"types": ["checks_requested"], "branches": ["master"]}
        for wf_name in FORBIDDEN_MERGE_GROUP_WORKFLOWS:
            with self.subTest(workflow=wf_name), tempfile.TemporaryDirectory() as tmp:
                errors = _mutate_workflow_copy(tmp, wf_name, valid)
                self.assertIn(f"{wf_name}: must not declare merge_group trigger", errors)

    def test_planted_cost_guard_without_merge_group_fails_closed(self) -> None:
        """Fail when docker-publish-tester.yml build-gpu drops its merge_group exclusion."""
        flagged = "github.event_name != 'pull_request' && github.event_name != 'schedule'"
        with tempfile.TemporaryDirectory() as tmp:
            errors = _replace_in_workflow_copy(
                tmp,
                "docker-publish-tester.yml",
                " && github.event_name != 'merge_group'",
                "",
            )
        self.assertEqual(
            errors,
            [
                "docker-publish-tester.yml: job build-gpu (if) skips pull_request without "
                f"merge_group: {flagged}"
            ],
        )

    def test_cost_guard_boundary_cases(self) -> None:
        """Flag only unadapted PR cost guards on merge_group workflows across jobs and steps.

        An equality comparison on merge_group is no exclusion.
        """
        flagged = "github.event_name != 'pull_request' && github.event_name != 'schedule'"
        draft = "(github.event_name != 'pull_request' || github.event.pull_request.draft == false)"
        adapted = "github.event_name != 'pull_request' && github.event_name != 'merge_group'"
        equality = (
            "github.event_name != 'pull_request' "
            "&& github.event_name == 'merge_group' "
            "&& github.event_name != 'schedule'"
        )
        queued_docs: dict[str, dict[str, Any]] = {
            "sample.yml": {
                "on": {"merge_group": {"types": ["checks_requested"], "branches": ["master"]}},
                "jobs": {
                    "cost-job": {"if": flagged},
                    "draft-job": {
                        "if": draft,
                        "steps": [
                            {"name": "Draft step", "if": draft},
                            {"name": "Cost step", "if": flagged},
                            {"name": "Bool step", "if": True},
                        ],
                    },
                    "adapted-job": {"if": adapted},
                    "equality-job": {"if": equality},
                },
            }
        }
        self.assertEqual(
            check_merge_group_cost_guards(queued_docs),
            [
                f"sample.yml: job cost-job (if) skips pull_request without merge_group: {flagged}",
                "sample.yml: job draft-job (steps[1].if) skips pull_request without "
                f"merge_group: {flagged}",
                "sample.yml: job equality-job (if) skips pull_request without "
                f"merge_group: {equality}",
            ],
        )
        unqueued_docs: dict[str, dict[str, Any]] = {
            "push-only.yml": {
                "on": {"push": {"branches": ["master"]}},
                "jobs": {"cost-job": {"if": flagged, "steps": [{"if": flagged}]}},
            }
        }
        self.assertEqual(check_merge_group_cost_guards(unqueued_docs), [])


if __name__ == "__main__":
    unittest.main()
