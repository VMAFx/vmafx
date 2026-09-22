#!/usr/bin/env python3
"""Reclaim rebuildable bulk from the local working directories, keep the evidence.

The gitignored state trees (`.workingdir/` and friends) accumulate one directory
per gate run. Each holds a small amount of evidence — receipts, help dumps,
logs, CSVs, JSON — inside a large amount of rebuildable output: Go build caches,
meson build trees, downloaded tool binaries, a rendered mkdocs `site/`.

Committed documents cite those run directories by path, so the directories
themselves are part of the audit trail and must survive. Their build output is
not: it is regenerable and duplicated across runs.

This prunes the rebuildable part, leaves the evidence, and records what it took
in a `GC-MANIFEST.md` inside each pruned directory, so the trail says "the
objects were reclaimed on this date" instead of going quiet.

Paths cited by any local or remote committed ref are never removed, whatever
their class. Pass `--no-citations` to skip that scan (faster, less safe).

Dry run by default. `--apply` performs the deletions.

Copyright 2026 Lusoris
SPDX-License-Identifier: EUPL-1.2
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from collections.abc import Iterable, Iterator
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
"""Repository the state tree belongs to. `--repo-root` overrides it, which is how
the script runs from a worktree against the main checkout's gitignored tree."""

# Directory names whose entire contents are rebuildable.
REBUILDABLE_DIRS = frozenset(
    {
        "go-cache",
        "gomodcache",
        "build",
        "bin",
        "site",
        "private-data",
        "node_modules",
        "htmlcov",
        "__pycache__",
        ".pytest_cache",
        ".ruff_cache",
        ".mypy_cache",
        "subprojects",
        ".venv",
    }
)
REBUILDABLE_DIR_PREFIXES = ("build-", "build_")

# Extensions that are compiler or linker output wherever they appear.
REBUILDABLE_SUFFIXES = frozenset(
    {".o", ".obj", ".a", ".lib", ".pyc", ".pyo", ".gch", ".pch", ".ninja_deps", ".ninja_log"}
)
SHARED_OBJECT = re.compile(r"\.(so|dylib|dll)(\.[0-9]+)*$")

# Roots that are never pruned: rescued git objects and curated evidence.
PROTECTED_NAMES = frozenset({"rescue", "evidence", "archive", "netflix"})

MANIFEST = "GC-MANIFEST.md"
GIT = shutil.which("git") or "/usr/bin/git"
KIB = 1024
RUN_DEPTH = 2
MANIFEST_LIST_CAP = 200


def cited_paths(state_root: Path, repo_root: Path) -> set[str]:
    """Collect every `<state-root>/...` path any committed ref mentions."""
    refs = subprocess.run(  # noqa: S603 -- fixed git read query, absolute binary, no shell
        [
            GIT,
            "-C",
            str(repo_root),
            "for-each-ref",
            "--format=%(refname)",
            "refs/heads",
            "refs/remotes",
        ],
        capture_output=True,
        text=True,
        check=True,
        timeout=120,
    ).stdout.split()
    if not refs:
        return set()
    pattern = re.escape(state_root.name) + r"/[A-Za-z0-9._/-]*"
    prefix = state_root.name + "/"
    found: set[str] = set()
    # git grep over every ref at once is one process but a large output; chunk it.
    for start in range(0, len(refs), 40):
        chunk = refs[start : start + 40]
        result = subprocess.run(  # noqa: S603 -- fixed git read query, refs from git itself
            [GIT, "-C", str(repo_root), "grep", "--no-color", "-h", "-o", "-E", pattern, *chunk],
            capture_output=True,
            text=True,
            timeout=900,
        )
        for line in result.stdout.splitlines():
            text = line.strip()
            if not text:
                continue
            # Store relative to the state root, which is what is_protected compares.
            found.add(text[len(prefix) :] if text.startswith(prefix) else text)
    found.discard("")
    return found


def is_curated(relative: Path) -> bool:
    """True for the roots that hold rescued git objects and curated evidence."""
    return bool(relative.parts) and relative.parts[0] in PROTECTED_NAMES


def cited_at_or_below(relative: Path, citations: set[str]) -> bool:
    """True when a citation names this path or something inside it.

    Removing such a path would break a link in a committed document, so it stays.
    A citation does **not** shield the rebuildable content further down: a run
    directory is cited so the directory survives, while its object tree, Go cache
    and meson build tree are still reclaimed from underneath it.
    """
    text = relative.as_posix()
    return any(cite == text or cite.startswith(text + "/") for cite in citations)


def rebuildable_dir(name: str) -> bool:
    return name in REBUILDABLE_DIRS or name.startswith(REBUILDABLE_DIR_PREFIXES)


def walk(state_root: Path, citations: set[str]) -> Iterator[tuple[Path, int, str]]:
    """Yield (path, bytes, reason) for every rebuildable directory and file."""
    stack = [state_root]
    while stack:
        current = stack.pop()
        try:
            entries = sorted(current.iterdir())
        except OSError:
            continue
        for entry in entries:
            relative = entry.relative_to(state_root)
            if is_curated(relative) or entry.is_symlink():
                continue
            if entry.is_dir():
                if rebuildable_dir(entry.name) and not cited_at_or_below(relative, citations):
                    yield entry, directory_size(entry), f"rebuildable directory {entry.name}/"
                else:
                    # Either ordinary, or cited and therefore kept as a path while
                    # its own rebuildable children are still judged individually.
                    stack.append(entry)
                continue
            if cited_at_or_below(relative, citations):
                continue
            suffix = entry.suffix
            if suffix in REBUILDABLE_SUFFIXES or SHARED_OBJECT.search(entry.name):
                try:
                    yield entry, entry.stat().st_size, f"build output {suffix or entry.name}"
                except OSError:
                    continue


def directory_size(path: Path) -> int:
    total = 0
    for item in path.rglob("*"):
        try:
            if item.is_file() and not item.is_symlink():
                total += item.stat().st_size
        except OSError:
            continue
    return total


def human(size: int) -> str:
    value = float(size)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < KIB or unit == "TiB":
            return f"{value:.1f} {unit}"
        value /= KIB
    return f"{value:.1f} TiB"


def run_directory_of(relative: Path) -> Path | None:
    """The `<state-root>/cache/<run>` style directory a victim belongs to."""
    parts = relative.parts
    if len(parts) >= RUN_DEPTH:
        return Path(parts[0]) / parts[1]
    return Path(parts[0]) if parts else None


def write_manifest(state_root: Path, grouped: dict[Path, list[tuple[str, int]]]) -> list[Path]:
    """Record the reclaim inside each affected run directory."""
    written = []
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    for run, victims in sorted(grouped.items()):
        target = state_root / run
        if not target.is_dir():
            continue
        total = sum(size for _, size in victims)
        lines = [
            f"# Reclaimed build output — {stamp}",
            "",
            "`scripts/dev/gc_workingdir.py` removed the rebuildable part of this run",
            "directory and kept its evidence. The paths below were regenerable output:",
            "Go build caches, meson build trees, compiled objects, downloaded tool",
            "binaries, a rendered docs site. Re-running the gate recreates them.",
            "",
            f"Reclaimed: **{human(total)}** across {len(victims)} paths.",
            "",
        ]
        lines += [
            f"- `{name}` — {human(size)}" for name, size in sorted(victims)[:MANIFEST_LIST_CAP]
        ]
        if len(victims) > MANIFEST_LIST_CAP:
            lines.append(f"- ... and {len(victims) - MANIFEST_LIST_CAP} more")
        lines.append("")
        path = target / MANIFEST
        existing = path.read_text(encoding="utf-8") if path.exists() else ""
        path.write_text("\n".join(lines) + ("\n" + existing if existing else ""), encoding="utf-8")
        written.append(path)
    return written


def remove(path: Path) -> None:
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    else:
        path.unlink()


def collect(state_root: Path, citations: set[str]) -> list[tuple[Path, int, str]]:
    return sorted(walk(state_root, citations), key=lambda item: -item[1])


def _parse_args(argv: Iterable[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "state_root",
        nargs="?",
        default=".workingdir",
        help="gitignored state tree to prune (default: .workingdir)",
    )
    parser.add_argument("--apply", action="store_true", help="perform the deletions")
    parser.add_argument(
        "--no-citations",
        action="store_true",
        help="skip the committed-citation scan (faster, less safe)",
    )
    parser.add_argument("--top", type=int, default=25, help="how many paths to list")
    parser.add_argument(
        "--repo-root",
        default=None,
        help="repository the state tree belongs to (default: this script's repository)",
    )
    return parser.parse_args(list(argv) if argv is not None else None)


def _remove_victims(victims: list[tuple[Path, int, str]]) -> tuple[int, int]:
    removed = 0
    failed = 0
    for path, size, _ in victims:
        try:
            remove(path)
            removed += size
        except OSError as error:
            failed += 1
            print(f"  could not remove {path}: {error}", file=sys.stderr)
    return removed, failed


def main(argv: Iterable[str] | None = None) -> int:
    args = _parse_args(argv)

    repo_root = Path(args.repo_root).resolve() if args.repo_root else ROOT
    state_root = (repo_root / args.state_root).resolve()
    if not state_root.is_dir():
        print(f"no such state tree: {state_root}", file=sys.stderr)
        return 66
    if repo_root not in state_root.parents:
        print("refusing to operate outside the repository", file=sys.stderr)
        return 65

    citations = set() if args.no_citations else cited_paths(state_root, repo_root)
    victims = collect(state_root, citations)
    total = sum(size for _, size, _ in victims)

    print(f"state tree: {state_root.relative_to(repo_root)}")
    print(f"citations honoured: {len(citations)}")
    print(f"reclaimable: {human(total)} across {len(victims)} paths")
    for path, size, reason in victims[: args.top]:
        print(f"  {human(size):>10}  {reason:<34} {path.relative_to(state_root)}")
    if len(victims) > args.top:
        print(f"  ... {len(victims) - args.top} more")

    if not args.apply:
        print("\ndry run — pass --apply to reclaim")
        return 0

    grouped: dict[Path, list[tuple[str, int]]] = {}
    for path, size, _ in victims:
        run = run_directory_of(path.relative_to(state_root))
        if run is not None:
            grouped.setdefault(run, []).append(
                (path.relative_to(state_root / run).as_posix(), size)
            )
    write_manifest(state_root, grouped)

    removed, failed = _remove_victims(victims)
    print(f"\nreclaimed {human(removed)}; {failed} path(s) could not be removed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
