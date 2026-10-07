#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Sum this month's Depot runner base minutes from the GitHub Actions API.

Read-only. A job counts when one of its labels starts with ``depot-``; its
base minutes are its elapsed seconds times the label's minutes multiplier
(ADR-2168). The month total is rounded up to whole minutes, as Depot bills.

    python3 scripts/ci/depot_minutes.py [--month YYYY-MM] [--limit 10000]
                                        [--threshold 0.9] [--repo OWNER/REPO]

Prints ``used / limit`` and exits 0 below ``limit * threshold``, 1 at or above
it, 2 when the API cannot be read (a failed read is never "under the limit").
"""

from __future__ import annotations

import argparse
import calendar
import json
import math
import re
import subprocess
import sys
from collections.abc import Callable, Iterable
from concurrent.futures import ThreadPoolExecutor
from datetime import date, datetime
from typing import Any

DEFAULT_REPO = "VMAFx/vmafx"
DEFAULT_LIMIT = 10_000
DEFAULT_THRESHOLD = 0.9
# Workflows holding a job that can run on Depot (ADR-2168).
WORKFLOWS = (
    "tests-and-quality-gates.yml",
    "dev-container-build.yml",
    "docker-image.yml",
    "ffmpeg-integration.yml",
)
API_TIMEOUT_S = 120
RETRIES = 3
MAX_PAGES = 50
WORKERS = 16

_LINUX = re.compile(r"^depot-ubuntu-\d+\.\d+(?:-arm)?(?:-(\d+))?$")
_WINDOWS = re.compile(r"^depot-windows-\d+(?:-(\d+))?$")


def multiplier(label: str) -> int:
    """Minutes multiplier of a Depot label (docs: depot.dev runner-types).

    Linux and Arm: vCPUs / 2 (2 vCPU = 1x). Windows: the vCPU count (2 = 2x).
    Anything else (macOS is priced per minute, not by multiplier) raises.
    """
    match = _LINUX.match(label)
    if match:
        return int(match.group(1) or 2) // 2
    match = _WINDOWS.match(label)
    if match:
        return int(match.group(1) or 2)
    raise ValueError(f"no known minutes multiplier for Depot label {label!r}")


def job_seconds(job: dict[str, Any]) -> float:
    """Elapsed seconds of a finished job, 0 for one that never started."""
    started, completed = job.get("started_at"), job.get("completed_at")
    if not started or not completed:
        return 0.0
    fmt = "%Y-%m-%dT%H:%M:%SZ"
    delta = datetime.strptime(completed, fmt) - datetime.strptime(started, fmt)
    return max(delta.total_seconds(), 0.0)


def base_minutes(jobs: Iterable[dict[str, Any]]) -> int:
    """Whole base minutes (rounded up) of every Depot job in ``jobs``."""
    total = 0.0
    for job in jobs:
        depot = [x for x in job.get("labels", []) if x.startswith("depot-")]
        if depot:
            total += job_seconds(job) * multiplier(depot[0])
    return math.ceil(total / 60)


def _gh(args: list[str]) -> list[dict[str, Any]]:
    """One paginated read-only ``gh api`` call, retried on a transient failure."""
    err = ""
    for _ in range(RETRIES):
        proc = subprocess.run(  # noqa: S603 - fixed argv, read-only gh call
            ["gh", "api", "--paginate", "--slurp", *args],  # noqa: S607
            capture_output=True,
            text=True,
            check=False,
            timeout=API_TIMEOUT_S,
        )
        if proc.returncode == 0:
            return list(json.loads(proc.stdout))[:MAX_PAGES]
        err = proc.stderr.strip()
    raise RuntimeError(f"gh api {' '.join(args)}: {err}")


def month_days(month: str) -> list[str]:
    year, mon = (int(x) for x in month.split("-"))
    last = calendar.monthrange(year, mon)[1]
    return [date(year, mon, d).isoformat() for d in range(1, last + 1)]


def _run_ids(repo: str, month: str, gh: Callable[[list[str]], list[dict[str, Any]]]) -> list[int]:
    """Ids of the month's master push and dispatch runs, one query per day and workflow."""
    ids: dict[int, None] = {}
    for day in month_days(month):
        if day > date.today().isoformat():
            break
        for wf in WORKFLOWS:
            args = [
                f"repos/{repo}/actions/workflows/{wf}/runs",
                "-X", "GET", "-f", f"created={day}", "-f", "per_page=100",
            ]  # fmt: skip
            for page in gh(args):
                for run in page.get("workflow_runs", []):
                    if run.get("event") in ("push", "workflow_dispatch"):
                        ids[run["id"]] = None
    return list(ids)


def fetch_jobs(
    repo: str, month: str, gh: Callable[[list[str]], list[dict[str, Any]]] = _gh
) -> list[dict[str, Any]]:
    """Every job of the month's master push and dispatch runs of the Depot workflows."""

    def jobs_of(run_id: int) -> list[dict[str, Any]]:
        args = [
            f"repos/{repo}/actions/runs/{run_id}/jobs",
            "-X", "GET", "-f", "per_page=100", "-f", "filter=all",
        ]  # fmt: skip
        return [job for page in gh(args) for job in page.get("jobs", [])]

    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        return [job for jobs in pool.map(jobs_of, _run_ids(repo, month, gh)) for job in jobs]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--month", default=date.today().strftime("%Y-%m"))
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument("--limit", type=int, default=DEFAULT_LIMIT)
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)
    ns = parser.parse_args(argv)
    try:
        used = base_minutes(fetch_jobs(ns.repo, ns.month))
    except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"depot_minutes: cannot read usage: {exc}", file=sys.stderr)
        return 2
    print(f"{used} / {ns.limit} base minutes ({ns.month}, {ns.repo})")
    return 1 if used >= ns.limit * ns.threshold else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
