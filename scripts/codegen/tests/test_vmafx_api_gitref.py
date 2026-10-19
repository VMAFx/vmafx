# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Reading another revision through git (vmafx_api.gitref).

Runs against a throw-away repository, so a shallow clone runs it too.
Positive: `tree_at` extracts a directory as committed, not as in the working
tree, and the text readers return its files. Negative: an unknown revision
and a directory the revision lacks raise `Unavailable` with git's reason.
Boundary: a CRLF file reads back with universal newlines; a nested directory
is extracted whole.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from support import ROOT  # noqa: F401 -- puts scripts/codegen on sys.path
from vmafx_api import gitref

GIT = shutil.which("git") or "git"
TIMEOUT_S = 60
IDENTITY = {
    "GIT_AUTHOR_NAME": "test",
    "GIT_AUTHOR_EMAIL": "test@example.invalid",
    "GIT_COMMITTER_NAME": "test",
    "GIT_COMMITTER_EMAIL": "test@example.invalid",
    "GIT_CONFIG_GLOBAL": os.devnull,
    "GIT_CONFIG_NOSYSTEM": "1",
}


def git(repo: Path, *args: str) -> None:
    subprocess.run(  # noqa: S603 -- resolved git, fixed argv
        [GIT, "-C", str(repo), *args],
        check=True,
        capture_output=True,
        timeout=TIMEOUT_S,
        env={**{k: v for k, v in os.environ.items() if not k.startswith("GIT_")}, **IDENTITY},
    )


@unittest.skipIf(shutil.which("git") is None, "git is not on PATH")
class TreeAtTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name) / "repo"
        chart = self.repo / "chart" / "templates"
        chart.mkdir(parents=True)
        (self.repo / "chart" / "Chart.yaml").write_bytes(b"name: demo\r\nversion: 1\r\n")
        (chart / "a.yaml").write_text("kind: A\n")
        git(self.repo, "init", "-q")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-q", "-m", "one")
        (chart / "a.yaml").write_text("kind: B\n")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_extracts_the_committed_tree(self) -> None:
        dest = Path(self.tmp.name) / "out"
        dest.mkdir()
        chart = gitref.tree_at(self.repo, "HEAD", "chart", dest)
        self.assertEqual(chart, dest / "chart")
        self.assertEqual((chart / "templates" / "a.yaml").read_text(), "kind: A\n")
        self.assertEqual((chart / "Chart.yaml").read_bytes(), b"name: demo\r\nversion: 1\r\n")

    def test_text_reads_use_universal_newlines(self) -> None:
        files = gitref.files_at(self.repo, "HEAD", "chart", ".yaml")
        self.assertEqual(files, {"chart/Chart.yaml": "name: demo\nversion: 1\n"})

    def test_unknown_revision_is_unavailable(self) -> None:
        with self.assertRaisesRegex(gitref.Unavailable, "git archive"):
            gitref.tree_at(self.repo, "no-such-ref", "chart", Path(self.tmp.name))

    def test_missing_directory_is_unavailable(self) -> None:
        with self.assertRaisesRegex(gitref.Unavailable, "missing"):
            gitref.tree_at(self.repo, "HEAD", "missing", Path(self.tmp.name))


if __name__ == "__main__":
    unittest.main()
