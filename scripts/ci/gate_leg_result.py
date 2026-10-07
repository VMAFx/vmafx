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
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any


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


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--job", required=True, help="exact name of the leg's work job")
    parser.add_argument("--jobs-file", help="read the jobs JSON here instead of stdin")
    args = parser.parse_args(argv)
    text = Path(args.jobs_file).read_text(encoding="utf-8") if args.jobs_file else sys.stdin.read()
    try:
        jobs = parse_jobs(text)
    except (ValueError, AttributeError) as error:
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
