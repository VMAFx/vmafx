#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fail closed when workflows feeding merge_group lack the merge_group trigger (ADR-3556)."""

from __future__ import annotations

import itertools
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

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
MATRIX_EXPR_RE = re.compile(r"\$\{\{\s*matrix\.([A-Za-z0-9_-]+)\s*\}\}")
LOCAL_WORKFLOW_RE = re.compile(r"^\./\.github/workflows/([^/@]+\.ya?ml)$")


def _load_workflows(workflows_dir: Path) -> dict[str, dict[str, Any]]:
    """Parse every workflow YAML file in ``workflows_dir``."""
    docs: dict[str, dict[str, Any]] = {}
    for path in sorted(workflows_dir.iterdir()):
        if path.suffix in (".yml", ".yaml") and path.is_file():
            parsed = yaml.safe_load(path.read_text(encoding="utf-8"))
            if isinstance(parsed, dict):
                docs[path.name] = parsed
    return docs


def _on_mapping(doc: dict[str, Any]) -> dict[str, Any]:
    """Normalize a workflow's ``on`` block into an event-name mapping."""
    raw = doc.get("on", doc.get(True))
    if isinstance(raw, str):
        return {raw: None}
    if isinstance(raw, list):
        return {str(item): None for item in raw}
    if isinstance(raw, dict):
        return {str(key): value for key, value in raw.items()}
    return {}


def _row_matches_exclude(row: dict[str, Any], excludes: list[dict[str, Any]]) -> bool:
    """Return True when ``row`` matches every key-value pair of any exclude entry."""
    return any(all(row.get(key) == val for key, val in entry.items()) for entry in excludes)


def _matrix_rows(matrix_cfg: Any) -> list[dict[str, Any]]:
    """Expand a job's ``strategy.matrix`` dimensions, includes and excludes."""
    if not isinstance(matrix_cfg, dict):
        return [{}]
    dims = {
        key: val
        for key, val in matrix_cfg.items()
        if key not in ("include", "exclude") and isinstance(val, list) and val
    }
    keys = list(dims.keys())
    rows = (
        [
            dict(zip(keys, combo, strict=True))
            for combo in itertools.product(*(dims[k] for k in keys))
        ]
        if keys
        else []
    )
    rows.extend(dict(ex) for ex in (matrix_cfg.get("include") or []) if isinstance(ex, dict))
    excludes = [ex for ex in (matrix_cfg.get("exclude") or []) if isinstance(ex, dict)]
    filtered = [row for row in rows if not _row_matches_exclude(row, excludes)]
    return filtered or [{}]


def _expand_job_names(job_id: str, job_cfg: dict[str, Any]) -> list[str]:
    """Expand static and matrix-templated check-run names for a single job."""
    raw_name = str(job_cfg.get("name", job_id))
    if "matrix." not in raw_name:
        return [raw_name]
    strategy = job_cfg.get("strategy")
    matrix_cfg = strategy.get("matrix") if isinstance(strategy, dict) else None
    return [
        MATRIX_EXPR_RE.sub(lambda m, r=row: str(r.get(m.group(1), m.group(0))), raw_name)
        for row in _matrix_rows(matrix_cfg)
    ]


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
    docs = _load_workflows(workflows_dir)
    triples: list[tuple[str, str, str]] = []
    pr_only_workflows: set[str] = set()
    for wf_name, doc in docs.items():
        if wf_name == AGGREGATOR_FILE or set(_on_mapping(doc)) == {"workflow_call"}:
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
    """Validate ``on.merge_group`` for a workflow that owes ``merge_group`` runs."""
    if "merge_group" not in on_map:
        return f"{wf_name}: missing merge_group trigger"
    cfg = on_map["merge_group"]
    if wf_name in PRAETOR_LOCKED_BARE_MERGE_GROUP and cfg is None:
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


def check_merge_group_workflow_contract(workflows_dir: Path = WORKFLOWS_DIR) -> list[str]:
    """Return all ``merge_group`` trigger contract violations under ``workflows_dir``."""
    triples, pr_only_workflows, errors = compute_merge_group_feeding_map(workflows_dir)
    docs = _load_workflows(workflows_dir)
    feeding_workflows = {wf for _, wf, _ in triples}
    for wf_name in sorted(feeding_workflows | {AGGREGATOR_FILE}):
        problem = _validate_merge_group_trigger(wf_name, _on_mapping(docs.get(wf_name, {})))
        if problem is not None:
            errors.append(problem)
    for locked_wf in sorted(PRAETOR_LOCKED_BARE_MERGE_GROUP):
        on_map = _on_mapping(docs.get(locked_wf, {}))
        if locked_wf not in feeding_workflows or on_map.get("merge_group", "absent") is not None:
            errors.append(f"{locked_wf}: stale PRAETOR_LOCKED_BARE_MERGE_GROUP exception")
    forbidden = set(FORBIDDEN_MERGE_GROUP_WORKFLOWS) | pr_only_workflows
    for forbidden_wf in sorted(forbidden):
        if "merge_group" in _on_mapping(docs.get(forbidden_wf, {})):
            errors.append(f"{forbidden_wf}: must not declare merge_group trigger")
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


class MergeGroupWorkflowContractTests(unittest.TestCase):
    """Static and planted-mutation tests for the merge_group workflow trigger contract."""

    def test_repository_workflows_satisfy_merge_group_contract(self) -> None:
        """Require merge_group on required-aggregator.yml and every feeding workflow only."""
        self.assertEqual(check_merge_group_workflow_contract(WORKFLOWS_DIR), [])

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


if __name__ == "__main__":
    unittest.main()
