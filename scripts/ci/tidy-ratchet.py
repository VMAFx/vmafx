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
import copy
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Iterable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

if os.name == "posix":
    import fcntl

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
    sources: list[str] = field(default_factory=list)

    @property
    def total_warnings(self) -> int:
        return sum(self.warnings.values())

    @property
    def total_nolint_uncited(self) -> int:
        return sum(self.nolint_uncited.values())

    def to_json(self) -> dict[str, Any]:
        return {
            "schema": BASELINE_SCHEMA,
            "lane": self.lane,
            "generator": "scripts/ci/tidy-ratchet.py",
            "clang_tidy_version": self.clang_tidy_version,
            "tus": self.tus,
            "measured_sources": self.sources,
            "compile_failures": sorted(self.compile_failures),
            "total_warnings": self.total_warnings,
            "total_nolint_uncited": self.total_nolint_uncited,
            "warnings": dict(sorted(self.warnings.items())),
            "nolint_uncited": dict(sorted(self.nolint_uncited.items())),
        }

    @classmethod
    def from_json(cls, data: dict[str, Any]) -> Measurement:
        if data.get("schema") != BASELINE_SCHEMA:
            raise ValueError(f"unsupported baseline schema {data.get('schema')!r}")
        return cls(
            lane=str(data.get("lane", "")),
            tus=int(data.get("tus", 0)),
            sources=list(data.get("measured_sources", [])),
            compile_failures=list(data.get("compile_failures", [])),
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
            # Reject diagnostic-looking lines the parser cannot account for.
            # Source excerpts ("42 | ...") and summary counts are not diagnostics.
            if re.match(
                r"^(?:.+:\d+(?::\d+)?:\s*|[\w-]+:\s*)?(?:fatal error|error|warning):",
                raw,
            ):
                compile_failed = True
            continue
        if match["check"] == COMPILE_ERROR_CHECK:
            compile_failed = True
            continue
        rel = relpath(match["path"], repo_root, cwd)
        if rel is None:
            continue
        diags.add((rel, int(match["line"]), int(match["col"]), match["check"]))
    return diags, compile_failed


def _cited_in_block_comment(lines: list[str], index: int, in_block: bool) -> bool:
    """Return True when the block comment holding line *index* cites an ADR.

    *in_block* says whether line *index* is already inside a ``/* ... */``
    comment opened on an earlier line. The forward scan stops at the closing
    ``*/`` so a citation belonging to the next comment never counts.
    """
    line = lines[index]
    opened = line.rfind("/*")
    if not in_block:
        if opened < 0 or "*/" in line[opened:]:
            return False
    elif "*/" in line:
        return False
    for follow in lines[index + 1 :]:
        if ADR_CITE_RE.search(follow):
            return True
        if "*/" in follow:
            break
    return False


def count_uncited_nolints(text: str) -> int:
    """Count NOLINT markers that carry no inline ``ADR-NNNN`` citation.

    A marker is cited when ``ADR-NNNN`` appears on the previous, the same or
    the next line, or anywhere in the ``/* ... */`` block comment that holds
    the marker (the ADR-1138 ``NOLINTBEGIN`` brackets explain themselves in a
    multi-line comment and cite the ADR on its last line). ``NOLINTEND`` is a
    closing bracket, never a suppression of its own.
    """
    lines = text.splitlines()
    uncited = 0
    in_block = False
    for index, line in enumerate(lines):
        markers = len(NOLINT_RE.findall(line))
        if markers:
            window = (
                lines[index - 1] if index > 0 else "",
                line,
                lines[index + 1] if index + 1 < len(lines) else "",
            )
            cited = any(ADR_CITE_RE.search(item) for item in window)
            if not cited and not _cited_in_block_comment(lines, index, in_block):
                uncited += markers
        # Track block-comment state for the next line. Markers live in
        # comments, so string literals holding comment tokens are not modelled.
        rest = line
        if in_block:
            if "*/" not in rest:
                continue
            in_block = False
            rest = rest[rest.index("*/") + 2 :]
        opened = rest.rfind("/*")
        if opened >= 0 and "*/" not in rest[opened:]:
            in_block = True
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
) -> tuple[str, str, int]:
    source, directory = unit
    argv = [binary, "-p", str(build_dir), *extra_args, str(source)]
    proc = subprocess.run(  # noqa: S603 -- argv built from compile_commands, no shell
        argv, capture_output=True, text=True, check=False, cwd=str(directory)
    )
    return str(source), proc.stdout + "\n" + proc.stderr, proc.returncode


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
        missing = wanted - {unit[0].resolve() for unit in units}
        if missing:
            raise ValueError(
                f"requested TUs missing from compile database: {sorted(map(str, missing))}"
            )
        units = [u for u in units if u[0].resolve() in wanted]
    if not units:
        raise ValueError("compile database selected no translation units")
    result = Measurement(lane=lane, tus=len(units))
    result.sources = sorted(source.relative_to(repo_root).as_posix() for source, _ in units)
    result.clang_tidy_version = clang_tidy_version(binary)
    diags: set[tuple[str, int, int, str]] = set()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        futures = {
            pool.submit(run_one, binary, build_dir, extra_args, unit): unit for unit in units
        }
        for future in concurrent.futures.as_completed(futures):
            source, directory = futures[future]
            _source, output, returncode = future.result()
            unit_diags, failed = parse_diagnostics(output, repo_root, directory)
            # The ratchet has always counted promoted checks as debt. A normal
            # warnings-as-errors exit is distinct from a tool/compile failure.
            promoted_only = (
                returncode == 1
                and re.search(r"^\d+ warnings? treated as errors?$", output, re.MULTILINE)
                and any("-warnings-as-errors" in diag[3].split(",") for diag in unit_diags)
            )
            if failed or (returncode and not promoted_only):
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
        except OSError as exc:
            raise ValueError(f"cannot measure NOLINTs in {path}: {exc}") from exc
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


class ScopedRegressionError(ValueError):
    """A scoped write must never retain a larger debt allowance."""


def merge_scoped_baseline(
    original: dict[str, Any], measured: Measurement, requested: list[str]
) -> dict[str, Any]:
    """Tighten selected TUs only; preserve the last full measurement (ADR-1243)."""
    wanted = set(requested)
    if (
        not wanted
        or set(measured.sources) != wanted
        or len(measured.sources) != len(wanted)
        or measured.tus != len(wanted)
        or measured.compile_failures
    ):
        raise ValueError("scoped write requires exact, nonempty, successful TU coverage")
    baseline = Measurement.from_json(original)
    if baseline.lane != measured.lane or not measured.clang_tidy_version:
        raise ValueError("scoped write requires a matching lane and known tool version")
    if baseline.clang_tidy_version != measured.clang_tidy_version:
        raise ValueError("scoped write requires the original clang-tidy version")
    result = copy.deepcopy(original)
    changes: dict[str, dict[str, list[int]]] = {}
    for metric in ("warnings", "nolint_uncited"):
        before: dict[str, int] = getattr(baseline, metric)
        after: dict[str, int] = getattr(measured, metric)
        raw_before = original.get(metric, {})
        if any(
            not isinstance(value, int) or isinstance(value, bool) or value < 0
            for value in (*raw_before.values(), *after.values())
        ):
            raise ValueError("debt counts must be nonnegative integers")
        for path, count in after.items():
            if count > before.get(path, 0):
                raise ScopedRegressionError(f"{path}: {metric} would increase to {count}")
        merged = dict(before)
        for path in sorted(wanted):
            count = after.get(path, 0)
            if count != before.get(path, 0):
                changes.setdefault(path, {})[metric] = [before.get(path, 0), count]
            if count:
                merged[path] = count
            else:
                merged.pop(path, None)
        result[metric] = dict(sorted(merged.items()))
        result[f"total_{metric}"] = sum(merged.values())
    if changes:
        provenance = {
            "sources": sorted(wanted),
            "clang_tidy_version": measured.clang_tidy_version,
            "changes": changes,
            "previous_baseline_sha256": hashlib.sha256(
                json.dumps(original, sort_keys=True).encode("utf-8")
            ).hexdigest(),
        }
        result.setdefault("scoped_updates", []).append(provenance)
    return result


@contextmanager
def baseline_lock(path: Path) -> Iterator[None]:
    """Serialize cooperating POSIX writers by resolved filename (ADR-1243)."""
    if os.name != "posix":
        raise OSError("baseline writes require POSIX advisory locking")
    lock_root = Path(tempfile.gettempdir()) / f"vmafx-tidy-locks-{os.getuid()}"
    lock_root.mkdir(mode=0o700, exist_ok=True)
    key = hashlib.sha256(str(path.resolve()).encode("utf-8")).hexdigest()
    # Keep the lock inode: unlinking would let a third process lock a different
    # inode while another writer still held the old one. Locks release on exit.
    descriptor = os.open(lock_root / key, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(descriptor)


def atomic_write_baseline(path: Path, result: dict[str, Any], expected: bytes | None) -> None:
    """Replace validated output after checking for non-cooperating file drift."""
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", dir=path.parent, delete=False
        ) as output:
            temporary = Path(output.name)
            output.write(json.dumps(result, indent=2) + "\n")
            output.flush()
            os.fsync(output.fileno())
        mode = path.stat().st_mode if path.exists() else 0o644
        temporary.chmod(mode)
        current = path.read_bytes() if path.exists() else None
        if current != expected:
            raise ValueError("baseline changed during update; rerun measurement")
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def write_scoped_baseline(
    path: Path, measured: Measurement, requested: list[str], expected: bytes | None = None
) -> int:
    """Validate every guard before writing any byte of the existing baseline."""
    try:
        path = path.resolve(strict=True)
        with baseline_lock(path):
            original_bytes = path.read_bytes()
            if expected is not None and original_bytes != expected:
                raise ValueError("baseline changed during measurement; rerun measurement")
            original = json.loads(original_bytes)
            result = merge_scoped_baseline(original, measured, requested)
            if result != original:
                atomic_write_baseline(path, result, original_bytes)
    except ScopedRegressionError as exc:
        annotate("error", str(exc))
        return 2
    except (OSError, ValueError, TypeError, AttributeError, RuntimeError) as exc:
        annotate("error", f"cannot tighten scoped baseline: {exc}")
        return 5
    print(
        f"tidy-ratchet: scoped baseline tightened for {len(set(requested))} TUs; full metadata retained"
    )
    return 0


def write_full_baseline(path: Path, measured: Measurement, expected: bytes | None) -> int:
    """Use the same lock and atomic replacement for the original full writer."""
    try:
        path = path.resolve()
        with baseline_lock(path):
            atomic_write_baseline(path, measured.to_json(), expected)
    except (OSError, ValueError, RuntimeError) as exc:
        annotate("error", f"cannot write baseline: {exc}")
        return 5
    print(f"tidy-ratchet: wrote {path} ({measured.total_warnings} warnings)")
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
        "--only",
        action="append",
        default=[],
        help="measure exactly these TUs; with --write, tighten only their allowance",
    )
    parser.add_argument("--report", type=Path, help="write the measurement JSON here")
    parser.add_argument(
        "--write",
        action="store_true",
        help="write the full measurement, or guarded scoped tightening with --only",
    )
    parser.add_argument(
        "--allow-slack", action="store_true", help="do not fail when files improved"
    )
    return parser.parse_args(argv)


def same_output_file(first: Path, second: Path) -> bool:
    """Resolved names catch symlinks; samefile also catches hard-link aliases."""
    if first.resolve() == second.resolve():
        return True
    try:
        return first.samefile(second)
    except FileNotFoundError:
        return False


def prepare_output(args: argparse.Namespace, baseline_path: Path) -> bytes | None:
    """Reject report aliases and snapshot the baseline before any measurement."""
    if args.report and same_output_file(args.report, baseline_path):
        raise ValueError("measurement report must not overwrite the baseline")
    if args.write:
        try:
            return baseline_path.read_bytes()
        except FileNotFoundError:
            if args.only:
                raise ValueError("scoped write requires an existing baseline") from None
    return None


def finish_measurement(
    args: argparse.Namespace,
    repo_root: Path,
    baseline_path: Path,
    baseline_before: bytes | None,
    measured: Measurement,
) -> int:
    """Apply the full gate or explicitly selected local write/report mode."""
    if args.only:
        if args.write:
            requested = [
                Path(path).resolve().relative_to(repo_root).as_posix() for path in args.only
            ]
            return write_scoped_baseline(baseline_path, measured, requested, baseline_before)
        annotate("notice", "--only given: comparison against the baseline skipped")
        return 0
    if args.write:
        return write_full_baseline(baseline_path, measured, baseline_before)
    baseline = Measurement.from_json(json.loads(baseline_path.read_text(encoding="utf-8")))
    return report(baseline, measured, args.allow_slack)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    baseline_path = (
        args.baseline or repo_root / "scripts" / "ci" / f"tidy-baseline-{args.lane}.json"
    )
    try:
        baseline_before = prepare_output(args, baseline_path)
        binary = shutil.which(args.clang_tidy) or args.clang_tidy
        measured = measure(
            args.lane,
            args.build_dir.resolve(),
            repo_root,
            binary,
            args.extra_arg,
            args.jobs,
            args.only,
        )
        if args.report:
            args.report.write_text(
                json.dumps(measured.to_json(), indent=2) + "\n", encoding="utf-8"
            )
        if measured.compile_failures:
            for path in sorted(measured.compile_failures):
                annotate(
                    "error",
                    f"{path}: clang-tidy compile/tool/diagnostic parse failure; measurement unusable",
                )
            return 4
        return finish_measurement(args, repo_root, baseline_path, baseline_before, measured)
    except (OSError, ValueError, RuntimeError) as exc:
        annotate("error", f"measurement/output failed: {exc}")
        return 5


if __name__ == "__main__":
    sys.exit(main())
