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
resolves `git merge-base <ref> HEAD`, lists changed and untracked files under
`<pathspec>` (`scripts/ci/tests` in default discovery or the normalised
root-relative explicit paths) with
`git diff --name-status -z -M --no-color --no-ext-diff <merge-base> -- <pathspec>`
and `git ls-files -z --others --exclude-standard -- <pathspec>`, and checks
weak tests touched by a zero-context diff hunk, including their decorators and
enclosing definition headers. Pure deletions touch the new-side start and next
line. Findings exit 1. Added, type-changed, and untracked files are checked in
full. Unreadable files, files above 1 MiB,
paths outside the repository root, changed or untracked Python paths inside
`<pathspec>` not in the discovered test files, unsupported diff status letters,
exceeded AST or file bounds, syntax errors, bad refs, and git errors exit 2
with the cause on stderr.

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
MAX_ENV_VARS = 4_096
MIN_REASON_LENGTH = 10
TOP_FILES_LIMIT = 5
EQUAL_OPERAND_COUNT = 2
GIT_TIMEOUT_SECONDS = 30.0

RC_NAMES = frozenset({"rc", "returncode", "return_code", "exit_code", "status", "code"})
ALLOWED_SIMPLE_STATUSES = frozenset({"A", "M", "D", "T"})
ESCAPE_RE = re.compile(r"#\s*exit-code-only(?:\s*:(.*)|$)")
HUNK_RE = re.compile(r"@@ -[0-9]+(?:,[0-9]+)? \+([0-9]+)(?:,([0-9]+))? @@(?: .*)?")

FuncNode: TypeAlias = ast.FunctionDef | ast.AsyncFunctionDef
DefNode: TypeAlias = ast.FunctionDef | ast.AsyncFunctionDef | ast.ClassDef


class ScanError(Exception):
    """Raised when a file, syntax tree, or git reference cannot be checked."""


@dataclass(frozen=True)
class Finding:
    """One test function that asserts only exit code 0 or has an invalid escape."""

    path: str
    lineno: int
    func_name: str
    message: str

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
    if len(func.body) > MAX_BODY_NODES:
        raise ScanError(f"{func.name}: function body exceeds {MAX_BODY_NODES} AST nodes")
    stack: list[ast.AST] = list(reversed(func.body[:MAX_BODY_NODES]))
    nodes: list[ast.AST] = []
    for _ in range(MAX_BODY_NODES):
        if not stack:
            return nodes
        curr = stack.pop()
        nodes.append(curr)
        if isinstance(curr, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            continue
        children = list(itertools.islice(ast.iter_child_nodes(curr), MAX_BODY_NODES + 1))
        if len(children) > MAX_BODY_NODES:
            raise ScanError(f"{func.name}: function body exceeds {MAX_BODY_NODES} AST nodes")
        stack.extend(reversed(children[:MAX_BODY_NODES]))
    raise ScanError(f"{func.name}: function body exceeds {MAX_BODY_NODES} AST nodes")


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
    if _is_runner_call(expr):
        return True
    if isinstance(expr, ast.Attribute) and expr.attr == "returncode":
        val = expr.value
        return _is_runner_call(val) or (isinstance(val, ast.Name) and val.id in rc_vars)
    if isinstance(expr, ast.Subscript) and _is_zero_literal(expr.slice):
        val = expr.value
        return _is_runner_call(val) or (isinstance(val, ast.Name) and val.id in rc_vars)
    return isinstance(expr, ast.Name) and expr.id in rc_vars


def _record_target_vars(target: ast.AST, rc_vars: set[str]) -> None:
    """Record variable names bound to a runner call result or return code."""
    if isinstance(target, ast.Name):
        rc_vars.add(target.id)
    elif isinstance(target, (ast.Tuple, ast.List)) and target.elts:
        if len(target.elts) > MAX_ASSIGN_TARGETS:
            raise ScanError(f"assignment target unpack exceeds {MAX_ASSIGN_TARGETS} elements")
        elts = target.elts[:MAX_ASSIGN_TARGETS]
        named = [e.id for e in elts if isinstance(e, ast.Name) and e.id in RC_NAMES]
        if named:
            rc_vars.update(named)
        elif isinstance(elts[0], ast.Name):
            rc_vars.add(elts[0].id)


def _collect_rc_vars(nodes: list[ast.AST]) -> set[str]:
    """Return local variable names bound to a runner call result or return code."""
    rc_vars: set[str] = set()
    # len(nodes) <= MAX_BODY_NODES is enforced by _body_nodes at line 143.
    for node in nodes[:MAX_BODY_NODES]:
        if isinstance(node, ast.Assign) and _is_rc_expr(node.value, rc_vars):
            if len(node.targets) > MAX_ASSIGN_TARGETS:
                raise ScanError(f"assignment exceeds {MAX_ASSIGN_TARGETS} targets")
            for target in node.targets[:MAX_ASSIGN_TARGETS]:
                _record_target_vars(target, rc_vars)
        elif (
            isinstance(node, (ast.AnnAssign, ast.NamedExpr))
            and node.value is not None
            and _is_rc_expr(node.value, rc_vars)
        ):
            _record_target_vars(node.target, rc_vars)
    return rc_vars


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
    if len(call.keywords) > MAX_CALL_ARGS:
        raise ScanError(f"assertion call exceeds {MAX_CALL_ARGS} keyword arguments")
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


def _escape_status(func: FuncNode, lines: list[str]) -> str:
    """Return 'valid', 'short', or 'none' for an `# exit-code-only:` comment."""
    if len(func.decorator_list) > MAX_DECORATORS:
        raise ScanError(f"{func.name}: function exceeds {MAX_DECORATORS} decorators")
    candidate_lines = [func.lineno - 1, func.lineno]
    if func.decorator_list:
        first_dec = min(d.lineno for d in func.decorator_list[:MAX_DECORATORS])
        candidate_lines.extend([first_dec - 1, first_dec])
    # len(candidate_lines) <= 4 <= MAX_DECORATORS.
    for line_no in sorted(set(candidate_lines))[:MAX_DECORATORS]:
        if 1 <= line_no <= len(lines):
            match = ESCAPE_RE.search(lines[line_no - 1])
            if match is not None:
                reason = (match.group(1) or "").strip()
                return "valid" if len(reason) >= MIN_REASON_LENGTH else "short"
    return "none"


def _check_test_function(func: FuncNode, lines: list[str], rel_path: str) -> Finding | None:
    """Inspect one `def test_*` node and return a Finding when weak or unescaped."""
    escape = _escape_status(func, lines)
    if escape == "short":
        return Finding(
            rel_path,
            func.lineno,
            func.name,
            f"# exit-code-only: reason must be at least {MIN_REASON_LENGTH} characters",
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
            func.name,
            "runs a command and asserts nothing",
        )
    if all(k == "rc_zero" for k in kinds):
        return Finding(
            rel_path,
            func.lineno,
            func.name,
            "asserts only exit code 0",
        )
    return None


def _statement_start(node: ast.stmt) -> int:
    """Return the first decorator or statement line."""
    if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
        return min((dec.lineno for dec in node.decorator_list[:MAX_AST_NODES]), default=node.lineno)
    return node.lineno


def _bounded_children(node: ast.AST, end: int, rel_path: str) -> list[tuple[ast.AST, int]]:
    """Assign each statement its next sibling's start or its parent's end."""
    children = list(itertools.islice(ast.iter_child_nodes(node), MAX_AST_NODES + 1))
    if len(children) > MAX_AST_NODES:
        raise ScanError(f"{rel_path}: exceeds {MAX_AST_NODES} AST nodes")
    bounds = dict.fromkeys(children, end)
    for _, value in ast.iter_fields(node):
        if isinstance(value, list):
            statements = [item for item in value if isinstance(item, ast.stmt)]
            for current, following in itertools.pairwise(statements):
                bounds[current] = _statement_start(following) - 1
    return [(child, bounds[child]) for child in children]


def _extract_test_functions(
    tree: ast.AST, rel_path: str, file_end: int
) -> list[tuple[FuncNode, tuple[DefNode, ...], int]]:
    """Return test functions with their enclosing definitions in source order."""
    stack: list[tuple[ast.AST, tuple[DefNode, ...], int]] = [(tree, (), file_end)]
    funcs: list[tuple[FuncNode, tuple[DefNode, ...], int]] = []
    for _ in range(MAX_AST_NODES + 1):
        if not stack:
            funcs.sort(key=lambda item: (item[0].lineno, item[0].col_offset))
            return funcs
        curr, scope_chain, end = stack.pop()
        if isinstance(curr, (ast.FunctionDef, ast.AsyncFunctionDef)) and curr.name.startswith(
            "test_"
        ):
            funcs.append((curr, scope_chain, end))
        next_chain = (
            (*scope_chain, curr)
            if isinstance(curr, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef))
            else scope_chain
        )
        children = _bounded_children(curr, end, rel_path)
        for child, child_end in reversed(children[:MAX_AST_NODES]):
            stack.append((child, next_chain, child_end))
    raise ScanError(f"{rel_path}: exceeds {MAX_AST_NODES} AST nodes")


def _definition_lines(node: DefNode) -> list[tuple[int, int]]:
    """Return the definition line and all lines occupied by its decorators."""
    if len(node.decorator_list) > MAX_DECORATORS:
        raise ScanError(f"{node.name}: definition exceeds {MAX_DECORATORS} decorators")
    return [(node.lineno, node.lineno)] + [
        (dec.lineno, dec.end_lineno or dec.lineno) for dec in node.decorator_list[:MAX_DECORATORS]
    ]


def _function_touched(
    func: FuncNode, parents: tuple[DefNode, ...], end: int, hunks: list[tuple[int, int]]
) -> bool:
    """Match a function span or an enclosing definition header against hunks."""
    start = min(line for line, _ in _definition_lines(func))
    spans = [(start, end)]
    for parent in parents[:MAX_AST_NODES]:
        spans.extend(_definition_lines(parent))
    return any(a <= end and start_line <= b for a, b in spans for start_line, end in hunks)


def file_findings(
    source: str,
    rel_path: str,
    hunks: list[tuple[int, int]] | None = None,
) -> list[Finding]:
    """Parse one Python test file and return exit-code-only findings in (changed) functions."""
    try:
        tree = ast.parse(source, filename=rel_path)
    except SyntaxError as exc:
        raise ScanError(f"{rel_path}:{exc.lineno or 1}: syntax error: {exc.msg}") from exc
    lines = source.splitlines()
    funcs = _extract_test_functions(tree, rel_path, len(lines))
    findings: list[Finding] = []
    # len(funcs) <= MAX_AST_NODES is enforced by _extract_test_functions.
    for func, parents, end in funcs[:MAX_AST_NODES]:
        if hunks is not None and not _function_touched(func, parents, end, hunks):
            continue
        finding = _check_test_function(func, lines, rel_path)
        if finding is not None:
            findings.append(finding)
    return findings


def _run_git(root: Path, args: Sequence[str]) -> TextCommandResult:
    """Run a bounded read-only git command in `root` without inherited GIT_* vars."""
    git = shutil.which("git")
    if git is None:
        raise ScanError(f"{root}: git executable not found on PATH")
    if len(os.environ) > MAX_ENV_VARS:
        raise ScanError(f"environment exceeds {MAX_ENV_VARS} variables")
    env = {
        k: v
        for k, v in itertools.islice(os.environ.items(), MAX_ENV_VARS)
        if not k.startswith("GIT_")
    }
    try:
        return run_command(
            [git, *args],
            allowed_executables=(git,),
            cwd=root,
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
            timeout_seconds=GIT_TIMEOUT_SECONDS,
        )
    except (CommandTimedOut, CommandOutputLimitExceeded, OSError, UnicodeDecodeError) as exc:
        raise ScanError(f"{root}: git {' '.join(args[:2])} failed: {exc}") from exc


def _resolve_merge_base(root: Path, ref: str) -> str:
    """Resolve `git merge-base <ref> HEAD` in `root`, raising ScanError on failure."""
    if not ref or ref.startswith("-"):
        raise ScanError(f"{root}: invalid git ref {ref!r}")
    res = _run_git(root, ["merge-base", "--end-of-options", ref, "HEAD"])
    base = res.stdout.strip()
    if res.returncode != 0 or not base:
        detail = res.stderr.strip() or "no merge base found"
        raise ScanError(f"{root}: git merge-base {ref!r} HEAD failed: {detail}")
    return base


def _consume_status_record(
    tokens: Sequence[str],
    idx: int,
    changes: dict[str, str | None],
) -> int:
    """Parse one NUL-separated `--name-status` record at `idx` and return the next index."""
    status = tokens[idx]
    if status in ALLOWED_SIMPLE_STATUSES:
        if idx + 1 >= len(tokens) or not tokens[idx + 1]:
            raise ScanError(f"malformed git diff record for status {status!r}")
        path = tokens[idx + 1]
        if status == "M":
            changes[path] = path
        elif status in ("A", "T"):
            changes[path] = None
        return idx + 2
    if status.startswith("R") and status[1:].isdigit():
        if idx + 2 >= len(tokens) or not tokens[idx + 1] or not tokens[idx + 2]:
            raise ScanError(f"malformed git diff rename record for status {status!r}")
        changes[tokens[idx + 2]] = tokens[idx + 1]
        return idx + 3
    raise ScanError(f"unsupported git diff status {status!r}")


def _parse_name_status_z(output: str) -> dict[str, str | None]:
    """Parse `git diff --name-status -z -M` output into `new_path -> old_path | None`."""
    if not output:
        return {}
    if not output.endswith("\0"):
        raise ScanError("git diff --name-status output is not NUL-terminated")
    tokens = output[:-1].split("\0", 3 * MAX_TEST_FILES + 1)
    changes: dict[str, str | None] = {}
    idx = 0
    for record_count in range(MAX_TEST_FILES + 1):
        if idx >= len(tokens):
            return changes
        if record_count >= MAX_TEST_FILES:
            raise ScanError(f"git diff exceeds {MAX_TEST_FILES} file records")
        idx = _consume_status_record(tokens, idx, changes)
    raise ScanError(f"git diff exceeds {MAX_TEST_FILES} file records")


def _add_untracked_files(
    root: Path,
    output: str,
    changes: dict[str, str | None],
) -> None:
    """Parse `git ls-files -z --others` output and record untracked paths in `changes`."""
    if not output:
        return
    if not output.endswith("\0"):
        raise ScanError(f"{root}: git ls-files output is not NUL-terminated")
    untracked_paths = output[:-1].split("\0", MAX_TEST_FILES + 1)
    if len(untracked_paths) > MAX_TEST_FILES:
        raise ScanError(f"{root}: git ls-files exceeds {MAX_TEST_FILES} untracked files")
    for untracked in untracked_paths[:MAX_TEST_FILES]:
        if untracked:
            changes.setdefault(untracked, None)


def _changed_files_by_path(
    root: Path,
    ref: str,
    discovered_rels: frozenset[str],
    diff_pathspecs: Sequence[str],
) -> tuple[str, dict[str, str | None]]:
    """Return `(merge_base, {rel_path: old_rel_path | None})` for changed and untracked files."""
    if len(diff_pathspecs) > MAX_TEST_FILES:
        raise ScanError(f"{root}: exceeds {MAX_TEST_FILES} test files")
    merge_base = _resolve_merge_base(root, ref)
    diff_res = _run_git(
        root,
        [
            "diff",
            "--name-status",
            "-z",
            "-M",
            "--no-color",
            "--no-ext-diff",
            merge_base,
            "--",
            *diff_pathspecs[:MAX_TEST_FILES],
        ],
    )
    if diff_res.returncode != 0:
        raise ScanError(f"{root}: git diff against {ref} failed: {diff_res.stderr.strip()}")
    changes = _parse_name_status_z(diff_res.stdout)
    others = _run_git(
        root,
        [
            "ls-files",
            "-z",
            "--others",
            "--exclude-standard",
            "--",
            *diff_pathspecs[:MAX_TEST_FILES],
        ],
    )
    if others.returncode != 0:
        raise ScanError(f"{root}: git ls-files failed: {others.stderr.strip()}")
    _add_untracked_files(root, others.stdout, changes)
    # len(changes) <= 2 * MAX_TEST_FILES is enforced by _parse_name_status_z and _add_untracked_files.
    for changed_path in list(changes)[: 2 * MAX_TEST_FILES]:
        if changed_path.endswith(".py") and changed_path not in discovered_rels:
            raise ScanError(
                f"{changed_path}: changed or untracked Python path is not in discovered test files"
            )
    return merge_base, changes


def _changed_hunks(
    root: Path, merge_base: str, old_path: str, new_path: str
) -> list[tuple[int, int]]:
    """Read new-side zero-context hunk ranges for one changed or renamed file."""
    res = _run_git(
        root,
        [
            "diff",
            "--unified=0",
            "--no-color",
            "--no-ext-diff",
            "--no-textconv",
            "--text",
            "-M",
            merge_base,
            "--",
            old_path,
            new_path,
        ],
    )
    if res.returncode != 0:
        raise ScanError(f"{root}: git diff hunks for {new_path} failed: {res.stderr.strip()}")
    lines = res.stdout.splitlines()
    if len(lines) > MAX_AST_NODES:
        raise ScanError(f"{new_path}: diff exceeds {MAX_AST_NODES} lines")
    hunks: list[tuple[int, int]] = []
    for line in lines[:MAX_AST_NODES]:
        if line.startswith("@@"):
            match = HUNK_RE.fullmatch(line)
            if match is None:
                raise ScanError(f"{new_path}: malformed git diff hunk header")
            start = int(match.group(1))
            count = int(match.group(2)) if match.group(2) is not None else 1
            hunks.append((start, start + count - 1 if count else start + 1))
    return hunks


def _normalise_explicit_path(root: Path, root_resolved: Path, raw: str) -> tuple[Path, str]:
    """Resolve one explicit path argument against `root` into `(abs_path, rel_posix)`."""
    candidate = Path(raw)
    resolved = (candidate if candidate.is_absolute() else (root_resolved / candidate)).resolve()
    try:
        rel_path = resolved.relative_to(root_resolved)
    except ValueError as exc:
        raise ScanError(f"{raw}: path is outside repository root {root}") from exc
    rel_posix = rel_path.as_posix()
    if rel_posix == ".":
        raise ScanError(f"{raw}: not a regular file")
    return resolved, rel_posix


def discover_test_files(root: Path, explicit_paths: Sequence[str]) -> list[tuple[Path, str]]:
    """Return `(abs_path, rel_posix)` pairs for test files under `scripts/ci/tests/`."""
    root_resolved = root.resolve()
    if explicit_paths:
        if len(explicit_paths) > MAX_TEST_FILES:
            raise ScanError(f"{root}: exceeds {MAX_TEST_FILES} test files")
        return [
            _normalise_explicit_path(root, root_resolved, raw)
            for raw in explicit_paths[:MAX_TEST_FILES]
        ]
    tests_dir = root_resolved / TESTS_REL_DIR
    if not tests_dir.is_dir():
        raise ScanError(f"{root / TESTS_REL_DIR}: test directory not found")
    files = sorted(tests_dir.glob("test_*.py"))
    if len(files) > MAX_TEST_FILES:
        raise ScanError(f"{root / TESTS_REL_DIR}: exceeds {MAX_TEST_FILES} test files")
    return [(path, path.relative_to(root_resolved).as_posix()) for path in files[:MAX_TEST_FILES]]


def scan_repository(
    root: Path,
    changed_against: str | None = None,
    explicit_paths: Sequence[str] = (),
) -> list[Finding]:
    """Scan test files under `root` and return all applicable findings."""
    files = discover_test_files(root, explicit_paths)
    rel_paths = [rel for _, rel in files]
    diff_pathspecs: Sequence[str] = rel_paths if explicit_paths else (TESTS_REL_DIR.as_posix(),)
    merge_base = ""
    changes: dict[str, str | None] | None = None
    if changed_against is not None:
        merge_base, changes = _changed_files_by_path(
            root,
            changed_against,
            frozenset(rel_paths),
            diff_pathspecs,
        )
    findings: list[Finding] = []
    # len(files) <= MAX_TEST_FILES is enforced by discover_test_files.
    for full_path, rel_path in files[:MAX_TEST_FILES]:
        source = read_bounded_source(full_path, rel_path)
        if changes is None:
            current = file_findings(source, rel_path)
        elif rel_path not in changes:
            file_findings(source, rel_path)
            continue
        else:
            old_rel = changes[rel_path]
            hunks = _changed_hunks(root, merge_base, old_rel, rel_path) if old_rel else None
            current = file_findings(source, rel_path, hunks)
        if len(findings) + len(current) > MAX_AST_NODES:
            raise ScanError(f"{root}: exceeds {MAX_AST_NODES} findings")
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
