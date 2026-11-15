#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Execute the embedded required-check aggregator against synthetic check results.

``run_required_aggregator`` runs the real Actions JavaScript with one selected check
result or absence. Its keyword inputs:

``event`` is the triggering event the script sees (``pull_request`` or ``push``;
a push is to ``master``) and ``action`` its ``pull_request`` action (``edited``
arms the ready-for-review window anchor). Every other required check succeeds in
check suite 1; the selected one reports from ``selected_suite``. ``workflow_runs``
are the workflow runs on the commit (``event``, ``head_branch`` and
``check_suite_id`` each), as the Actions API lists them. ``head_ref`` is the pull
request's head branch (a ``release-please--`` ref arms the release must-report
list). ``absent_checks`` omits the named required contexts from the baseline
synthetic checks. ``extra_checks`` are further check runs on the commit (``name``,
``conclusion``, ``check_suite``, and optionally ``status``). ``later_checks``,
when given, replace every check run from the second listing on, as the Actions
API shows a commit later.

Timestamps: ``run_created_at`` is the aggregator run's own ``created_at`` and the
zero point of every ``*_offset_s`` value. ``pr_created_at`` or
``pr_created_offset_s`` sets ``pull_request.created_at``; ``pr_number`` its number.
``default_started_offset_s`` sets ``started_at`` on every synthetic check, and
``started_offsets_s`` / ``created_offsets_s`` set it per check name.
``timeline_events`` are the issue timeline entries (``event`` and ``created_at``
or ``created_at_offset_s``) that ``issues.listEventsForTimeline`` pages through.

``api_errors`` maps API stub names ("getWorkflowRun", "listForRef",
"listWorkflowRunsForRepo", "listEventsForTimeline") to sequences of HTTP status
codes raised in order by successive calls to that stub. ``api_always`` maps stub
names to an HTTP status code raised on every call to that stub. ``info_logs``,
when given, collects every ``core.info`` message.
"""

from __future__ import annotations

import json
import re
import shutil
import textwrap
from collections.abc import Collection, Mapping, Sequence
from pathlib import Path

from scripts.lib.safe_subprocess import run as run_command

REPO_ROOT = Path(__file__).resolve().parents[2]
AGGREGATOR_PATH = REPO_ROOT / ".github" / "workflows" / "required-aggregator.yml"
SUBPROCESS_TIMEOUT_S = 120

_NODE_DRIVER = r"""
const fs = require('fs');
const input = JSON.parse(fs.readFileSync(0, 'utf8'));
const defaultNow = Date.now();
const parsedRunCreated =
  typeof input.runCreatedAt === 'string' ? Date.parse(input.runCreatedAt) : Number.NaN;
const now = Number.isFinite(parsedRunCreated) ? parsedRunCreated : defaultNow;
let clock = now;
class VirtualDate extends Date { static now() { clock += 180000; return clock; } }
const runCreatedAt =
  input.runCreatedAt != null ? input.runCreatedAt : new Date(now).toISOString();
const offsetIso = offsetSec => new Date(now + offsetSec * 1000).toISOString();
const expectedApps = input.expectedApps || {};
const withDefaults = c => {
  const copy = {...c};
  const startedOffset = copy.started_at_offset_s;
  const createdOffset = copy.created_at_offset_s;
  delete copy.started_at_offset_s;
  delete copy.created_at_offset_s;
  if (startedOffset !== undefined) copy.started_at = offsetIso(startedOffset);
  if (createdOffset !== undefined) copy.created_at = offsetIso(createdOffset);
  const hasTimestamp =
    copy.started_at !== undefined ||
    copy.created_at !== undefined ||
    copy.completed_at !== undefined;
  return {
    status: 'completed',
    ...(hasTimestamp ? {} : {started_at: new Date(now).toISOString()}),
    app: {slug: Object.hasOwn(expectedApps, copy.name) ? expectedApps[copy.name] : 'github-actions'},
    ...copy,
  };
};
const withTimelineDefaults = ev => {
  const copy = {...ev};
  const createdOffset = copy.created_at_offset_s;
  delete copy.created_at_offset_s;
  if (createdOffset !== undefined) copy.created_at = offsetIso(createdOffset);
  return copy;
};
const checks = input.checks.map(withDefaults);
const laterChecks = (input.laterChecks || []).map(withDefaults);
const timelineEvents = (input.timelineEvents || []).map(withTimelineDefaults);
let listings = 0;
const failures = [];
const infos = [];
const apiErrors = input.apiErrors || {};
const apiAlways = input.apiAlways || {};
function checkApiError(name) {
  const status = apiAlways[name] ?? (apiErrors[name] || []).shift();
  if (status === undefined) return;
  const error = new Error(`HTTP ${status}`);
  error.status = status;
  throw error;
}
const github = {rest: {
  actions: {
    getWorkflowRun: async () => {
      checkApiError('getWorkflowRun');
      return {data: {created_at: runCreatedAt}};
    },
    listWorkflowRunsForRepo: async () => {
      checkApiError('listWorkflowRunsForRepo');
      return {data: {workflow_runs: input.workflowRuns || []}};
    },
  },
  checks: {listForRef: async ({page = 1, per_page: perPage = 30} = {}) => {
    checkApiError('listForRef');
    if (page === 1) listings += 1;
    const all = listings > 1 && laterChecks.length ? laterChecks : checks;
    return {data: {check_runs: all.slice((page - 1) * perPage, page * perPage)}};
  }},
  issues: {listEventsForTimeline: async ({page = 1, per_page: perPage = 30} = {}) => {
    checkApiError('listEventsForTimeline');
    return {data: timelineEvents.slice((page - 1) * perPage, page * perPage)};
  }},
}};
const eventName = input.event || 'pull_request';
const prCreatedAt = input.prCreatedOffsetS != null
  ? offsetIso(input.prCreatedOffsetS)
  : input.prCreatedAt;
const prPayload = {
  ...(input.prNumber != null ? {number: input.prNumber} : {}),
  ...(prCreatedAt != null ? {created_at: prCreatedAt} : {}),
  head: {ref: input.headRef || 'fix/example', sha: 'abc'},
};
const context = {
  eventName,
  ref: input.ref || (eventName === 'pull_request' ? 'refs/pull/1/merge' : 'refs/heads/master'),
  sha: 'abc',
  repo: {owner: 'test', repo: 'test'},
  payload: eventName === 'pull_request'
    ? {
      ...(input.action != null ? {action: input.action} : {}),
      pull_request: prPayload,
    }
    : {},
};
const core = {
  info: message => infos.push(String(message)),
  warning: () => {},
  setFailed: message => failures.push(message),
};
const AsyncFunction = Object.getPrototypeOf(async function(){}).constructor;
new AsyncFunction('github', 'context', 'core', 'process', 'Date', 'setTimeout', input.script)(
  github,
  context,
  core,
  {env: {GITHUB_RUN_ID: '1', ...(input.env || {})}},
  VirtualDate,
  callback => callback(),
).then(() => process.stdout.write(JSON.stringify({failures, infos}))).catch(error => {
  console.error(error);
  process.exitCode = 1;
});
"""


def _embedded_script(workflow_text: str) -> str:
    marker = "          script: |\n"
    if workflow_text.count(marker) != 1:
        raise AssertionError("required aggregator must contain exactly one embedded script")
    return textwrap.dedent(workflow_text.split(marker, 1)[1])


def _required_names(script: str) -> list[str]:
    required_block = re.search(r"const required = \[(.*?)\];", script, re.DOTALL)
    if required_block is None:
        raise AssertionError("required aggregator must declare its check list")
    # Whole-line comments go first, as in scripts/ci/check-aggregator-names.sh: an
    # apostrophe in one ("praetor's") otherwise pairs with the next name's quote and
    # every name after it is lost (ADR-1687).
    body = re.sub(r"(?m)^\s*//.*$", "", required_block.group(1))
    return re.findall(r"'([^']+)'", body)


def _expected_apps(script: str) -> dict[str, str]:
    """Return check-name to expected app-slug mappings declared by the aggregator script."""
    block = re.search(r"const expectedAppByCheck = (\{.*?\});", script, re.DOTALL)
    if block is None:
        raise AssertionError("required aggregator must declare expectedAppByCheck")
    parsed: object = json.loads(block.group(1))
    if not isinstance(parsed, dict):
        raise AssertionError("expectedAppByCheck must be a JSON object")
    return {str(key): str(value) for key, value in parsed.items()}


def _apply_check_offsets(
    item: dict[str, object],
    name: str,
    default_started_offset_s: float | None,
    started_offsets_s: Mapping[str, float],
    created_offsets_s: Mapping[str, float],
) -> None:
    if name in started_offsets_s:
        item["started_at_offset_s"] = started_offsets_s[name]
    elif name not in created_offsets_s and default_started_offset_s is not None:
        item["started_at_offset_s"] = default_started_offset_s
    if name in created_offsets_s:
        item["created_at_offset_s"] = created_offsets_s[name]


def _synthetic_checks(
    names: list[str],
    check_name: str,
    conclusion: str | None,
    selected_suite: int,
    absent_checks: Collection[str] = (),
    *,
    default_started_offset_s: float | None = None,
    started_offsets_s: Mapping[str, float] | None = None,
    created_offsets_s: Mapping[str, float] | None = None,
) -> list[dict[str, object]]:
    """Every required check succeeds in suite 1; the selected one reports as asked."""
    omitted = set(absent_checks)
    started_map = started_offsets_s or {}
    created_map = created_offsets_s or {}
    records: list[dict[str, object]] = []
    for name in names:
        if name in omitted or (name == check_name and conclusion is None):
            continue
        item: dict[str, object] = {
            "name": name,
            "conclusion": conclusion if name == check_name else "success",
            "check_suite": {"id": selected_suite if name == check_name else 1},
        }
        _apply_check_offsets(item, name, default_started_offset_s, started_map, created_map)
        records.append(item)
    return records


def _validate_declared_names(
    names: list[str], check_name: str, absent_checks: Collection[str]
) -> None:
    declared = set(names)
    if check_name not in declared:
        raise AssertionError(f"required aggregator must declare {check_name!r}")
    for omitted_name in absent_checks:
        if omitted_name not in declared:
            raise AssertionError(f"required aggregator must declare {omitted_name!r}")


def _prepare_checks(
    script: str,
    check_name: str,
    conclusion: str | None,
    selected_suite: int,
    absent_checks: Collection[str],
    extra_checks: Sequence[Mapping[str, object]],
    default_started_offset_s: float | None,
    started_offsets_s: Mapping[str, float] | None,
    created_offsets_s: Mapping[str, float] | None,
) -> list[dict[str, object]]:
    names = _required_names(script)
    _validate_declared_names(names, check_name, absent_checks)
    checks = _synthetic_checks(
        names,
        check_name,
        conclusion,
        selected_suite,
        absent_checks,
        default_started_offset_s=default_started_offset_s,
        started_offsets_s=started_offsets_s,
        created_offsets_s=created_offsets_s,
    )
    checks.extend(dict(check) for check in extra_checks)
    return checks


def _string_list(values: object, label: str) -> list[str]:
    if not isinstance(values, list):
        raise AssertionError(f"aggregator driver must return a list of {label}")
    items: list[str] = []
    for item in values:
        if not isinstance(item, str):
            raise AssertionError(f"aggregator {label} must be strings")
        items.append(item)
    return items


def _decode_driver_output(stdout: str, info_logs: list[str] | None) -> list[str]:
    payload: object = json.loads(stdout)
    if not isinstance(payload, dict):
        raise AssertionError("aggregator driver must return a JSON object")
    if info_logs is not None:
        info_logs.extend(_string_list(payload.get("infos"), "info messages"))
    return _string_list(payload.get("failures"), "failure messages")


def _run_driver(
    driver_input: Mapping[str, object], info_logs: list[str] | None = None
) -> list[str]:
    node = shutil.which("node")
    if node is None:
        raise AssertionError("Node.js is needed to exercise the Actions JavaScript")

    result = run_command(
        [node, "-e", _NODE_DRIVER],
        allowed_executables=(node,),
        input_data=json.dumps(driver_input),
        text=True,
        capture_output=True,
        check=True,
        timeout_seconds=SUBPROCESS_TIMEOUT_S,
    )
    return _decode_driver_output(result.stdout, info_logs)


def run_required_aggregator(
    check_name: str,
    conclusion: str | None,
    *,
    env: Mapping[str, str] | None = None,
    event: str = "pull_request",
    action: str | None = None,
    pr_number: int | None = 1,
    pr_created_at: str | None = None,
    pr_created_offset_s: float | None = None,
    run_created_at: str | None = None,
    timeline_events: Sequence[Mapping[str, object]] = (),
    default_started_offset_s: float | None = None,
    started_offsets_s: Mapping[str, float] | None = None,
    created_offsets_s: Mapping[str, float] | None = None,
    selected_suite: int = 1,
    workflow_runs: Sequence[Mapping[str, object]] = (),
    head_ref: str = "fix/example",
    absent_checks: Collection[str] = (),
    extra_checks: Sequence[Mapping[str, object]] = (),
    later_checks: Sequence[Mapping[str, object]] = (),
    api_errors: Mapping[str, Sequence[int]] | None = None,
    api_always: Mapping[str, int] | None = None,
    info_logs: list[str] | None = None,
) -> list[str]:
    """Run the real Actions JavaScript; the module docstring lists the inputs."""
    script = _embedded_script(AGGREGATOR_PATH.read_text(encoding="utf-8"))
    checks = _prepare_checks(
        script,
        check_name,
        conclusion,
        selected_suite,
        absent_checks,
        extra_checks,
        default_started_offset_s,
        started_offsets_s,
        created_offsets_s,
    )
    driver_input: dict[str, object] = {
        "script": script,
        "checks": checks,
        "expectedApps": _expected_apps(script),
        "env": dict(env or {}),
        "event": event,
        "action": action,
        "prNumber": pr_number,
        "prCreatedAt": pr_created_at,
        "prCreatedOffsetS": pr_created_offset_s,
        "runCreatedAt": run_created_at,
        "timelineEvents": [dict(item) for item in timeline_events],
        "workflowRuns": [dict(run) for run in workflow_runs],
        "headRef": head_ref,
        "laterChecks": [dict(check) for check in later_checks],
        "apiErrors": {name: list(codes) for name, codes in (api_errors or {}).items()},
        "apiAlways": dict(api_always or {}),
    }
    return _run_driver(driver_input, info_logs)
