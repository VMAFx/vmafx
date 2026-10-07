#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Require every tester leg to be green on the exact head of a release candidate (ADR-2198).

    GH_TOKEN=$(gh auth token) check-candidate-legs.py --sha <40 hex> [--repo VMAFx/vmafx]

A pull request builds only the legs whose inputs it changed and a master push skips them
all when nothing they read changed, so a leg can be red for days without any required
check noticing (the x64 SYCL zip failed on master from 2026-10-05 until the rc.3
publish run of 2026-10-07). The cut therefore asks the forge, per workflow of
``scripts/release/candidate-legs.json``, for runs of that exact commit and requires each
listed job to have concluded ``success`` in one of them. A run counts for the candidate
when it is a push or a schedule run on the candidate commit, or a dispatch whose run title
carries the candidate's full SHA (``run-name`` of the three workflows: a dispatch titled
for ``master`` or a tag proves nothing about a commit). ``skipped`` is not green.

Exit 0 every leg green, 1 a leg is not, 2 a usage or API error.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.ci_tier import TierError, api_request

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LEGS = ROOT / "scripts" / "release" / "candidate-legs.json"
API = "https://api.github.com"
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
RUNS_PER_PAGE = 100
MAX_RUN_PAGES = 5
MAX_JOB_PAGES = 5
MAX_RUNS_READ = 40
SELF_TITLED_EVENTS = frozenset({"push", "schedule", "pull_request"})

Fetcher = Callable[[str], object]


class LegsError(RuntimeError):
    """The legs cannot be checked (bad input or an unreadable answer)."""


def load_legs(path: Path) -> list[dict[str, Any]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    workflows = data.get("workflows")
    if not isinstance(workflows, list) or not workflows:
        raise LegsError(f"{path}: no workflows")
    for entry in workflows:
        jobs = entry.get("jobs") if isinstance(entry, dict) else None
        if not (isinstance(entry.get("file"), str) and isinstance(jobs, list) and jobs):
            raise LegsError(f"{path}: every workflow needs a file and jobs: {entry}")
    return list(workflows)


def _pages(fetch: Fetcher, url: str, key: str, limit: int) -> list[dict[str, Any]]:
    """Every object under ``key`` of up to ``limit`` pages of ``url``."""
    found: list[dict[str, Any]] = []
    joiner = "&" if "?" in url else "?"
    for page in range(1, limit + 1):
        body = fetch(f"{url}{joiner}per_page={RUNS_PER_PAGE}&page={page}")
        rows = body.get(key) if isinstance(body, dict) else None
        if not isinstance(rows, list):
            raise LegsError(f"{url}: the answer has no list {key!r}")
        found.extend(row for row in rows if isinstance(row, dict))
        if len(rows) < RUNS_PER_PAGE:
            break
    return found


def counts_for(run: Mapping[str, Any], sha: str) -> bool:
    """Whether a run is evidence about the commit ``sha``."""
    if run.get("status") != "completed":
        return False
    if run.get("event") == "workflow_dispatch":
        return sha in str(run.get("display_title", ""))
    return run.get("event") in SELF_TITLED_EVENTS and run.get("head_sha") == sha


def workflow_runs(fetch: Fetcher, repo: str, filename: str, sha: str) -> list[dict[str, Any]]:
    """Completed runs that count for ``sha``: its own push or schedule runs, newest first,
    and the recent dispatches (their head is the dispatching commit, the title names the source)."""
    base = f"/repos/{repo}/actions/workflows/{filename}/runs"
    own = _pages(fetch, f"{base}?head_sha={sha}&status=completed", "workflow_runs", MAX_RUN_PAGES)
    dispatched = _pages(
        fetch, f"{base}?event=workflow_dispatch&status=completed", "workflow_runs", MAX_RUN_PAGES
    )
    runs = [run for run in own + dispatched if counts_for(run, sha)]
    runs.sort(key=lambda run: str(run.get("created_at", "")), reverse=True)
    return runs[:MAX_RUNS_READ]


def job_conclusions(fetch: Fetcher, repo: str, run_id: object) -> dict[str, str]:
    """Job name -> conclusion of one run (the latest attempt)."""
    url = f"/repos/{repo}/actions/runs/{run_id}/jobs?filter=latest"
    rows = _pages(fetch, url, "jobs", MAX_JOB_PAGES)
    return {str(row.get("name")): str(row.get("conclusion")) for row in rows}


def check_workflow(fetch: Fetcher, repo: str, entry: Mapping[str, Any], sha: str) -> list[str]:
    """Problems of one workflow's legs on ``sha``; empty when each listed job is green."""
    filename = str(entry["file"])
    runs = workflow_runs(fetch, repo, filename, sha)
    if not runs:
        return [
            f"{filename}: no completed run for {sha[:12]} (push, schedule or a dispatch titled with the full SHA)"
        ]
    best: dict[str, str] = {}
    for run in runs:
        for name, conclusion in job_conclusions(fetch, repo, run.get("id")).items():
            if best.get(name) != "success":
                best[name] = conclusion
    problems = []
    for job in entry["jobs"]:
        state = best.get(str(job), "absent or skipped (a skipped matrix job carries no leg name)")
        if state != "success":
            problems.append(f"{filename}: {job!r} is {state} on {sha[:12]}")
    return problems


def check(fetch: Fetcher, repo: str, legs: Sequence[Mapping[str, Any]], sha: str) -> list[str]:
    if not SHA_RE.match(sha):
        raise LegsError(f"--sha must be a full lower-case 40 hex commit id, not {sha!r}")
    problems: list[str] = []
    for entry in legs:
        problems.extend(check_workflow(fetch, repo, entry, sha))
    return problems


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sha", required=True, help="the candidate's full commit id")
    parser.add_argument("--repo", default="VMAFx/vmafx")
    parser.add_argument("--legs", type=Path, default=DEFAULT_LEGS)
    args = parser.parse_args(argv)
    token = os.environ.get("GH_TOKEN", "")
    if not token:
        print("check-candidate-legs: set GH_TOKEN (GH_TOKEN=$(gh auth token))", file=sys.stderr)
        return 2
    try:
        legs = load_legs(args.legs)
        problems = check(
            lambda path: api_request("GET", API + path, token), args.repo, legs, args.sha
        )
    except (LegsError, TierError, OSError, ValueError) as exc:
        print(f"check-candidate-legs: {exc}", file=sys.stderr)
        return 2
    for problem in problems:
        print(f"check-candidate-legs: {problem}", file=sys.stderr)
    if problems:
        print(
            f"check-candidate-legs: {len(problems)} leg(s) not green on {args.sha}", file=sys.stderr
        )
        return 1
    print(f"check-candidate-legs: every listed leg is green on {args.sha}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
