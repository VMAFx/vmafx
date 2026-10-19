#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Python tests run the vmaf under test, never a host install.

``scripts/lib/vmaftest.py`` is the one resolver of the ``vmaf`` CLI for
Python tests: ``VMAF_BIN``, ``VMAF_BIN_FOR_TESTS``, then the repository's
build directories. This contract scans every Python test file and test
helper of the suites in ``.github/test-suites.json`` and fails on a
``which("vmaf")`` call (a ``PATH`` lookup) and on the installed path under
``/usr/local/bin`` outside :data:`ALLOWED`, the files that name that path as
data for a tool's own discovery and never run it.
"""

from __future__ import annotations

import ast
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.ci import suite_registry as registry_lib  # noqa: E402

# Spelled in parts so this file does not name the path it looks for.
HOST_INSTALL = "/".join(("", "usr", "local", "bin", "vmaf"))
HOST_INSTALL_RE = re.compile(re.escape(HOST_INSTALL) + r"x?(?![\w-])")
VMAF_NAMES = frozenset({"vmaf", "vmafx", "vmaf.exe", "vmafx.exe"})
TEST_DIRS = frozenset({"test", "tests"})

# File -> why it may name the installed path. Each entry passes the path as
# data to a fake runner or to the discovery code of a shipped tool (which
# serves users and keeps its installed-binary candidate); none executes it.
ALLOWED = {
    "ai/tests/test_extract_k150k_consistency.py": "argparse fixture of the extractor; never run",
    "mcp-server/vmaf-mcp/tests/test_coverage_round3.py": "stubbed _vmaf_binary(); calls faked",
    "mcp-server/vmaf-mcp/tests/test_coverage_round4.py": "stubbed _vmaf_binary(); calls faked",
    "mcp-server/vmaf-mcp/tests/test_p1_tools.py": "stubbed _vmaf_binary(); calls faked",
    "mcp-server/vmaf-mcp/tests/test_path_and_bench_env.py": "server discovery order under test",
    "testdata/test_run_sycl_scores.py": "argv and environment builders under test; never run",
    "tools/rc1-tester/tests/test_probe.py": "stubbed shutil.which of the shipped probe",
    "tools/vmaf-tune/tests/test_report_coverage_push.py": "value recorded in a report profile",
}


def _in_test_dir(path: str) -> bool:
    return any(part in TEST_DIRS for part in path.split("/")[:-1])


def scanned_files(root: Path = ROOT) -> list[str]:
    """Python test files and test helpers (``tests/`` modules, conftest) of every suite."""
    registry = registry_lib.load_registry(root)
    prefixes = [prefix for suite in registry.suites for prefix in suite.paths]
    return [
        path
        for path in registry_lib.tracked_files(root)
        if path.endswith(".py")
        and any(registry_lib.path_under(path, prefix) for prefix in prefixes)
        and (registry_lib.is_test_file(registry, path) or _in_test_dir(path))
        and (root / path).is_file()
    ]


def _which_target(node: ast.Call) -> str | None:
    func = node.func
    name = func.attr if isinstance(func, ast.Attribute) else getattr(func, "id", None)
    if name != "which":
        return None
    args = [*node.args, *(kw.value for kw in node.keywords if kw.arg == "cmd")]
    first = args[0] if args else None
    if isinstance(first, ast.Constant) and first.value in VMAF_NAMES:
        return str(first.value)
    return None


def which_findings(label: str, text: str) -> list[str]:
    """Each ``which("vmaf")``-style call in ``text``."""
    findings = []
    calls = [
        node for node in ast.walk(ast.parse(text, filename=label)) if isinstance(node, ast.Call)
    ]
    for call in calls:
        target = _which_target(call)
        if target is not None:
            findings.append(f"{label}:{call.lineno}: looks {target} up on PATH")
    return findings


def host_install_findings(label: str, text: str) -> list[str]:
    """Each line of ``text`` that names the installed vmaf or vmafx."""
    return [
        f"{label}:{number}: names {HOST_INSTALL}"
        for number, line in enumerate(text.splitlines(), start=1)
        if HOST_INSTALL_RE.search(line)
    ]


def _read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class TestsUseVmafUnderTest(unittest.TestCase):
    def test_no_test_looks_vmaf_up_on_path(self) -> None:
        findings = [f for path in scanned_files() for f in which_findings(path, _read(path))]
        self.assertEqual(findings, [], "use scripts/lib/vmaftest.py, which never reads PATH")

    def test_no_test_names_the_host_install_outside_the_allowed_list(self) -> None:
        findings = [
            f
            for path in scanned_files()
            if path not in ALLOWED
            for f in host_install_findings(path, _read(path))
        ]
        self.assertEqual(findings, [], "use scripts/lib/vmaftest.py, never a host install")

    def test_every_allowed_file_is_scanned_and_still_names_the_path(self) -> None:
        scanned = set(scanned_files())
        for path in ALLOWED:
            with self.subTest(path=path):
                self.assertIn(path, scanned)
                self.assertTrue(host_install_findings(path, _read(path)), "stale entry")

    def test_scan_covers_test_helpers_and_not_the_resolver(self) -> None:
        scanned = set(scanned_files())
        for helper in (
            "tools/vmaf-tune/tests/_vmaf_cli.py",
            "mcp-server/vmaf-mcp/tests/conftest.py",
            "ai/tests/test_chug_extract_features_smoke.py",
            "python/test/cuda_default_model_test.py",
            "testdata/test_sycl_4k_repeat_determinism.py",
        ):
            with self.subTest(helper=helper):
                self.assertIn(helper, scanned)
        self.assertNotIn("scripts/lib/vmaftest.py", scanned)


class PlantedDefects(unittest.TestCase):
    def test_which_lookups_are_found(self) -> None:
        planted = (
            "import shutil\n"
            "from shutil import which\n"
            "a = shutil.which('vmaf')\n"
            "b = which(cmd='vmafx')\n"
            "c = shutil.which('ffmpeg')\n"
            "d = shutil.which(name)\n"
        )
        self.assertEqual(
            which_findings("planted.py", planted),
            ["planted.py:3: looks vmaf up on PATH", "planted.py:4: looks vmafx up on PATH"],
        )

    def test_host_install_is_found_and_other_names_are_not(self) -> None:
        planted = "\n".join(
            (
                f'BIN = "{HOST_INSTALL}"',
                f'ALIAS = Path("{HOST_INSTALL}x")',
                f'TUNE = "{HOST_INSTALL}-tune"',
                '"/usr/local/bin"',
            )
        )
        self.assertEqual(
            host_install_findings("planted.py", planted),
            [f"planted.py:1: names {HOST_INSTALL}", f"planted.py:2: names {HOST_INSTALL}"],
        )


if __name__ == "__main__":
    unittest.main()
