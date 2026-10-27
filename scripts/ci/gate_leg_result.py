#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Judge one matrix leg for a required-aggregator gate job.

A matrix job's ``needs.<job>.result`` is the aggregate of every leg, so a gate
that reads it turns red for a leg it does not name. A gate calls this script
instead; it reads the run's jobs (the JSON of
``gh api repos/<repo>/actions/runs/<id>/jobs --paginate``, on standard input
or from ``--jobs-file``) and judges the one job whose name is ``--job``.

Verdicts, matching the gates this replaces:

* impact planning must have succeeded (``PLAN_RESULT``);
* ``SELECTED=true``: the own job must exist, be unique and have concluded
  ``success``; absent, unfinished or any other conclusion fails;
* ``SELECTED=false``: the own job must be absent or ``skipped``.

Exit 0 passes, exit 1 fails with a ``::error::`` line. Another leg's
conclusion is never read.

With ``--fetch`` the script reads the jobs itself (``gh api`` on
``GITHUB_REPOSITORY`` / ``GITHUB_RUN_ID``). The jobs API can report the own
job without a conclusion for a few seconds after it finished, so while the own
job is present but unfinished the script reads again, at most ``--attempts``
times ``--interval`` seconds apart, and then fails as before.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

MAX_ATTEMPTS = 20
MAX_INTERVAL = 60.0


def parse_jobs(text: str) -> list[dict[str, Any]]:
    """Return every job in one or more concatenated ``jobs`` documents."""
    decoder = json.JSONDecoder()
    jobs: list[dict[str, Any]] = []
    index = 0
    while index < len(text):
        while index < len(text) and text[index].isspace():
            index += 1
        if index >= len(text):
            break
        document, index = decoder.raw_decode(text, index)
        page = document.get("jobs", []) if isinstance(document, dict) else document
        jobs.extend(page)
    return jobs


def _own_verdict(selected: str, job_name: str, own: list[dict[str, Any]]) -> str | None:
    conclusion = own[0].get("conclusion") if own else None
    if selected == "false":
        if own and conclusion != "skipped":
            return f"selected=false but {job_name!r} concluded {conclusion!r}"
        return None
    if not own:
        return f"selected=true but the job {job_name!r} is not in the run"
    if conclusion != "success":
        return f"selected=true but {job_name!r} concluded {conclusion!r}"
    return None


def unfinished(job_name: str, jobs: list[dict[str, Any]]) -> bool:
    """Return True when the own job is in the run but has not concluded yet."""
    own = [job for job in jobs if job.get("name") == job_name]
    if len(own) != 1:
        return False
    return own[0].get("status") != "completed" or own[0].get("conclusion") is None


def judge(plan: str, selected: str, job_name: str, jobs: list[dict[str, Any]]) -> str | None:
    """Return None when the leg passes, else the reason it fails."""
    own = [job for job in jobs if job.get("name") == job_name]
    if plan != "success":
        return f"impact planning did not succeed ({plan})"
    if selected not in ("true", "false"):
        return f"invalid selected value {selected!r}"
    if len(own) > 1:
        return f"{len(own)} jobs are named {job_name!r}; the leg is ambiguous"
    return _own_verdict(selected, job_name, own)


def fetch_jobs_text() -> str:
    """Read this run's jobs through ``gh api``; raise OSError on failure."""
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    run_id = os.environ.get("GITHUB_RUN_ID", "")
    if not repo or not run_id.isdigit():
        raise OSError("GITHUB_REPOSITORY and GITHUB_RUN_ID must be set")
    executable = shutil.which("gh")
    if executable is None:
        raise OSError("GitHub CLI is required for --fetch")
    gh = str(Path(executable).resolve(strict=True))
    url = f"repos/{repo}/actions/runs/{run_id}/jobs?per_page=100"
    result = subprocess.run(  # noqa: S603 -- resolved gh, fixed argv, URL from the run's env
        [gh, "api", url, "--paginate"], capture_output=True, text=True, timeout=120, check=False
    )
    if result.returncode != 0:
        raise OSError(f"gh api exited {result.returncode}: {result.stderr.strip()}")
    return result.stdout


def read_settled_jobs(
    job_name: str,
    read: Callable[[], str],
    attempts: int,
    interval: float,
    sleep: Callable[[float], None] = time.sleep,
) -> list[dict[str, Any]]:
    """Read the jobs until the own job has concluded or the attempts run out."""
    jobs = parse_jobs(read())
    for attempt in range(1, attempts):
        if not unfinished(job_name, jobs):
            break
        print(f"{job_name!r} has not concluded yet; reading again ({attempt}/{attempts - 1})")
        sleep(interval)
        jobs = parse_jobs(read())
    return jobs


def _read_jobs(args: argparse.Namespace) -> list[dict[str, Any]]:
    if args.fetch:
        return read_settled_jobs(args.job, fetch_jobs_text, args.attempts, args.interval)
    text = Path(args.jobs_file).read_text(encoding="utf-8") if args.jobs_file else sys.stdin.read()
    return parse_jobs(text)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--job", required=True, help="exact name of the leg's work job")
    parser.add_argument("--jobs-file", help="read the jobs JSON here instead of stdin")
    parser.add_argument("--fetch", action="store_true", help="read this run's jobs with gh api")
    parser.add_argument("--attempts", type=int, default=7, help="reads while unfinished (1-20)")
    parser.add_argument("--interval", type=float, default=10.0, help="seconds between reads")
    args = parser.parse_args(argv)
    if not 1 <= args.attempts <= MAX_ATTEMPTS or not 0 <= args.interval <= MAX_INTERVAL:
        print(f"::error::--attempts must be 1-{MAX_ATTEMPTS} and --interval 0-{MAX_INTERVAL:g}")
        return 1
    try:
        jobs = _read_jobs(args)
    except (ValueError, AttributeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"::error::cannot read the run's jobs: {error}")
        return 1
    reason = judge(
        os.environ.get("PLAN_RESULT", ""), os.environ.get("SELECTED", ""), args.job, jobs
    )
    if reason:
        print(f"::error::{reason}")
        return 1
    print(f"gate ok: {args.job!r} selected={os.environ.get('SELECTED')}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
