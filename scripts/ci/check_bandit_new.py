#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Refuse new Codacy Bandit findings in changed Python files."""

from __future__ import annotations

import argparse
import io
import json
import os
import sys
import tempfile
from pathlib import Path

from identify.identify import parse_shebang, tags_from_filename, tags_from_interpreter

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.new_scanner_findings import (
    Comparison,
    Finding,
    ToolError,
    added_findings,
    comparison_arguments,
    python_text,
    revision_bytes,
)
from scripts.lib.safe_subprocess import run as run_command

CONFIG = Path(__file__).resolve().parents[2] / "bandit.yml"
PYTHON_TAGS = frozenset({"python", "pyi"})
MAX_SHEBANG_BYTES = 4096
MAX_BATCH_FILES = 200
MAX_BATCH_ARG_CHARS = 24_000
BASE_BATCH_TIMEOUT_SECONDS = 60
PER_FILE_TIMEOUT_SECONDS = 2
MAX_BATCH_TIMEOUT_SECONDS = 300
MAX_BATCH_OUTPUT_BYTES = 32 * 1_048_576
MAX_LABEL_PATHS = 3


def is_python_source(source: Path) -> bool:
    """Classify named files from filename tags and extensionless copies from their shebang."""
    tags = tags_from_filename(source.name)
    if tags:
        return bool(PYTHON_TAGS & tags)
    with source.open("rb") as handle:
        shebang = parse_shebang(io.BytesIO(handle.read(MAX_SHEBANG_BYTES)))
    return bool(shebang and PYTHON_TAGS & tags_from_interpreter(shebang[0]))


def batch_paths(paths: list[str]) -> list[list[str]]:
    """Split relative paths into chunks bounded by file count and joined character length."""
    batches: list[list[str]] = []
    current: list[str] = []
    used_chars = 0
    for path in paths:
        if len(path) > MAX_BATCH_ARG_CHARS:
            raise ToolError(f"path exceeds {MAX_BATCH_ARG_CHARS} argument characters: {path}")
        added_chars = len(path) if not current else len(path) + 1
        if current and (
            len(current) >= MAX_BATCH_FILES or used_chars + added_chars > MAX_BATCH_ARG_CHARS
        ):
            batches.append(current)
            current = [path]
            used_chars = len(path)
        else:
            current.append(path)
            used_chars += added_chars
    if current:
        batches.append(current)
    return batches


def batch_timeout_seconds(file_count: int) -> int:
    """Scale the Bandit process deadline with batch size up to MAX_BATCH_TIMEOUT_SECONDS."""
    return min(
        MAX_BATCH_TIMEOUT_SECONDS,
        BASE_BATCH_TIMEOUT_SECONDS + PER_FILE_TIMEOUT_SECONDS * max(1, file_count),
    )


def _batch_label(chunk: list[str]) -> str:
    """Format a concise relative-path label for batch-level diagnostics."""
    if len(chunk) <= MAX_LABEL_PATHS:
        return ", ".join(chunk)
    return f"{chunk[0]} (+{len(chunk) - 1} more)"


def _report_keys(relative: str) -> tuple[str, ...]:
    """Return the filename spellings Bandit may emit for a staged relative path."""
    keys = [f"./{relative}"]
    if relative != "_totals":
        keys.append(relative)
    if os.sep != "/":
        native = relative.replace("/", os.sep)
        keys.extend((f".{os.sep}{relative}", f".{os.sep}{native}", native))
    return tuple(dict.fromkeys(keys))


def _error_target(errors: object, aliases: dict[str, str], fallback: str) -> str:
    """Extract the first matching staged relative path from Bandit error entries, if any."""
    if isinstance(errors, list):
        for entry in errors:
            if isinstance(entry, dict) and isinstance(entry.get("filename"), str):
                matched = aliases.get(entry["filename"])
                if matched is not None:
                    return matched
    return fallback


def _read_source_lines(root: Path, relative: str) -> list[str]:
    """Decode staged Python bytes into source lines, naming the relative path on failure."""
    try:
        return python_text((root / relative).read_bytes()).splitlines()
    except (OSError, ValueError, SyntaxError, LookupError) as error:
        raise ToolError(f"Bandit failed on {relative}: {error}") from error


def _collect_chunk_findings(
    results: list[object],
    aliases: dict[str, str],
    lines_by_path: dict[str, list[str]],
    label: str,
) -> dict[str, list[Finding]]:
    """Validate result entries against chunk membership and per-file line bounds."""
    found: dict[str, list[Finding]] = {relative: [] for relative in lines_by_path}
    for issue in results:
        if not isinstance(issue, dict):
            raise ToolError(f"invalid Bandit issue on {label}")
        raw_name = issue.get("filename")
        relative = aliases.get(raw_name) if isinstance(raw_name, str) else None
        if relative is None:
            raise ToolError(f"unexpected Bandit result filename on {label}: {raw_name!r}")
        lines = lines_by_path[relative]
        number, rule = issue.get("line_number"), issue.get("test_id")
        if (
            isinstance(number, bool)
            or not isinstance(number, int)
            or number < 1
            or number > len(lines)
        ):
            raise ToolError(f"invalid Bandit line on {relative}: {number}")
        if not isinstance(rule, str) or not rule:
            raise ToolError(f"invalid Bandit rule on {relative}")
        found[relative].append((number, rule, lines[number - 1].strip()))
    return found


def _parse_batch_report(raw_stdout: str, root: Path, chunk: list[str]) -> dict[str, list[Finding]]:
    """Validate Bandit JSON, check metrics coverage, and map findings back to relative paths."""
    label = _batch_label(chunk)
    aliases = {key: relative for relative in chunk for key in _report_keys(relative)}
    report = json.loads(raw_stdout)
    if not isinstance(report, dict) or not isinstance(report.get("results"), list):
        raise ToolError(f"invalid Bandit JSON on {label}")
    errors = report.get("errors")
    if not isinstance(errors, list) or errors:
        target = _error_target(errors, aliases, label)
        raise ToolError(f"Bandit reported scan errors on {target}: {errors}")
    metrics = report.get("metrics")
    if not isinstance(metrics, dict):
        raise ToolError(f"missing Bandit metrics on {label}")
    lines_by_path: dict[str, list[str]] = {}
    for relative in chunk:
        if not any(isinstance(metrics.get(key), dict) for key in _report_keys(relative)):
            raise ToolError(f"Bandit skipped {relative}: missing from metrics")
        lines_by_path[relative] = _read_source_lines(root, relative)
    return _collect_chunk_findings(report["results"], aliases, lines_by_path, label)


def _run_batch(bandit: str, root: Path, chunk: list[str], config: Path) -> dict[str, list[Finding]]:
    """Run one bounded Bandit batch in `root` and return findings keyed by relative path."""
    label = _batch_label(chunk)
    try:
        # -q raises Bandit's logger above INFO so >50 files do not emit rich.progress on stdout.
        proc = run_command(
            [bandit, "-c", str(config), "-f", "json", "-q", "--", *chunk],
            allowed_executables=(bandit,),
            cwd=root,
            capture_output=True,
            text=True,
            timeout_seconds=batch_timeout_seconds(len(chunk)),
            max_output_bytes=MAX_BATCH_OUTPUT_BYTES,
        )
        if proc.returncode not in (0, 1):
            raise ToolError(f"bandit exited {proc.returncode} on {label}: {proc.stderr.strip()}")
        return _parse_batch_report(proc.stdout, root, chunk)
    except ToolError:
        raise
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        raise ToolError(f"Bandit failed on {label}: {error}") from error


def findings(
    bandit: str, root: Path, paths: list[str], config: Path = CONFIG
) -> dict[str, list[Finding]]:
    """Scan staged `paths` under `root` in bounded batches and return findings per path."""
    collected: dict[str, list[Finding]] = {}
    for chunk in batch_paths(paths):
        collected.update(_run_batch(bandit, root, chunk, config))
    return collected


def _stage_sources(
    comparison: Comparison,
    paths: list[Path],
    current_root: Path,
    base_root: Path,
    *,
    force: bool,
) -> tuple[list[str], list[str]]:
    """Stage selected Python sources under current/base roots; skip identical or empty base copies."""
    current_paths: list[str] = []
    base_paths: list[str] = []
    seen: set[str] = set()
    for path in paths:
        tags = tags_from_filename(str(path))
        if tags and not PYTHON_TAGS & tags:
            continue
        copied = comparison.copy(path, current_root)
        if copied is None:
            continue
        relative, source = copied
        if relative in seen:
            continue
        seen.add(relative)
        if not is_python_source(source):
            source.unlink()
            continue
        old_bytes = comparison.old_bytes(relative)
        if not force and source.read_bytes() == old_bytes:
            source.unlink()
            continue
        current_paths.append(relative)
        if old_bytes:
            base_target = base_root / relative
            base_target.parent.mkdir(parents=True, exist_ok=True)
            base_target.write_bytes(old_bytes)
            base_paths.append(relative)
    return current_paths, base_paths


def scan_changes(bandit: str, comparison: Comparison, paths: list[Path]) -> int:
    """Count new findings using cached revision modes, including configuration changes."""
    current_config = comparison.current_bytes("bandit.yml")
    if current_config is None:
        raise ToolError("Bandit configuration missing: bandit.yml")
    base_config = revision_bytes(
        comparison.git,
        comparison.root,
        comparison.base,
        "bandit.yml",
        modes=comparison.modes(comparison.base),
    )
    changed_config = base_config != current_config
    if changed_config:
        paths = sorted(set(paths + comparison.tracked_paths()))
    failed = 0
    with tempfile.TemporaryDirectory(prefix="bandit-comparison-") as temp:
        directory = Path(temp).resolve()
        current, old = directory / "current.yml", directory / "base.yml"
        current.write_bytes(current_config)
        old.write_bytes(base_config if base_config is not None else current_config)
        current_root, base_root = directory / "current", directory / "base"
        current_paths, base_paths = _stage_sources(
            comparison, paths, current_root, base_root, force=changed_config
        )
        current_found = (
            findings(bandit, current_root, current_paths, current) if current_paths else {}
        )
        base_found = findings(bandit, base_root, base_paths, old) if base_paths else {}
        for relative in current_paths:
            for line, rule, source_line in added_findings(
                current_found[relative], base_found.get(relative, [])
            ):
                print(f"{relative}:{line}: {rule}: {source_line}")
                failed += 1
    return failed


def main(argv: list[str] | None = None) -> int:
    """Return 0 for no new Bandit findings, 1 for new findings, or 2 for scan errors."""
    parser = argparse.ArgumentParser(description=(__doc__ or "").strip())
    try:
        scanner, comparison, paths = comparison_arguments(parser, argv, "bandit")
        failed = scan_changes(scanner, comparison, paths)
    except (ToolError, OSError, ValueError, RuntimeError) as error:
        print(f"check_bandit_new: {error}", file=sys.stderr)
        return 2
    if failed:
        print(
            f"{failed} new Bandit finding(s); fix each finding "
            "(process calls go through scripts.lib.safe_subprocess.run; "
            "# noqa does not silence Bandit)",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
