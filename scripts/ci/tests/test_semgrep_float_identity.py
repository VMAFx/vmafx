#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Planted-defect recall for the float-identity Semgrep rule (ADR-1502).

`vmaf-test-float-identity-via-float-bits` in `.semgrep.yml` refuses `==` and
`!=` between two computed floating-point values in `core/test/`. CodeQL's
`cpp/equality-on-floats` reports the same shape (alerts 1593-1598 on
`test_read_pictures_convert.c`), but only in the sources its build compiles;
39 such comparisons in the CUDA, HIP and SYCL twin tests never reached it.

The test copies the real rule file into a throwaway project and plants:

- computed `double` and `float` comparisons in `core/test/`, which the rule
  must report;
- the comparisons CodeQL allows (a literal, a macro or k-constant, the
  `x != x` NaN idiom, integers), which it must not report;
- the same computed comparisons outside `core/test/`, which are out of the
  rule's scope.

The last test scans the real `core/test/` tree and requires zero findings. A
region Semgrep cannot parse is invisible to the rule, so the scan also fails on
every parse diagnostic outside the files listed in
`.config/lint-exceptions.d/vmaf-test-float-identity-parse.toml` (repair work:
issue 3252), on an expired entry, and on a listed file that now parses.
Semgrep is located as in `test_semgrep_vendored_scope.py`.
"""

from __future__ import annotations

import datetime as dt
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import lint_exceptions
from scripts.ci.tests import test_semgrep_vendored_scope as vendored_scope
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
RULE = "vmaf-test-float-identity-via-float-bits"
IN_SCOPE = "core/test/planted_float_identity.c"
OUT_OF_SCOPE = "core/src/planted_float_identity.c"
PLANTED = """\
#include <stdbool.h>

#define LIMIT 2.0
static const double kLimit = 3.0;
static double computed(void);

bool planted(double x, float p, float q, int i, int k)
{
    double y = computed();
    bool hits = x != y;          /* reported: computed double != */
    hits = hits || x == y;       /* reported: computed double == */
    hits = hits || p == q;       /* reported: computed float == */
    hits = hits || p != q;       /* reported: computed float != */
    hits = hits || 1.0 + y == x;  /* reported: literal prefix, computed */
    hits = hits || kLimit + y != x; /* reported: k-constant prefix, computed */
    hits = hits || (double)(y * 2.0) == x; /* reported: cast of a computed value */
    hits = hits || -LIMIT * y == x; /* reported: macro prefix, computed */
    bool quiet = x == 0.0;       /* literal */
    quiet = quiet || 1.5 != y;   /* literal on the left */
    quiet = quiet || x == LIMIT; /* macro */
    quiet = quiet || y == kLimit; /* k-constant */
    quiet = quiet || x != x;     /* NaN idiom */
    quiet = quiet || i == k;     /* integers */
    quiet = quiet || x == -1.5e3; /* signed literal with exponent */
    quiet = quiet || p == .5f;   /* float literal */
    quiet = quiet || y != -kLimit; /* signed k-constant */
    quiet = quiet || i == (int)y; /* integer cast of a double */
    return hits || quiet;
}
"""
PLANTED_HITS = (10, 11, 12, 13, 14, 15, 16, 17)
EXCEPTED = "core/test/test_pelorus_interop.c"  # pelorus mirror, declared exception
PARSE_RULE = "vmaf-test-float-identity-parse"  # files with a known parse diagnostic
UNPARSABLE = "core/test/planted_unparsable.c"
UNPARSABLE_TEXT = "int broken(void) { return ((1 + ; }\n"


def rule_excludes() -> set[str]:
    """The repository paths the production rule's paths.exclude names."""
    rules = yaml.safe_load((ROOT / ".semgrep.yml").read_text(encoding="utf-8"))["rules"]
    (rule,) = [r for r in rules if r["id"] == RULE]
    return {path.removeprefix("/") for path in rule.get("paths", {}).get("exclude", [])}


def live_exceptions(day: dt.date, rule: str = RULE) -> set[str]:
    """The files the declared exception list excepts from `rule` on `day`."""
    entries, findings = lint_exceptions.load(ROOT)
    assert not findings, findings
    return {e.path for e in entries if e.rule == rule and e.expires >= day}


def diagnostic_problems(diagnosed: set[str], listed: set[str]) -> list[str]:
    """Each parse diagnostic outside `listed`, and each listed file that now parses."""
    return [
        f"{path}: Semgrep parse diagnostic, not listed" for path in sorted(diagnosed - listed)
    ] + [
        f"{path}: listed as unparsable but parses; drop its entry"
        for path in sorted(listed - diagnosed)
    ]


class SemgrepFloatIdentity(unittest.TestCase):
    def setUp(self) -> None:
        # One Semgrep launcher: the vendored-scope case finds semgrep on PATH
        # or the pinned image, and fails when neither is available.
        self.launcher = vendored_scope.SemgrepVendoredScope("setUp")
        self.launcher.setUp()

    def scan(self, project: Path, targets: list[str]) -> dict[str, list[int]]:
        """Return the rule's finding lines per path, from the production rule file."""
        return self.scan_with_diagnostics(project, targets)[0]

    def scan_with_diagnostics(
        self, project: Path, targets: list[str]
    ) -> tuple[dict[str, list[int]], set[str]]:
        """Return the rule's finding lines per path and the paths Semgrep could not parse."""
        prefix, executable = self.launcher.semgrep_argv(project)
        argv = [
            *prefix,
            "scan",
            "--config=.semgrep.yml",
            "--json",
            "--quiet",
            "--metrics=off",
            "--disable-version-check",
            "--jobs",
            "1",
            "--no-git-ignore",
            *targets,
        ]
        proc = run_command(
            argv,
            allowed_executables=(executable,),
            cwd=project,
            capture_output=True,
            text=True,
            timeout_seconds=300,
            max_output_bytes=16 * 1_048_576,
        )
        self.assertIn(proc.returncode, (0, 1), proc.stderr)
        report = json.loads(proc.stdout)
        diagnosed = set()
        for error in report["errors"]:
            self.assertIn("path", error, f"Semgrep error without a path: {error}")
            diagnosed.add(Path(error["path"]).as_posix())
        lines: dict[str, list[int]] = {}
        for result in report["results"]:
            if result["check_id"].rsplit(".", 1)[-1] == RULE:
                path = Path(result["path"]).as_posix()
                lines.setdefault(path, []).append(result["start"]["line"])
        return {path: sorted(found) for path, found in lines.items()}, diagnosed

    def planted_project(self) -> Path:
        temp = tempfile.TemporaryDirectory(prefix="vmafx-semgrep-float-")
        self.addCleanup(temp.cleanup)
        project = Path(temp.name)
        shutil.copyfile(ROOT / ".semgrep.yml", project / ".semgrep.yml")
        for relative in (IN_SCOPE, OUT_OF_SCOPE):
            target = project / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(PLANTED, encoding="utf-8")
        return project

    def test_reports_computed_comparisons_only(self) -> None:
        lines = self.scan(self.planted_project(), [IN_SCOPE, OUT_OF_SCOPE])
        self.assertEqual(
            lines.get(IN_SCOPE),
            list(PLANTED_HITS),
            "the rule must report the eight computed comparisons and none of the "
            "literal, macro, k-constant, NaN-idiom or integer comparisons",
        )

    def test_scope_is_core_test(self) -> None:
        lines = self.scan(self.planted_project(), [OUT_OF_SCOPE])
        self.assertNotIn(OUT_OF_SCOPE, lines, "the rule reached outside core/test/")

    def test_excludes_are_the_declared_exceptions(self) -> None:
        self.assertEqual(
            rule_excludes(),
            live_exceptions(lint_exceptions.today()),
            "paths.exclude of the rule must name exactly the live entries of "
            f".config/lint-exceptions.d/{RULE}.toml; an expired entry leaves the rule",
        )

    def test_an_expired_exception_no_longer_matches(self) -> None:
        self.assertIn(EXCEPTED, rule_excludes())
        expiry = max(e.expires for e in lint_exceptions.load(ROOT)[0] if e.rule == RULE)
        self.assertNotEqual(rule_excludes(), live_exceptions(expiry + dt.timedelta(days=1)))

    def test_exclusion_covers_the_excepted_file_only(self) -> None:
        project = self.planted_project()
        sibling = "core/test/planted_beside_mirror.c"
        for relative in (EXCEPTED, sibling):
            (project / relative).write_text(PLANTED, encoding="utf-8")
        lines = self.scan(project, [EXCEPTED, sibling])
        self.assertNotIn(EXCEPTED, lines, "the declared exception was not applied")
        self.assertEqual(lines.get(sibling), list(PLANTED_HITS))

    def test_unparsable_file_is_a_diagnostic(self) -> None:
        project = self.planted_project()
        (project / UNPARSABLE).write_text(UNPARSABLE_TEXT, encoding="utf-8")
        _, diagnosed = self.scan_with_diagnostics(project, [UNPARSABLE, IN_SCOPE])
        self.assertEqual(diagnosed, {UNPARSABLE}, "the planted parse error went unreported")
        # A list naming a file that parses, and missing the one that does not.
        self.assertEqual(
            diagnostic_problems(diagnosed, {IN_SCOPE}),
            [
                f"{UNPARSABLE}: Semgrep parse diagnostic, not listed",
                f"{IN_SCOPE}: listed as unparsable but parses; drop its entry",
            ],
        )

    def test_parse_entries_expire(self) -> None:
        entries = [e for e in lint_exceptions.load(ROOT)[0] if e.rule == PARSE_RULE]
        self.assertTrue(entries)
        self.assertEqual(
            live_exceptions(max(e.expires for e in entries) + dt.timedelta(days=1), PARSE_RULE),
            set(),
        )

    def test_real_tests_compare_floats_through_float_bits(self) -> None:
        targets = sorted(
            path.relative_to(ROOT).as_posix()
            for path in (ROOT / "core/test").rglob("*")
            if path.suffix in {".c", ".cpp", ".h"} and "data" not in path.parts
        )
        self.assertTrue(targets, "core/test has no C or C++ sources")
        lines, diagnosed = self.scan_with_diagnostics(ROOT, targets)
        listed = live_exceptions(lint_exceptions.today(), PARSE_RULE)
        self.assertEqual(
            diagnostic_problems(diagnosed, listed),
            [],
            "Semgrep could not parse a region the float-identity rule must read; repair the "
            "file (issue 3252) or, for a measured existing file, list it in "
            f".config/lint-exceptions.d/{PARSE_RULE}.toml",
        )
        self.assertEqual(
            lines,
            {},
            "a test compares two computed floating-point values with == or !=; use "
            "vmaf_test_identical_f64() / _f32() from core/test/float_bits.h",
        )


if __name__ == "__main__":
    unittest.main()
