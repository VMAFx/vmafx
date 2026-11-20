#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Audit every project Python lock with pip-audit and a planted vulnerable pin.

The lock set comes from ``requirements/locks/manifest.json``; the manifest is
already the install authority, so a new lock is audited without a second list.
Each finding is keyed by lock file, package and advisory. A retained finding
needs one dated entry in ``scripts/ci/pip-audit-ignore.json``; an expired entry
or an entry that matches nothing is a failure. ``--self-test`` first audits
``scripts/ci/fixtures/pip-audit-known-vuln.txt`` and refuses to trust a green
run unless pip-audit reports the planted advisory (issue 2749).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import date
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "requirements" / "locks" / "manifest.json"
DEFAULT_IGNORE = REPO_ROOT / "scripts" / "ci" / "pip-audit-ignore.json"
DEFAULT_FIXTURE = REPO_ROOT / "scripts" / "ci" / "fixtures" / "pip-audit-known-vuln.txt"
LOCK_SUFFIXES = ("-lock.txt", "requirements-frozen.txt")
FIELDS = ("file", "package", "id", "reason", "expires")


class AuditError(Exception):
    """The audit could not run, or the exception list is malformed."""


def project_locks(root: Path = REPO_ROOT) -> list[str]:
    """Return the project locks named by the lock manifest, not the tool lock."""
    manifest = root / "requirements" / "locks" / "manifest.json"
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AuditError(f"cannot read {manifest}: {exc}") from exc
    entries = data.get("locks") if isinstance(data, dict) else None
    if not isinstance(entries, list):
        raise AuditError(f"{manifest}: locks must be a list")
    locks: list[str] = []
    for entry in entries:
        output = entry.get("output") if isinstance(entry, dict) else None
        if not isinstance(output, str):
            raise AuditError(f"{manifest}: every lock needs a string output")
        if output.startswith("requirements/locks/"):
            continue
        if output.endswith(LOCK_SUFFIXES) and output not in locks:
            locks.append(output)
    if not locks:
        raise AuditError(f"{manifest}: no project locks found")
    return sorted(locks)


def load_ignore(path: Path) -> list[dict[str, str]]:
    """Load and validate the dated exception list."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise AuditError(f"cannot read {path}: {exc}") from exc
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise AuditError(f"{path}: schema_version must be 1")
    exceptions = data.get("exceptions")
    if not isinstance(exceptions, list):
        raise AuditError(f"{path}: exceptions must be a list")
    parsed: list[dict[str, str]] = []
    for index, entry in enumerate(exceptions):
        if not isinstance(entry, dict):
            raise AuditError(f"{path}: exceptions[{index}] must be an object")
        for field in FIELDS:
            if not isinstance(entry.get(field), str) or not entry[field]:
                raise AuditError(f"{path}: exceptions[{index}].{field} must be a string")
        try:
            expiry = date.fromisoformat(entry["expires"])
        except ValueError as exc:
            raise AuditError(f"{path}: exceptions[{index}].expires must be YYYY-MM-DD") from exc
        if expiry < date.today():
            raise AuditError(f"{path}: exceptions[{index}] expired on {entry['expires']}")
        parsed.append({field: entry[field] for field in FIELDS})
    return parsed


def run_audit(root: Path, requirement: Path) -> dict[str, object]:
    """Run pip-audit for one lock and return its JSON report."""
    command = [
        sys.executable,
        "-m",
        "pip_audit",
        "--requirement",
        str(requirement),
        "--no-deps",
        "--disable-pip",
        "--format",
        "json",
        "--progress-spinner",
        "off",
    ]
    try:
        completed = subprocess.run(  # noqa: S603 - fixed argv, no shell
            command,
            cwd=root,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
            timeout=900,
        )
    except subprocess.TimeoutExpired as exc:
        raise AuditError(f"pip-audit timed out on {requirement}") from exc
    if completed.returncode not in (0, 1):
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise AuditError(f"pip-audit exited {completed.returncode} on {requirement}: {detail}")
    try:
        report = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise AuditError(f"pip-audit produced no JSON for {requirement}") from exc
    if not isinstance(report, dict) or not isinstance(report.get("dependencies"), list):
        raise AuditError(f"pip-audit JSON for {requirement} has no dependencies list")
    return report


def finding_key(lock: str, package: str, advisory: str) -> tuple[str, str, str]:
    return lock, package.lower(), advisory


def collect_findings(
    lock: str, report: dict[str, object], used: set[int], exceptions: list[dict[str, str]]
) -> list[str]:
    """Filter one pip-audit report through the exception list."""
    findings: list[str] = []
    dependencies = report.get("dependencies")
    assert isinstance(dependencies, list)
    for dependency in dependencies:
        if not isinstance(dependency, dict):
            raise AuditError(f"{lock}: pip-audit dependency is not an object")
        name = dependency.get("name")
        version = dependency.get("version")
        skip = dependency.get("skip_reason")
        if not isinstance(name, str) or not isinstance(version, str):
            raise AuditError(f"{lock}: pip-audit dependency lacks name/version")
        if skip and skip != "none":
            raise AuditError(f"{lock}: pip-audit skipped {name}=={version}: {skip}")
        vulns = dependency.get("vulns")
        if not isinstance(vulns, list):
            raise AuditError(f"{lock}: pip-audit dependency {name} lacks vulns")
        for vuln in vulns:
            if not isinstance(vuln, dict) or not isinstance(vuln.get("id"), str):
                raise AuditError(f"{lock}: pip-audit vulnerability lacks an id")
            key = finding_key(lock, name, vuln["id"])
            matched = next(
                (
                    index
                    for index, exception in enumerate(exceptions)
                    if (
                        exception["file"] == key[0]
                        and exception["package"].lower() == key[1]
                        and exception["id"] == key[2]
                    )
                ),
                None,
            )
            if matched is None:
                findings.append(f"{lock}: {name}=={version} {vuln['id']}")
            else:
                used.add(matched)
    return findings


def self_test(root: Path, fixture: Path) -> str:
    """Require pip-audit to report the planted advisory before trusting a green run."""
    if not fixture.is_file():
        raise AuditError(f"planted-defect fixture is missing: {fixture}")
    report = run_audit(root, fixture)
    advisories = [
        vuln.get("id")
        for dependency in report.get("dependencies", [])
        if isinstance(dependency, dict)
        for vuln in dependency.get("vulns", [])
        if isinstance(vuln, dict)
    ]
    if not advisories:
        raise AuditError("the planted-defect fixture produced no advisory")
    return ", ".join(sorted(str(item) for item in advisories))


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--ignore", type=Path, default=DEFAULT_IGNORE)
    parser.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE)
    parser.add_argument("--self-test-only", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    root = args.root.resolve()
    ignore = args.ignore if args.ignore.is_absolute() else root / args.ignore
    fixture = args.fixture if args.fixture.is_absolute() else root / args.fixture
    try:
        advisory = self_test(root, fixture)
        print(f"python-audit: planted defect detected ({advisory})")
        if args.self_test_only:
            return 0
        exceptions = load_ignore(ignore)
        locks = project_locks(root)
        used: set[int] = set()
        findings: list[str] = []
        for lock in locks:
            report = run_audit(root, root / lock)
            findings.extend(collect_findings(lock, report, used, exceptions))
        for index, exception in enumerate(exceptions):
            if index not in used:
                findings.append(
                    f"{exception['file']}: {exception['package']} {exception['id']} "
                    "exception matches no finding"
                )
    except AuditError as exc:
        print(f"python-audit: error: {exc}", file=sys.stderr)
        return 2
    if findings:
        for finding in findings:
            print(f"python-audit: {finding}", file=sys.stderr)
        print(
            f"python-audit: {len(findings)} finding(s); fix them or add a dated "
            "entry to scripts/ci/pip-audit-ignore.json",
            file=sys.stderr,
        )
        return 1
    print(
        f"python-audit: OK ({len(locks)} locks, {len(exceptions)} exception(s), "
        "planted defect rejected)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
