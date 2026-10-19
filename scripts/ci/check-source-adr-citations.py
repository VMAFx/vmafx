#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Bind source ``ADR-NNNN`` citations to exact decisions (ADR-1311).

Markdown links carry a slug and are checked by ``check-adr-links.py``. Plain
source citations carry only a number. The gate derives the live binding
(number, exact ADR filename, source sites and counts) from the tree on every
run, so a change that adds or moves a citation edits no shared file (ADR-2200).
The registry keeps only what no tree can derive and a reviewer must author:
retired identities (a number cited for a decision that no longer has its file)
and the exact-path fixture exemptions (synthetic numbers). The gate refuses a
citation of a number that has no ADR file and no record, a retired number that
was reallocated, and a fixture number cited outside its recorded paths.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

TOKEN_BYTES = re.compile(rb"\bADR-([0-9]{4})\b")
NUMBER = re.compile(r"[0-9]{4}")
COMMIT = re.compile(r"[0-9a-f]{40}")

# This is deliberately an allowlist. Human prose, changelogs, patches, model
# data and generated reports are governed by their own checks and must not turn
# into source-citation false positives.
SOURCE_SUFFIXES = frozenset(
    {
        ".bash",
        ".build",
        ".c",
        ".cc",
        ".cpp",
        ".cu",
        ".cuh",
        ".cxx",
        ".go",
        ".h",
        ".hh",
        ".hip",
        ".hpp",
        ".in",
        ".ini",
        ".lua",
        ".m",
        ".metal",
        ".mk",
        ".mm",
        ".proto",
        ".ps1",
        ".pxd",
        ".py",
        ".pyx",
        ".rs",
        ".sh",
        ".sql",
        ".toml",
        ".yaml",
        ".yml",
        ".zsh",
    }
)
SOURCE_BASENAMES = frozenset({"CMakeLists.txt", "Makefile", "meson.build", "meson_options.txt"})
SOURCE_NAME_PREFIXES = ("Containerfile", "Dockerfile")
PROSE_CONTROL_FILES = frozenset({"mkdocs.yml"})
DEFAULT_REGISTRY = Path("scripts/ci/source-adr-citations.json")
VALID_RETIREMENT_STATUSES = frozenset({"abandoned", "superseded", "unfiled-historical"})
REGISTRY_SCHEMA_VERSION = 2
RETIREMENT_KEYS = frozenset(
    {"status", "reason", "evidence", "git_commits", "successor", "related", "sites"}
)
MIN_ADR_FILENAME_LENGTH = len("0000-a.md")
MIN_RETIREMENT_REASON_LENGTH = 20
MIN_FIXTURE_REASON_LENGTH = 12
GIT = shutil.which("git") or "/usr/bin/git"


class GateError(RuntimeError):
    """A deterministic repository or registry contract failure."""


def run_git(root: Path, args: list[str]) -> str:
    """Run Git without consulting caller aliases; any failure is fatal."""
    proc = subprocess.run(  # noqa: S603 -- fixed Git binary; validated internal argv only
        [GIT, "-c", "alias.ls-files=", *args],
        cwd=root,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if proc.returncode != 0:
        detail = proc.stderr.strip() or proc.stdout.strip() or "no diagnostic"
        raise GateError(f"git {' '.join(args)} failed: {detail}")
    return proc.stdout


def tracked_paths(root: Path) -> list[str]:
    """Return normalized tracked paths from Git, rejecting unsafe names."""
    raw = run_git(root, ["ls-files", "-z"])
    paths: list[str] = []
    for value in raw.split("\0"):
        if not value:
            continue
        path = PurePosixPath(value)
        if path.is_absolute() or ".." in path.parts:
            raise GateError(f"git returned unsafe tracked path: {value!r}")
        paths.append(path.as_posix())
    return sorted(paths)


def is_source_path(relative: str) -> bool:
    """Whether a tracked path belongs to the source/control citation corpus."""
    path = PurePosixPath(relative)
    if not path.parts or path.parts[0] in {"docs", "changelog.d"}:
        return False
    if relative in PROSE_CONTROL_FILES:
        return False
    name = path.name
    return (
        path.suffix.lower() in SOURCE_SUFFIXES
        or name in SOURCE_BASENAMES
        or name.startswith(SOURCE_NAME_PREFIXES)
    )


def discover_sites(root: Path, paths: Iterable[str]) -> tuple[dict[str, dict[str, int]], int]:
    """Collect every citation count by number and source path."""
    sites: dict[str, dict[str, int]] = defaultdict(dict)
    scanned = 0
    for relative in paths:
        if not is_source_path(relative):
            continue
        scanned += 1
        path = root / relative
        # The inherited MATLAB corpus contains Windows-1252 comments. ADR
        # tokens are ASCII, so byte scanning covers it without decoding
        # unrelated prose or silently dropping invalid bytes.
        counts = Counter(value.decode("ascii") for value in TOKEN_BYTES.findall(path.read_bytes()))
        for number, count in sorted(counts.items()):
            sites[number][relative] = count
    return {
        number: dict(sorted(by_path.items())) for number, by_path in sorted(sites.items())
    }, scanned


def adr_corpus(root: Path) -> dict[str, str]:
    """Map each live ADR number to its one exact filename."""
    directory = root / "docs/adr"
    if not directory.is_dir():
        raise GateError(f"ADR directory does not exist: {directory}")
    by_number: dict[str, list[str]] = defaultdict(list)
    for path in sorted(directory.glob("[0-9][0-9][0-9][0-9]-*.md")):
        by_number[path.name[:4]].append(path.name)
    duplicates = {number: names for number, names in by_number.items() if len(names) != 1}
    if duplicates:
        rendered = ", ".join(
            f"ADR-{number}={names}" for number, names in sorted(duplicates.items())
        )
        raise GateError(f"ADR corpus has duplicate numbers: {rendered}")
    return {number: names[0] for number, names in sorted(by_number.items())}


def load_registry(path: Path) -> dict[str, Any]:
    """Load the checked registry with a precise diagnostic."""
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise GateError(f"registry does not exist: {path}") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise GateError(f"registry is not valid UTF-8 JSON: {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise GateError("registry root must be an object")
    if "live" in value:
        raise GateError(
            "registry has a 'live' section: live bindings are derived from the tree "
            "(ADR-2200); delete it and set schema_version to 2"
        )
    if value.get("schema_version") != REGISTRY_SCHEMA_VERSION:
        raise GateError(f"registry schema_version must be {REGISTRY_SCHEMA_VERSION}")
    expected = {"schema_version", "retired", "fixtures"}
    extra = set(value) - expected
    missing = expected - set(value)
    if extra or missing:
        raise GateError(f"registry keys differ: missing={sorted(missing)} extra={sorted(extra)}")
    for section in ("retired", "fixtures"):
        if not isinstance(value[section], dict):
            raise GateError(f"registry {section!r} must be an object")
    return value


def validate_number(number: str, section: str) -> None:
    if not NUMBER.fullmatch(number):
        raise GateError(f"{section} contains invalid ADR number {number!r}")


def validate_sites(
    value: Any, label: str, tracked: set[str], *, allow_empty: bool
) -> dict[str, int]:
    if not isinstance(value, dict) or (not value and not allow_empty):
        qualifier = "an object" if allow_empty else "a non-empty object"
        raise GateError(f"{label}.sites must be {qualifier}")
    result: dict[str, int] = {}
    for path, count in value.items():
        if not isinstance(path, str) or path not in tracked:
            raise GateError(f"{label}.sites names untracked path {path!r}")
        if not is_source_path(path):
            raise GateError(f"{label}.sites names out-of-scope path {path!r}")
        if not isinstance(count, int) or isinstance(count, bool) or count <= 0:
            raise GateError(f"{label}.sites[{path!r}] must be a positive integer")
        result[path] = count
    return dict(sorted(result.items()))


def validate_adr_filename(value: Any, label: str, corpus: dict[str, str]) -> str:
    if (
        not isinstance(value, str)
        or len(value) < MIN_ADR_FILENAME_LENGTH
        or not NUMBER.fullmatch(value[:4])
    ):
        raise GateError(f"{label} must be an exact NNNN-slug.md filename")
    number = value[:4]
    actual = corpus.get(number)
    if actual != value:
        if actual is None:
            raise GateError(f"{label} points to missing ADR file {value!r}")
        raise GateError(
            f"{label} expected {value!r}, but ADR-{number} is now {actual!r} (reallocated)"
        )
    return value


def validate_retirement_evidence(value: Any, label: str, tracked: set[str]) -> list[str]:
    if not isinstance(value, list) or not value:
        raise GateError(f"{label}.evidence must be a non-empty path list")
    for path in value:
        if not isinstance(path, str) or path not in tracked:
            raise GateError(f"{label}.evidence names untracked path {path!r}")
    return value


def validate_retirement_commits(root: Path, value: Any, label: str) -> list[str]:
    if not isinstance(value, list) or not value:
        raise GateError(f"{label}.git_commits must be a non-empty list")
    for commit in value:
        if not isinstance(commit, str) or not COMMIT.fullmatch(commit):
            raise GateError(f"{label} has invalid full commit id {commit!r}")
        run_git(root, ["cat-file", "-e", f"{commit}^{{commit}}"])
    return value


def validate_retirement_targets(
    successor: Any, related: Any, label: str, corpus: dict[str, str]
) -> tuple[str | None, list[str]]:
    if successor is not None:
        successor = validate_adr_filename(successor, f"{label}.successor", corpus)
    if not isinstance(related, list):
        raise GateError(f"{label}.related must be a list")
    normalized_related = [
        validate_adr_filename(target, f"{label}.related", corpus) for target in related
    ]
    if successor is None and not normalized_related:
        raise GateError(f"{label} needs a successor or at least one related ADR")
    return successor, normalized_related


def validate_retirement_entry(
    root: Path,
    number: str,
    value: Any,
    corpus: dict[str, str],
    tracked: set[str],
) -> dict[str, Any]:
    label = f"retired ADR-{number}"
    if not isinstance(value, dict) or set(value) != RETIREMENT_KEYS:
        raise GateError(f"{label} must contain exactly {sorted(RETIREMENT_KEYS)}")
    status = value["status"]
    if status not in VALID_RETIREMENT_STATUSES:
        raise GateError(f"{label}.status is invalid: {status!r}")
    reason = value["reason"]
    if not isinstance(reason, str) or len(reason.strip()) < MIN_RETIREMENT_REASON_LENGTH:
        raise GateError(f"{label}.reason must be a substantive sentence")
    evidence = validate_retirement_evidence(value["evidence"], label, tracked)
    commits = validate_retirement_commits(root, value["git_commits"], label)
    successor, related = validate_retirement_targets(
        value["successor"], value["related"], label, corpus
    )
    sites = validate_sites(value["sites"], label, tracked, allow_empty=True)
    return {
        "status": status,
        "reason": reason,
        "evidence": evidence,
        "git_commits": commits,
        "successor": successor,
        "related": related,
        "sites": sites,
    }


def validate_retired(
    root: Path,
    retired: dict[str, Any],
    corpus: dict[str, str],
    tracked: set[str],
) -> dict[str, dict[str, Any]]:
    normalized: dict[str, dict[str, Any]] = {}
    for number, value in sorted(retired.items()):
        validate_number(number, "retired")
        if number in corpus:
            raise GateError(
                f"retired number ADR-{number} was reallocated to {corpus[number]!r}; "
                "retired identities are reserved"
            )
        normalized[number] = validate_retirement_entry(root, number, value, corpus, tracked)
    return normalized


def validate_fixtures(fixtures: dict[str, Any], tracked: set[str]) -> dict[str, dict[str, Any]]:
    normalized: dict[str, dict[str, Any]] = {}
    for number, value in sorted(fixtures.items()):
        validate_number(number, "fixtures")
        if not isinstance(value, dict) or set(value) != {"reason", "sites"}:
            raise GateError(f"fixture ADR-{number} must contain only reason and sites")
        reason = value["reason"]
        if not isinstance(reason, str) or len(reason.strip()) < MIN_FIXTURE_REASON_LENGTH:
            raise GateError(f"fixture ADR-{number}.reason must explain the synthetic use")
        sites = validate_sites(value["sites"], f"fixture ADR-{number}", tracked, allow_empty=False)
        normalized[number] = {"reason": reason, "sites": sites}
    return normalized


def take_fixture_sites(
    discovered: dict[str, dict[str, int]], fixtures: dict[str, dict[str, Any]]
) -> tuple[dict[str, dict[str, int]], dict[str, dict[str, int]]]:
    """Separate exact fixture path/number pairs from decision citations."""
    remaining = {number: dict(sites) for number, sites in discovered.items()}
    actual: dict[str, dict[str, int]] = {}
    for number, entry in fixtures.items():
        actual[number] = {}
        for path in entry["sites"]:
            count = remaining.get(number, {}).pop(path, None)
            if count is not None:
                actual[number][path] = count
        if number in remaining and not remaining[number]:
            del remaining[number]
    return remaining, actual


def expected_live(
    discovered: dict[str, dict[str, int]],
    corpus: dict[str, str],
    retired: dict[str, dict[str, Any]],
) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, int]], list[str]]:
    live: dict[str, dict[str, Any]] = {}
    retired_sites: dict[str, dict[str, int]] = {number: {} for number in retired}
    unknown: list[str] = []
    for number, sites in sorted(discovered.items()):
        if number in retired:
            retired_sites[number] = sites
        elif number in corpus:
            live[number] = {"target": corpus[number], "sites": sites}
        else:
            rendered = ", ".join(f"{path} ({count})" for path, count in sites.items())
            unknown.append(f"ADR-{number}: {rendered}")
    return live, retired_sites, unknown


def compare_sites(
    label: str, expected: dict[str, dict[str, int]], actual_entries: dict[str, dict[str, Any]]
) -> list[str]:
    errors: list[str] = []
    for number, entry in actual_entries.items():
        actual = expected.get(number, {})
        if entry["sites"] != actual:
            errors.append(
                f"{label} ADR-{number} site drift: registry={entry['sites']} source={actual}"
            )
    return errors


def check(root: Path, registry_path: Path) -> int:
    tracked_list = tracked_paths(root)
    tracked = set(tracked_list)
    corpus = adr_corpus(root)
    registry = load_registry(registry_path)
    retired = validate_retired(root, registry["retired"], corpus, tracked)
    fixtures = validate_fixtures(registry["fixtures"], tracked)

    discovered, scanned = discover_sites(root, tracked_list)
    decision_sites, actual_fixture_sites = take_fixture_sites(discovered, fixtures)
    live, actual_retired_sites, unknown = expected_live(decision_sites, corpus, retired)
    if unknown:
        detail = "\n  ".join(unknown)
        raise GateError(
            "source cites a number that has no ADR file and no retirement record "
            "(file the ADR, or audit and record the retirement):\n  " + detail
        )

    errors = compare_sites("retired", actual_retired_sites, retired)
    errors.extend(compare_sites("fixture", actual_fixture_sites, fixtures))
    if errors:
        raise GateError(
            "\n".join(errors) + "\nretired and fixture records are hand-governed: correct them"
        )

    occurrences = sum(sum(entry["sites"].values()) for entry in live.values())
    occurrences += sum(sum(entry["sites"].values()) for entry in retired.values())
    print(
        "check-source-adr-citations: OK "
        f"({len(live)} live derived, {len(retired)} retired, {len(fixtures)} fixture identities; "
        f"{occurrences} governed occurrences in {scanned} source/control files)"
    )
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--registry", type=Path, default=DEFAULT_REGISTRY)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    root = args.root.resolve()
    registry_path = args.registry if args.registry.is_absolute() else root / args.registry
    try:
        return check(root, registry_path)
    except GateError as exc:
        print(f"check-source-adr-citations: ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
