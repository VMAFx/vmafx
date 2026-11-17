#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract and unit tests for symbolic-link fixture probes in scripts/ci/tests."""

from __future__ import annotations

import ast
import errno
import io
import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import TypeAlias
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci.tests import symlink_test_support as links
from scripts.lib.safe_subprocess import TextCommandResult
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
TESTS_DIR = Path(__file__).resolve().parent
BASH = shutil.which("bash")
PLANTED_REASON = "planted: volume rejects links"
SHELL_PROBE_SNIPPET = 'source scripts/lib/symlink-probe.sh; symlink_probe "$1"'
SUBPROCESS_TIMEOUT_S = 30.0

FuncNode: TypeAlias = ast.FunctionDef | ast.AsyncFunctionDef

_SHELL_COMMENT_RE = re.compile(r"(?:^|\s)#.*$")
_SHELL_LN_S_RE = re.compile(r"(?:^|[;&|(\s/])ln(?:\s+-[A-Za-z]*)*\s+-[A-Za-z]*s[A-Za-z]*(?:\s|$)")
_SHELL_PROBE_RE = re.compile(r"(?:^|[;&|(\s])symlink_probe\s")
_SHELL_IF_OPEN_RE = re.compile(r"^if(?:$|[\s;|&()<>])")
_SHELL_ELIF_RE = re.compile(r"^elif(?:$|[\s;|&()<>])")
_SHELL_ELSE_RE = re.compile(r"^else(?:$|[\s;|&()<>])")
_SHELL_START_FI_RE = re.compile(r"^fi(?:$|[\s;|&()<>])")
_SHELL_END_FI_RE = re.compile(r";\s*fi$")


def _is_link_call(node: ast.Call) -> bool:
    func = node.func
    if not isinstance(func, ast.Attribute):
        return False
    if func.attr == "symlink_to":
        return True
    return func.attr == "symlink" and isinstance(func.value, ast.Name) and func.value.id == "os"


def _is_skip_call(node: ast.Call) -> bool:
    func = node.func
    if isinstance(func, ast.Name):
        return func.id == "skip_without_symlinks"
    return isinstance(func, ast.Attribute) and func.attr == "skip_without_symlinks"


def _innermost_function(
    node: ast.AST,
    parent_of: dict[ast.AST, ast.AST],
    body_roots: dict[ast.AST, FuncNode],
) -> FuncNode | None:
    current = node
    for _ in range(len(parent_of) + 1):
        if current in body_roots:
            return body_roots[current]
        parent = parent_of.get(current)
        if parent is None:
            return None
        current = parent
    return None


def _ast_index(
    tree: ast.AST,
) -> tuple[list[ast.Call], dict[ast.AST, ast.AST], dict[ast.AST, FuncNode]]:
    calls: list[ast.Call] = []
    parent_of: dict[ast.AST, ast.AST] = {}
    body_roots: dict[ast.AST, FuncNode] = {}
    for node in ast.walk(tree):
        for child in ast.iter_child_nodes(node):
            parent_of[child] = node
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            for stmt in node.body:
                body_roots[stmt] = node
        elif isinstance(node, ast.Call):
            calls.append(node)
    return calls, parent_of, body_roots


def unguarded_python_links(source: str, name: str) -> list[str]:
    """Return findings for ``symlink_to`` / ``os.symlink`` calls without an earlier skip."""
    calls, parent_of, body_roots = _ast_index(ast.parse(source, filename=name))
    earliest_skip: dict[FuncNode, int] = {}
    link_calls: list[tuple[int, int, FuncNode | None]] = []
    for call in calls:
        owner = _innermost_function(call, parent_of, body_roots)
        if _is_skip_call(call) and owner is not None:
            earliest_skip[owner] = min(earliest_skip.get(owner, call.lineno), call.lineno)
        if _is_link_call(call):
            link_calls.append((call.lineno, call.col_offset, owner))

    findings: list[str] = []
    for lineno, _col, owner in sorted(link_calls):
        if owner is None or earliest_skip.get(owner, lineno) >= lineno:
            findings.append(f"{name}:{lineno}: symlink fixture without links.skip_without_symlinks")
    return findings


def _update_shell_if_stack_head(stripped: str, stack: list[bool]) -> None:
    has_probe = bool(_SHELL_PROBE_RE.search(stripped))
    if _SHELL_IF_OPEN_RE.match(stripped):
        stack.append(has_probe)
    elif _SHELL_ELIF_RE.match(stripped):
        if stack:
            stack[-1] = has_probe
    elif _SHELL_ELSE_RE.match(stripped):
        if stack:
            stack[-1] = False
    elif _SHELL_START_FI_RE.match(stripped) and stack:
        stack.pop()


def unguarded_shell_links(text: str, name: str) -> list[str]:
    """Return findings for shell ``ln -s*`` lines outside a ``symlink_probe`` ``then`` branch.

    An ``ln -s*`` line counts as guarded only while inside the ``then`` branch
    of an ``if`` (or ``elif``) whose condition calls ``symlink_probe``.
    """
    stack: list[bool] = []
    findings: list[str] = []
    for lineno, raw in enumerate(text.splitlines(), start=1):
        stripped = _SHELL_COMMENT_RE.sub("", raw).strip()
        if not stripped:
            continue
        _update_shell_if_stack_head(stripped, stack)
        if _SHELL_LN_S_RE.search(stripped) and not any(stack):
            findings.append(f"{name}:{lineno}: shell symlink fixture without symlink_probe")
        if _SHELL_END_FI_RE.search(stripped) and stack:
            stack.pop()
    return findings


def _run_badge_fixture(qualname: str) -> unittest.TestResult:
    suite = unittest.defaultTestLoader.loadTestsFromName(
        f"scripts.ci.tests.test_readme_badges.{qualname}"
    )
    return unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)


def _run_shell_probe(target: Path, env: dict[str, str] | None = None) -> TextCommandResult:
    if BASH is None:
        raise unittest.SkipTest("bash not on PATH")
    return run_command(
        [BASH, "-c", SHELL_PROBE_SNIPPET, "_", str(target)],
        allowed_executables=(BASH,),
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        timeout_seconds=SUBPROCESS_TIMEOUT_S,
    )


class RepositorySymlinkGuardTest(unittest.TestCase):
    """Every Python and shell test in scripts/ci/tests guards symbolic-link fixtures."""

    def test_repository_python_tests_have_no_unguarded_links(self) -> None:
        files = sorted(TESTS_DIR.glob("test_*.py"))
        self.assertTrue(files)
        findings: list[str] = []
        for path in files:
            rel = path.relative_to(ROOT).as_posix()
            findings.extend(unguarded_python_links(path.read_text(encoding="utf-8"), rel))
        self.assertEqual(findings, [])

    def test_repository_shell_tests_have_no_unguarded_links(self) -> None:
        files = sorted(TESTS_DIR.glob("test-*.sh"))
        self.assertTrue(files)
        findings: list[str] = []
        for path in files:
            rel = path.relative_to(ROOT).as_posix()
            findings.extend(unguarded_shell_links(path.read_text(encoding="utf-8"), rel))
        self.assertEqual(findings, [])


class ScannerContractTest(unittest.TestCase):
    """Positive, negative and boundary cases for the Python and shell link scanners."""

    def test_python_link_without_skip_is_reported(self) -> None:
        src = "def f(p, q):\n    p.symlink_to(q)\n"
        self.assertEqual(
            unguarded_python_links(src, "sample.py"),
            ["sample.py:2: symlink fixture without links.skip_without_symlinks"],
        )

    def test_python_skip_before_link_passes(self) -> None:
        src = (
            "def f(d, p, q):\n    links.skip_without_symlinks(d)\n    p.symlink_to(q)\n"
            "async def g(d, p, q):\n    skip_without_symlinks(d)\n    p.symlink_to(q)\n"
        )
        self.assertEqual(unguarded_python_links(src, "sample.py"), [])

    def test_python_link_before_skip_is_reported(self) -> None:
        src = "def f(d, p, q):\n    p.symlink_to(q)\n    links.skip_without_symlinks(d)\n"
        self.assertEqual(
            unguarded_python_links(src, "sample.py"),
            ["sample.py:2: symlink fixture without links.skip_without_symlinks"],
        )

    def test_python_skip_only_in_outer_function_reports_nested_helper(self) -> None:
        src = (
            "def outer(d, p, q):\n"
            "    links.skip_without_symlinks(d)\n"
            "    def inner():\n"
            "        p.symlink_to(q)\n"
        )
        self.assertEqual(
            unguarded_python_links(src, "sample.py"),
            ["sample.py:4: symlink fixture without links.skip_without_symlinks"],
        )

    def test_python_os_symlink_is_detected(self) -> None:
        bad = "def f(a, b):\n    os.symlink(a, b)\n"
        good = "def f(d, a, b):\n    links.skip_without_symlinks(d)\n    os.symlink(a, b)\n"
        self.assertEqual(
            unguarded_python_links(bad, "sample.py"),
            ["sample.py:2: symlink fixture without links.skip_without_symlinks"],
        )
        self.assertEqual(unguarded_python_links(good, "sample.py"), [])

    def test_python_module_level_link_is_reported(self) -> None:
        src = "p.symlink_to(q)\n"
        self.assertEqual(
            unguarded_python_links(src, "sample.py"),
            ["sample.py:1: symlink fixture without links.skip_without_symlinks"],
        )

    def test_shell_ln_sf_without_probe_is_reported(self) -> None:
        text = "ln -sf target link\nln -sfn target2 link2\n"
        self.assertEqual(
            unguarded_shell_links(text, "sample.sh"),
            [
                "sample.sh:1: shell symlink fixture without symlink_probe",
                "sample.sh:2: shell symlink fixture without symlink_probe",
            ],
        )

    def test_shell_probe_before_ln_s_passes(self) -> None:
        text = 'if reason=$(symlink_probe "$d"); then\n  ln -s target link\nfi\n'
        self.assertEqual(unguarded_shell_links(text, "sample.sh"), [])

    def test_shell_probe_after_ln_s_is_reported(self) -> None:
        text = 'ln -s target link\nsymlink_probe "$d"\n'
        self.assertEqual(
            unguarded_shell_links(text, "sample.sh"),
            ["sample.sh:1: shell symlink fixture without symlink_probe"],
        )

    def test_shell_ln_s_inside_comment_passes(self) -> None:
        text = "# ln -s target link\necho ok # ln -sf target link\n"
        self.assertEqual(unguarded_shell_links(text, "sample.sh"), [])

    def test_shell_plain_hard_link_passes(self) -> None:
        text = "ln target hardlink\nln -f target hardlink2\n"
        self.assertEqual(unguarded_shell_links(text, "sample.sh"), [])

    def test_shell_ln_s_after_closed_probe_block_is_reported(self) -> None:
        text = 'if reason=$(symlink_probe "$d"); then\n  ln -s target link\nfi\nln -s a b\n'
        self.assertEqual(
            unguarded_shell_links(text, "sample.sh"),
            ["sample.sh:4: shell symlink fixture without symlink_probe"],
        )

    def test_shell_ln_s_in_else_branch_of_probe_block_is_reported(self) -> None:
        text = 'if reason=$(symlink_probe "$d"); then\n  echo ok\nelse\n  ln -s a b\nfi\n'
        self.assertEqual(
            unguarded_shell_links(text, "sample.sh"),
            ["sample.sh:4: shell symlink fixture without symlink_probe"],
        )

    def test_shell_ln_s_in_nested_if_inside_probe_then_passes(self) -> None:
        text = (
            'if reason=$(symlink_probe "$d"); then\n  if [ -d x ]; then\n    ln -s a b\n  fi\nfi\n'
        )
        self.assertEqual(unguarded_shell_links(text, "sample.sh"), [])

    def test_shell_middle_unguarded_block_between_probe_blocks_is_reported(self) -> None:
        text = (
            'if reason=$(symlink_probe "$d1"); then\n'
            "  ln -s a1 b1\n"
            "fi\n"
            "if true; then\n"
            "  ln -s a2 b2\n"
            "fi\n"
            'if reason=$(symlink_probe "$d3"); then\n'
            "  ln -s a3 b3\n"
            "fi\n"
        )
        self.assertEqual(
            unguarded_shell_links(text, "sample.sh"),
            ["sample.sh:5: shell symlink fixture without symlink_probe"],
        )

    def test_shell_bare_probe_statement_before_ln_s_is_reported(self) -> None:
        text = 'symlink_probe "$d"\nln -s a b\n'
        self.assertEqual(
            unguarded_shell_links(text, "sample.sh"),
            ["sample.sh:2: shell symlink fixture without symlink_probe"],
        )

    def test_shell_one_line_probe_if_then_ln_s_fi_passes(self) -> None:
        text = 'if reason=$(symlink_probe "$d"); then ln -s a b; fi\n'
        self.assertEqual(unguarded_shell_links(text, "sample.sh"), [])

    def test_shell_extra_fi_on_empty_stack_then_ln_s_is_reported(self) -> None:
        text = "fi\nln -s a b\n"
        self.assertEqual(
            unguarded_shell_links(text, "sample.sh"),
            ["sample.sh:2: shell symlink fixture without symlink_probe"],
        )

    def test_shell_elif_and_inline_fi_update_stack(self) -> None:
        cleared = 'if reason=$(symlink_probe "$d"); then\n  :\nelif true; then\n  ln -s a b\nfi\n'
        guarded = 'if false; then\n  :\nelif reason=$(symlink_probe "$d"); then\n  ln -s a b\nfi\n'
        inline_closed = 'if reason=$(symlink_probe "$d"); then ln -s a b;fi\nln -s c d\n'
        self.assertEqual(
            unguarded_shell_links(cleared, "sample.sh"),
            ["sample.sh:4: shell symlink fixture without symlink_probe"],
        )
        self.assertEqual(unguarded_shell_links(guarded, "sample.sh"), [])
        self.assertEqual(
            unguarded_shell_links(inline_closed, "sample.sh"),
            ["sample.sh:2: shell symlink fixture without symlink_probe"],
        )


class SymlinkProbeSupportTest(unittest.TestCase):
    """Runtime checks for Python and shell probes plus fixture skip replay."""

    def test_forced_false_probe_returns_error_and_raises_skip(self) -> None:
        planted = OSError(errno.EINVAL, PLANTED_REASON)
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            with mock.patch("os.symlink", side_effect=planted):
                self.assertIs(links.symlink_probe_error(tmp), planted)
                with self.assertRaises(unittest.SkipTest) as ctx:
                    links.skip_without_symlinks(tmp)
            self.assertIn(str(tmp), str(ctx.exception))
            self.assertIn(PLANTED_REASON, str(ctx.exception))
            self.assertEqual(list(tmp.iterdir()), [])

    def test_real_directory_probe_succeeds_and_cleans_up(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            links.skip_without_symlinks(tmp)
            self.assertIsNone(links.symlink_probe_error(tmp))
            links.skip_without_symlinks(tmp)
            self.assertEqual(list(tmp.iterdir()), [])

    def test_forced_false_probe_skips_image_fault_fixture(self) -> None:
        planted = OSError(errno.EINVAL, PLANTED_REASON)
        with mock.patch("os.symlink", side_effect=planted):
            result = _run_badge_fixture(
                "ImageFault.test_a_link_out_of_the_readme_directory_is_a_fault"
            )
        self.assertEqual(result.errors, [])
        self.assertEqual(result.failures, [])
        self.assertEqual(len(result.skipped), 1)
        self.assertIn(PLANTED_REASON, result.skipped[0][1])

    def test_forced_false_probe_skips_only_link_subtest_in_collect(self) -> None:
        planted = OSError(errno.EINVAL, PLANTED_REASON)
        with mock.patch("os.symlink", side_effect=planted):
            result = _run_badge_fixture(
                "Collect.test_a_symlink_a_large_file_or_a_non_svg_is_refused"
            )
        self.assertEqual(result.errors, [])
        self.assertEqual(result.failures, [])
        self.assertEqual(len(result.skipped), 1)

    def test_true_probe_runs_image_fault_fixture_without_skips(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            links.skip_without_symlinks(Path(tmp_dir))
        result = _run_badge_fixture("ImageFault.test_a_link_out_of_the_readme_directory_is_a_fault")
        self.assertTrue(result.wasSuccessful())
        self.assertEqual(result.skipped, [])
        self.assertEqual(result.errors, [])
        self.assertEqual(result.failures, [])

    def test_mutant_dropping_skip_errors_under_forced_false_probe(self) -> None:
        planted = OSError(errno.EINVAL, PLANTED_REASON)
        with (
            mock.patch("os.symlink", side_effect=planted),
            mock.patch.object(links, "skip_without_symlinks", lambda directory: None),
        ):
            result = _run_badge_fixture(
                "ImageFault.test_a_link_out_of_the_readme_directory_is_a_fault"
            )
        self.assertEqual(result.skipped, [])
        self.assertEqual(result.failures, [])
        self.assertEqual(len(result.errors), 1)
        self.assertIn("OSError", result.errors[0][1])
        self.assertIn(PLANTED_REASON, result.errors[0][1])

    def test_shell_probe_real_directory_succeeds_and_cleans_up(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp = Path(tmp_dir)
            links.skip_without_symlinks(tmp)
            proc = _run_shell_probe(tmp)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(proc.stdout, "")
            self.assertEqual(list(tmp.iterdir()), [])

    def test_shell_probe_shim_refusal_exits_nonzero_with_reason(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            base = Path(tmp_dir)
            shim, target = base / "shim", base / "target"
            shim.mkdir()
            target.mkdir()
            ln_script = shim / "ln"
            ln_script.write_text(
                "#!/usr/bin/env bash\necho 'ln: planted refusal' >&2\nexit 1\n",
                encoding="utf-8",
            )
            ln_script.chmod(0o755)
            env = {**os.environ, "PATH": f"{shim}{os.pathsep}{os.environ.get('PATH', os.defpath)}"}
            proc = _run_shell_probe(target, env=env)
            self.assertEqual(proc.returncode, 1)
            self.assertIn("ln: planted refusal", proc.stdout)
            self.assertEqual(list(target.iterdir()), [])

    def test_shell_probe_shim_exiting_zero_without_link_fails_cleanly(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            base = Path(tmp_dir)
            shim, target = base / "shim", base / "target"
            shim.mkdir()
            target.mkdir()
            ln_script = shim / "ln"
            ln_script.write_text('#!/usr/bin/env bash\n: >"$3"\nexit 0\n', encoding="utf-8")
            ln_script.chmod(0o755)
            env = {**os.environ, "PATH": f"{shim}{os.pathsep}{os.environ.get('PATH', os.defpath)}"}
            proc = _run_shell_probe(target, env=env)
            self.assertEqual(proc.returncode, 1)
            self.assertIn("ln -s created no symbolic link", proc.stdout)
            self.assertEqual(list(target.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
