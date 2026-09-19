#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin that the banned-function Semgrep guard sees the vendored JSON code.

The guard is `vmaf-no-strcpy-strcat-sprintf` in `.semgrep.yml`. It runs twice:
the `semgrep-local` pre-commit hook passes it explicit file paths, and the
required `Semgrep` CI job scans the whole tree. Until 2026-09 both forms were
blind to `core/src/mcp/3rdparty/cJSON/`: the rule carried a path exclude and
`.semgrepignore` listed `cJSON.c`, so the cJSON 1.7.19 re-vendor put eleven
`sprintf` / `strcpy` calls back after two pull requests had removed them, and
every gate stayed green (docs/state.md,
T-VENDORED-CJSON-BANNED-FUNCTIONS-REVERTED-2026-09-19).

This is a planted-defect recall test. It copies the repository's real
`.semgrep.yml` and `.semgrepignore` into a throwaway project, plants banned
calls at the vendored paths, and requires Semgrep to report every one of them
in both scan forms. A control file outside any vendored path tells a guard
that was scoped away apart from a harness that found nothing at all. The last
test scans the real vendored files, so a future re-vendor fails here even
before the hook sees the staged file.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RULE = "vmaf-no-strcpy-strcat-sprintf"
CONTROL = "core/src/planted_control.c"
VENDORED = (
    "core/src/mcp/3rdparty/cJSON/cJSON.c",
    "core/src/mcp/3rdparty/cJSON/cJSON.h",
    "core/src/pdjson.c",
    "core/src/pdjson.h",
)
PLANTED = """\
#include <stdio.h>
#include <string.h>

void planted(char *destination, const char *source)
{
    strcpy(destination, source);
    strcat(destination, source);
    sprintf(destination, "%s", source);
}
"""
PLANTED_CALLS = 3


class SemgrepVendoredScope(unittest.TestCase):
    def setUp(self) -> None:
        binary = shutil.which("semgrep")
        if binary is None:
            raise RuntimeError("semgrep is required for the vendored-scope guard contract")
        self.semgrep = binary

    def scan(self, project: Path, targets: list[str]) -> dict[str, int]:
        """Return banned-call findings per path, using the production rule file."""
        argv = [
            self.semgrep,
            "scan",
            "--config=.semgrep.yml",
            "--json",
            "--quiet",
            "--metrics=off",
            "--disable-version-check",
            "--jobs",
            "1",
            # The throwaway project is not a repository, and a temporary
            # directory may sit below a git-ignored path: never let git state
            # decide what is scanned.
            "--no-git-ignore",
            *targets,
        ]
        proc = subprocess.run(  # noqa: S603 -- fixed argv, no shell
            argv, cwd=project, capture_output=True, text=True, check=False
        )
        self.assertIn(proc.returncode, (0, 1), proc.stderr)
        counts: dict[str, int] = {}
        for result in json.loads(proc.stdout)["results"]:
            if result["check_id"].rsplit(".", 1)[-1] == RULE:
                path = Path(result["path"]).as_posix()
                counts[path] = counts.get(path, 0) + 1
        return counts

    def planted_project(self) -> Path:
        temp = tempfile.TemporaryDirectory(prefix="vmafx-semgrep-scope-")
        self.addCleanup(temp.cleanup)
        project = Path(temp.name)
        for name in (".semgrep.yml", ".semgrepignore"):
            shutil.copyfile(ROOT / name, project / name)
        for relative in (*VENDORED, CONTROL):
            target = project / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(PLANTED, encoding="utf-8")
        return project

    def assert_every_vendored_path_reported(self, counts: dict[str, int], form: str) -> None:
        self.assertEqual(
            counts.get(CONTROL),
            PLANTED_CALLS,
            f"{form}: the control file was not reported, so the harness is broken",
        )
        for relative in VENDORED:
            self.assertEqual(
                counts.get(relative, 0),
                PLANTED_CALLS,
                f"{form}: banned calls planted in {relative} were not reported; a path "
                "exclude in .semgrep.yml or an entry in .semgrepignore hides vendored code",
            )

    def test_whole_tree_scan_reports_planted_calls_in_vendored_paths(self) -> None:
        """The CI form: a directory scan, where `.semgrepignore` applies."""
        counts = self.scan(self.planted_project(), ["."])
        self.assert_every_vendored_path_reported(counts, "whole-tree scan")

    def test_explicit_file_scan_reports_planted_calls_in_vendored_paths(self) -> None:
        """The pre-commit form: explicit paths, where only rule excludes apply."""
        counts = self.scan(self.planted_project(), [*VENDORED, CONTROL])
        self.assert_every_vendored_path_reported(counts, "explicit-file scan")

    def test_precommit_hook_does_not_skip_vendored_paths(self) -> None:
        text = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        hook = re.search(r"- id: semgrep-local\n(?P<body>.*?)(?=\n\s*- id: |\Z)", text, re.DOTALL)
        self.assertIsNotNone(hook, "semgrep-local hook not found")
        assert hook is not None
        exclude = re.search(r"^\s*exclude: '(?P<regex>[^']*)'\s*$", hook["body"], re.MULTILINE)
        self.assertIsNotNone(exclude, "semgrep-local hook has no single-quoted exclude")
        assert exclude is not None
        for relative in VENDORED:
            self.assertIsNone(
                re.search(exclude["regex"], relative),
                f"the semgrep-local hook skips {relative} at commit time",
            )

    def test_vendored_files_are_free_of_banned_calls(self) -> None:
        counts = self.scan(ROOT, list(VENDORED))
        self.assertEqual(
            counts,
            {},
            "banned libc calls in vendored code; after a re-vendor re-apply the fork delta "
            "(core/src/mcp/3rdparty/cJSON/AGENTS.md), never add an exclusion",
        )


if __name__ == "__main__":
    unittest.main()
