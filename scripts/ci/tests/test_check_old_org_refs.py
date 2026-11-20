#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise the old organisation reference guard and its exemptions."""

import datetime
import importlib.util
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import TextCommandResult, run  # noqa: E402

CHECKER = Path(
    os.environ.get("OLD_ORG_CHECKER_TEST_PATH", ROOT / "scripts/ci/check-old-org-refs.py")
).resolve()
git_path = shutil.which("git")
if git_path is None:
    raise RuntimeError("git not found on PATH")
GIT = str(Path(git_path).resolve())
GIT_TIMEOUT_S = 60
SCRIPT_TIMEOUT_S = 60
spec = importlib.util.spec_from_file_location("old_org_guard", CHECKER)
if spec is None or spec.loader is None:
    raise RuntimeError(f"cannot load checker: {CHECKER}")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
DAY = datetime.date(2026, 11, 30)


def run_checker(cwd: str, env: dict[str, str], *extra: str) -> TextCommandResult:
    return run(
        [sys.executable, "-B", str(CHECKER), "--all", *extra],
        allowed_executables=(sys.executable,),
        cwd=cwd,
        env=env,
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=SCRIPT_TIMEOUT_S,
    )


class OldOrgTests(unittest.TestCase):
    def hits(self, line: str, path: str = "example.txt", day: datetime.date = DAY) -> list[str]:
        findings: list[str] = module.line_findings(path, 1, line, day)
        return findings

    def test_forms(self) -> None:
        forms = {
            "a": "VMAFx/repo",
            "b": "cordanaLLM/repo",
            "c": "golusoris/golusoris",
            "d": "https://github.com/vmafx/repo",
            "e": "VMAFx.github.io",
            "f": "github.com/orgs/golusoris/people",
            "g": 'owner: "VMAFx"',
        }
        for letter, text in forms.items():
            with self.subTest(letter=letter):
                self.assertIn(f"({letter})", self.hits(text)[0])

    def test_exempt_forms(self) -> None:
        for value in (
            "github.com/VMAFx/vmafx",
            "pkg:golang/github.com/VMAFx/vmafx",
            "mod\tgithub.com/VMAFx/vmafx",
            "VMAFx",
            "vmafx/types.h",
            "../golusoris/",
            "golusoris/fx",
            "VMAFx/repo # old-org-ok",
        ):
            with self.subTest(value=value):
                self.assertEqual([], self.hits(value))
        self.assertTrue(self.hits("http://github.com/VMAFx/x"))
        self.assertTrue(self.hits("https://github.com/VMAFx/x"))

    def test_permanent_paths(self) -> None:
        for path in (
            "docs/adr/1234-title.md",
            "docs/adr/_index_fragments/a.md",
            "docs/research/x",
            "scripts/ci/research-digest-id-baseline.json",
            "docs/rebase-notes.d/x",
            "docs/rebase-notes.md",
            "changelog.d/x",
            "CHANGELOG.md",
            "docs/changelog-archive/x",
            "scripts/ci/tests/test_check_old_org_refs.py",
        ):
            with self.subTest(path=path):
                self.assertTrue(module.allowed(path))

    def test_dated_exceptions(self) -> None:
        entries = [
            ("core/include/libvmaf/pelorus/x.h", "VMAFx/x"),
            ("core/src/interop/pelorus_x.c", "VMAFx/x"),
            ("core/test/test_pelorus_interop.c", "VMAFx/x"),
            ("docs/api/pelorus-interop.md", "VMAFx/x"),
            ("build-config.env", "FFMPEG_FIX_SERIES_REPO=VMAFx/x"),
            ("lua/standards.lua", "VMAFx/x"),
            ("tools/markdownlint/package.json", "VMAFx/x"),
            ("tools/markdownlint/package-lock.json", "VMAFx/x"),
            ("other", "ghcr.io/VMAFx/x"),
        ]
        for path, line in entries:
            with self.subTest(path=path):
                self.assertEqual([], self.hits(line, path))
                self.assertIn(
                    "expired exception", self.hits(line, path, DAY + datetime.timedelta(days=1))[0]
                )
        self.assertTrue(self.hits("OTHER=VMAFx/x", "build-config.env"))

    def test_all_git_failure(self) -> None:
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        env["GIT_CONFIG_GLOBAL"] = os.devnull
        env["GIT_CONFIG_NOSYSTEM"] = "1"
        with tempfile.TemporaryDirectory() as tmp:
            result = run_checker(tmp, env)
            self.assertEqual(2, result.returncode)
            self.assertIn("check-old-org-refs:", result.stderr)

    def test_planted_all_and_usage(self) -> None:
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        env["GIT_CONFIG_GLOBAL"] = os.devnull
        env["GIT_CONFIG_NOSYSTEM"] = "1"
        env["OLD_ORG_REFS_TODAY"] = "2026-11-20"
        with tempfile.TemporaryDirectory() as tmp:
            run(
                [GIT, "init", "-q", tmp],
                allowed_executables=(GIT,),
                env=env,
                capture_output=True,
                text=True,
                check=True,
                timeout_seconds=GIT_TIMEOUT_S,
            )
            path = Path(tmp) / "planted.txt"
            path.write_text("https://github.com/VMAFx/vmafx/issues/1\n")
            run(
                [GIT, "-C", tmp, "add", "planted.txt"],
                allowed_executables=(GIT,),
                env=env,
                capture_output=True,
                text=True,
                check=True,
                timeout_seconds=GIT_TIMEOUT_S,
            )
            self.assertEqual(1, run_checker(tmp, env).returncode)
            path.write_text("clean\n")
            self.assertEqual(0, run_checker(tmp, env).returncode)
            self.assertEqual(2, run_checker(tmp, env, "file").returncode)


if __name__ == "__main__":
    unittest.main()
