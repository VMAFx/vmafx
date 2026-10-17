#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The declared lint exception list.

A standard binds every tracked file. A file that cannot meet one (a byte-locked mirror, a
third-party source that carries no notice) is named here, once, per rule:

    .config/lint-exceptions.d/<rule>.toml

    [[exception]]
    path = "core/src/interop/pelorus_version.c"     # one tracked file, never a pattern
    reason = "byte-identical mirror of VMAFx/pelorus (ADR-1113)"
    expires = 2026-12-31                            # the day the exception stops holding

An exception stops holding on its ``expires`` date: ``filter`` no longer drops the file, so
the gate that consulted the list fails on it, and ``check`` reports the entry. ``check`` also
fails on a missing field, a rule that differs from the file name, a path that is not one
tracked file, a duplicate, and an expiry further out than ``MAX_DAYS`` (an exception is a
debt with a due date, not a tier).

A rule has one entry per file. A rule listed in ``PART_KEYS`` excepts one named part of a
file instead (``hosted-runner-label``: one job of a workflow file, key ``job``), so two
entries of that rule may name the same file when their parts differ; the same file and part
twice is a duplicate.

The root ``osv-scanner.toml`` carries advisory exceptions. osv-scanner reads ``ignoreUntil``
itself, but no CI job runs osv-scanner, so the date is enforced here.

Usage:
    lint_exceptions.py check
    lint_exceptions.py filter <rule> -- <file>...   # print the files the rule still reads
"""

from __future__ import annotations

import datetime as dt
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import tomllib

ROOT = Path(__file__).resolve().parents[2]
LIST_DIR = Path(".config/lint-exceptions.d")
OSV_FILE = Path("osv-scanner.toml")
MAX_DAYS = 400
MAX_ENTRIES = 4096
CHECK_ARGC = 2  # lint_exceptions.py check
FILTER_ARGC = 4  # lint_exceptions.py filter <rule> --
REQUIRED = ("path", "reason", "expires")
GLOB_CHARS = frozenset("*?[]{}")
# Rules whose entry excepts one part of a file, and the key that names the part.
PART_KEYS = {"hosted-runner-label": "job"}


@dataclass(frozen=True)
class Entry:
    """One declared exception."""

    rule: str
    path: str
    reason: str
    expires: dt.date
    part: str = ""  # the part of the file a ``PART_KEYS`` rule excepts, else empty


def today() -> dt.date:
    """Today's date; ``LINT_EXCEPTIONS_TODAY`` (ISO) overrides it for tests."""
    override = os.environ.get("LINT_EXCEPTIONS_TODAY")
    return dt.date.fromisoformat(override) if override else dt.date.today()


def load(root: Path = ROOT) -> tuple[list[Entry], list[str]]:
    """Read every rule file; return the entries and the findings of malformed ones."""
    entries: list[Entry] = []
    findings: list[str] = []
    for file in sorted((root / LIST_DIR).glob("*.toml")):
        rule = file.stem
        try:
            data = tomllib.loads(file.read_text(encoding="utf-8"))
        except (tomllib.TOMLDecodeError, OSError) as exc:
            findings.append(f"{file.name}: unreadable: {exc}")
            continue
        for index, raw in enumerate(data.get("exception", [])[:MAX_ENTRIES], start=1):
            where = f"{file.name} entry {index}"
            missing = [key for key in REQUIRED if not raw.get(key)]
            if missing or not isinstance(raw.get("expires"), dt.date):
                findings.append(f"{where}: needs {', '.join(REQUIRED)} (expires a TOML date)")
                continue
            if raw.get("rule", rule) != rule:
                findings.append(
                    f"{where}: rule {raw['rule']!r} differs from the file name {rule!r}"
                )
                continue
            key = PART_KEYS.get(rule)
            part = str(raw.get(key, "")) if key else ""
            entries.append(Entry(rule, str(raw["path"]), str(raw["reason"]), raw["expires"], part))
    return entries, findings


def tracked_files(root: Path = ROOT) -> frozenset[str]:
    """Every tracked path of the repository."""
    git = shutil.which("git")
    if git is None:
        raise SystemExit("lint_exceptions: git not found on PATH")
    out = subprocess.run(  # noqa: S603 -- resolved git, fixed argv
        [git, "-C", str(root), "ls-files", "-z"],
        capture_output=True,
        check=True,
        timeout=60,
    ).stdout
    return frozenset(p for p in out.decode("utf-8", "surrogateescape").split("\0") if p)


def validate(entries: list[Entry], tracked: frozenset[str], now: dt.date) -> list[str]:
    """Findings for a parsed list: stale, expired, over-long, pattern or duplicate entries."""
    findings: list[str] = []
    seen: set[tuple[str, str, str]] = set()
    for entry in entries:
        label = f"{entry.rule}: {entry.path}" + (f" ({entry.part})" if entry.part else "")
        if (entry.rule, entry.path, entry.part) in seen:
            findings.append(f"{label}: declared twice")
        seen.add((entry.rule, entry.path, entry.part))
        if (
            GLOB_CHARS & set(entry.path)
            or entry.path.startswith("/")
            or ".." in entry.path.split("/")
        ):
            findings.append(f"{label}: a path names one tracked file, not a pattern")
        elif entry.path not in tracked:
            findings.append(f"{label}: not a tracked file (remove the entry)")
        if entry.expires < now:
            findings.append(f"{label}: expired on {entry.expires}")
        elif (entry.expires - now).days > MAX_DAYS:
            findings.append(f"{label}: expires {entry.expires}, more than {MAX_DAYS} days out")
    return findings


def _check_osv_entry(raw: object, index: int, seen: set[str], now: dt.date) -> list[str]:
    """Validate one IgnoredVulns entry from osv-scanner.toml."""
    if not isinstance(raw, dict):
        return [f"osv-scanner.toml entry {index}: needs id, reason, ignoreUntil (a TOML date)"]
    raw_id = raw.get("id")
    vuln_id = raw_id.strip() if isinstance(raw_id, str) else ""
    where = f"osv-scanner.toml {vuln_id}" if vuln_id else f"osv-scanner.toml entry {index}"
    raw_reason = raw.get("reason")
    reason_ok = isinstance(raw_reason, str) and bool(raw_reason.strip())
    until = raw.get("ignoreUntil")
    if not vuln_id or not reason_ok or type(until) is not dt.date:
        return [f"{where}: needs id, reason, ignoreUntil (a TOML date)"]
    findings: list[str] = []
    if vuln_id in seen:
        findings.append(f"osv-scanner.toml {vuln_id}: declared twice")
    seen.add(vuln_id)
    if until < now:
        findings.append(f"osv-scanner.toml {vuln_id}: expired on {until}")
    elif (until - now).days > MAX_DAYS:
        findings.append(
            f"osv-scanner.toml {vuln_id}: expires {until}, more than {MAX_DAYS} days out"
        )
    return findings


def osv_findings(root: Path, now: dt.date) -> list[str]:
    """Findings for root osv-scanner.toml: missing fields, expired or over-long dates."""
    target = root / OSV_FILE
    if not target.is_file():
        return []
    try:
        data = tomllib.loads(target.read_text(encoding="utf-8"))
    except (tomllib.TOMLDecodeError, OSError) as exc:
        return [f"osv-scanner.toml: unreadable: {exc}"]
    raw_list = data.get("IgnoredVulns", [])
    if not isinstance(raw_list, list):
        return ["osv-scanner.toml: IgnoredVulns must be a list"]
    findings: list[str] = []
    seen: set[str] = set()
    for index, raw in enumerate(raw_list[:MAX_ENTRIES], start=1):
        findings.extend(_check_osv_entry(raw, index, seen, now))
    return findings


def relative(path: str, root: Path) -> str:
    """The repository-relative form of ``path`` (unchanged when it is outside the root)."""
    candidate = Path(path)
    if candidate.is_absolute():
        try:
            return candidate.resolve().relative_to(root).as_posix()
        except ValueError:
            return path
    return candidate.as_posix().removeprefix("./")


def still_read(
    rule: str, files: list[str], root: Path = ROOT, now: dt.date | None = None
) -> list[str]:
    """The files ``rule`` still checks: the list minus the live exceptions of ``rule``."""
    entries, _ = load(root)
    day = now or today()
    live = {e.path for e in entries if e.rule == rule and e.expires >= day}
    return [f for f in files if relative(f, root) not in live]


def main(argv: list[str]) -> int:
    """Entry point: ``check`` or ``filter <rule> -- files...``."""
    if len(argv) >= CHECK_ARGC and argv[1] == "check":
        entries, findings = load()
        findings += validate(entries, tracked_files(), today())
        findings += osv_findings(ROOT, today())
        for finding in findings:
            print(f"error: {finding}", file=sys.stderr)
        print(f"lint_exceptions: {len(entries)} exception(s), {len(findings)} finding(s)")
        return 1 if findings else 0
    if len(argv) >= FILTER_ARGC and argv[1] == "filter" and argv[3] == "--":
        for kept in still_read(argv[2], argv[4:]):
            print(kept)
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
