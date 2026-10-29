# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for scripts/release/publish_homebrew_tap.py.

A fake transport stands for the GitHub API: it serves a tap with or without the formula,
records every call, and answers 422 where GitHub does for a branch or a pull request that
already exists. The live run against the real tap is in docs/development/distribution.md.
"""

from __future__ import annotations

import base64
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.release import publish_homebrew_tap as tap  # noqa: E402

TAP = "Example/homebrew-tap"
SIGNOFF = "Release Bot <bot@example.org>"
FORMULA = 'class Vmafx < Formula\n  url "https://example.org/archive/v1.2.3.tar.gz"\nend\n'


def encoded(text: str) -> str:
    return base64.b64encode(text.encode()).decode()


class FakeTap:
    """The GitHub API of one tap repository, as far as the script uses it."""

    def __init__(self, formula: str | None = None, branch_exists: bool = False,
                 pull_exists: bool = False) -> None:  # fmt: skip
        self.formula = formula
        self.branch_exists = branch_exists
        self.pull_exists = pull_exists
        self.calls: list[tuple[str, str, dict[str, Any] | None]] = []

    def __call__(self, method: str, path: str, body: dict[str, Any] | None) -> tuple[int, Any]:
        self.calls.append((method, path, body))
        repo = f"/repos/{TAP}"
        branch_formula = f"{repo}/contents/{tap.FORMULA_PATH}?ref=vmafx-1.2.3"
        routes: dict[tuple[str, str], tuple[int, Any]] = {
            ("GET", repo): (200, {"default_branch": "main"}),
            ("GET", f"{repo}/git/ref/heads/main"): (200, {"object": {"sha": "base"}}),
            ("GET", f"{repo}/contents/{tap.FORMULA_PATH}?ref=main"): self.formula_answer(True),
            ("GET", branch_formula): self.formula_answer(self.branch_exists),
            ("POST", f"{repo}/git/refs"): (
                (422, {"message": "Reference already exists"}) if self.branch_exists else (201, {})
            ),
            ("PATCH", f"{repo}/git/refs/heads/vmafx-1.2.3"): (200, {}),
            ("PUT", f"{repo}/contents/{tap.FORMULA_PATH}"): (201, {}),
            ("POST", f"{repo}/pulls"): (
                (422, {"message": "A pull request already exists"})
                if self.pull_exists
                else (201, {"html_url": "https://example.org/pull/1"})
            ),
            ("GET", f"{repo}/pulls?head=Example:vmafx-1.2.3&state=open"): (
                200,
                [{"html_url": "https://example.org/pull/7"}] if self.pull_exists else [],
            ),
        }
        return routes.get((method, path), (500, {"message": f"unexpected {method} {path}"}))

    def formula_answer(self, present: bool) -> tuple[int, Any]:
        if self.formula is None or not present:
            return 404, {"message": "Not Found"}
        return 200, {"content": encoded(self.formula), "sha": "blob"}

    def methods(self) -> list[str]:
        return [f"{method} {path.split('?')[0]}" for method, path, _ in self.calls]


def run(fake: FakeTap, apply: bool = True, version: str = "1.2.3",
        formula: str = FORMULA) -> str:  # fmt: skip
    return tap.publish(fake, TAP, version, formula, SIGNOFF, apply)


class PublishTests(unittest.TestCase):
    def test_new_formula_opens_a_pull_request(self) -> None:
        fake = FakeTap()
        self.assertEqual(run(fake), "opened: https://example.org/pull/1")
        put = next(body for method, _, body in fake.calls if method == "PUT")
        assert put is not None
        self.assertEqual(base64.b64decode(put["content"]).decode(), FORMULA)
        self.assertEqual(put["branch"], "vmafx-1.2.3")
        self.assertNotIn("sha", put)
        self.assertTrue(put["message"].endswith(f"\n\nSigned-off-by: {SIGNOFF}\n"))
        self.assertNotIn(f"PATCH /repos/{TAP}/git/refs/heads/vmafx-1.2.3", fake.methods())

    def test_dry_run_changes_nothing(self) -> None:
        fake = FakeTap()
        self.assertTrue(run(fake, apply=False).startswith("dry run: would branch vmafx-1.2.3"))
        self.assertEqual({call[0] for call in fake.calls}, {"GET"})

    def test_same_formula_is_unchanged(self) -> None:
        fake = FakeTap(formula=FORMULA)
        self.assertTrue(run(fake).startswith("unchanged:"))
        self.assertEqual({call[0] for call in fake.calls}, {"GET"})

    def test_older_formula_is_replaced_with_its_blob_sha(self) -> None:
        fake = FakeTap(formula=FORMULA.replace("1.2.3", "1.2.2"), branch_exists=True)
        self.assertEqual(run(fake), "opened: https://example.org/pull/1")
        self.assertIn(f"PATCH /repos/{TAP}/git/refs/heads/vmafx-1.2.3", fake.methods())
        put = next(body for method, _, body in fake.calls if method == "PUT")
        assert put is not None
        self.assertEqual(put["sha"], "blob")

    def test_existing_pull_request_is_updated(self) -> None:
        fake = FakeTap(branch_exists=True, pull_exists=True)
        self.assertEqual(run(fake), "updated: https://example.org/pull/7")

    def test_pull_request_422_without_an_open_one_fails(self) -> None:
        fake = FakeTap()

        def refuse(method: str, path: str, body: dict[str, Any] | None) -> tuple[int, Any]:
            if method == "POST" and path.endswith("/pulls"):
                return 422, {"message": "Validation Failed"}
            return fake(method, path, body)

        with self.assertRaisesRegex(tap.PublishError, "open the pull request"):
            tap.publish(refuse, TAP, "1.2.3", FORMULA, SIGNOFF, True)

    def test_http_error_names_the_step(self) -> None:
        def forbidden(method: str, path: str, body: dict[str, Any] | None) -> tuple[int, Any]:
            del method, path, body
            return 403, {"message": "Resource not accessible by integration"}

        with self.assertRaisesRegex(tap.PublishError, "read the tap: HTTP 403"):
            tap.publish(forbidden, TAP, "1.2.3", FORMULA, SIGNOFF, True)

    def test_prerelease_version_boundary(self) -> None:
        formula = FORMULA.replace("v1.2.3", "v1.0.0-rc.3")
        tap.validate(TAP, "1.0.0-rc.3", SIGNOFF, formula)

    def test_invalid_arguments_are_refused_before_any_call(self) -> None:
        cases = [
            ("not-a-tap", "1.2.3", SIGNOFF, FORMULA),
            (TAP, "v1.2.3", SIGNOFF, FORMULA),
            (TAP, "1.2", SIGNOFF, FORMULA),
            (TAP, "1.2.3", "no address", FORMULA),
            (TAP, "1.2.3", SIGNOFF, FORMULA.replace("v1.2.3", "v9.9.9")),
            (TAP, "1.2.3", SIGNOFF, "class Vmafx < Formula\nend\n"),
        ]
        for case in cases:
            fake = FakeTap()
            with self.subTest(case=case), self.assertRaises(tap.PublishError):
                tap.publish(fake, *case, True)
            self.assertEqual(fake.calls, [])


class MainTests(unittest.TestCase):
    def test_missing_token_exits_2(self) -> None:
        with tempfile.NamedTemporaryFile("w", suffix=".rb") as formula, \
                mock.patch.dict("os.environ", {"GH_TOKEN": ""}), \
                contextlib.redirect_stderr(io.StringIO()) as err:  # fmt: skip
            formula.write(FORMULA)
            formula.flush()
            rc = tap.main(["--tap", TAP, "--version", "1.2.3", "--formula", formula.name,
                           "--signoff", SIGNOFF])  # fmt: skip
        self.assertEqual(rc, 2)
        self.assertIn("GH_TOKEN is not set", err.getvalue())

    def test_missing_formula_file_exits_2(self) -> None:
        with mock.patch.dict("os.environ", {"GH_TOKEN": "t"}), \
                contextlib.redirect_stderr(io.StringIO()):  # fmt: skip
            rc = tap.main(["--tap", TAP, "--version", "1.2.3", "--formula",
                           "/nonexistent/vmafx.rb", "--signoff", SIGNOFF])  # fmt: skip
        self.assertEqual(rc, 2)

    def test_dry_run_through_main(self) -> None:
        fake = FakeTap()
        with tempfile.NamedTemporaryFile("w", suffix=".rb") as formula, \
                mock.patch.dict("os.environ", {"GH_TOKEN": "t"}), \
                mock.patch.object(tap, "github_transport", return_value=fake), \
                contextlib.redirect_stdout(io.StringIO()) as out:  # fmt: skip
            formula.write(FORMULA)
            formula.flush()
            rc = tap.main(["--tap", TAP, "--version", "1.2.3", "--formula", formula.name,
                           "--signoff", SIGNOFF])  # fmt: skip
        self.assertEqual(rc, 0)
        self.assertIn("dry run:", out.getvalue())


if __name__ == "__main__":
    unittest.main()
