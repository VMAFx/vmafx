#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Hold every workflow job's time limit at or above the sum of its step limits.

A step's ``timeout-minutes`` only means something when the job can run that
long. On 2026-10-22 the MCP Smoke job had a 35-minute limit and an apt step
with a 20-minute limit; a slow mirror took 19m59s, the job hit its own limit
in the build and was cancelled, though no step overran its cap.

For every job under ``.github/workflows/`` that has ``steps:``, this check
adds the ``timeout-minutes`` of its steps and fails when the job's own
``timeout-minutes`` is smaller. A job without a limit has GitHub's default of
360 minutes. A limit that is not a whole number (an expression) cannot be
checked and fails closed. Size a job limit as the step caps plus 1.25 times
the measured longest healthy run of the uncapped steps (ADR-3367).

A step that runs ``apt-get install`` without its own ``timeout-minutes`` is a
finding too: on 2026-10-23 an uncapped install of ``libcudart12`` took 30
minutes on a slow mirror and the Coverage Gate hit its job limit. A workflow
file listed in ``UNCAPPED_APT_EXCEPTIONS`` is exempt from that rule until its
expiry date; past it, the file fails like one without an exception.

Exit 0 when every job holds, 1 on a job below its step sum, 2 on a workflow
it cannot read or a limit it cannot evaluate.
"""

from __future__ import annotations

import argparse
import datetime
import sys
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]  # no PyYAML stubs in the hook environment

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = ROOT / ".github" / "workflows"
GITHUB_DEFAULT_JOB_MINUTES = 360

# Workflow files whose apt steps get their caps in one follow-up pull request
# each (ledger vmafx-coverage-apt-cache). Category B, entered 2026-10-23,
# expiry entry date plus 3 days. Remove a line when its file is capped.
UNCAPPED_APT_EXCEPTIONS: dict[str, datetime.date] = {
    name: datetime.date(2026, 10, 26)
    for name in (
        "docs.yml",
        "doxygen-public-api.yml",
        "fuzz.yml",
        "go-ci.yml",
        "lint-and-format.yml",
        "mini-retrain.yml",
        "nightly.yml",
        "published-rc-licence-companions.yml",
        "sanitizers.yml",
    )
}


class LimitError(ValueError):
    """A ``timeout-minutes`` value that is not a whole number of minutes."""


def minutes(value: Any, where: str) -> int:
    """The whole minutes of a ``timeout-minutes`` value, or ``LimitError``."""
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise LimitError(f"{where}: timeout-minutes {value!r} is not a whole number")
    return value


def runs_apt_install(step: dict[str, Any]) -> bool:
    """True when a step's shell script installs apt packages."""
    run = step.get("run")
    return isinstance(run, str) and "apt-get install" in run


def apt_exempt(workflow: str, today: datetime.date) -> bool:
    """True while ``workflow`` holds an unexpired uncapped-apt exception."""
    expiry = UNCAPPED_APT_EXCEPTIONS.get(workflow)
    return expiry is not None and today <= expiry


def job_findings(workflow: str, doc: Any, today: datetime.date | None = None) -> list[str]:
    """One line per job below its step sum and per uncapped apt install step."""
    exempt = apt_exempt(workflow, today or datetime.date.today())
    findings: list[str] = []
    jobs = doc.get("jobs") if isinstance(doc, dict) else None
    if not isinstance(jobs, dict):
        return findings
    for job_id, job in jobs.items():
        if not isinstance(job, dict) or not isinstance(job.get("steps"), list):
            continue  # a reusable-workflow call has no steps of its own
        where = f"{workflow}: job {job_id}"
        limit = minutes(job.get("timeout-minutes", GITHUB_DEFAULT_JOB_MINUTES), where)
        step_sum = 0
        for index, step in enumerate(job["steps"]):
            if not isinstance(step, dict):
                continue
            name = step.get("name", f"step {index}")
            if "timeout-minutes" in step:
                step_sum += minutes(step["timeout-minutes"], f"{where}, {name}")
            elif runs_apt_install(step) and not exempt:
                findings.append(f"{where}, {name}: apt-get install step has no timeout-minutes")
        if limit < step_sum:
            findings.append(
                f"{where}: timeout-minutes {limit} is below its step caps' sum {step_sum}"
            )
    return findings


def check(directory: Path) -> list[str]:
    """The findings of every workflow file in ``directory``."""
    findings: list[str] = []
    for path in sorted(directory.iterdir()):
        if path.suffix not in (".yml", ".yaml") or not path.is_file():
            continue
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        findings.extend(job_findings(path.name, doc))
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Hold every workflow job's time limit at or above its step limits' sum."
    )
    parser.add_argument("--dir", type=Path, default=WORKFLOW_DIR, help="workflow directory")
    args = parser.parse_args(argv)
    try:
        findings = check(args.dir)
    except (OSError, yaml.YAMLError, LimitError) as exc:
        print(f"check_job_timeout_budget: cannot check {args.dir}: {exc}", file=sys.stderr)
        return 2
    for line in findings:
        print(f"check_job_timeout_budget: {line}", file=sys.stderr)
    if findings:
        print(
            "check_job_timeout_budget: cap each apt step at 1.25 times its slowest measured"
            " run; raise the job limit to the step caps plus 1.25 times the measured"
            " longest healthy run of the other steps",
            file=sys.stderr,
        )
        return 1
    print("every job's timeout-minutes covers the sum of its step caps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
