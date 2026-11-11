#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Refuse new Codacy Bandit findings in changed Python files."""

from __future__ import annotations

import argparse
import io
import json
import sys
import tempfile
from pathlib import Path

from identify.identify import parse_shebang, tags_from_filename, tags_from_interpreter

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.new_scanner_findings import (
    Comparison,
    ToolError,
    comparison_arguments,
    new_findings,
    python_text,
    revision_bytes,
)
from scripts.lib.safe_subprocess import run as run_command

CONFIG = Path(__file__).resolve().parents[2] / "bandit.yml"
PYTHON_TAGS = frozenset({"python", "pyi"})
MAX_SHEBANG_BYTES = 4096


def is_python_source(source: Path) -> bool:
    """Classify named files from filename tags and extensionless copies from their shebang."""
    tags = tags_from_filename(source.name)
    if tags:
        return bool(PYTHON_TAGS & tags)
    with source.open("rb") as handle:
        shebang = parse_shebang(io.BytesIO(handle.read(MAX_SHEBANG_BYTES)))
    return bool(shebang and PYTHON_TAGS & tags_from_interpreter(shebang[0]))


def findings(bandit: str, source: Path, config: Path = CONFIG) -> list[tuple[int, str, str]]:
    """Read Bandit JSON and return rule IDs keyed by stripped source lines."""
    try:
        proc = run_command(
            [bandit, "-c", str(config), "-f", "json", str(source)],
            allowed_executables=(bandit,),
            capture_output=True,
            text=True,
            timeout_seconds=120,
            max_output_bytes=8 * 1_048_576,
        )
        if proc.returncode not in (0, 1):
            raise ToolError(f"bandit exited {proc.returncode} on {source}: {proc.stderr.strip()}")
        report = json.loads(proc.stdout)
        if not isinstance(report, dict) or not isinstance(report.get("results"), list):
            raise ToolError(f"invalid Bandit JSON on {source}")
        if not isinstance(report.get("errors"), list) or report["errors"]:
            raise ToolError(f"Bandit reported scan errors on {source}: {report.get('errors')}")
        lines = python_text(source.read_bytes()).splitlines()
        found = []
        for issue in report["results"]:
            number, rule = issue["line_number"], issue["test_id"]
            if not isinstance(number, int) or number < 1 or number > len(lines):
                raise ToolError(f"invalid Bandit line on {source}: {number}")
            if not isinstance(rule, str):
                raise ToolError(f"invalid Bandit rule on {source}")
            found.append((number, rule, lines[number - 1].strip()))
        return found
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        RuntimeError,
        SyntaxError,
        LookupError,
    ) as error:
        raise ToolError(f"Bandit failed on {source}: {error}") from error


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
        directory = Path(temp)
        current, old = directory / "current.yml", directory / "base.yml"
        current.write_bytes(current_config)
        old.write_bytes(base_config if base_config is not None else current_config)
        for path in paths:
            tags = tags_from_filename(str(path))
            if tags and not PYTHON_TAGS & tags:
                continue
            copied = comparison.copy(path, directory / "source")
            if copied is None:
                continue
            relative, source = copied
            if not is_python_source(source):
                continue
            added = new_findings(
                lambda source: findings(bandit, source, current),
                source,
                comparison.old_bytes(relative),
                old_scan=lambda source: findings(bandit, source, old),
                force=changed_config,
            )
            for line, rule, source_line in added:
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
