#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check GitHub Actions workflow files against engine byte size and runner expression bounds.

Enforces two bounds over `.github/workflows/*.yml` and `*.yaml`:
1. File byte size <= 64 KiB (`DEFAULT_MAX_BYTES = 65536`), matching Praetor engine
   `internal/adopt/verification_inputs.go` (`maxVerificationInputBytes = 64 * 1024`).
   Oversize files cause metadata byte bound errors during engine audit checks.
2. Expression length <= 21000 characters (`MAX_EXPRESSION_LENGTH = 21000`), mirroring how
   the GitHub Actions runner converts any YAML string scalar containing `${{` into one
   expression (`TemplateReader.cs` `ParseScalar`, `ExpressionConstants.cs` `MaxLength`).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import yaml  # type: ignore[import-untyped]

DEFAULT_MAX_BYTES = 65536  # 64 * 1024
# GitHub Actions runner template expression limit:
# - actions/runner src/Sdk/DTObjectTemplating/ObjectTemplating/TemplateReader.cs (ParseScalar)
# - actions/runner src/Sdk/DTExpressions2/Expressions2/ExpressionConstants.cs (MaxLength = 21000)
MAX_EXPRESSION_LENGTH = 21000
MAX_YAML_NODES = 65536


def check_workflow_sizes(workflow_paths: list[Path], max_bytes: int) -> list[tuple[Path, int]]:
    """Return a list of (path, size) tuples for workflows that exceed max_bytes."""
    oversized: list[tuple[Path, int]] = []
    for path in workflow_paths:
        size = path.stat().st_size
        if size > max_bytes:
            oversized.append((path, size))
    return oversized


def _find_expression_close(scalar: str, expr_start: int) -> int | None:
    """Return index of the first closing `}}` outside single quotes, or None if unclosed."""
    in_string = False
    limit = len(scalar)
    for idx in range(expr_start, limit):
        ch = scalar[idx]
        if ch == "'":
            in_string = not in_string
        elif not in_string and ch == "}" and idx + 1 < limit and scalar[idx + 1] == "}":
            return idx
    return None


def _split_template_segments(scalar: str) -> list[tuple[bool, str]] | None:
    """Split a scalar containing `${{` into `(is_expr, text)` segments, or None if unclosed."""
    segments: list[tuple[bool, str]] = []
    pos = 0
    total = len(scalar)
    for _ in range(total + 1):
        if pos >= total:
            break
        open_idx = scalar.find("${{", pos)
        if open_idx == -1:
            segments.append((False, scalar[pos:]))
            break
        if open_idx > pos:
            segments.append((False, scalar[pos:open_idx]))
        close_idx = _find_expression_close(scalar, open_idx + 3)
        if close_idx is None:
            return None
        segments.append((True, scalar[open_idx + 3 : close_idx]))
        pos = close_idx + 2
    return segments


def _escaped_literal_length(literal: str) -> int:
    """Return `format(...)` template length of a literal segment (`'`, `{`, `}` doubled)."""
    return len(literal) + literal.count("'") + literal.count("{") + literal.count("}")


def runner_expression_length(scalar: str) -> int | None:
    """Compute the expression length built by actions/runner `ParseScalar`, or None if unclosed."""
    segments = _split_template_segments(scalar)
    if segments is None:
        return None
    if len(segments) == 1 and segments[0][0]:
        return len(segments[0][1])
    literal_len = 0
    expr_placeholders_len = 0
    expr_args_len = 0
    expr_index = 0
    for is_expr, text in segments:
        if is_expr:
            expr_placeholders_len += len(f"{{{expr_index}}}")
            expr_args_len += 2 + len(text)
            expr_index += 1
        else:
            literal_len += _escaped_literal_length(text)
    return 8 + literal_len + expr_placeholders_len + 1 + expr_args_len + 1


def _format_mapping_path(prefix: str, key: object) -> str:
    """Format a dotted YAML mapping path segment."""
    key_name = "on" if key is True else str(key)
    return f"{prefix}.{key_name}" if prefix else key_name


def _enqueue_children(prefix: str, node: object, stack: list[tuple[str, object]]) -> None:
    """Push mapping values and sequence items onto `stack` in reverse source order."""
    if isinstance(node, dict):
        for key, value in reversed(list(node.items())):
            stack.append((_format_mapping_path(prefix, key), value))
    elif isinstance(node, list):
        for idx in range(len(node) - 1, -1, -1):
            stack.append((f"{prefix}[{idx}]", node[idx]))


def _scalar_expression_problem(yaml_path: str, scalar: str, max_expr_len: int) -> str | None:
    """Validate one string scalar for unclosed `${{` or runner expression length overflow."""
    if "${{" not in scalar:
        return None
    location = yaml_path or "<root>"
    expr_len = runner_expression_length(scalar)
    if expr_len is None:
        return f"{location}: unclosed '${{{{' expression"
    if expr_len > max_expr_len:
        return f"{location}: expression length {expr_len} exceeds {max_expr_len} character limit"
    return None


def _file_expression_problems(path: Path, max_expr_len: int) -> list[str]:
    """Walk all YAML string scalars iteratively and return expression diagnostics."""
    try:
        doc: object = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, yaml.YAMLError) as exc:
        return [f"failed to parse YAML: {exc}"]
    if doc is None:
        return []
    problems: list[str] = []
    stack: list[tuple[str, object]] = [("", doc)]
    for _ in range(MAX_YAML_NODES):
        if not stack:
            return problems
        yaml_path, node = stack.pop()
        if isinstance(node, str):
            problem = _scalar_expression_problem(yaml_path, node, max_expr_len)
            if problem is not None:
                problems.append(problem)
        else:
            _enqueue_children(yaml_path, node, stack)
    return [*problems, f"YAML structure exceeds {MAX_YAML_NODES} node limit"]


def check_workflow_expressions(
    workflow_paths: list[Path], max_expression_length: int = MAX_EXPRESSION_LENGTH
) -> list[tuple[Path, str]]:
    """Return `(path, problem)` tuples for workflow scalars violating runner expression bounds."""
    violations: list[tuple[Path, str]] = []
    for path in workflow_paths:
        for problem in _file_expression_problems(path, max_expression_length):
            violations.append((path, problem))
    return violations


def _resolve_workflow_paths(files: list[Path], root: Path) -> list[Path] | None:
    """Resolve explicit CLI workflow paths or discover `.github/workflows/*.y{a,}ml`."""
    if files:
        return [p.resolve() for p in files]
    workflows_dir = root / ".github" / "workflows"
    if not workflows_dir.is_dir():
        print(f"error: {workflows_dir} is not a directory", file=sys.stderr)
        return None
    return sorted(
        p for p in workflows_dir.iterdir() if p.is_file() and p.suffix in (".yml", ".yaml")
    )


def _display_path(path: Path, root: Path) -> Path:
    """Return `path` relative to `root` when inside `root`, otherwise `path`."""
    return path.relative_to(root) if path.is_relative_to(root) else path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify .github/workflows/*.yml and *.yaml stay within size and expression bounds."
    )
    parser.add_argument(
        "--max-bytes",
        type=int,
        default=DEFAULT_MAX_BYTES,
        help=f"Maximum allowed file size in bytes (default: {DEFAULT_MAX_BYTES})",
    )
    parser.add_argument(
        "files",
        nargs="*",
        type=Path,
        help="Optional workflow files to check. If empty, scans .github/workflows/*.yml and *.yaml",
    )
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parents[2]
    paths = _resolve_workflow_paths(args.files, root)
    if paths is None:
        return 1

    oversized = check_workflow_sizes(paths, args.max_bytes)
    expression_errors = check_workflow_expressions(paths)
    if oversized or expression_errors:
        for path, size in oversized:
            rel = _display_path(path, root)
            print(
                f"error: {rel} ({size} bytes) exceeds {args.max_bytes} byte limit",
                file=sys.stderr,
            )
        for path, problem in expression_errors:
            rel = _display_path(path, root)
            print(f"error: {rel}: {problem}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
