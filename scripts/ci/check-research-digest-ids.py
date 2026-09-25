#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Ratchet research-digest identifiers and their H1 headings.

The checked-in baseline describes inherited debt, but the current branch does
not get to define its own allowance. Every audit compares that file with the
trusted merge-base. Ordinary baseline writes require a canonical baseline at
that trusted revision; the one-time bootstrap path is explicit and bounded to
an immutable pre-ratchet commit.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import shutil
import sys
import tarfile
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable, NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import (
    BinaryCommandResult,
    CommandOutputLimitExceeded,
    CommandTimedOut,
    CommandValidationError,
)
from scripts.lib.safe_subprocess import run as run_command

DIGEST_NAME = re.compile(r"^(?P<number>[0-9]{4})-.+\.md$")
RESEARCH_H1 = re.compile(r"^# Research-(?P<number>[0-9]{4})(?=$|[:\s\u2013\u2014-])")
FULL_COMMIT = re.compile(r"^[0-9a-fA-F]{40}$")
SCHEMA_VERSION = 1
MIN_COLLISION_MEMBERS = 2
DEFAULT_BASELINE = Path("scripts/ci/research-digest-id-baseline.json")
CHECKER_RELATIVE = Path("scripts/ci/check-research-digest-ids.py")
DEFAULT_TRUSTED_REF = "origin/master"
TEMPLATE_NAME = "0000-template.md"
MAX_GIT_OUTPUT_BYTES = 16 * 1_048_576
_GIT_PATH = shutil.which("git")
GIT = str(Path(_GIT_PATH).resolve(strict=True)) if _GIT_PATH is not None else None


class GateError(RuntimeError):
    """A deterministic research-digest contract failure."""


class DebtAuthority(NamedTuple):
    """Legacy debt loaded from an immutable trusted revision."""

    payload: dict[str, Any]
    revision: str
    source: str


def _first_h1(text: str) -> str:
    """Return the first level-one Markdown heading, or an empty string."""
    return next((line for line in text.splitlines() if line.startswith("# ")), "")


def _scan_entries(
    entries: Iterable[tuple[str, str]],
) -> tuple[dict[str, list[str]], dict[str, str]]:
    """Return collisions and H1 exceptions from relative-path/text pairs."""
    by_number: dict[str, list[str]] = defaultdict(list)
    heading_exceptions: dict[str, str] = {}
    for relative, text in sorted(entries):
        path = Path(relative)
        match = DIGEST_NAME.fullmatch(path.name)
        if path.parent.as_posix() != "docs/research" or match is None:
            continue
        if path.name == TEMPLATE_NAME:
            continue

        number = match.group("number")
        by_number[number].append(relative)
        h1 = _first_h1(text)
        h1_match = RESEARCH_H1.match(h1)
        if h1_match is None or h1_match.group("number") != number:
            heading_exceptions[relative] = h1

    collisions = {
        number: sorted(paths) for number, paths in sorted(by_number.items()) if len(paths) > 1
    }
    return collisions, dict(sorted(heading_exceptions.items()))


def scan_repository(root: Path) -> tuple[dict[str, list[str]], dict[str, str]]:
    """Return current collision sets and non-canonical H1 headings."""
    research_dir = root / "docs/research"
    if not research_dir.is_dir():
        raise GateError(f"research directory does not exist: {research_dir}")

    entries: list[tuple[str, str]] = []
    for path in sorted(research_dir.glob("*.md")):
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise GateError(f"cannot read {path}: {exc}") from exc
        entries.append((path.relative_to(root).as_posix(), text))
    return _scan_entries(entries)


def _payload_from_scan(
    scan: tuple[dict[str, list[str]], dict[str, str]],
) -> dict[str, Any]:
    collisions, heading_exceptions = scan
    return {
        "schema_version": SCHEMA_VERSION,
        "legacy_collisions": collisions,
        "legacy_heading_exceptions": heading_exceptions,
    }


def _baseline_payload(root: Path) -> dict[str, Any]:
    return _payload_from_scan(scan_repository(root))


def _canonical_bytes(payload: dict[str, Any]) -> bytes:
    return (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _validate_payload(raw: bytes, source: str) -> dict[str, Any]:
    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise GateError(f"cannot load baseline {source}: {exc}") from exc

    expected_keys = {"schema_version", "legacy_collisions", "legacy_heading_exceptions"}
    if not isinstance(payload, dict) or set(payload) != expected_keys:
        raise GateError(f"baseline {source} has an invalid top-level schema")
    if payload["schema_version"] != SCHEMA_VERSION:
        raise GateError(
            f"baseline {source} schema_version must be {SCHEMA_VERSION}, "
            f"got {payload['schema_version']!r}"
        )

    collisions = payload["legacy_collisions"]
    headings = payload["legacy_heading_exceptions"]
    if not isinstance(collisions, dict) or not isinstance(headings, dict):
        raise GateError(f"baseline {source} collision and heading maps must be objects")
    _validate_collisions(collisions, source)
    if not all(isinstance(key, str) and isinstance(value, str) for key, value in headings.items()):
        raise GateError(f"baseline {source} has an invalid heading exception entry")
    if raw != _canonical_bytes(payload):
        raise GateError(f"baseline {source} is not in deterministic generated form")
    return payload


def _validate_collisions(collisions: dict[str, Any], source: str) -> None:
    for number, paths in collisions.items():
        valid = (
            isinstance(number, str)
            and re.fullmatch(r"[0-9]{4}", number) is not None
            and isinstance(paths, list)
            and len(paths) >= MIN_COLLISION_MEMBERS
            and paths == sorted(set(paths))
            and all(isinstance(item, str) for item in paths)
        )
        if not valid:
            raise GateError(f"baseline {source} has an invalid collision entry for {number!r}")


def load_baseline(path: Path) -> dict[str, Any]:
    """Load and validate the generated baseline schema."""
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise GateError(f"cannot load baseline {path}: {exc}") from exc
    return _validate_payload(raw, str(path))


def _git_environment() -> dict[str, str]:
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    env.update({"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1", "LC_ALL": "C"})
    return env


def _git(root: Path, *args: str, check: bool = True) -> BinaryCommandResult:
    if GIT is None:
        raise GateError("required executable not found: git")
    try:
        result = run_command(
            [GIT, "-C", str(root), "-c", "color.ui=false", *args],
            allowed_executables=(GIT,),
            env=_git_environment(),
            capture_output=True,
            check=False,
            timeout_seconds=60,
            max_output_bytes=MAX_GIT_OUTPUT_BYTES,
        )
    except (CommandOutputLimitExceeded, CommandTimedOut, CommandValidationError) as exc:
        raise GateError(f"bounded Git invocation failed: {exc}") from exc
    if check and result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise GateError(f"git {' '.join(args)} failed: {detail or 'unknown error'}")
    return result


def _resolve_commit(root: Path, revision: str) -> str:
    result = _git(root, "rev-parse", "--verify", "--end-of-options", f"{revision}^{{commit}}")
    return result.stdout.decode("ascii").strip()


def _trusted_merge_base(root: Path, trusted_ref: str) -> str:
    trusted = _resolve_commit(root, trusted_ref)
    head = _resolve_commit(root, "HEAD")
    result = _git(root, "merge-base", head, trusted)
    revision = result.stdout.decode("ascii").strip()
    if FULL_COMMIT.fullmatch(revision) is None:
        raise GateError(f"trusted merge-base for {trusted_ref!r} is not a commit")
    return revision


def _git_path(root: Path, revision: str, path: Path) -> bytes | None:
    result = _git(root, "show", f"{revision}:{path.as_posix()}", check=False)
    if result.returncode == 0:
        return result.stdout
    missing = _git(root, "cat-file", "-e", f"{revision}:{path.as_posix()}", check=False)
    if missing.returncode != 0:
        return None
    raise GateError(f"cannot read {path} from trusted revision {revision}")


def _scan_git_tree(root: Path, revision: str) -> dict[str, Any]:
    archive = _git(root, "archive", "--format=tar", revision, "--", "docs/research").stdout
    entries: list[tuple[str, str]] = []
    try:
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as stream:
            for member in stream.getmembers():
                path = Path(member.name)
                if not member.isfile() or path.parent.as_posix() != "docs/research":
                    continue
                source = stream.extractfile(member)
                if source is None:
                    raise GateError(f"cannot read {member.name} from trusted tree {revision}")
                entries.append((member.name, source.read().decode("utf-8")))
    except (tarfile.TarError, UnicodeError) as exc:
        raise GateError(f"cannot scan trusted research tree {revision}: {exc}") from exc
    return _payload_from_scan(_scan_entries(entries))


def _baseline_relative(root: Path, baseline_path: Path) -> Path:
    try:
        return baseline_path.resolve().relative_to(root.resolve())
    except ValueError as exc:
        raise GateError("baseline must live inside the repository") from exc


def _authority_at_revision(root: Path, revision: str, baseline_path: Path) -> DebtAuthority:
    relative = _baseline_relative(root, baseline_path)
    baseline = _git_path(root, revision, relative)
    if baseline is not None:
        payload = _validate_payload(baseline, f"{revision}:{relative.as_posix()}")
        return DebtAuthority(payload=payload, revision=revision, source="baseline")

    if _git_path(root, revision, CHECKER_RELATIVE) is not None:
        raise GateError(
            f"canonical trusted baseline is missing at {revision}:{relative.as_posix()}"
        )
    return DebtAuthority(payload=_scan_git_tree(root, revision), revision=revision, source="tree")


def load_trusted_authority(root: Path, trusted_ref: str, baseline_path: Path) -> DebtAuthority:
    """Load debt authority from the trusted merge-base."""
    return _authority_at_revision(root, _trusted_merge_base(root, trusted_ref), baseline_path)


def load_bootstrap_authority(root: Path, revision: str, baseline_path: Path) -> DebtAuthority:
    """Load the immutable pre-ratchet tree allowed for one-time bootstrap."""
    if FULL_COMMIT.fullmatch(revision) is None:
        raise GateError("--bootstrap-from-ref requires a full 40-character commit")
    resolved = _resolve_commit(root, revision)
    if resolved.lower() != revision.lower():
        raise GateError("bootstrap authority did not resolve to the requested commit")
    head = _resolve_commit(root, "HEAD")
    ancestry = _git(root, "merge-base", "--is-ancestor", resolved, head, check=False)
    if ancestry.returncode == 1:
        raise GateError("bootstrap authority must be an ancestor of HEAD")
    if ancestry.returncode != 0:
        detail = ancestry.stderr.decode("utf-8", errors="replace").strip()
        raise GateError(f"cannot verify bootstrap authority ancestry: {detail or 'unknown error'}")
    authority = _authority_at_revision(root, resolved, baseline_path)
    if authority.source != "tree":
        raise GateError("bootstrap is allowed only for a revision predating the ratchet")
    return authority


def _new_debt_errors(trusted: dict[str, Any], current: dict[str, Any]) -> list[str]:
    """Return debt present in current but absent from trusted authority."""
    errors: list[str] = []
    trusted_collisions = trusted["legacy_collisions"]
    for number, paths in current["legacy_collisions"].items():
        added = sorted(set(paths) - set(trusted_collisions.get(number, [])))
        if added:
            errors.append(f"Research-{number} adds collision members {added!r}")

    trusted_headings = trusted["legacy_heading_exceptions"]
    for relative, heading in current["legacy_heading_exceptions"].items():
        if trusted_headings.get(relative) != heading:
            errors.append(f"{relative} adds or changes a non-canonical H1")
    return errors


def _tree_drift_errors(baseline: dict[str, Any], current: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    expected_collisions = baseline["legacy_collisions"]
    actual_collisions = current["legacy_collisions"]
    for number in sorted(set(expected_collisions) | set(actual_collisions)):
        expected = expected_collisions.get(number, [])
        actual = actual_collisions.get(number, [])
        if actual != expected:
            errors.append(
                f"Research-{number} collision set drift: expected {expected!r}, found {actual!r}"
            )

    expected_headings = baseline["legacy_heading_exceptions"]
    actual_headings = current["legacy_heading_exceptions"]
    for relative in sorted(set(expected_headings) | set(actual_headings)):
        expected = expected_headings.get(relative)
        actual = actual_headings.get(relative)
        if actual != expected:
            errors.append(
                f"{relative} H1 drift: expected legacy exception {expected!r}, "
                f"found {actual!r}; canonical form is '# Research-NNNN: ...'"
            )
    return errors


def _authority_errors(authority: DebtAuthority, current: dict[str, Any]) -> list[str]:
    label = "trusted baseline" if authority.source == "baseline" else "trusted pre-ratchet tree"
    return [
        f"{label} {authority.revision} rejects {error}"
        for error in _new_debt_errors(authority.payload, current)
    ]


def audit_repository(
    root: Path,
    baseline_path: Path,
    authority: DebtAuthority | None = None,
) -> list[str]:
    """Compare current tree and baseline with immutable trusted debt."""
    baseline = load_baseline(baseline_path)
    current = _baseline_payload(root)
    errors = _tree_drift_errors(baseline, current)
    if authority is not None:
        errors.extend(_authority_errors(authority, baseline))
    return errors


def _write_payload(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(_canonical_bytes(payload))


def _write_payload_exclusive(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("xb") as stream:
            stream.write(_canonical_bytes(payload))
    except FileExistsError as exc:
        raise GateError("bootstrap refuses to overwrite an existing baseline") from exc
    except OSError as exc:
        raise GateError(f"cannot create baseline {path}: {exc}") from exc


def write_baseline(root: Path, baseline_path: Path, authority: DebtAuthority) -> None:
    """Write reduced debt only, anchored to a canonical trusted baseline."""
    if authority.source != "baseline":
        raise GateError(
            f"canonical trusted baseline is missing at {authority.revision}; "
            "ordinary --write cannot bootstrap it"
        )
    payload = _baseline_payload(root)
    growth = _authority_errors(authority, payload)
    if growth:
        raise GateError(f"refusing to absorb new research-digest debt: {'; '.join(growth)}")
    _write_payload(baseline_path, payload)


def bootstrap_baseline(root: Path, baseline_path: Path, authority: DebtAuthority) -> None:
    """Create the first baseline from one immutable pre-ratchet tree."""
    payload = _baseline_payload(root)
    growth = _new_debt_errors(authority.payload, payload)
    if growth:
        raise GateError(f"refusing initial baseline debt growth: {'; '.join(growth)}")
    _write_payload_exclusive(baseline_path, payload)


def _resolve_baseline(root: Path, value: Path | None) -> Path:
    if value is None:
        return root / DEFAULT_BASELINE
    return value if value.is_absolute() else root / value


def _print_success(prefix: str, root: Path) -> None:
    collisions, headings = scan_repository(root)
    print(f"{prefix}: {len(collisions)} collision sets, {len(headings)} H1 exceptions")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root (default: inferred from this script)",
    )
    parser.add_argument(
        "--baseline", type=Path, help=f"baseline path (default: {DEFAULT_BASELINE})"
    )
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument(
        "--write", action="store_true", help="write reduced debt from trusted baseline"
    )
    modes.add_argument(
        "--bootstrap-from-ref",
        metavar="COMMIT",
        help="one-time baseline creation from an immutable pre-ratchet commit",
    )
    parser.add_argument(
        "--trusted-ref",
        default=os.environ.get("VMAFX_RESEARCH_BASE_REF", DEFAULT_TRUSTED_REF),
        help=f"trusted base ref (default: $VMAFX_RESEARCH_BASE_REF or {DEFAULT_TRUSTED_REF})",
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()
    baseline_path = _resolve_baseline(root, args.baseline)

    try:
        if args.bootstrap_from_ref:
            authority = load_bootstrap_authority(root, args.bootstrap_from_ref, baseline_path)
            bootstrap_baseline(root, baseline_path, authority)
            _print_success("research-digest ID baseline bootstrapped", root)
            return 0

        authority = load_trusted_authority(root, args.trusted_ref, baseline_path)
        if args.write:
            write_baseline(root, baseline_path, authority)
            _print_success("research-digest ID baseline written", root)
            return 0
        errors = audit_repository(root, baseline_path, authority)
    except GateError as exc:
        print(f"research-digest ID gate: {exc}", file=sys.stderr)
        return 1

    if errors:
        for error in errors:
            print(f"research-digest ID gate: {error}", file=sys.stderr)
        print(
            "research-digest ID gate failed; fix the tree and regenerate only "
            "against the canonical trusted baseline",
            file=sys.stderr,
        )
        return 1

    print(f"research-digest ID gate: OK (trusted {authority.revision})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
