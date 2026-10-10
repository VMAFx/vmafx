#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Sum this month's paid fast-runner units from the GitHub Actions API.

Read-only (ADR-2168). A job counts when one of its labels starts with a
provider prefix of ``PROVIDERS``; its units are elapsed seconds times the
provider's per-minute rate for that label, rounded up to whole units:

* Depot (``depot-``): base minutes. Linux and Arm labels weigh vCPUs / 2
  (``depot-ubuntu-24.04`` is 1, ``-8`` is 4); Windows labels weigh their vCPU
  count (``depot-windows-2025`` is 2). Source: depot.dev runner-types page.
* Namespace (``namespace-``): unit minutes = vCPUs x minutes x platform factor
  (Linux 1, Windows 2, macOS 10), per the maintainer's brief (not checked
  against namespace.so). A ``namespace-profile-<name>`` label does not carry
  its shape, so each profile is declared with ``--namespace-profile
  NAME=VCPUS[:linux|windows|macos]``; an undeclared profile is an error.

    python3 scripts/ci/fast_runner_minutes.py --limit 10000 [--month YYYY-MM]
        [--threshold 0.9] [--repo OWNER/REPO] [--namespace-profile NAME=4]

Prints ``used / limit`` and exits 0 below ``limit * threshold``, 1 at or above
it, 2 when the API cannot be read or a label has no rule (a failed read is
never "under the limit").
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
DEFAULT_THRESHOLD = 0.9
# Workflows holding a job that can run on a fast runner (ADR-2168).
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
_NS_PROFILE = re.compile(
    r"^(?P<name>[A-Za-z0-9_.-]+?)=(?P<vcpus>\d+)(?::(?P<os>linux|windows|macos))?$"
)
_NS_FACTOR = {"linux": 1, "windows": 2, "macos": 10}
_NS_PREFIX = "namespace-profile-"

Profiles = dict[str, tuple[int, str]]


def depot_rate(label: str, _profiles: Profiles) -> float:
    """Base minutes per elapsed minute of a Depot label.

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


def namespace_rate(label: str, profiles: Profiles) -> float:
    """Unit minutes per elapsed minute: vCPUs x platform factor of the profile."""
    name = label.removeprefix(_NS_PREFIX) if label.startswith(_NS_PREFIX) else ""
    if name not in profiles:
        raise ValueError(f"Namespace label {label!r} has no declared --namespace-profile")
    vcpus, platform = profiles[name]
    return vcpus * _NS_FACTOR[platform]


def parse_profiles(specs: Iterable[str]) -> Profiles:
    """``NAME=VCPUS[:linux|windows|macos]`` entries of ``--namespace-profile``."""
    profiles: Profiles = {}
    for spec in specs:
        match = _NS_PROFILE.match(spec)
        if match is None:
            raise ValueError(
                f"bad --namespace-profile {spec!r}, want NAME=VCPUS[:linux|windows|macos]"
            )
        profiles[match["name"]] = (int(match["vcpus"]), match["os"] or "linux")
    return profiles


# Provider table: label prefix -> rate rule (ADR-2168).
PROVIDERS: tuple[tuple[str, Callable[[str, Profiles], float]], ...] = (
    ("depot-", depot_rate),
    ("namespace-", namespace_rate),
)


def label_rate(label: str, profiles: Profiles) -> float | None:
    """Units per minute of a label, None when no provider owns it."""
    for prefix, rule in PROVIDERS:
        if label.startswith(prefix):
            return rule(label, profiles)
    return None


def job_seconds(job: dict[str, Any]) -> float:
    """Elapsed seconds of a finished job, 0 for one that never started."""
    started, completed = job.get("started_at"), job.get("completed_at")
    if not started or not completed:
        return 0.0
    fmt = "%Y-%m-%dT%H:%M:%SZ"
    delta = datetime.strptime(completed, fmt) - datetime.strptime(started, fmt)
    return max(delta.total_seconds(), 0.0)


def used_units(jobs: Iterable[dict[str, Any]], profiles: Profiles | None = None) -> int:
    """Whole units (rounded up) of every fast-runner job in ``jobs``."""
    total = 0.0
    for job in jobs:
        for label in job.get("labels", []):
            rate = label_rate(label, profiles or {})
            if rate is not None:
                total += job_seconds(job) * rate
                break
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
    parser.add_argument(
        "--limit", type=int, required=True, help="the provider's monthly unit limit"
    )
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)
    parser.add_argument(
        "--namespace-profile", action="append", default=[], metavar="NAME=VCPUS[:OS]"
    )
    ns = parser.parse_args(argv)
    try:
        profiles = parse_profiles(ns.namespace_profile)
        used = used_units(fetch_jobs(ns.repo, ns.month), profiles)
    except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"fast_runner_minutes: cannot read usage: {exc}", file=sys.stderr)
        return 2
    print(f"{used} / {ns.limit} units ({ns.month}, {ns.repo})")
    return 1 if used >= ns.limit * ns.threshold else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
