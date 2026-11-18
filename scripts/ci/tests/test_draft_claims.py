# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise numeric claim checking with positive and planted negative cases."""

from __future__ import annotations

import importlib.util
import io
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from typing import NamedTuple

CHECKER = Path(__file__).resolve().parents[1] / "draft_claims.py"
SPEC = importlib.util.spec_from_file_location("draft_claims", CHECKER)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
EVIDENCE = "suite 28 of 28, GCC 16.2.1, size 576x324, sha cf677baa"
LIMIT = 1024 * 1024


class Result(NamedTuple):
    returncode: int
    stdout: str
    stderr: str


class DraftClaimsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def file(self, name: str, content: str) -> Path:
        path = self.root / name
        path.write_text(content, encoding="utf-8")
        return path

    def run_checker(self, *paths: Path) -> Result:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = MODULE.main([str(CHECKER), *(str(path) for path in paths)])
        return Result(code, stdout.getvalue(), stderr.getvalue())

    def test_matching_claims(self) -> None:
        draft = self.file("draft", "Suite passes 28 of 28 on GCC 16.2.1 at 576x324 vs cf677baa.")
        result = self.run_checker(draft, self.file("evidence", EVIDENCE))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_planted_missing_number(self) -> None:
        draft = self.file("draft", "Suite passes 29 of 29 on GCC 16.2.1.")
        result = self.run_checker(draft, self.file("evidence", EVIDENCE))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(
            result.stdout.splitlines(), ["missing in evidence: 29", "draft tokens 2, missing 1"]
        )

    def test_token_boundaries(self) -> None:
        draft = self.file("draft", "-1,796,403,056 576x324 [link](https://example.test/98765) 7")
        evidence = self.file("evidence", "-1796403056 src01_hrc00_576x324.yuv")
        result = self.run_checker(draft, evidence)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("draft tokens 2, missing 0", result.stdout)

    def test_multiple_evidence_files(self) -> None:
        draft = self.file("draft", "28")
        result = self.run_checker(draft, self.file("first", "nothing"), self.file("second", "28"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_claims_after_100000_characters_are_checked(self) -> None:
        draft = self.file("draft", " " * 100_000 + "29")
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 29", result.stdout)

    def test_exact_size_limit_is_allowed(self) -> None:
        draft = self.file("draft", " " * (LIMIT - 2) + "28")
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_evidence(self) -> None:
        missing = self.root / "missing"
        result = self.run_checker(self.file("draft", "28"), missing)
        self.assertEqual(result.returncode, 2)
        self.assertIn(str(missing), result.stdout + result.stderr)

    def test_size_limit(self) -> None:
        for name in ("draft", "evidence"):
            with self.subTest(name=name):
                draft = self.file("draft", "x" * (LIMIT + 1) if name == "draft" else "28")
                evidence = self.file("evidence", "x" * (LIMIT + 1) if name == "evidence" else "28")
                result = self.run_checker(draft, evidence)
                self.assertEqual(result.returncode, 2)
                self.assertIn(str(self.root / name), result.stdout + result.stderr)

    def test_no_arguments(self) -> None:
        self.assertEqual(self.run_checker().returncode, 2)


if __name__ == "__main__":
    unittest.main()
