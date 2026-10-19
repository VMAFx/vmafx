#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Work out, offline, which jobs of the real workflow files run for a synthetic event.

This is the engine of ``scripts/ci/tests/test_ci_routing_contract.py`` (ADR-2169).
It reads ``.github/workflows/*.yml``, decides which workflows the event triggers
(event, activity types, branch and tag filters), evaluates every job's ``needs``
and ``if`` with ``ci_expressions``, and takes the tier decision from the real
``ci_tier.py``. A job that runs is assumed to succeed; a job whose output a
condition reads, other than the tier job's, reads as ``'true'``, so a planner
selects its work (the hardware probes read ``'false'``, as an unprovisioned
runner would).

It simulates routing, not behaviour: a step-level condition inside a job is not
evaluated, and a ``paths:`` filter is assumed to match.
"""

from __future__ import annotations

import fnmatch
import itertools
import re
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

from scripts.ci import ci_expressions, ci_tier

TIER_WORKFLOW = "./.github/workflows/ci-tier.yml"
DEFAULT_TYPES = {
    "pull_request": ("opened", "synchronize", "reopened"),
    "pull_request_target": ("opened", "synchronize", "reopened"),
}
OUTPUT_DEFAULTS = {"available": "false", "publish": "false", "docs": "false"}
MAX_JOBS = 256


class Outputs(dict[str, object]):
    """Outputs of a job that ran: unknown keys read as a planner selecting its work."""

    def get(self, key: str, default: object = None) -> object:
        return super().get(key, OUTPUT_DEFAULTS.get(key, "true"))


@dataclass(frozen=True)
class SyntheticEvent:
    """One synthetic GitHub event."""

    name: str  # push | pull_request | pull_request_target | schedule | workflow_dispatch
    action: str = ""
    ref: str = "refs/heads/master"
    base_ref: str = "master"
    head_ref: str = ""
    head_repo: str = ""
    repository: str = "VMAFx/vmafx"
    draft: bool = False
    author: str = "someone"
    author_type: str = "User"
    labels: tuple[str, ...] = ()
    label: str = ""
    paths: tuple[str, ...] = (".release-please-manifest.json",)


@dataclass
class JobRun:
    workflow: str
    job: str
    names: tuple[str, ...]
    ran: bool
    legs: dict[str, bool] = field(default_factory=dict)


def load_workflows(directory: Path) -> dict[str, dict[str, Any]]:
    """Parse every workflow in a directory, keyed by file name."""
    workflows: dict[str, dict[str, Any]] = {}
    for path in sorted(directory.glob("*.yml")):
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
        data["on"] = data.pop(True, data.get("on"))
        workflows[path.name] = data
    return workflows


def triggers_of(workflow: Mapping[str, Any]) -> dict[str, dict[str, Any]]:
    """Normalise ``on:`` to ``{event: filters}``."""
    trigger = workflow.get("on")
    if isinstance(trigger, str):
        return {trigger: {}}
    if isinstance(trigger, list):
        return {name: {} for name in trigger}
    return {name: (value or {}) for name, value in (trigger or {}).items()}


def _matches(patterns: Sequence[str] | str | None, value: str) -> bool:
    items = [patterns] if isinstance(patterns, str) else list(patterns or [])
    return any(fnmatch.fnmatchcase(value, pattern) for pattern in items)


def _push_matches(filters: Mapping[str, Any], event: SyntheticEvent) -> bool:
    is_tag = event.ref.startswith("refs/tags/")
    short = event.ref.split("/", 2)[2]
    if "tags" in filters or "tags-ignore" in filters:
        return is_tag and _matches(filters.get("tags", ["**"]), short)
    if "branches" in filters:
        return not is_tag and _matches(filters["branches"], short)
    return True


def is_triggered(workflow: Mapping[str, Any], event: SyntheticEvent) -> bool:
    """Whether the workflow's ``on:`` selects this event."""
    filters = triggers_of(workflow).get(event.name)
    if filters is None:
        return False
    if event.name == "push":
        return _push_matches(filters, event)
    if event.name in DEFAULT_TYPES:
        types = filters.get("types", DEFAULT_TYPES[event.name])
        branches = filters.get("branches")
        return event.action in types and (branches is None or _matches(branches, event.base_ref))
    return True


def _labels(event: SyntheticEvent) -> list[dict[str, str]]:
    return [{"name": name} for name in event.labels]


def build_context(event: SyntheticEvent) -> dict[str, Any]:
    """The ``github`` context an event produces, as far as conditions read it."""
    payload: dict[str, Any] = {"action": event.action or None}
    if event.name in DEFAULT_TYPES:
        payload["pull_request"] = {
            "draft": event.draft,
            "labels": _labels(event),
            "head": {"ref": event.head_ref, "repo": {"full_name": event.head_repo}},
            "base": {"ref": event.base_ref},
            "user": {"login": event.author, "type": event.author_type},
        }
    if event.label:
        payload["label"] = {"name": event.label}
    return {
        "github": {
            "event_name": event.name,
            "event": payload,
            "repository": event.repository,
            "ref": event.ref,
        },
        "vars": {},
        "inputs": {},
    }


def tier_decision(event: SyntheticEvent, config_path: Path) -> ci_tier.Decision:
    """The real ``ci_tier.py`` decision for a synthetic event."""
    config = ci_tier.load_config(config_path)
    facts = ci_tier.Event(
        name=event.name,
        repository=event.repository,
        head_repository=event.head_repo,
        head_ref=event.head_ref,
        author=event.author,
        author_type=event.author_type,
        labels=event.labels,
    )
    return ci_tier.decide(
        facts,
        config,
        release_exempt=lambda: ci_tier.release_exempt_from_script(facts, event.paths),
    )


def _as_list(value: object) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    return [str(item) for item in value]  # type: ignore[attr-defined]


def _matrix_rows(job: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Expand ``strategy.matrix`` (list-valued keys times ``include`` rows)."""
    matrix = (job.get("strategy") or {}).get("matrix")
    if not isinstance(matrix, dict):
        return [{}]
    axes = {
        k: v for k, v in matrix.items() if isinstance(v, list) and k not in ("include", "exclude")
    }
    rows: list[dict[str, Any]] = [
        dict(zip(axes, combo, strict=True)) for combo in itertools.product(*axes.values())
    ]
    rows = rows if axes else []
    expanded = rows + [dict(row) for row in matrix.get("include", [])]
    return expanded or [{}]


def _expand(name: str, row: Mapping[str, Any]) -> str:
    return re.sub(
        r"\$\{\{\s*matrix\.([A-Za-z0-9_-]+)\s*\}\}", lambda m: str(row.get(m.group(1), "")), name
    )


def _needs_ok(needs: Sequence[str], results: Mapping[str, bool], condition: str | None) -> bool:
    if condition is not None and ci_expressions.uses_status_function(condition):
        return True
    return all(results.get(need, False) for need in needs)


def _evaluate_job(
    job: Mapping[str, Any],
    context: dict[str, Any],
    results: dict[str, bool],
    outputs: dict[str, Any],
) -> bool:
    needs = _as_list(job.get("needs"))
    condition = job.get("if")
    condition = str(condition) if condition is not None else None
    if not _needs_ok(needs, results, condition):
        return False
    context["needs"] = {
        n: {"outputs": outputs.get(n, {}), "result": "success" if results.get(n) else "skipped"}
        for n in needs
    }
    return (
        True
        if condition is None
        else ci_expressions.truthy(ci_expressions.evaluate(condition, context))
    )


def _leg_runs(row: Mapping[str, Any], tier_outputs: Mapping[str, str]) -> bool:
    return row.get("tier") != "full" or tier_outputs.get("full") == "true"


def _ordered(jobs: Mapping[str, Any]) -> list[str]:
    """Job ids in dependency order, without recursion."""
    done: list[str] = []
    pending = list(jobs)
    for _ in range(MAX_JOBS):
        if not pending:
            return done
        ready = [j for j in pending if all(n in done for n in _as_list(jobs[j].get("needs")))]
        if not ready:
            raise ValueError(f"cyclic or unknown needs among {pending}")
        done.extend(ready)
        pending = [j for j in pending if j not in ready]
    raise ValueError("too many jobs")


def simulate_workflow(
    name: str,
    workflow: Mapping[str, Any],
    event: SyntheticEvent,
    config_path: Path,
) -> list[JobRun]:
    """Jobs of one triggered workflow with whether each ran."""
    jobs = workflow.get("jobs", {})
    base = build_context(event)
    results: dict[str, bool] = {}
    outputs: dict[str, Any] = {}
    runs: list[JobRun] = []
    for job_id in _ordered(jobs):
        job = jobs[job_id]
        ran = _evaluate_job(job, {**base}, results, outputs)
        results[job_id] = ran
        if ran and job.get("uses") == TIER_WORKFLOW:
            decision = tier_decision(event, config_path).outputs()
            outputs[job_id] = {key: decision[key] for key in ("tier", "light", "full")}
        elif ran:
            outputs[job_id] = Outputs()
        rows = _matrix_rows(job)
        label = str(job.get("name", job_id))
        names = tuple(dict.fromkeys(_expand(label, row) for row in rows))
        legs = {
            _expand(label, row): ran and _leg_runs(row, outputs.get("tier", {})) for row in rows
        }
        runs.append(JobRun(name, job_id, names, ran, legs))
    return runs


def simulate(
    workflows: Mapping[str, Mapping[str, Any]], event: SyntheticEvent, config_path: Path
) -> tuple[list[str], list[JobRun]]:
    """Triggered workflow files and every job of them, for one event."""
    triggered = [name for name, wf in workflows.items() if is_triggered(wf, event)]
    runs: list[JobRun] = []
    for name in triggered:
        runs.extend(simulate_workflow(name, workflows[name], event, config_path))
    return triggered, runs


def ran_jobs(runs: Sequence[JobRun]) -> set[tuple[str, str]]:
    """``(workflow, job)`` pairs that ran."""
    return {(run.workflow, run.job) for run in runs if run.ran}


def ran_names(runs: Sequence[JobRun]) -> set[str]:
    """Check-run names that ran (matrix legs expanded)."""
    return {name for run in runs for name, ran in run.legs.items() if ran}
