#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The pre-push mkdocs gate must refuse a tree whose strict build warns.

Each case builds a disposable repository with a two-page MkDocs site and runs
``scripts/git-hooks/pre-push-mkdocs-strict.sh`` in it. A broken anchor makes
MkDocs log a WARNING; ``--strict`` turns that into a failed build only when the
WARNING is emitted, so a hook that adds ``--quiet`` (log level ERROR) passes the
broken tree. That form shipped and let two rewrites break master's docs build.

Positive / negative / boundary per HISS-15: a clean site passes, a broken
anchor blocks with the WARNING line on stderr, and the quiet form of the build
is shown to exit 0 on the same broken tree, which is the defect this guards.
The suite skips, with the reason, when MkDocs is not installed.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
HOOK = REPO / "scripts" / "git-hooks" / "pre-push-mkdocs-strict.sh"
MKDOCS = shutil.which("mkdocs")
GIT_TIMEOUT_S = 60
BUILD_TIMEOUT_S = 120

MKDOCS_YML = """site_name: probe
docs_dir: docs
nav:
  - Home: index.md
  - Other: other.md
validation:
  links:
    anchors: warn
"""


def _run(cmd: list[str], cwd: Path, timeout: int) -> subprocess.CompletedProcess[str]:
    # No GIT_DIR / GIT_INDEX_FILE from a hook: they would point git at the caller's repository.
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.pop("SKIP", None)
    env.pop("PRE_COMMIT_REMOTE_NAME", None)
    return subprocess.run(  # noqa: S603 -- fixed argv, no shell, disposable fixture repo
        cmd, cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout, check=False
    )


def _site(root: Path, anchor: str) -> None:
    (root / "docs").mkdir()
    (root / "mkdocs.yml").write_text(MKDOCS_YML, encoding="utf-8")
    (root / "docs" / "index.md").write_text(
        f"# Home\n\nSee [the section](other.md#{anchor}).\n", encoding="utf-8"
    )
    (root / "docs" / "other.md").write_text(
        "# Other\n\n## Real section\n\nText.\n", encoding="utf-8"
    )
    for cmd in (["git", "init", "-q"], ["git", "add", "-A"]):
        _run(cmd, root, GIT_TIMEOUT_S)
    _run(
        [
            "git",
            "-c",
            "user.name=t",
            "-c",
            "user.email=t@example.invalid",
            "commit",
            "-q",
            "-m",
            "s",
        ],
        root,
        GIT_TIMEOUT_S,
    )


@unittest.skipUnless(MKDOCS, "mkdocs is not installed; pip install -r docs/requirements.txt")
class PrePushMkdocsStrict(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="mkdocs-hook-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_clean_site_passes(self) -> None:
        _site(self.tmp, "real-section")
        result = _run(["bash", str(HOOK)], self.tmp, BUILD_TIMEOUT_S)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("passed", result.stderr)

    def test_broken_anchor_blocks_with_the_warning(self) -> None:
        _site(self.tmp, "missing-section")
        result = _run(["bash", str(HOOK)], self.tmp, BUILD_TIMEOUT_S)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("BLOCKED", result.stderr)
        self.assertIn("missing-section", result.stderr)

    def test_quiet_build_hides_the_warning(self) -> None:
        """The defect: the quiet strict build of the broken tree exits 0."""
        _site(self.tmp, "missing-section")
        site_dir = self.tmp / "site-quiet"
        quiet = _run(
            [MKDOCS or "mkdocs", "build", "--strict", "--quiet", "--site-dir", str(site_dir)],
            self.tmp,
            BUILD_TIMEOUT_S,
        )
        loud = _run(
            [MKDOCS or "mkdocs", "build", "--strict", "--site-dir", str(site_dir)],
            self.tmp,
            BUILD_TIMEOUT_S,
        )
        self.assertNotEqual(loud.returncode, 0, loud.stderr)
        if quiet.returncode != 0:
            self.skipTest(
                "this MkDocs fails quiet strict builds too; the hook test above still holds"
            )
        text = HOOK.read_text(encoding="utf-8")
        build = re.search(r"^mkdocs build .*?^mkdocs_exit=", text, flags=re.M | re.S)
        if build is None:
            self.fail("hook has no `mkdocs build` command block")
        self.assertNotRegex(build.group(0), r"--quiet|\s-q\b", "the hook's build must not be quiet")


if __name__ == "__main__":
    unittest.main()
