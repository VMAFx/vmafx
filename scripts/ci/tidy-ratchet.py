#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Whole-tree clang-tidy debt ratchet (ADR-1142).

Measures every translation unit in a ``compile_commands.json``, deduplicates
the clang-tidy diagnostics by ``(path, line, column, check)`` exactly like the
2026-08-31 / 2026-09-02 baselines, counts ``NOLINT`` markers that carry no
inline ``ADR-NNNN`` citation, and compares the per-file numbers against a
committed baseline.  The comparison is a *ratchet*: a file may never grow
above its baseline (regression), and when a file shrinks the baseline must be
tightened in the same change (``--write``), so the committed numbers are the
measured numbers at every commit.

Exit codes: 0 baseline matches, 2 regression, 3 baseline is stale-high (ratchet
must be tightened), 4 a translation unit failed to compile under clang-tidy
(the measurement is unusable — fail closed), 5 usage / IO error.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

BASELINE_SCHEMA = 1
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".cu", ".hip", ".mm", ".m"}
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".cuh", ".inl"}
DIAG_RE = re.compile(
    r"^(?P<path>[^:\n]+):(?P<line>\d+):(?P<col>\d+): "
    r"(?P<level>warning|error): .*?\[(?P<check>[A-Za-z0-9_.,\-]+)\]\s*$"
)
COMPILE_ERROR_CHECK = "clang-diagnostic-error"
NOLINT_RE = re.compile(r"NOLINT(?:NEXTLINE|BEGIN)?(?:\([^)]*\))?(?!END)")
ADR_CITE_RE = re.compile(r"ADR-\d{4}")
GITHUB_ACTIONS = os.environ.get("GITHUB_ACTIONS") == "true"


@dataclass
class Measurement:
    """Per-file counts for one lane."""

    lane: str
    tus: int = 0
    warnings: dict[str, int] = field(default_factory=dict)
    nolint_uncited: dict[str, int] = field(default_factory=dict)
    compile_failures: list[str] = field(default_factory=list)
    clang_tidy_version: str = ""

    @property
    def total_warnings(self) -> int:
        return sum(self.warnings.values())

    @property
    def total_nolint_uncited(self) -> int:
        return sum(self.nolint_uncited.values())

    def to_json(self) -> dict:
        return {
            "schema": BASELINE_SCHEMA,
            "lane": self.lane,
            "generator": "scripts/ci/tidy-ratchet.py",
            "clang_tidy_version": self.clang_tidy_version,
            "tus": self.tus,
            "total_warnings": self.total_warnings,
            "total_nolint_uncited": self.total_nolint_uncited,
            "warnings": dict(sorted(self.warnings.items())),
            "nolint_uncited": dict(sorted(self.nolint_uncited.items())),
        }

    @classmethod
    def from_json(cls, data: dict) -> Measurement:
        if data.get("schema") != BASELINE_SCHEMA:
            raise ValueError(f"unsupported baseline schema {data.get('schema')!r}")
        return cls(
            lane=str(data.get("lane", "")),
            tus=int(data.get("tus", 0)),
            warnings={str(k): int(v) for k, v in data.get("warnings", {}).items()},
            nolint_uncited={str(k): int(v) for k, v in data.get("nolint_uncited", {}).items()},
            clang_tidy_version=str(data.get("clang_tidy_version", "")),
        )


def relpath(path: str, repo_root: Path, cwd: Path) -> str | None:
    """Return *path* relative to *repo_root*, or None when it lies outside."""
    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = cwd / candidate
    try:
        resolved = candidate.resolve()
    except OSError:
        return None
    try:
        return resolved.relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return None


def parse_diagnostics(
    output: str, repo_root: Path, cwd: Path
) -> tuple[set[tuple[str, int, int, str]], bool]:
    """Parse clang-tidy output into a deduplicated diagnostic set.

    Returns ``(diagnostics, compile_failed)``.  Diagnostics outside the
    repository (system headers) are dropped; a ``clang-diagnostic-error``
    marks the translation unit as unusable.
    """
    diags: set[tuple[str, int, int, str]] = set()
    compile_failed = False
    for raw in output.splitlines():
        match = DIAG_RE.match(raw.rstrip())
        if match is None:
            continue
        if match["check"] == COMPILE_ERROR_CHECK:
            compile_failed = True
            continue
        rel = relpath(match["path"], repo_root, cwd)
        if rel is None:
            continue
        diags.add((rel, int(match["line"]), int(match["col"]), match["check"]))
    return diags, compile_failed


def count_uncited_nolints(text: str) -> int:
    """Count NOLINT markers whose line or previous line has no ADR citation.

    ``NOLINTEND`` is a closing bracket, never a suppression of its own.
    """
    lines = text.splitlines()
    uncited = 0
    for index, line in enumerate(lines):
        markers = len(NOLINT_RE.findall(line))
        if markers == 0:
            continue
        previous = lines[index - 1] if index > 0 else ""
        if ADR_CITE_RE.search(line) or ADR_CITE_RE.search(previous):
            continue
        uncited += markers
    return uncited


def load_compile_commands(build_dir: Path, repo_root: Path) -> list[tuple[Path, Path]]:
    """Return ``(source, directory)`` pairs for in-repo translation units."""
    compdb = build_dir / "compile_commands.json"
    try:
        entries = json.loads(compdb.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise SystemExit(f"tidy-ratchet: cannot read {compdb}: {exc}") from exc
    seen: set[str] = set()
    units: list[tuple[Path, Path]] = []
    for entry in entries:
        directory = Path(entry.get("directory", build_dir))
        rel = relpath(entry.get("file", ""), repo_root, directory)
        if rel is None or rel in seen or rel.startswith("subprojects/"):
            continue
        if Path(rel).suffix not in SOURCE_SUFFIXES:
            continue
        seen.add(rel)
        units.append((repo_root / rel, directory))
    return sorted(units)


def clang_tidy_version(binary: str) -> str:
    """Return the LLVM version string of *binary*, or "" when unavailable."""
    try:
        out = subprocess.run(  # noqa: S603 -- fixed argv, no shell
            [binary, "--version"], capture_output=True, text=True, check=False
        ).stdout
    except OSError:
        return ""
    match = re.search(r"LLVM version (\S+)", out)
    return match.group(1) if match else ""


def run_one(
    binary: str, build_dir: Path, extra_args: list[str], unit: tuple[Path, Path]
) -> tuple[str, str]:
    source, directory = unit
    argv = [binary, "-p", str(build_dir), *extra_args, str(source)]
    proc = subprocess.run(  # noqa: S603 -- argv built from compile_commands, no shell
        argv, capture_output=True, text=True, check=False, cwd=str(directory)
    )
    return str(source), proc.stdout + "\n" + proc.stderr


def measure(
    lane: str,
    build_dir: Path,
    repo_root: Path,
    binary: str,
    extra_args: list[str],
    jobs: int,
    only: Iterable[str] = (),
) -> Measurement:
    """Run clang-tidy over every TU of *build_dir* and count NOLINTs."""
    units = load_compile_commands(build_dir, repo_root)
    wanted = {Path(p).resolve() for p in only}
    if wanted:
        units = [u for u in units if u[0].resolve() in wanted]
    result = Measurement(lane=lane, tus=len(units))
    result.clang_tidy_version = clang_tidy_version(binary)
    diags: set[tuple[str, int, int, str]] = set()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        futures = {
            pool.submit(run_one, binary, build_dir, extra_args, unit): unit for unit in units
        }
        for future in concurrent.futures.as_completed(futures):
            source, directory = futures[future]
            unit_diags, failed = parse_diagnostics(future.result()[1], repo_root, directory)
            if failed:
                rel = relpath(str(source), repo_root, directory) or str(source)
                result.compile_failures.append(rel)
            diags |= unit_diags
    for path, _line, _col, _check in diags:
        result.warnings[path] = result.warnings.get(path, 0) + 1
    result.nolint_uncited = scan_nolints(repo_root, units, lane)
    return result


def scan_nolints(repo_root: Path, units: list[tuple[Path, Path]], lane: str) -> dict[str, int]:
    """Count uncited NOLINTs in every measured TU and the headers of its lane.

    The ``cpu`` lane owns every header under ``core/``; a GPU lane only owns
    the headers that live next to its translation units, so a header is
    never counted twice across lanes.
    """
    counts: dict[str, int] = {}
    paths = {u[0] for u in units}
    if lane == "cpu":
        roots = [repo_root / d for d in ("core/include", "core/src", "core/tools", "core/test")]
    else:
        roots = sorted({u[0].parent for u in units})
    for root in roots:
        if root.is_dir():
            paths.update(p for p in root.rglob("*") if p.suffix in HEADER_SUFFIXES)
    for path in sorted(paths):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = relpath(str(path), repo_root, repo_root)
        if rel is None:
            continue
        uncited = count_uncited_nolints(text)
        if uncited:
            counts[rel] = uncited
    return counts


@dataclass
class Delta:
    metric: str
    path: str
    baseline: int
    measured: int

    @property
    def change(self) -> int:
        return self.measured - self.baseline


def compare(baseline: Measurement, measured: Measurement) -> tuple[list[Delta], list[Delta]]:
    """Return ``(regressions, slack)`` per file and metric."""
    regressions: list[Delta] = []
    slack: list[Delta] = []
    for metric in ("warnings", "nolint_uncited"):
        before: dict[str, int] = getattr(baseline, metric)
        after: dict[str, int] = getattr(measured, metric)
        for path in sorted(set(before) | set(after)):
            delta = Delta(metric, path, before.get(path, 0), after.get(path, 0))
            if delta.change > 0:
                regressions.append(delta)
            elif delta.change < 0:
                slack.append(delta)
    return regressions, slack


def annotate(level: str, message: str) -> None:
    prefix = f"::{level}::" if GITHUB_ACTIONS else f"{level}: "
    print(f"{prefix}{message}")


def report(baseline: Measurement, measured: Measurement, allow_slack: bool) -> int:
    """Print the comparison and return the process exit code."""
    regressions, slack = compare(baseline, measured)
    print(
        f"tidy-ratchet[{measured.lane}]: {measured.tus} TUs, "
        f"{measured.total_warnings} warnings (baseline {baseline.total_warnings}), "
        f"{measured.total_nolint_uncited} uncited NOLINTs "
        f"(baseline {baseline.total_nolint_uncited})"
    )
    if baseline.clang_tidy_version and measured.clang_tidy_version != baseline.clang_tidy_version:
        annotate(
            "warning",
            f"clang-tidy {measured.clang_tidy_version} differs from baseline "
            f"{baseline.clang_tidy_version}; counts may not be comparable",
        )
    for delta in regressions:
        annotate(
            "error",
            f"{delta.path}: {delta.metric} {delta.baseline} -> {delta.measured} "
            f"(+{delta.change}) — fix the code, never raise the baseline",
        )
    for delta in slack:
        annotate(
            "notice" if allow_slack else "error",
            f"{delta.path}: {delta.metric} {delta.baseline} -> {delta.measured} "
            f"({delta.change}) — tighten the baseline: tidy-ratchet.py --write",
        )
    if regressions:
        return 2
    if slack and not allow_slack:
        return 3
    print("tidy-ratchet: baseline matches measurement")
    return 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lane", default="cpu", help="baseline lane name (cpu, cuda, sycl, hip)")
    parser.add_argument("--build-dir", default="build", type=Path)
    parser.add_argument("--repo-root", default=".", type=Path)
    parser.add_argument(
        "--baseline", type=Path, help="baseline JSON (default scripts/ci/tidy-baseline-<lane>.json)"
    )
    parser.add_argument("--clang-tidy", default=os.environ.get("CLANG_TIDY_BIN", "clang-tidy"))
    parser.add_argument(
        "--extra-arg", action="append", default=[], help="passed through to clang-tidy"
    )
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument(
        "--only", action="append", default=[], help="measure only these TUs (debugging)"
    )
    parser.add_argument("--report", type=Path, help="write the measurement JSON here")
    parser.add_argument(
        "--write", action="store_true", help="overwrite the baseline with the measurement"
    )
    parser.add_argument(
        "--allow-slack", action="store_true", help="do not fail when files improved"
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    baseline_path = (
        args.baseline or repo_root / "scripts" / "ci" / f"tidy-baseline-{args.lane}.json"
    )
    binary = shutil.which(args.clang_tidy) or args.clang_tidy
    measured = measure(
        args.lane, args.build_dir.resolve(), repo_root, binary, args.extra_arg, args.jobs, args.only
    )
    if args.report:
        args.report.write_text(json.dumps(measured.to_json(), indent=2) + "\n", encoding="utf-8")
    if measured.compile_failures:
        for path in sorted(measured.compile_failures):
            annotate("error", f"{path}: clang-tidy could not compile this TU; measurement unusable")
        return 4
    if args.only:
        annotate("notice", "--only given: comparison against the baseline skipped")
        return 0
    if args.write:
        baseline_path.write_text(json.dumps(measured.to_json(), indent=2) + "\n", encoding="utf-8")
        print(f"tidy-ratchet: wrote {baseline_path} ({measured.total_warnings} warnings)")
        return 0
    try:
        baseline = Measurement.from_json(json.loads(baseline_path.read_text(encoding="utf-8")))
    except (OSError, ValueError) as exc:
        annotate("error", f"cannot load baseline {baseline_path}: {exc}")
        return 5
    return report(baseline, measured, args.allow_slack)


if __name__ == "__main__":
    sys.exit(main())
