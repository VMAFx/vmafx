#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Validate same-run Scorecard reports without confusing local and remote scopes.

ADR-1247; schema/risk weights are pinned to Scorecard 5.5.0. The upstream
publisher scans remote HEAD, while PR mode scans files and reports unknown SHA.
"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import math
import os
import re
import shutil
import stat
import subprocess
import sys
from dataclasses import asdict, dataclass
from datetime import datetime
from fractions import Fraction
from pathlib import Path
from typing import cast

VERSION = "v5.5.0"
TOOL_COMMIT = "c395761df6afe1a69e476bc60a013a94bcbc153f"
ACTION_COMMIT = "2d1146689b8cda280b9bc96326124645441f03bc"
MINIMUM = Fraction(17, 2)
MAX_SCORE = 10
MAX_LINK_HOPS = 40  # Bound resolution on the Linux hosted runner, including growing cycles.
ROUNDING_TOLERANCE = 0.050000001
# Upstream Critical/High/Medium/Low = 10/7.5/5/2.5, scaled by 1/2.5.
WEIGHTS = {
    "Binary-Artifacts": 3,
    "Branch-Protection": 3,
    "CI-Tests": 1,
    "CII-Best-Practices": 1,
    "Code-Review": 3,
    "Contributors": 1,
    "Dangerous-Workflow": 4,
    "Dependency-Update-Tool": 3,
    "Fuzzing": 2,
    "License": 1,
    "Maintained": 3,
    "Packaging": 2,
    "Pinned-Dependencies": 2,
    "SAST": 2,
    "Security-Policy": 2,
    "Signed-Releases": 3,
    "Token-Permissions": 3,
    "Vulnerabilities": 3,
}
LOCAL_CHECKS = frozenset(WEIGHTS) - {
    "Branch-Protection",
    "CI-Tests",
    "CII-Best-Practices",
    "Code-Review",
    "Contributors",
    "Maintained",
    "Signed-Releases",
}
SHA = re.compile(r"[0-9a-f]{40}\Z")


class InvalidReport(ValueError):
    """Missing, contradictory or unbound evidence cannot satisfy the gate."""


def object_value(value: object, label: str) -> dict[str, object]:
    if not isinstance(value, dict) or not all(isinstance(k, str) for k in value):
        raise InvalidReport(f"{label} must be an object")
    return cast(dict[str, object], value)


def text_value(value: object, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise InvalidReport(f"{label} must be a nonempty string")
    return value


def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise InvalidReport(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(path: Path) -> dict[str, object]:
    if path.stat().st_size > 16 * 1024 * 1024:
        raise InvalidReport("report exceeds 16 MiB")
    value: object = json.loads(path.read_text(), object_pairs_hook=unique_object)
    return object_value(value, str(path))


@dataclass(frozen=True)
class Check:
    name: str
    score: int
    reason: str
    state: str


@dataclass(frozen=True)
class Assessment:
    scope: str
    aggregate: float
    checks: list[Check]
    failures: list[str]


def validate_context(report: dict[str, object], scope: str, repository: str, sha: str) -> None:
    if scope not in {"master", "local"} or not SHA.fullmatch(sha):
        raise InvalidReport("expected scope or full commit is invalid")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise InvalidReport("expected repository must be owner/name")
    expected = {"name": f"github.com/{repository}", "commit": sha}
    if scope == "local":
        expected = {"name": "file://.", "commit": "unknown"}
    if object_value(report.get("repo"), "repo") != expected:
        raise InvalidReport(f"report repository/commit does not match {scope} scope")
    if object_value(report.get("scorecard"), "scorecard") != {
        "version": VERSION,
        "commit": TOOL_COMMIT,
    }:
        raise InvalidReport("Scorecard version/source commit differs from reviewed tool")
    date = text_value(report.get("date"), "date")
    try:
        datetime.fromisoformat(date.replace("Z", "+00:00"))
    except ValueError as error:
        raise InvalidReport("invalid report date") from error


def parse_check(raw: object, scope: str) -> Check:
    item = object_value(raw, "check")
    name = text_value(item.get("name"), "check name")
    if name not in WEIGHTS:
        raise InvalidReport(f"unknown check: {name}")
    score = item.get("score")
    if not isinstance(score, int) or isinstance(score, bool) or not -1 <= score <= MAX_SCORE:
        raise InvalidReport(f"{name}: score must be an integer from -1 through 10")
    reason = text_value(item.get("reason"), f"{name} reason")
    state = "scored" if score > 0 else "zero"
    if score < 0:
        state = "inconclusive"
        if scope == "master" and name == "Signed-Releases" and reason == "no releases found":
            state = "unassessed: no releases (not signed)"
    return Check(name, score, reason, state)


def validate_rounded(reported: object, aggregate: Fraction) -> None:
    if (
        not isinstance(reported, (int, float))
        or isinstance(reported, bool)
        or not 0 <= reported <= MAX_SCORE
        or not math.isfinite(reported)
        or abs(float(aggregate) - reported) > ROUNDING_TOLERANCE
        or not math.isclose(reported * 10, round(reported * 10))
    ):
        raise InvalidReport("reported rounded aggregate contradicts per-check scores")


def assess(report: dict[str, object], scope: str, repository: str, sha: str) -> Assessment:
    validate_context(report, scope, repository, sha)
    raw_checks = report.get("checks")
    if not isinstance(raw_checks, list):
        raise InvalidReport("checks must be a list")
    checks = [parse_check(raw, scope) for raw in raw_checks]
    names = {check.name for check in checks}
    required = LOCAL_CHECKS if scope == "local" else frozenset(WEIGHTS)
    if names != required or len(names) != len(checks):
        raise InvalidReport(
            f"duplicate/wrong check coverage; missing={sorted(required - names)}, extra={sorted(names - required)}"
        )
    failures: list[str] = []
    numerator = denominator = 0
    for check in checks:
        if check.state == "inconclusive" or "internal error" in check.reason.lower():
            failures.append(f"{check.name}: {check.state}: {check.reason}")
        if check.score >= 0:
            numerator += WEIGHTS[check.name] * check.score
            denominator += WEIGHTS[check.name]
    if denominator == 0:
        raise InvalidReport("no assessed checks")
    aggregate = Fraction(numerator, denominator)
    validate_rounded(report.get("score"), aggregate)
    if aggregate < MINIMUM:
        failures.append(f"unrounded aggregate {float(aggregate):.6f} is below 8.5")
    return Assessment(scope, float(aggregate), sorted(checks, key=lambda c: c.name), failures)


def git(root: Path, *args: str) -> bytes:
    # ADR-1239: neither -C nor temporary repositories isolate inherited GIT_*.
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    executable = shutil.which("git")
    if executable is None:
        raise InvalidReport("Git is required for source binding")
    return subprocess.run(  # noqa: S603 -- ADR-1247: fixed Git inspection argv, no shell
        [executable, "--no-optional-locks", "-C", str(root), *args],
        env=env,
        check=True,
        capture_output=True,
        timeout=60,
    ).stdout


def validate_link(name: str, modes: dict[str, str], targets: dict[str, str]) -> None:
    """Every followed component must itself be committed, never mutable metadata."""
    pending = name.split("/")
    resolved: list[str] = []
    seen: set[tuple[str, tuple[str, ...]]] = set()
    hops = 0
    while pending:
        part, *pending = pending
        if part in {"", "."}:
            continue
        if part == "..":
            if not resolved:
                raise InvalidReport(f"symlink escapes scan root: {name}")
            resolved.pop()
            continue
        candidate = "/".join([*resolved, part])
        mode = modes.get(candidate)
        if mode == "120000":
            state = (candidate, tuple(pending))
            hops += 1
            if state in seen or hops > MAX_LINK_HOPS or Path(targets[candidate]).is_absolute():
                raise InvalidReport(f"cyclic or absolute symlink input: {name}")
            seen.add(state)
            pending = targets[candidate].split("/") + pending
            continue
        if mode not in {"040000", "100644", "100755"} or (pending and mode != "040000"):
            raise InvalidReport(f"symlink target is not a tracked input: {name}")
        resolved.append(part)


def blob_bytes(path: Path, mode: str) -> tuple[bytes, str | None]:
    info = path.lstat()
    if mode == "120000":
        if not stat.S_ISLNK(info.st_mode):
            raise InvalidReport(f"expected symlink: {path}")
        target = os.readlink(path)  # noqa: PTH115 -- ADR-1247: literal symlink bytes
        return os.fsencode(target), target
    if not stat.S_ISREG(info.st_mode) or bool(info.st_mode & 0o111) != (mode == "100755"):
        raise InvalidReport(f"unexpected file mode: {path}")
    return path.read_bytes(), None


def source_identity(root: Path, sha: str, allowed_output: str | None = None) -> dict[str, object]:
    """Bind actual file bytes to Git objects, including skip-worktree paths."""
    if not SHA.fullmatch(sha) or git(root, "rev-parse", "HEAD").decode().strip() != sha:
        raise InvalidReport("checkout is not the exact requested commit")
    extras = {os.fsdecode(p) for p in git(root, "ls-files", "--others", "-z").split(b"\0") if p}
    if extras - ({allowed_output} if allowed_output else set()):
        raise InvalidReport("scan checkout contains untracked/ignored input files")
    entries: list[str] = []
    modes: dict[str, str] = {}
    targets: dict[str, str] = {}
    for entry in git(root, "ls-tree", "-rtz", "HEAD").split(b"\0"):
        if not entry:
            continue
        metadata, raw_name = entry.split(b"\t", 1)
        mode, kind, oid = metadata.decode().split()
        name = os.fsdecode(raw_name)
        path = root / name
        info = path.lstat()
        modes[name] = mode
        if kind == "tree" and mode == "040000":
            if not stat.S_ISDIR(info.st_mode):
                raise InvalidReport(f"tracked directory replaced: {name}")
            continue
        if kind != "blob" or mode not in {"100644", "100755", "120000"}:
            raise InvalidReport(f"unsupported scan input mode: {name}")
        data, target = blob_bytes(path, mode)
        if target is not None:
            targets[name] = target
        # A git blob object id IS SHA-1 — this recomputes the oid git itself
        # recorded and compares it, so the algorithm is dictated by the object
        # format and cannot be "upgraded" to SHA-256 without reading a different
        # value than git stored. It is an integrity check against a name git
        # chose, not a signature. `usedforsecurity=False` already says so to
        # hashlib; this says it to semgrep.
        # nosemgrep: python.lang.security.insecure-hash-algorithms.insecure-hash-algorithm-sha1
        actual = hashlib.sha1(
            b"blob " + str(len(data)).encode() + b"\0" + data, usedforsecurity=False
        ).hexdigest()
        if actual != oid:
            raise InvalidReport(f"scan input differs from requested commit: {name}")
        entries.append(f"{mode} {oid} {name}\0")
    for name in targets:
        validate_link(name, modes, targets)
    if not entries:
        raise InvalidReport("scan checkout has no tracked inputs")
    return {
        "commit": sha,
        "tree": git(root, "rev-parse", "HEAD^{tree}").decode().strip(),
        "files": len(entries),
        "inventory_sha256": hashlib.sha256("".join(entries).encode()).hexdigest(),
    }


def master_identity(reference: dict[str, object], sha: str) -> dict[str, object]:
    """Reject advancement between upstream's separate GraphQL/archive reads."""
    target = object_value(reference.get("object"), "master ref object")
    if (
        reference.get("ref") != "refs/heads/master"
        or target.get("type") != "commit"
        or target.get("sha") != sha
        or not SHA.fullmatch(sha)
    ):
        raise InvalidReport("remote master moved or its final ref is invalid")
    return {"ref": "refs/heads/master", "commit": sha}


def markdown(assessment: Assessment) -> str:
    label = (
        "PR local file checks (not the full repository score)"
        if assessment.scope == "local"
        else "master full repository checks"
    )
    lines = [
        f"## Scorecard: {label}",
        "",
        f"Unrounded aggregate: {assessment.aggregate:.6f}; required: 8.5.",
        "",
        "| Check | Score | State | Reason |",
        "| --- | --- | --- | --- |",
    ]
    for check in assessment.checks:
        values = [check.name, str(check.score), check.state, check.reason]
        lines.append(
            "| "
            + " | ".join(
                html.escape(v).replace("|", "&#124;").replace("\n", " ").replace("\r", " ")
                for v in values
            )
            + " |"
        )
    lines += ["", "Result: " + ("FAIL" if assessment.failures else "PASS")]
    lines.extend("- " + html.escape(f).replace("\n", " ") for f in assessment.failures)
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["snapshot", "local", "master"])
    parser.add_argument("--sha", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--report", type=Path)
    parser.add_argument("--snapshot", type=Path)
    parser.add_argument("--master-ref", type=Path)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--summary", type=Path)
    args = parser.parse_args()
    try:
        if args.mode == "snapshot":
            if args.receipt.resolve().is_relative_to(args.root.resolve()):
                raise InvalidReport("snapshot must be outside the scanned checkout")
            receipt = source_identity(args.root, args.sha)
            receipt.update(
                repository=args.repository,
                run_id=os.environ.get("GITHUB_RUN_ID"),
                run_attempt=os.environ.get("GITHUB_RUN_ATTEMPT"),
            )
            args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")
            return 0
        if args.report is None:
            raise InvalidReport("--report is required")
        binding: dict[str, object] | None = None
        if args.mode == "local":
            if args.snapshot is None:
                raise InvalidReport("local mode requires a before-scan source snapshot")
            binding = source_identity(
                args.root,
                args.sha,
                (
                    str(args.report.relative_to(args.root))
                    if args.report.is_absolute()
                    else str(args.report)
                ),
            )
            before = load_json(args.snapshot)
            expected_binding = dict(
                binding,
                repository=args.repository,
                run_id=os.environ.get("GITHUB_RUN_ID"),
                run_attempt=os.environ.get("GITHUB_RUN_ATTEMPT"),
            )
            if before != expected_binding:
                raise InvalidReport(
                    "before/after source, repository or workflow-run binding differs"
                )
        if args.mode == "master":
            if args.master_ref is None:
                raise InvalidReport("master mode requires the final live master-ref receipt")
            binding = master_identity(load_json(args.master_ref), args.sha)
        report = load_json(args.report)
        assessment = assess(report, args.mode, args.repository, args.sha)
        receipt = asdict(assessment)
        receipt.update(
            repository=args.repository,
            commit=args.sha,
            source=binding,
            report_sha256=hashlib.sha256(args.report.read_bytes()).hexdigest(),
            scorecard=report["scorecard"],
            action_commit=ACTION_COMMIT,
            run_id=os.environ.get("GITHUB_RUN_ID"),
            run_attempt=os.environ.get("GITHUB_RUN_ATTEMPT"),
        )
        args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")
        summary = markdown(assessment)
        print(summary)
        if args.summary:
            with args.summary.open("a") as stream:
                stream.write(summary)
        return int(bool(assessment.failures))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Scorecard gate failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
