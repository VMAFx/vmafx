# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""shfmt reads every tracked shell file; what it skips is declared, dated and still needed.

A file `praetorctl adopt` refreshes only while it is praetor's own text must not be rewritten
by a formatter, so it is a declared exception in `.config/lint-exceptions.d/shfmt.toml`.
Three places skip the listed files: the `exclude` of the `shfmt-src` hook, the `SHFMT_FILES` list
of the Makefile and the shell branch of the native hook. These cases hold the three to the list,
refuse an entry that is incomplete, untracked, expired or stale (the file is shfmt-clean again),
and refuse any other shell file falling out of scope.
"""

from __future__ import annotations

import datetime as dt
import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path
from typing import Any, cast

import tomllib
import yaml  # type: ignore[import-untyped]  # PyYAML ships no stubs in the hook env, as in scripts/ci/check-helm-selector-isolation.py

ROOT = Path(__file__).resolve().parents[3]
LIST_FILE = ROOT / ".config" / "lint-exceptions.d" / "shfmt.toml"
HOOK_ID = "shfmt-src"
SHFMT_ARGS = ("-i", "2", "-ci")
MAX_DAYS = 400
NATIVE_SKIP = re.compile(r'^\s*if \[\[ "\$f" =~ (\^\S+\$) \]\]; then$', re.M)


def declared() -> list[dict[str, Any]]:
    data = tomllib.loads(LIST_FILE.read_text(encoding="utf-8"))
    return cast("list[dict[str, Any]]", data.get("exception", []))


def declared_paths() -> list[str]:
    return sorted(e["path"] for e in declared())


def hook() -> dict[str, Any]:
    config = yaml.safe_load((ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8"))
    found = [h for r in config["repos"] for h in r["hooks"] if h["id"] == HOOK_ID]
    if len(found) != 1:
        raise AssertionError(f"expected one {HOOK_ID} hook, found {len(found)}")
    return cast("dict[str, Any]", found[0])


def run_tool(exe: str, *args: str) -> int:
    """Exit status of a resolved tool with a fixed argv."""
    return subprocess.run(  # noqa: S603 -- resolved executable, fixed argv
        [exe, *args], cwd=ROOT, capture_output=True, timeout=120, check=False
    ).returncode


def tracked_shell() -> list[str]:
    git = shutil.which("git")
    if git is None:
        raise AssertionError("git is not on PATH")
    out = subprocess.run(  # noqa: S603 -- resolved git, fixed argv
        [git, "-C", str(ROOT), "ls-files", "*.sh", "*.bash"],
        capture_output=True,
        text=True,
        check=True,
        timeout=60,
    ).stdout
    return out.split()


def today() -> dt.date:
    override = os.environ.get("LINT_EXCEPTIONS_TODAY")
    return dt.date.fromisoformat(override) if override else dt.date.today()


def anchored(path: str) -> str:
    """The bash and pre-commit spelling of "exactly this path": dots escaped, both anchors."""
    return "^" + path.replace(".", r"\.") + "$"


class ShfmtScopeTests(unittest.TestCase):
    def test_hook_has_no_files_filter(self) -> None:
        self.assertNotIn("files", hook(), "the shfmt hook is scoped by a files: regex")

    def test_hook_exclude_is_exactly_the_declared_files(self) -> None:
        exclude = re.compile(hook().get("exclude", r"\A(?!)"))
        excluded = sorted(p for p in tracked_shell() if exclude.search(p))
        self.assertEqual(excluded, declared_paths())

    def test_hook_exclude_names_whole_paths(self) -> None:
        # An unanchored alternative would also skip a file that only contains the path.
        alternatives = hook().get("exclude", "").split("|") if declared() else []
        self.assertEqual(sorted(alternatives), sorted(anchored(p) for p in declared_paths()))

    def test_every_entry_is_complete_tracked_and_unexpired(self) -> None:
        tracked = set(tracked_shell())
        now = today()
        for entry in declared():
            with self.subTest(path=entry["path"]):
                self.assertTrue(entry.get("reason"), "no reason")
                self.assertIsInstance(entry.get("expires"), dt.date)
                self.assertIn(entry["path"], tracked)
                self.assertGreaterEqual(entry["expires"], now, f"expired on {entry['expires']}")
                self.assertLessEqual((entry["expires"] - now).days, MAX_DAYS)

    def test_a_listed_file_still_fails_shfmt(self) -> None:
        exe = shutil.which("shfmt")
        if exe is None:
            self.skipTest("shfmt is not on PATH; the lint job runs this test with it")
        for path in declared_paths():
            with self.subTest(path=path):
                status = run_tool(exe, "-d", *SHFMT_ARGS, path)
                self.assertNotEqual(status, 0, "no finding: the entry is stale")

    def test_an_unlisted_file_is_read(self) -> None:
        exclude = re.compile(hook().get("exclude", r"\A(?!)"))
        for path in (
            "scripts/githooks/pre-commit.sh",
            ".config/lefthook/python.sh.bak",
            "x/.config/lefthook/python.sh",
            "scripts/config/lefthook/pythonXsh",
        ):
            self.assertIsNone(exclude.search(path), path)

    def test_makefile_format_targets_share_one_filtered_list(self) -> None:
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        match = re.search(r"^SHFMT_FILES = (.*?)\n\n", text, re.M | re.S)
        self.assertIsNotNone(match, "SHFMT_FILES is not defined")
        assert match is not None
        listing = match.group(1)
        for needle in ("scripts/ci/lint_exceptions.py filter shfmt --", "git ls-files '*.sh'"):
            self.assertIn(needle, listing)
        self.assertEqual(text.count("$(SHFMT_FILES)"), 2)
        for line in text.splitlines():
            if re.search(r"\bshfmt -[wd] ", line):
                self.assertIn("$(SHFMT_FILES)", line, "a shfmt call reads its own file list")

    def test_makefile_filter_drops_exactly_the_declared_files(self) -> None:
        tracked = tracked_shell()
        out = subprocess.run(  # noqa: S603 -- this interpreter, fixed argv
            [
                shutil.which("python3") or "python3",
                "-B",
                str(ROOT / "scripts/ci/lint_exceptions.py"),
                "filter",
                "shfmt",
                "--",
                *tracked,
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=True,
            timeout=60,
        ).stdout.split()
        self.assertEqual(sorted(set(tracked) - set(out)), declared_paths())

    def test_native_hook_skips_exactly_the_declared_files(self) -> None:
        text = (ROOT / "scripts/githooks/pre-commit.sh").read_text(encoding="utf-8")
        start = text.index(r'if [[ "$f" =~ \.(sh|bash)$ ]]; then')
        branch = text[start : text.index("sh_files+=", start)]
        skipped = sorted(NATIVE_SKIP.findall(branch))
        self.assertEqual(skipped, sorted(anchored(p) for p in declared_paths()))


if __name__ == "__main__":
    unittest.main()
