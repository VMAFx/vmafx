#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Guard dormant GPU registrations and the introspection checker contract."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parents[2]
CORE_TEST_DIR = REPO_ROOT / "core" / "test"
CHECKER_PATH = CORE_TEST_DIR / "check_gpu_test_serialization.py"

SPEC = importlib.util.spec_from_file_location("check_gpu_test_serialization", CHECKER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load checker module from {CHECKER_PATH}")
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


def _call_end(source: str, opening: int) -> int:
    depth = 0
    quote = ""
    escaped = False
    comment = False
    for index in range(opening, len(source)):  # HISS-02: scalar upper bound
        char = source[index]
        if comment:
            comment = char != "\n"
        elif quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
        elif char in {'"', "'"}:
            quote = char
        elif char == "#":
            comment = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return index
    raise ValueError(f"unterminated test() call at line {source.count(chr(10), 0, opening) + 1}")


def _test_calls(source: str) -> list[tuple[int, str]]:
    """Return top-level Meson test calls while ignoring quoted parentheses."""
    calls: list[tuple[int, str]] = []
    cursor = 0
    pattern = re.compile(r"(?m)^[ \t]*test\s*\(")
    for _ in range(len(source) // 5 + 1):  # HISS-02: scalar upper bound
        match = pattern.search(source, cursor)
        if match is None:
            break
        start = match.start()
        cursor = _call_end(source, match.end() - 1) + 1
        calls.append((source.count("\n", 0, start) + 1, source[start:cursor]))
    return calls


def _parallel_gpu_registrations(path: Path) -> list[str]:
    violations: list[str] = []
    gpu_suite = re.compile(r"suite\s*:\s*(?:\[[^]]*['\"]gpu['\"][^]]*\]|['\"]gpu['\"])")
    for line, call in _test_calls(path.read_text(encoding="utf-8")):
        if gpu_suite.search(call) is None:
            continue
        if re.search(r"is_parallel\s*:\s*false", call) is not None:
            continue
        name = re.search(r"test\s*\(\s*['\"]([^'\"]+)['\"]", call)
        violations.append(f"{path}:{line}: {name.group(1) if name else '<dynamic>'}")
    return violations


def _run_checker(metadata: Path, suite: str) -> tuple[int, str]:
    output = io.StringIO()
    argv = ["check_gpu_test_serialization.py", str(metadata), suite]
    with contextlib.redirect_stdout(output), patch.object(sys, "argv", argv):
        return CHECKER.main(), output.getvalue()


class GpuSerializationContractTest(unittest.TestCase):
    def test_all_gpu_registrations_are_exclusive(self) -> None:
        paths = (
            CORE_TEST_DIR / "meson.build",
            REPO_ROOT / "core" / "tools" / "test" / "meson.build",
        )
        violations = [item for path in paths for item in _parallel_gpu_registrations(path)]
        self.assertEqual(
            violations, [], "GPU tests missing is_parallel : false:\n" + "\n".join(violations)
        )

    def test_checker_accepts_qualified_and_bare_suite_names(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            metadata = Path(temp_dir) / "tests.json"
            metadata.write_text(
                json.dumps([{"name": "gpu", "suite": ["libvmaf:gpu"], "is_parallel": False}]),
                encoding="utf-8",
            )
            for suite in ("libvmaf:gpu", "gpu"):
                with self.subTest(suite=suite):
                    status, output = _run_checker(metadata, suite)
                    self.assertEqual(status, 0)
                    self.assertIn("1 GPU tests are registered", output)

    def test_checker_rejects_parallel_and_skips_absent_suite(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            metadata = Path(temp_dir) / "tests.json"
            metadata.write_text(
                json.dumps([{"name": "gpu", "suite": ["libvmaf:gpu"], "is_parallel": True}]),
                encoding="utf-8",
            )
            status, output = _run_checker(metadata, "gpu")
            self.assertEqual(status, 1)
            self.assertIn("gpu", output)

            metadata.write_text(
                json.dumps([{"name": "cpu", "suite": ["libvmaf:fast"]}]),
                encoding="utf-8",
            )
            status, _ = _run_checker(metadata, "gpu")
            self.assertEqual(status, 77)


if __name__ == "__main__":
    unittest.main()
