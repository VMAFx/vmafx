#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Start the full suite on a pull request that was just labelled for it (ADR-2169).

Applying ``ci: full`` to a pull request from this repository, or
``autorelease: cut`` to the generated release pull request, changes the tier
the pull request owes from light to full. No workflow listens for ``labeled``
itself: that would start one run per workflow for every label anybody
applies, and a skipped aggregator run on the same commit would read as a pass.
One workflow, ``pr-label-lanes.yml``, listens instead and calls
``ci-escalate.yml``, which re-runs the latest run of every other
pull-request workflow on the head commit (the label-lanes run itself is
skipped: it is the run doing the escalation); each re-run decides its tier again
from the live labels (``ci_tier.py``), so the jobs the light tier skipped now
run, and the Required Checks Aggregator re-reads the new tier.

A run that is still in progress is cancelled first (GitHub refuses to re-run
a running workflow) and awaited, within a bound. A run that completes between
the listing and the cancel answers the cancel with 409 Conflict: that is no
error, the run is awaited and re-run like any other. A run that cannot be
re-run is reported with its reason, the runs after it are still started, and
the step then fails.
"""

from __future__ import annotations

import os
import sys
import time
import urllib.error
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci import ci_tier

API_TIMEOUT_S = 30
CANCEL_WAIT_S = 180
CANCEL_POLL_S = 5
HTTP_CONFLICT = 409
SKIPPED_WORKFLOWS = ("pr-label-lanes.yml",)

Api = Callable[..., object]


def api_call(base: str, token: str) -> Api:
    """Return ``call(method, path)`` bound to one API root and token."""

    def call(method: str, path: str) -> object:
        return ci_tier.api_request(method, base + path, token)

    return call


def latest_runs(call: Api, repository: str, head_sha: str, own_run: str) -> list[dict[str, object]]:
    """The newest pull-request run of each workflow on a commit, this run excluded."""
    path = f"/repos/{repository}/actions/runs"
    newest: dict[object, dict[str, object]] = {}
    for page in range(1, ci_tier.MAX_PAGES + 1):
        query = f"?head_sha={head_sha}&event=pull_request&per_page={ci_tier.PAGE_SIZE}&page={page}"
        data = call("GET", path + query)
        runs = data.get("workflow_runs", []) if isinstance(data, dict) else []
        for run in runs:
            if str(run["id"]) == own_run or str(run.get("path", "")).endswith(SKIPPED_WORKFLOWS):
                continue
            previous = newest.get(run["workflow_id"])
            if previous is None or run["id"] > previous["id"]:
                newest[run["workflow_id"]] = run
        if len(runs) < ci_tier.PAGE_SIZE:
            break
    return list(newest.values())


def wait_until_completed(
    call: Api,
    repository: str,
    run_id: int,
    *,
    sleep: Callable[[float], None] = time.sleep,
) -> bool:
    """Poll a cancelled run until it is completed; False when the bound is reached."""
    for _ in range(CANCEL_WAIT_S // CANCEL_POLL_S):
        data = call("GET", f"/repos/{repository}/actions/runs/{run_id}")
        if isinstance(data, dict) and data.get("status") == "completed":
            return True
        sleep(CANCEL_POLL_S)
    return False


def cancel(call: Api, repository: str, run_id: int) -> None:
    """Ask for a run to be cancelled; a run that already completed is no error."""
    try:
        call("POST", f"/repos/{repository}/actions/runs/{run_id}/cancel")
    except urllib.error.HTTPError as exc:
        exc.close()
        if exc.code != HTTP_CONFLICT:
            raise


def rerun(
    call: Api,
    repository: str,
    run: Mapping[str, object],
    *,
    sleep: Callable[[float], None] = time.sleep,
) -> str:
    """Re-run one workflow run, cancelling it first when it is still going."""
    run_id = int(run["id"])  # type: ignore[call-overload]
    name = str(run.get("name", run_id))
    if run.get("status") != "completed":
        cancel(call, repository, run_id)
        if not wait_until_completed(call, repository, run_id, sleep=sleep):
            return f"{name}: still running after {CANCEL_WAIT_S}s, not re-run"
    call("POST", f"/repos/{repository}/actions/runs/{run_id}/rerun")
    return f"{name}: re-run"


def rerun_all(
    call: Api,
    repository: str,
    runs: Sequence[Mapping[str, object]],
    *,
    sleep: Callable[[float], None] = time.sleep,
) -> int:
    """Re-run every run and return how many were not: one failure stops no other run."""
    failed = 0
    for run in runs:
        try:
            message = rerun(call, repository, run, sleep=sleep)
        except (urllib.error.URLError, OSError, KeyError, ValueError) as exc:
            if isinstance(exc, urllib.error.HTTPError):
                exc.close()
            message = f"{run.get('name', run.get('id'))}: {exc}, not re-run"
        failed += message.endswith("not re-run")
        print(f"ci-escalate: {message}")
    return failed


def owes_full_suite(env: Mapping[str, str], fetch: ci_tier.Fetcher, config_path: Path) -> bool:
    """True when the live labels now put the pull request in the full tier."""
    decision = ci_tier.run(env, config_path, fetch)
    print(f"ci-escalate: tier={decision.tier} ({decision.reason})")
    return decision.tier == ci_tier.FULL


def main(argv: Sequence[str] | None = None) -> int:
    del argv
    env = os.environ
    token = env.get("GH_TOKEN", "")
    if not token:
        print("ci-escalate: GH_TOKEN is required", file=sys.stderr)
        return 2
    base = env.get("GITHUB_API_URL", "https://api.github.com")
    call = api_call(base, token)

    def fetch(path: str) -> object:
        return call("GET", path)

    if not owes_full_suite(env, fetch, ci_tier.DEFAULT_CONFIG):
        print("ci-escalate: the pull request does not owe the full suite; nothing to start")
        return 0
    try:
        runs = latest_runs(
            call, env["GITHUB_REPOSITORY"], env["HEAD_SHA"], env.get("GITHUB_RUN_ID", "")
        )
        failed = rerun_all(call, env["GITHUB_REPOSITORY"], runs)
    except (urllib.error.URLError, OSError, KeyError, ValueError) as exc:
        print(f"ci-escalate: {exc}", file=sys.stderr)
        return 1
    if failed:
        print(f"ci-escalate: {failed} of {len(runs)} runs were not re-run", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
