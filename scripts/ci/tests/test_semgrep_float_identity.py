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

The last test scans the real `core/test/` tree and requires zero findings.
Semgrep is located as in `test_semgrep_vendored_scope.py`.
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

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
    bool quiet = x == 0.0;       /* literal */
    quiet = quiet || 1.5 != y;   /* literal on the left */
    quiet = quiet || x == LIMIT; /* macro */
    quiet = quiet || y == kLimit; /* k-constant */
    quiet = quiet || x != x;     /* NaN idiom */
    quiet = quiet || i == k;     /* integers */
    return hits || quiet;
}
"""
PLANTED_HITS = (10, 11, 12, 13)


class SemgrepFloatIdentity(unittest.TestCase):
    def setUp(self) -> None:
        # One Semgrep launcher: the vendored-scope case finds semgrep on PATH
        # or the pinned image, and fails when neither is available.
        self.launcher = vendored_scope.SemgrepVendoredScope("setUp")
        self.launcher.setUp()

    def scan(self, project: Path, targets: list[str]) -> dict[str, list[int]]:
        """Return the rule's finding lines per path, from the production rule file."""
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
        lines: dict[str, list[int]] = {}
        for result in json.loads(proc.stdout)["results"]:
            if result["check_id"].rsplit(".", 1)[-1] == RULE:
                path = Path(result["path"]).as_posix()
                lines.setdefault(path, []).append(result["start"]["line"])
        return {path: sorted(found) for path, found in lines.items()}

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
            "the rule must report the four computed comparisons and none of the "
            "literal, macro, k-constant, NaN-idiom or integer comparisons",
        )

    def test_scope_is_core_test(self) -> None:
        lines = self.scan(self.planted_project(), [OUT_OF_SCOPE])
        self.assertNotIn(OUT_OF_SCOPE, lines, "the rule reached outside core/test/")

    def test_real_tests_compare_floats_through_float_bits(self) -> None:
        targets = sorted(
            path.relative_to(ROOT).as_posix()
            for path in (ROOT / "core/test").rglob("*")
            if path.suffix in {".c", ".cpp", ".h"} and "data" not in path.parts
        )
        self.assertTrue(targets, "core/test has no C or C++ sources")
        lines = self.scan(ROOT, targets)
        self.assertEqual(
            lines,
            {},
            "a test compares two computed floating-point values with == or !=; use "
            "vmaf_test_identical_f64() / _f32() from core/test/float_bits.h",
        )


if __name__ == "__main__":
    unittest.main()
