#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/ci/check_testlog_credentials.py (ADR-1333).

Run with:  python3 -m unittest scripts/ci/tests/test_check_testlog_credentials.py
"""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import check_testlog_credentials as check

# Synthetic shapes only; none is a real credential.
FAKE_GITHUB = "ghp_" + "A" * 36
FAKE_FINE_GRAINED = "github_pat_" + "B" * 30
FAKE_GATEWAY = "sk-" + "C" * 24


def _write(root: Path, relative: str, text: str) -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def _run(root: Path) -> tuple[int, str]:
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        code = check.main([str(root)])
    return code, out.getvalue()


class CheckTestlogCredentialsTest(unittest.TestCase):
    def test_planted_github_token_in_testlog_fails_without_printing_it(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "build/meson-logs/testlog.txt", f"MY_ODD_PAT={FAKE_GITHUB}\n")
            code, output = _run(root)
        self.assertEqual(code, 1)
        self.assertIn("testlog.txt: 1 line(s)", output)
        self.assertNotIn(FAKE_GITHUB, output)

    def test_every_value_shape_is_counted(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            body = f'{{"env": {{"A": "{FAKE_FINE_GRAINED}"}}}}\n{{"B": "{FAKE_GATEWAY}"}}\n'
            log = _write(root, ".workingdir/build-x/meson-logs/testlog.json", body)
            self.assertEqual(check.count_credential_lines(log), 2)

    def test_clean_logs_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "build/meson-logs/testlog.txt", "PATH=/usr/bin\nHOME=/home/x\n")
            code, output = _run(root)
        self.assertEqual(code, 0)
        self.assertEqual(output, "")

    def test_short_token_shape_is_not_a_match(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            log = _write(Path(tmp), "b/meson-logs/testlog.txt", "X=ghp_" + "A" * 35 + "\n")
            self.assertEqual(check.count_credential_lines(log), 0)

    def test_files_outside_meson_logs_and_git_are_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "docs/testlog.txt", FAKE_GITHUB)
            _write(root, ".git/meson-logs/testlog.txt", FAKE_GITHUB)
            _write(root, "build/meson-logs/other.txt", FAKE_GITHUB)
            code, _ = _run(root)
        self.assertEqual(code, 0)

    def test_unreadable_root_is_an_error_not_a_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = _write(root, "build/meson-logs/testlog.txt", "")
            original = check.MAX_LOG_BYTES
            check.MAX_LOG_BYTES = -1
            try:
                code, output = _run(root)
            finally:
                check.MAX_LOG_BYTES = original
        self.assertEqual(code, 2)
        self.assertIn(log.name, output)


if __name__ == "__main__":
    unittest.main()
