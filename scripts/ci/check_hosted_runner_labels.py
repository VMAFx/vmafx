#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check which hosted Ubuntu image every workflow job names (ADR-3251).

A job names its hosted Linux image by version: ``ubuntu-26.04`` or
``ubuntu-26.04-arm``. A floating label (``ubuntu-latest``, its larger-runner forms,
``ubuntu-slim``) moves the image under a job on a date GitHub picks, so it is refused
everywhere. Another versioned image is accepted in two cases only:

1. The job's subject is that image. Those jobs are listed in ``COMPATIBILITY_TARGETS``
   with the record that says why.
2. The job fails on the current image and falls back for a bounded time. It is declared in
   ``.config/lint-exceptions.d/hosted-runner-label.toml`` (format and expiry rules of the
   other entries: ``scripts/ci/lint_exceptions.py``) with two more keys, ``job`` and
   ``label``, and an end date at most ``FALLBACK_MAX_DAYS`` days out::

       [[exception]]
       path = ".github/workflows/example.yml"
       job = "build"
       label = "ubuntu-24.04"
       reason = "what fails on ubuntu-26.04, and the issue that tracks it"
       expires = 2026-12-01

A label is read wherever a workflow can spell one: a literal ``runs-on``, a matrix value,
the fallback of an expression, the matrix a job prints as JSON for a later job, an input
handed to a reusable workflow. The check therefore reads every string of the parsed
workflow (YAML comments are gone after parsing; comment lines inside a ``run:`` block are
dropped) and attributes it to the job that holds it. Text outside ``jobs`` belongs to the
pseudo job ``(workflow)``.

Exit 0 when every workflow passes, 1 on a finding.
"""

from __future__ import annotations

import datetime as dt
import re
import sys
from dataclasses import dataclass
from pathlib import Path

import tomllib
import yaml  # type: ignore[import-untyped]  # no PyYAML stubs in the hook environment

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lint_exceptions  # the shared exception list reader, beside this file

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_DIR = Path(".github/workflows")
FALLBACK_LIST = lint_exceptions.LIST_DIR / "hosted-runner-label.toml"
ALLOWED = frozenset({"ubuntu-26.04", "ubuntu-26.04-arm"})
FLOATING_PREFIXES = ("ubuntu-latest", "ubuntu-slim")
# (workflow file, job id) -> (image the job is about, why).
COMPATIBILITY_TARGETS: dict[tuple[str, str], tuple[str, str]] = {
    ("supply-chain.yml", "verify-native-artifacts"): (
        "ubuntu-24.04",
        "ADR-1354: oldest hosted image that loads the native Linux bundle",
    ),
}
FALLBACK_MAX_DAYS = 90
FALLBACK_KEYS = ("path", "job", "label", "reason", "expires")
OUTSIDE_JOBS = "(workflow)"
MAX_WORKFLOWS = 512
MAX_NODES = 200_000
MAX_FALLBACKS = 256
# `ubuntu-` followed by a version, `latest` or `slim`, with its suffixes (`-arm`,
# `-4-cores`). The look-behind keeps image names such as `dev-ubuntu-26.04` and apt
# sources such as `ubuntu-toolchain-r` out.
LABEL = re.compile(r"(?<![\w.-])ubuntu-(?:latest|slim|\d+\.\d+)(?:-[a-z0-9]+)*")


@dataclass(frozen=True)
class Fallback:
    """One declared fallback of one job to an older image."""

    workflow: str
    job: str
    label: str
    expires: dt.date


def labels_in(text: str) -> set[str]:
    """Hosted Ubuntu labels in ``text``, comment lines of a script dropped."""
    kept = [line for line in text.splitlines() if not line.lstrip().startswith("#")]
    return set(LABEL.findall("\n".join(kept)))


def strings_of(node: object) -> list[str]:
    """Every string below ``node``, mapping keys included, without recursion."""
    found: list[str] = []
    stack: list[object] = [node]
    for _ in range(MAX_NODES + 1):
        if not stack:
            return found
        item = stack.pop()
        if isinstance(item, str):
            found.append(item)
        elif isinstance(item, dict):
            stack.extend(item.keys())
            stack.extend(item.values())
        elif isinstance(item, list):
            stack.extend(item)
    msg = f"more than {MAX_NODES} nodes in one job, raise the bound"
    raise ValueError(msg)


def job_labels(workflow: object) -> dict[str, set[str]]:
    """Labels per job id; text outside ``jobs`` is collected under ``OUTSIDE_JOBS``."""
    if not isinstance(workflow, dict):
        msg = "workflow is not a mapping"
        raise ValueError(msg)  # one finding type for every unreadable workflow
    jobs = workflow.get("jobs")
    if not isinstance(jobs, dict):
        msg = "workflow has no `jobs` mapping"
        raise ValueError(msg)  # as above
    rest = {key: value for key, value in workflow.items() if key != "jobs"}
    found = {OUTSIDE_JOBS: set().union(*(labels_in(s) for s in strings_of(rest)))}
    for job_id, body in jobs.items():
        strings = strings_of(body)
        found[str(job_id)] = set().union(*(labels_in(s) for s in strings))
    return found


def read_workflows(root: Path) -> tuple[dict[tuple[str, str], set[str]], list[str]]:
    """Labels of every ``(workflow file, job id)`` under ``root``, and the read findings."""
    base = root / WORKFLOW_DIR
    paths = sorted(p for p in base.glob("*.y*ml") if p.is_file())
    table: dict[tuple[str, str], set[str]] = {}
    findings: list[str] = []
    if len(paths) > MAX_WORKFLOWS:
        findings.append(f"{WORKFLOW_DIR}: more than {MAX_WORKFLOWS} workflows, raise the bound")
    for path in paths[:MAX_WORKFLOWS]:
        try:
            parsed = yaml.safe_load(path.read_text(encoding="utf-8"))
            per_job = job_labels(parsed)
        except (OSError, UnicodeDecodeError, yaml.YAMLError, ValueError) as exc:
            findings.append(f"{WORKFLOW_DIR / path.name}: unreadable: {exc}")
            continue
        for job_id, labels in per_job.items():
            table[(path.name, job_id)] = labels
    if not paths:
        findings.append(f"no workflow found under {WORKFLOW_DIR}")
    return table, findings


def _fallback_of(raw: object, index: int) -> tuple[Fallback | None, str]:
    """One entry of the fallback list, or the reason it cannot be read."""
    where = f"{FALLBACK_LIST}: entry {index + 1}"
    if not isinstance(raw, dict):
        return None, f"{where}: not a table"
    missing = [key for key in FALLBACK_KEYS if key not in raw]
    if missing:
        return None, f"{where}: missing {', '.join(missing)}"
    expires = raw["expires"]
    if not isinstance(expires, dt.date) or isinstance(expires, dt.datetime):
        return None, f"{where}: `expires` is not a date (write expires = YYYY-MM-DD)"
    path = Path(str(raw["path"]))
    if path.parent != WORKFLOW_DIR:
        return None, f"{where}: `path` is not a file of {WORKFLOW_DIR}"
    return Fallback(path.name, str(raw["job"]), str(raw["label"]), expires), ""


def load_fallbacks(root: Path) -> tuple[list[Fallback], list[str]]:
    """Declared fallbacks under ``root`` and the findings of malformed entries."""
    path = root / FALLBACK_LIST
    if not path.is_file():
        return [], []
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as exc:
        return [], [f"{FALLBACK_LIST}: unreadable: {exc}"]
    raw_entries = data.get("exception", [])
    if not isinstance(raw_entries, list):
        return [], [f"{FALLBACK_LIST}: `exception` is not an array of tables"]
    fallbacks: list[Fallback] = []
    findings: list[str] = []
    if len(raw_entries) > MAX_FALLBACKS:
        findings.append(f"{FALLBACK_LIST}: more than {MAX_FALLBACKS} entries, raise the bound")
    for index, raw in enumerate(raw_entries[:MAX_FALLBACKS]):
        fallback, reason = _fallback_of(raw, index)
        if fallback is None:
            findings.append(reason)
        else:
            fallbacks.append(fallback)
    return fallbacks, findings


def check_fallbacks(
    fallbacks: list[Fallback], table: dict[tuple[str, str], set[str]], now: dt.date
) -> tuple[set[tuple[str, str, str]], list[str]]:
    """Live ``(workflow, job, label)`` fallbacks and the findings of the others."""
    live: set[tuple[str, str, str]] = set()
    findings: list[str] = []
    limit = now + dt.timedelta(days=FALLBACK_MAX_DAYS)
    for entry in fallbacks:
        where = f"{FALLBACK_LIST}: {entry.workflow} job '{entry.job}'"
        if entry.label in ALLOWED or entry.label.startswith(FLOATING_PREFIXES):
            findings.append(f"{where}: '{entry.label}' is not an older versioned image")
        elif entry.label not in table.get((entry.workflow, entry.job), set()):
            findings.append(f"{where}: the job does not name '{entry.label}' (remove the entry)")
        elif entry.expires < now:
            findings.append(f"{where}: expired on {entry.expires.isoformat()}")
        elif entry.expires > limit:
            findings.append(
                f"{where}: ends {entry.expires.isoformat()}, more than "
                f"{FALLBACK_MAX_DAYS} days out"
            )
        else:
            live.add((entry.workflow, entry.job, entry.label))
    return live, findings


def check_labels(
    table: dict[tuple[str, str], set[str]], live: set[tuple[str, str, str]]
) -> list[str]:
    """Findings for every label that is floating, or older without a reason on record."""
    findings: list[str] = []
    for (workflow, job), labels in sorted(table.items()):
        where = f"{WORKFLOW_DIR / workflow}: job '{job}'"
        target = COMPATIBILITY_TARGETS.get((workflow, job), ("", ""))[0]
        for label in sorted(labels - ALLOWED):
            if label.startswith(FLOATING_PREFIXES):
                findings.append(
                    f"{where}: names the floating label '{label}'; name a version, "
                    "`ubuntu-26.04` or `ubuntu-26.04-arm`"
                )
            elif label != target and (workflow, job, label) not in live:
                findings.append(
                    f"{where}: names '{label}' without a dated fallback in {FALLBACK_LIST}"
                )
    for (workflow, job), (label, why) in sorted(COMPATIBILITY_TARGETS.items()):
        if label not in table.get((workflow, job), set()):
            findings.append(
                f"{WORKFLOW_DIR / workflow}: job '{job}' no longer names '{label}' ({why}); "
                "remove it from COMPATIBILITY_TARGETS"
            )
    return findings


def main(argv: list[str]) -> int:
    """Check every workflow under the repository root (argv[1], default: this checkout)."""
    root = Path(argv[1]).resolve() if len(argv) > 1 else ROOT
    table, findings = read_workflows(root)
    fallbacks, malformed = load_fallbacks(root)
    live, stale = check_fallbacks(fallbacks, table, lint_exceptions.today())
    findings += malformed + stale + check_labels(table, live)
    for finding in findings:
        print(f"error: {finding}", file=sys.stderr)
    named = sum(1 for labels in table.values() if labels)
    print(
        f"check_hosted_runner_labels: {len(table)} job(s), {named} naming a hosted Ubuntu "
        f"image, {len(live)} dated fallback(s), {len(findings)} finding(s)"
    )
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
