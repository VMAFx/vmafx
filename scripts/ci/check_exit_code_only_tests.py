#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Detect Python test functions in scripts/ci/tests/ that assert only exit code 0.

A test that runs a process or checker (`subprocess.run`, a helper named `run*`
or `_run*`, or a `main(...)` call) and compares only its return code with `0`
(`== 0`, `assert not rc`, `assertFalse(rc)`), or makes no assertion at all
(including only `check=True` or `proc.check_returncode()`), passes even when
the checker silently drops its input.

Default mode scans `scripts/ci/tests/test_*.py`, prints all findings with an
informational count and top 5 files, and exits 0. `--changed-against <ref>`
checks only test functions whose line span (from the first decorator or excuse
comment) intersects the changed ranges from `git diff -M -U0 <ref>...` (plus
working-tree changes against `HEAD`) and exits 1 on findings. Unreadable files,
files above 1 MiB, syntax errors, bad refs, and git diff errors exit 2 with the
path on stderr.

Justified cases carry `# exit-code-only: <reason>` (reason >= 10 characters)
on the `def` line or the line immediately above it (or its first decorator); an
empty or shorter reason is itself a finding.
"""

from __future__ import annotations

import argparse
import ast
import itertools
import os
import re
import shutil
import sys
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import TypeAlias

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.lib.safe_subprocess import (  # noqa: E402
    CommandOutputLimitExceeded,
    CommandTimedOut,
    TextCommandResult,
)
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

TESTS_REL_DIR = Path("scripts/ci/tests")
MAX_FILE_BYTES = 1_048_576
MAX_TEST_FILES = 2_048
MAX_AST_NODES = 100_000
MAX_BODY_NODES = 50_000
MAX_CALL_ARGS = 64
MAX_DECORATORS = 32
MAX_ASSIGN_TARGETS = 32
MAX_DIFF_HUNKS = 10_000
MIN_REASON_LENGTH = 10
TOP_FILES_LIMIT = 5
EQUAL_OPERAND_COUNT = 2
GIT_TIMEOUT_SECONDS = 30.0

RC_NAMES = frozenset({"rc", "returncode", "return_code", "exit_code", "status", "code"})
ESCAPE_RE = re.compile(r"#\s*exit-code-only(?:\s*:(.*)|$)")
HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", re.MULTILINE)
REMOVED_DEF_RE = re.compile(r"^-\s*(?:async\s+)?def\b", re.MULTILINE)
REMOVED_META_RE = re.compile(r"^-(?:\s*@|.*#\s*exit-code-only(?:\s*:|$))", re.MULTILINE)

FuncNode: TypeAlias = ast.FunctionDef | ast.AsyncFunctionDef


class ScanError(Exception):
    """Raised when a file, syntax tree, or git reference cannot be checked."""


@dataclass(frozen=True)
class Finding:
    """One test function that asserts only exit code 0 or has an invalid escape."""

    path: str
    lineno: int
    end_lineno: int
    func_name: str
    message: str
    span_start: int | None = None

    def format(self) -> str:
        """Render the finding with file, line, function name, and reason."""
        return f"{self.path}:{self.lineno}: {self.func_name}: {self.message}"


def read_bounded_source(path: Path, label: str) -> str:
    """Read a UTF-8 Python source file up to MAX_FILE_BYTES (1 MiB)."""
    try:
        if not path.is_file():
            raise ScanError(f"{label}: not a regular file")
        with path.open("rb") as handle:
            raw = handle.read(MAX_FILE_BYTES + 1)
    except OSError as exc:
        raise ScanError(f"{label}: {exc}") from exc
    if len(raw) > MAX_FILE_BYTES:
        raise ScanError(f"{label}: exceeds {MAX_FILE_BYTES} byte read limit")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ScanError(f"{label}: invalid UTF-8: {exc}") from exc


def _is_runner_call(node: ast.AST) -> bool:
    """True for subprocess.run, run*/_run* helpers, or main(...) calls."""
    if not isinstance(node, ast.Call):
        return False
    func = node.func
    if isinstance(func, ast.Attribute):
        if func.attr.startswith(("run", "_run")):
            return True
        if func.attr == "main":
            return not (isinstance(func.value, ast.Name) and func.value.id == "unittest")
        return False
    if isinstance(func, ast.Name):
        return func.id.startswith(("run", "_run")) or func.id == "main"
    return False


def _body_nodes(func: FuncNode) -> list[ast.AST]:
    """Walk a function body without recursion or descending into nested definitions."""
    stack: list[ast.AST] = list(reversed(func.body[:MAX_BODY_NODES]))
    nodes: list[ast.AST] = []
    for _ in range(MAX_BODY_NODES):
        if not stack:
            return nodes
        curr = stack.pop()
        nodes.append(curr)
        if isinstance(curr, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            continue
        children = list(itertools.islice(ast.iter_child_nodes(curr), MAX_BODY_NODES))
        stack.extend(reversed(children))
    raise ScanError(f"{func.name}: function body exceeds {MAX_BODY_NODES} AST nodes")


def _contains_runner_value(node: ast.AST) -> bool:
    """True if an assigned expression invokes a runner or reads .returncode."""
    stack: list[ast.AST] = [node]
    for _ in range(MAX_BODY_NODES):
        if not stack:
            return False
        curr = stack.pop()
        if _is_runner_call(curr):
            return True
        if isinstance(curr, ast.Attribute) and curr.attr == "returncode":
            return True
        if not isinstance(curr, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            stack.extend(itertools.islice(ast.iter_child_nodes(curr), MAX_CALL_ARGS))
    return False


def _record_target_vars(target: ast.AST, rc_vars: set[str]) -> None:
    """Record variable names bound to a runner call result or return code."""
    if isinstance(target, ast.Name):
        rc_vars.add(target.id)
    elif isinstance(target, (ast.Tuple, ast.List)) and target.elts:
        elts = target.elts[:MAX_ASSIGN_TARGETS]
        named = [e.id for e in elts if isinstance(e, ast.Name) and e.id in RC_NAMES]
        if named:
            rc_vars.update(named)
        elif isinstance(elts[0], ast.Name):
            rc_vars.add(elts[0].id)


def _collect_rc_vars(nodes: list[ast.AST]) -> set[str]:
    """Return local variable names assigned from a process or checker call."""
    rc_vars: set[str] = set()
    for node in nodes[:MAX_BODY_NODES]:
        if isinstance(node, ast.Assign) and _contains_runner_value(node.value):
            for target in node.targets[:MAX_ASSIGN_TARGETS]:
                _record_target_vars(target, rc_vars)
        elif (
            isinstance(node, (ast.AnnAssign, ast.NamedExpr))
            and node.value is not None
            and _contains_runner_value(node.value)
        ):
            _record_target_vars(node.target, rc_vars)
    return rc_vars


def _is_zero_literal(node: ast.AST) -> bool:
    """True only for the integer literal 0 (excluding booleans)."""
    return (
        isinstance(node, ast.Constant)
        and isinstance(node.value, int)
        and not isinstance(node.value, bool)
        and node.value == 0
    )


def _is_rc_expr(node: ast.AST, rc_vars: set[str]) -> bool:
    """True when an expression represents a process or checker return code."""
    expr = node.value if isinstance(node, ast.NamedExpr) else node
    if isinstance(expr, ast.Attribute) and expr.attr == "returncode":
        return True
    if _is_runner_call(expr):
        return True
    if isinstance(expr, ast.Subscript) and _is_zero_literal(expr.slice):
        val = expr.value
        if _is_runner_call(val) or (isinstance(val, ast.Name) and val.id in rc_vars):
            return True
    return isinstance(expr, ast.Name) and (expr.id in rc_vars or expr.id in RC_NAMES)


def _is_rc_zero_pair(left: ast.AST, right: ast.AST, rc_vars: set[str]) -> bool:
    """True when one operand is a return code and the other is literal 0."""
    return (_is_rc_expr(left, rc_vars) and _is_zero_literal(right)) or (
        _is_zero_literal(left) and _is_rc_expr(right, rc_vars)
    )


def _is_rc_eq_zero_compare(node: ast.AST, rc_vars: set[str]) -> bool:
    """True when a comparison is `<rc> == 0` or `0 == <rc>`."""
    if not isinstance(node, ast.Compare) or len(node.ops) != 1 or len(node.comparators) != 1:
        return False
    if not isinstance(node.ops[0], ast.Eq):
        return False
    return _is_rc_zero_pair(node.left, node.comparators[0], rc_vars)


def _is_rc_zero_test(node: ast.AST, rc_vars: set[str]) -> bool:
    """True when a boolean test checks `<rc> == 0`, `0 == <rc>`, or `not <rc>`."""
    if _is_rc_eq_zero_compare(node, rc_vars):
        return True
    return (
        isinstance(node, ast.UnaryOp)
        and isinstance(node.op, ast.Not)
        and _is_rc_expr(node.operand, rc_vars)
    )


def _equal_operands(call: ast.Call) -> tuple[ast.AST, ast.AST] | None:
    """Extract the two compared expressions of an assertEqual/assertEquals call."""
    if len(call.args) >= EQUAL_OPERAND_COUNT:
        return call.args[0], call.args[1]
    kw = {k.arg: k.value for k in call.keywords[:MAX_CALL_ARGS] if k.arg in ("first", "second")}
    if len(call.args) == 1 and "second" in kw:
        return call.args[0], kw["second"]
    if "first" in kw and "second" in kw:
        return kw["first"], kw["second"]
    return None


def _is_pytest_raises(func: ast.AST) -> bool:
    """True when a call target is `pytest.raises`."""
    return (
        isinstance(func, ast.Attribute)
        and func.attr == "raises"
        and isinstance(func.value, ast.Name)
        and func.value.id == "pytest"
    )


def _classify_assertion(node: ast.AST, rc_vars: set[str]) -> str | None:
    """Return 'rc_zero' for an exit-code-0 assertion, 'other' for other assertions."""
    if isinstance(node, ast.Assert):
        return "rc_zero" if _is_rc_zero_test(node.test, rc_vars) else "other"
    if not isinstance(node, ast.Call):
        return None
    func = node.func
    name = (
        func.attr
        if isinstance(func, ast.Attribute)
        else (func.id if isinstance(func, ast.Name) else "")
    )
    if name in ("assertEqual", "assertEquals"):
        pair = _equal_operands(node)
        return "rc_zero" if pair and _is_rc_zero_pair(pair[0], pair[1], rc_vars) else "other"
    if name == "assertTrue" and node.args and _is_rc_zero_test(node.args[0], rc_vars):
        return "rc_zero"
    if name == "assertFalse" and node.args and _is_rc_expr(node.args[0], rc_vars):
        return "rc_zero"
    return "other" if name.startswith("assert") or _is_pytest_raises(func) else None


def _func_span_start(func: FuncNode, lines: list[str]) -> int:
    """Return the earliest line of `func` including decorators and excuse comments."""
    start = func.lineno
    if func.decorator_list:
        start = min(start, *(d.lineno for d in func.decorator_list[:MAX_DECORATORS]))
    if 1 <= start - 1 <= len(lines) and ESCAPE_RE.search(lines[start - 2]) is not None:
        return start - 1
    return start


def _escape_status(func: FuncNode, lines: list[str]) -> str:
    """Return 'valid', 'short', or 'none' for an `# exit-code-only:` comment."""
    candidate_lines = [func.lineno - 1, func.lineno]
    if func.decorator_list:
        first_dec = min(d.lineno for d in func.decorator_list[:MAX_DECORATORS])
        candidate_lines.extend([first_dec - 1, first_dec])
    for line_no in sorted(set(candidate_lines)):
        if 1 <= line_no <= len(lines):
            match = ESCAPE_RE.search(lines[line_no - 1])
            if match is not None:
                reason = (match.group(1) or "").strip()
                return "valid" if len(reason) >= MIN_REASON_LENGTH else "short"
    return "none"


def _check_test_function(func: FuncNode, lines: list[str], rel_path: str) -> Finding | None:
    """Inspect one `def test_*` node and return a Finding when weak or unescaped."""
    end_lineno = func.end_lineno or func.lineno
    span_start = _func_span_start(func, lines)
    escape = _escape_status(func, lines)
    if escape == "short":
        return Finding(
            rel_path,
            func.lineno,
            end_lineno,
            func.name,
            f"# exit-code-only: reason must be at least {MIN_REASON_LENGTH} characters",
            span_start,
        )
    if escape == "valid":
        return None
    nodes = _body_nodes(func)
    if not any(_is_runner_call(n) for n in nodes):
        return None
    rc_vars = _collect_rc_vars(nodes)
    kinds = [k for n in nodes if (k := _classify_assertion(n, rc_vars)) is not None]
    if not kinds:
        return Finding(
            rel_path,
            func.lineno,
            end_lineno,
            func.name,
            "runs a command and asserts nothing",
            span_start,
        )
    if all(k == "rc_zero" for k in kinds):
        return Finding(
            rel_path,
            func.lineno,
            end_lineno,
            func.name,
            "asserts only exit code 0",
            span_start,
        )
    return None


def file_findings(source: str, rel_path: str) -> list[Finding]:
    """Parse one Python test file and return all exit-code-only findings in it."""
    try:
        tree = ast.parse(source, filename=rel_path)
    except SyntaxError as exc:
        raise ScanError(f"{rel_path}:{exc.lineno or 1}: syntax error: {exc.msg}") from exc
    lines = source.splitlines()
    funcs: list[FuncNode] = []
    walked = list(itertools.islice(ast.walk(tree), MAX_AST_NODES + 1))
    if len(walked) > MAX_AST_NODES:
        raise ScanError(f"{rel_path}: exceeds {MAX_AST_NODES} AST nodes")
    for node in walked:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name.startswith(
            "test_"
        ):
            funcs.append(node)
    funcs.sort(key=lambda n: (n.lineno, n.col_offset))
    return [
        finding
        for func in funcs[:MAX_AST_NODES]
        if (finding := _check_test_function(func, lines, rel_path)) is not None
    ]


def _run_git(root: Path, args: Sequence[str]) -> TextCommandResult:
    """Run a bounded read-only git command in `root` without inherited GIT_* vars."""
    git = shutil.which("git")
    if git is None:
        raise ScanError(f"{root}: git executable not found on PATH")
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    try:
        return run_command(
            [git, *args],
            allowed_executables=(git,),
            cwd=root,
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=GIT_TIMEOUT_SECONDS,
        )
    except (CommandTimedOut, CommandOutputLimitExceeded, OSError) as exc:
        raise ScanError(f"{root}: git {' '.join(args[:2])} failed: {exc}") from exc


def _verify_ref(root: Path, ref: str) -> None:
    """Ensure `ref` resolves to a commit in `root`, raising ScanError otherwise."""
    if not ref or ref.startswith("-"):
        raise ScanError(f"{root}: invalid git ref {ref!r}")
    res = _run_git(root, ["rev-parse", "--verify", "--end-of-options", f"{ref}^{{commit}}"])
    if res.returncode != 0:
        detail = res.stderr.strip() or "unknown revision"
        raise ScanError(f"{root}: bad ref {ref!r}: {detail}")


def _file_diff_section(diff_text: str, rel_path: str) -> str:
    """Extract the `diff --git` block(s) whose destination path is `rel_path`."""
    if "diff --git " not in diff_text:
        return diff_text
    plus_header = f"+++ b/{rel_path}"
    rename_header = f"rename to {rel_path}"
    matched: list[str] = []
    for block in ("\n" + diff_text).split("\ndiff --git ")[1 : MAX_TEST_FILES + 1]:
        header = block.split("\n@@ ", 1)[0]
        header_lines = header.splitlines()[:MAX_CALL_ARGS]
        if plus_header in header_lines or rename_header in header_lines:
            matched.append(block)
    return "\n".join(matched)


def _parse_diff_hunks(diff_text: str) -> list[tuple[int, int]]:
    """Extract (new_start, new_count) line ranges from `git diff -U0` output."""
    matches = list(itertools.islice(HUNK_RE.finditer(diff_text), MAX_DIFF_HUNKS))
    hunks: list[tuple[int, int]] = []
    for idx, match in enumerate(matches[:MAX_DIFF_HUNKS]):
        start = int(match.group(1))
        count = int(match.group(2)) if match.group(2) is not None else 1
        end_pos = matches[idx + 1].start() if idx + 1 < len(matches) else len(diff_text)
        body = diff_text[match.end() : end_pos]
        if REMOVED_META_RE.search(body) and not REMOVED_DEF_RE.search(body):
            hunks.append((start + 1, 1) if count == 0 else (start, count + 1))
        else:
            hunks.append((start, count))
    return hunks


def _changed_hunks(root: Path, ref: str, rel_path: str) -> list[tuple[int, int]]:
    """Return added/modified line ranges for `rel_path` relative to `ref`."""
    res = _run_git(root, ["diff", "-M", "-U0", f"{ref}..."])
    if res.returncode != 0:
        raise ScanError(f"{rel_path}: git diff against {ref!r} failed: {res.stderr.strip()}")
    ref_section = _file_diff_section(res.stdout, rel_path)
    hunks = _parse_diff_hunks(ref_section)
    work = _run_git(root, ["diff", "-M", "-U0", "HEAD"])
    if work.returncode != 0:
        raise ScanError(f"{rel_path}: git diff against HEAD failed: {work.stderr.strip()}")
    work_section = _file_diff_section(work.stdout, rel_path)
    if work_section:
        hunks.extend(_parse_diff_hunks(work_section))
    if not hunks and not ref_section and not work_section:
        tracked = _run_git(root, ["ls-files", "--error-unmatch", "--", rel_path])
        if tracked.returncode != 0:
            return [(1, MAX_AST_NODES)]
    return hunks


def _spans_intersect(func_start: int, func_end: int, hunks: Sequence[tuple[int, int]]) -> bool:
    """True when `[func_start, func_end]` intersects any diff hunk range.

    In `git diff -U0`, a pure deletion hunk `+N,0` (`new_count == 0`) sits
    immediately after new line `N`, so it matches `[func_start, func_end]` when
    `func_start <= N <= func_end` (including deletion of the function's last
    statement at `N == func_end`) without selecting a later function (`start > N`).
    """
    for new_start, new_count in hunks[:MAX_DIFF_HUNKS]:
        if new_count > 0:
            hunk_end = new_start + new_count - 1
            if max(func_start, new_start) <= min(func_end, hunk_end):
                return True
        elif func_start <= new_start <= func_end:
            return True
    return False


def discover_test_files(root: Path, explicit_paths: Sequence[str]) -> list[tuple[Path, str]]:
    """Return `(abs_path, rel_posix)` pairs for test files under `scripts/ci/tests/`."""
    if explicit_paths:
        pairs: list[tuple[Path, str]] = []
        for raw in explicit_paths[:MAX_TEST_FILES]:
            candidate = Path(raw)
            full = candidate if candidate.is_absolute() else (root / candidate)
            rel = candidate.as_posix() if not candidate.is_absolute() else full.as_posix()
            pairs.append((full, rel))
        return pairs
    tests_dir = root / TESTS_REL_DIR
    if not tests_dir.is_dir():
        raise ScanError(f"{tests_dir}: test directory not found")
    files = sorted(tests_dir.glob("test_*.py"))
    if len(files) > MAX_TEST_FILES:
        raise ScanError(f"{tests_dir}: exceeds {MAX_TEST_FILES} test files")
    return [(path, path.relative_to(root).as_posix()) for path in files[:MAX_TEST_FILES]]


def scan_repository(
    root: Path,
    changed_against: str | None = None,
    explicit_paths: Sequence[str] = (),
) -> list[Finding]:
    """Scan test files under `root` and return all applicable findings."""
    if changed_against is not None:
        _verify_ref(root, changed_against)
    files = discover_test_files(root, explicit_paths)
    findings: list[Finding] = []
    for full_path, rel_path in files[:MAX_TEST_FILES]:
        source = read_bounded_source(full_path, rel_path)
        current = file_findings(source, rel_path)
        if not current:
            continue
        if changed_against is not None:
            hunks = _changed_hunks(root, changed_against, rel_path)
            current = [
                f
                for f in current
                if _spans_intersect(f.span_start or f.lineno, f.end_lineno, hunks)
            ]
        findings.extend(current)
    return findings


def _print_default_report(findings: Sequence[Finding]) -> None:
    """Print informational findings, total count, and top 5 files in default mode."""
    counts: Counter[str] = Counter()
    for finding in findings[:MAX_AST_NODES]:
        print(finding.format())
        counts[finding.path] += 1
    print(
        f"check_exit_code_only_tests: {len(findings)} function(s) flagged "
        f"across {len(counts)} file(s)"
    )
    if counts:
        print(f"top {min(TOP_FILES_LIMIT, len(counts))} file(s):")
        for path, count in counts.most_common(TOP_FILES_LIMIT):
            print(f"  {count:3d} {path}")


def main(argv: Sequence[str] | None = None) -> int:
    """CLI entry point: 0 on pass or default mode, 1 on changed findings, 2 on error."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root",
        type=Path,
        default=None,
        help="repository root (default: current directory if scripts/ci/tests exists, else repo)",
    )
    parser.add_argument(
        "--changed-against",
        dest="changed_against",
        default=None,
        help="fail (exit 1) only on test functions added or modified relative to <ref>",
    )
    parser.add_argument("paths", nargs="*", help="optional explicit test file paths")
    args = parser.parse_args(argv)
    cwd = Path.cwd()
    root = (
        args.root
        if args.root is not None
        else (cwd if (cwd / TESTS_REL_DIR).is_dir() else REPO_ROOT)
    )
    try:
        findings = scan_repository(root, args.changed_against, args.paths)
    except ScanError as exc:
        print(f"check_exit_code_only_tests: error: {exc}", file=sys.stderr)
        return 2
    if args.changed_against is None:
        _print_default_report(findings)
        return 0
    for finding in findings[:MAX_AST_NODES]:
        print(finding.format(), file=sys.stderr)
    if findings:
        print(
            f"check_exit_code_only_tests: {len(findings)} added or modified test function(s) "
            "assert only exit code 0",
            file=sys.stderr,
        )
        return 1
    print("check_exit_code_only_tests: 0 exit-code-only test findings in changed functions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
