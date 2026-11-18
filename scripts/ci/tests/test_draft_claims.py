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

    def test_balanced_link_target_and_numeric_label(self) -> None:
        draft = self.file("draft", "[29](https://example.test/a(b)/98765)")
        result = self.run_checker(draft, self.file("evidence", "nothing"))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(
            result.stdout.splitlines(), ["missing in evidence: 29", "draft tokens 1, missing 1"]
        )

    def test_parenthesized_prose_after_bracket_is_checked(self) -> None:
        draft = self.file("draft", "[results](see (baseline) and 29)")
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(
            result.stdout.splitlines(), ["missing in evidence: 29", "draft tokens 1, missing 1"]
        )

    def test_link_destination_with_title_is_ignored(self) -> None:
        draft = self.file(
            "draft",
            '[x](https://example.test/1234 "title 5678") '
            "[y](https://example.test/2345 'title 6789') "
            "[z](https://example.test/3456 (title 7890))",
        )
        result = self.run_checker(draft, self.file("evidence", "nothing"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("draft tokens 0, missing 0", result.stdout)

    def test_angle_link_destination_with_space_is_ignored(self) -> None:
        draft = self.file("draft", "[x](<a b 1234>)")
        result = self.run_checker(draft, self.file("evidence", "nothing"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("draft tokens 0, missing 0", result.stdout)

    def test_bare_destination_followed_by_prose_is_checked(self) -> None:
        draft = self.file("draft", "[x](target and 29)")
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 29", result.stdout)

    def test_tab_and_line_break_in_parentheses_are_checked(self) -> None:
        draft = self.file("draft", "[x](target\t29) [y](target\n31)")
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 29", result.stdout)
        self.assertIn("missing in evidence: 31", result.stdout)

    def test_unclosed_title_in_link_is_checked(self) -> None:
        draft = self.file("draft", '[x](target "open 29)')
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 29", result.stdout)

    def test_escaped_space_in_bare_destination_is_checked(self) -> None:
        draft = self.file("draft", r"[x](target\ 29)")
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 29", result.stdout)

    def test_line_break_in_angle_destination_or_title_is_checked(self) -> None:
        draft = self.file("draft", '[x](<target\n29>) [y](target "open\n31") [z](target\n"32")')
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 29", result.stdout)
        self.assertIn("missing in evidence: 31", result.stdout)
        self.assertIn("missing in evidence: 32", result.stdout)

    def test_unclosed_single_quote_and_paren_titles_are_checked(self) -> None:
        draft = self.file("draft", '[x](target \'open 29) [y](target (open 31) [z](target "32\\")')
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 29", result.stdout)
        self.assertIn("missing in evidence: 31", result.stdout)
        self.assertIn("missing in evidence: 32", result.stdout)

    def test_unclosed_or_nested_angle_destination_is_checked(self) -> None:
        draft = self.file("draft", "[x](<29) [y](<a<31>)")
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 29", result.stdout)
        self.assertIn("missing in evidence: 31", result.stdout)

    def test_escaped_closing_angle_in_destination_is_checked(self) -> None:
        draft = self.file("draft", r"[x](<29\>)")
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 29", result.stdout)

    def test_angle_destination_with_escapes_is_ignored(self) -> None:
        evidence = self.file("evidence", "28")
        for text in (
            "[x](<https://example.test/29>)",
            r"[x](<a\>b29>)",
            r"[x](<29\\>)",
        ):
            self.assertEqual(MODULE.LINK.sub("]", text), "[x]")
            result = self.run_checker(self.file("draft", text), evidence)
            self.assertEqual(result.returncode, 0, f"{text}: {result.stdout}{result.stderr}")

    def test_trailing_horizontal_whitespace_before_close_paren_is_ignored(self) -> None:
        evidence = self.file("evidence", "28")
        for text in (
            "[x](https://example.test/98765 )",
            '[x](https://example.test/98765  "t" )',
            "[x](https://example.test/98765\t)",
        ):
            result = self.run_checker(self.file("draft", text), evidence)
            self.assertEqual(result.returncode, 0, f"{text!r}: {result.stdout}{result.stderr}")

    def test_prose_between_quoted_or_parenthesized_spans_is_checked(self) -> None:
        draft = self.file("draft", '[x](target "a" 29 "b") [y](target (a) 31 (b))')
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 29", result.stdout)
        self.assertIn("missing in evidence: 31", result.stdout)

    def test_leading_whitespace_after_open_paren_is_checked(self) -> None:
        draft = self.file("draft", '[x]( 29) [y]( "31")')
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 29", result.stdout)
        self.assertIn("missing in evidence: 31", result.stdout)

    def test_escaped_parenthesis_in_link_target(self) -> None:
        draft = self.file("draft", r"[ref](https://example.test/a\)b/98765) 28")
        result = self.run_checker(draft, self.file("evidence", "28"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unterminated_link_keeps_following_number(self) -> None:
        draft = self.file("draft", "[ref](unfinished text 29")
        result = self.run_checker(draft, self.file("evidence", "nothing"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 29", result.stdout)

    def test_link_with_two_and_three_nested_levels_is_ignored(self) -> None:
        draft = self.file("draft", "[x](a(b(12345))) [y](a(b(c(67890))))")
        result = self.run_checker(draft, self.file("evidence", "nothing"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("draft tokens 0, missing 0", result.stdout)

    def test_link_beyond_three_nested_levels_stays_visible(self) -> None:
        # Four nested pairs exceed the limit: check both target and following numbers.
        draft = self.file("draft", "[ref](a(b(c(d(98765))))) 29")
        result = self.run_checker(draft, self.file("evidence", "nothing"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing in evidence: 98765", result.stdout)
        self.assertIn("missing in evidence: 29", result.stdout)

    def test_link_does_not_span_evidence_files(self) -> None:
        draft = self.file("draft", "29")
        result = self.run_checker(draft, self.file("first", "[ref]("), self.file("second", "29)"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

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

    def test_draft_only_argument(self) -> None:
        result = self.run_checker(self.file("draft", "28"))
        self.assertEqual(result.returncode, 2)
        self.assertEqual(
            result.stderr,
            "usage: draft_claims.py <draft.md> <evidence.md> [<evidence.md> ...]\n",
        )

    def test_two_file_arguments_boundary(self) -> None:
        draft = self.file("draft", "28")
        passing = self.run_checker(draft, self.file("evidence_ok", "28"))
        failing = self.run_checker(draft, self.file("evidence_miss", "29"))
        self.assertEqual(passing.returncode, 0)
        self.assertEqual(failing.returncode, 1)


if __name__ == "__main__":
    unittest.main()
