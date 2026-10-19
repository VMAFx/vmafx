#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Execute the embedded required-check aggregator against synthetic check results."""

from __future__ import annotations

import json
import re
import shutil
import textwrap
from collections.abc import Mapping, Sequence
from pathlib import Path

from scripts.lib.safe_subprocess import run as run_command

REPO_ROOT = Path(__file__).resolve().parents[2]
AGGREGATOR_PATH = REPO_ROOT / ".github" / "workflows" / "required-aggregator.yml"
SUBPROCESS_TIMEOUT_S = 120

_NODE_DRIVER = r"""
const fs = require('fs');
const input = JSON.parse(fs.readFileSync(0, 'utf8'));
const now = Date.now();
let clock = now;
class VirtualDate extends Date { static now() { clock += 180000; return clock; } }
const withDefaults = c => ({
  status: 'completed',
  started_at: new Date(now).toISOString(),
  ...c,
});
const checks = input.checks.map(withDefaults);
const laterChecks = (input.laterChecks || []).map(withDefaults);
let listings = 0;
const failures = [];
const github = {rest: {
  actions: {
    getWorkflowRun: async () => ({data: {created_at: new Date(now).toISOString()}}),
    listWorkflowRunsForRepo: async () => ({data: {workflow_runs: input.workflowRuns || []}}),
  },
  checks: {listForRef: async () => {
    listings += 1;
    return {data: {check_runs: listings > 1 && laterChecks.length ? laterChecks : checks}};
  }},
}};
const eventName = input.event || 'pull_request';
const context = {
  eventName,
  ref: input.ref || (eventName === 'pull_request' ? 'refs/pull/1/merge' : 'refs/heads/master'),
  sha: 'abc',
  repo: {owner: 'test', repo: 'test'},
  payload: eventName === 'pull_request'
    ? {pull_request: {head: {ref: input.headRef || 'fix/example', sha: 'abc'}}}
    : {},
};
const core = {info: () => {}, setFailed: message => failures.push(message)};
const AsyncFunction = Object.getPrototypeOf(async function(){}).constructor;
new AsyncFunction('github', 'context', 'core', 'process', 'Date', 'setTimeout', input.script)(
  github,
  context,
  core,
  {env: {GITHUB_RUN_ID: '1', ...(input.env || {})}},
  VirtualDate,
  callback => callback(),
).then(() => process.stdout.write(JSON.stringify(failures))).catch(error => {
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


def _synthetic_checks(
    names: list[str], check_name: str, conclusion: str | None, selected_suite: int
) -> list[dict[str, object]]:
    """Every required check succeeds in suite 1; the selected one reports as asked."""
    return [
        {
            "name": name,
            "conclusion": conclusion if name == check_name else "success",
            "check_suite": {"id": selected_suite if name == check_name else 1},
        }
        for name in names
        if name != check_name or conclusion is not None
    ]


def _failure_messages(stdout: str) -> list[str]:
    payload: object = json.loads(stdout)
    if not isinstance(payload, list):
        raise AssertionError("aggregator driver must return a list of failure messages")

    failures: list[str] = []
    for message in payload:
        if not isinstance(message, str):
            raise AssertionError("aggregator failure messages must be strings")
        failures.append(message)
    return failures


def run_required_aggregator(
    check_name: str,
    conclusion: str | None,
    *,
    env: Mapping[str, str] | None = None,
    event: str = "pull_request",
    selected_suite: int = 1,
    workflow_runs: Sequence[Mapping[str, object]] = (),
    head_ref: str = "fix/example",
    extra_checks: Sequence[Mapping[str, object]] = (),
    later_checks: Sequence[Mapping[str, object]] = (),
) -> list[str]:
    """Run the real Actions JavaScript with one selected check result or absence.

    ``event`` is the triggering event the script sees (``pull_request`` or ``push``;
    a push is to ``master``). Every other required check succeeds in check suite 1;
    the selected one reports from ``selected_suite``. ``workflow_runs`` are the
    workflow runs on the commit (``event``, ``head_branch`` and ``check_suite_id``
    each), as the Actions API lists them. ``head_ref`` is the pull request's head branch
    (a ``release-please--`` ref arms the release must-report list). ``extra_checks`` are
    further check runs on the commit (``name``, ``conclusion``, ``check_suite``, and
    optionally ``status``). ``later_checks``, when given, replace every check run from the
    second listing on, as the Actions API shows a commit later.
    """
    workflow_text = AGGREGATOR_PATH.read_text(encoding="utf-8")
    script = _embedded_script(workflow_text)
    names = _required_names(script)
    if check_name not in names:
        raise AssertionError(f"required aggregator must declare {check_name!r}")

    checks = _synthetic_checks(names, check_name, conclusion, selected_suite)
    checks.extend(dict(check) for check in extra_checks)
    node = shutil.which("node")
    if node is None:
        raise AssertionError("Node.js is needed to exercise the Actions JavaScript")

    result = run_command(
        [node, "-e", _NODE_DRIVER],
        allowed_executables=(node,),
        input_data=json.dumps(
            {
                "script": script,
                "checks": checks,
                "env": dict(env or {}),
                "event": event,
                "workflowRuns": [dict(run) for run in workflow_runs],
                "headRef": head_ref,
                "laterChecks": [dict(check) for check in later_checks],
            }
        ),
        text=True,
        capture_output=True,
        check=True,
        timeout_seconds=SUBPROCESS_TIMEOUT_S,
    )
    return _failure_messages(result.stdout)
