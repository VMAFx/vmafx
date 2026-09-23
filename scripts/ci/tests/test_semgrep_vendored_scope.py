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

The same blindness was filed a second time as BUG-061. Three further
exemptions — `/matlab/**` and `/core/tools/{cli_parse,y4m_input}.c` in the
rule, `compat/python-vmaf/matlab/` in `.semgrepignore`, and
`^compat/python-vmaf/matlab/` in the `semgrep-local` hook — outlived the code
they were written for: `cli_parse.c` was deleted under ADR-1155, `/matlab/**`
never matched the tree's actual MEX directory, and the remaining banned calls
in `y4m_input.c` and `MEX/innerProd.c` were fixed in `bc00da0be`. Each was
justified in comments by the file's Netflix origin, which ADR-1142 retired as
a reason. They are removed, and pinned here so they cannot come back.

This is a planted-defect recall test. It copies the repository's real
`.semgrep.yml` and `.semgrepignore` into a throwaway project, plants banned
calls at the vendored and formerly-exempt paths, and requires Semgrep to
report every one of them in both scan forms. A control file outside any
guarded path tells a guard that was scoped away apart from a harness that
found nothing at all. The last two tests scan the real files, so a future
re-vendor — or a banned call reintroduced into an upstream-mirror source —
fails here even before the hook sees the staged file.
"""

from __future__ import annotations

import json
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
RULE = "vmaf-no-strcpy-strcat-sprintf"
CONTROL = "core/src/planted_control.c"
VENDORED = (
    "core/src/mcp/3rdparty/cJSON/cJSON.c",
    "core/src/mcp/3rdparty/cJSON/cJSON.h",
    "core/src/pdjson.c",
    "core/src/pdjson.h",
)
# Paths that carried an origin-based exemption until 2026-09-21 (BUG-061).
# `matlab/mex_stub.c` and `core/tools/cli_parse.c` do not exist in the tree —
# they are the shapes the removed globs `/matlab/**` and
# `/core/tools/cli_parse.c` would match, planted so that re-adding either glob
# fails here rather than silently un-gating a directory.
ORIGIN_EXEMPT = (
    "core/tools/y4m_input.c",
    "core/tools/cli_parse.c",
    "core/tools/cli_parse.cpp",
    "matlab/mex_stub.c",
    "compat/python-vmaf/matlab/strred/matlabPyrTools/MEX/innerProd.c",
)
GUARDED = (*VENDORED, *ORIGIN_EXEMPT)
# The subset of ORIGIN_EXEMPT that is a real file, scanned in place below.
REAL_ORIGIN_EXEMPT = (
    "core/tools/y4m_input.c",
    "core/tools/cli_parse.cpp",
)
MATLAB_MEX = "compat/python-vmaf/matlab"
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
        proc = run_command(
            argv,
            allowed_executables=(self.semgrep,),
            cwd=project,
            capture_output=True,
            text=True,
            timeout_seconds=300,
            max_output_bytes=16 * 1_048_576,
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
        for relative in (*GUARDED, CONTROL):
            target = project / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(PLANTED, encoding="utf-8")
        return project

    def assert_every_guarded_path_reported(self, counts: dict[str, int], form: str) -> None:
        self.assertEqual(
            counts.get(CONTROL),
            PLANTED_CALLS,
            f"{form}: the control file was not reported, so the harness is broken",
        )
        for relative in GUARDED:
            self.assertEqual(
                counts.get(relative, 0),
                PLANTED_CALLS,
                f"{form}: banned calls planted in {relative} were not reported; a path "
                "exclude in .semgrep.yml or an entry in .semgrepignore hides code that "
                "ADR-1142 puts in scope. Origin is not an exemption: fix the call site "
                "instead of carving the path out",
            )

    def test_whole_tree_scan_reports_planted_calls_in_guarded_paths(self) -> None:
        """The CI form: a directory scan, where `.semgrepignore` applies."""
        counts = self.scan(self.planted_project(), ["."])
        self.assert_every_guarded_path_reported(counts, "whole-tree scan")

    def test_explicit_file_scan_reports_planted_calls_in_guarded_paths(self) -> None:
        """The pre-commit form: explicit paths, where only rule excludes apply."""
        counts = self.scan(self.planted_project(), [*GUARDED, CONTROL])
        self.assert_every_guarded_path_reported(counts, "explicit-file scan")

    def test_banned_call_rule_carries_no_path_filter(self) -> None:
        """The rule must have no `paths:` key — an include exempts by omission."""
        text = (ROOT / ".semgrep.yml").read_text(encoding="utf-8")
        rule = re.search(
            rf"^  - id: {re.escape(RULE)}$\n(?P<body>.*?)(?=^  - id: |\Z)",
            text,
            re.DOTALL | re.MULTILINE,
        )
        self.assertIsNotNone(rule, f"{RULE} not found in .semgrep.yml")
        assert rule is not None
        code = [line for line in rule["body"].splitlines() if not line.lstrip().startswith("#")]
        offenders = [line for line in code if re.match(r"\s*paths:", line)]
        self.assertEqual(
            offenders,
            [],
            f"{RULE} grew a `paths:` filter. Every path filter on this rule so far "
            "has outlived the code it was written for and then hidden a whole "
            "directory (cJSON 1.7.19, BUG-061). Fix the call site instead",
        )

    def test_precommit_hook_does_not_skip_guarded_paths(self) -> None:
        text = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        hook = re.search(r"- id: semgrep-local\n(?P<body>.*?)(?=\n\s*- id: |\Z)", text, re.DOTALL)
        self.assertIsNotNone(hook, "semgrep-local hook not found")
        assert hook is not None
        exclude = re.search(r"^\s*exclude: '(?P<regex>[^']*)'\s*$", hook["body"], re.MULTILINE)
        self.assertIsNotNone(exclude, "semgrep-local hook has no single-quoted exclude")
        assert exclude is not None
        for relative in GUARDED:
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

    def test_formerly_exempt_files_are_free_of_banned_calls(self) -> None:
        """The upstream-mirror sources the removed excludes used to cover."""
        targets = list(REAL_ORIGIN_EXEMPT)
        targets += sorted(
            path.relative_to(ROOT).as_posix()
            for path in (ROOT / MATLAB_MEX).rglob("*")
            if path.suffix in {".c", ".h"}
        )
        for relative in targets:
            self.assertTrue((ROOT / relative).is_file(), f"{relative} is missing")
        counts = self.scan(ROOT, targets)
        self.assertEqual(
            counts,
            {},
            "banned libc calls in upstream-mirror sources. ADR-1142 puts them in scope: "
            "bound the write, never restore the path exclude",
        )


if __name__ == "__main__":
    unittest.main()
