#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Planted-defect recall for the count-store-before-bound Semgrep rule.

`vmaf-no-count-store-before-bound-check` in `.semgrep.yml` detects struct count
fields stored before bound checks that can reject them. If the bound check
rejects the value, failure cleanup loops that iterate over the count field read
or free past the array bound.

The test copies the real `.semgrep.yml` into a throwaway project and plants:

- store followed by bound check returning/exiting (arrow and dot struct access,
  direct and reversed comparisons), which the rule must report;
- clean patterns (check-then-store, literal reset, branch comparisons on literals,
  check without exit), which the rule must not report;
- the open_context pre-fix regression shape, which the rule must report.

The rule was also checked against the real history of that file: it reports the
pre-fix revision (commit ccf65faea, line 218) and nothing on the fix (6f1f47006).

The tree test scans the real repository root `.` and requires zero findings.
"""

from __future__ import annotations

import contextlib
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci.tests import test_semgrep_vendored_scope as vendored_scope
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
RULE = "vmaf-no-count-store-before-bound-check"
IN_SCOPE = "planted.c"
PLANTED_C = """\
#include <stdbool.h>

#define MAX_RING 32u

typedef struct {
    unsigned n_ring;
    unsigned n_planes;
    unsigned n_features;
    int *ring[MAX_RING];
} Live;

bool fail_context(Live *l);
unsigned max_in_flight(void);
int check(void);
int f(void);
void free_ring(Live *l);
void log_error(const char *msg);

bool bad_arrow_return_block(Live *live, unsigned n_ring)
{
    live->n_ring = n_ring; /* reported: arrow store then block return */
    if (n_ring > MAX_RING) {
        return fail_context(live);
    }
    return true;
}

bool bad_arrow_field_return_bare(Live *live, unsigned n)
{
    live->n_ring = n; /* reported: arrow store then bare return */
    if (live->n_ring >= MAX_RING)
        return false;
    return true;
}

bool bad_arrow_call_goto(Live *live)
{
    live->n_ring = max_in_flight(); /* reported: arrow call store then goto */
    if (live->n_ring > MAX_RING || check() != 0)
        goto fail;
    return true;
fail:
    return fail_context(live);
}

bool bad_arrow_goto_block(Live *live, unsigned n_ring)
{
    live->n_ring = n_ring; /* reported: arrow store then block goto */
    if (n_ring > MAX_RING) {
        check();
        goto fail;
    }
    return true;
fail:
    return fail_context(live);
}

bool bad_dot_return_block(Live live, unsigned n_ring)
{
    live.n_ring = n_ring; /* reported: dot store then block return */
    if (n_ring > MAX_RING) {
        return fail_context(&live);
    }
    return true;
}

bool bad_dot_field_return_bare(Live live, unsigned n)
{
    live.n_ring = n; /* reported: dot store then bare return */
    if (live.n_ring >= MAX_RING)
        return false;
    return true;
}

bool bad_dot_call_goto(Live live)
{
    live.n_ring = max_in_flight(); /* reported: dot call store then goto */
    if (live.n_ring > MAX_RING || check() != 0)
        goto fail;
    return true;
fail:
    return fail_context(&live);
}

bool bad_dot_goto_block(Live live, unsigned n_ring)
{
    live.n_ring = n_ring; /* reported: dot store then block goto */
    if (n_ring > MAX_RING) {
        check();
        goto fail;
    }
    return true;
fail:
    return fail_context(&live);
}

bool bad_regression_open_context(Live *live, unsigned n_ring)
{
    live->n_ring = n_ring; /* reported: open_context pre-fix regression shape */
    if (live->n_features == 0 || n_ring > MAX_RING || f() != 0) {
        return fail_context(live);
    }
    return true;
}

bool bad_restore_on_failure(Live *live, unsigned n_ring)
{
    live->n_ring = n_ring; /* reported: nonzero re-store in failure block */
    if (n_ring > MAX_RING) {
        live->n_ring = 1;
        return fail_context(live);
    }
    return true;
}

bool bad_reversed_lt(Live *live, unsigned n_ring)
{
    live->n_ring = n_ring; /* reported: reversed lt */
    if (MAX_RING < n_ring) {
        return fail_context(live);
    }
    return true;
}

bool bad_reversed_le_field(Live *live, unsigned n_ring)
{
    live->n_ring = n_ring; /* reported: reversed le field */
    if (MAX_RING <= live->n_ring) {
        return fail_context(live);
    }
    return true;
}

bool bad_reversed_dot_goto(Live live, unsigned n_ring)
{
    live.n_ring = n_ring; /* reported: reversed dot goto */
    if (MAX_RING < live.n_ring) {
        goto fail;
    }
    return true;
fail:
    return fail_context(&live);
}

bool bad_cleanup_before_reset(Live *live, unsigned n_ring)
{
    live->n_ring = n_ring; /* reported: cleanup before reset */
    if (n_ring > MAX_RING) {
        free_ring(live);
        live->n_ring = 0;
        return false;
    }
    return true;
}

bool bad_cleanup_before_reset_dot_goto(Live live, unsigned n_ring)
{
    live.n_ring = n_ring; /* reported: cleanup before reset dot goto */
    if (n_ring > MAX_RING) {
        free_ring(&live);
        live.n_ring = 0;
        goto fail;
    }
    return true;
fail:
    return fail_context(&live);
}

bool good_check_first(Live *live, unsigned n_ring)
{
    if (n_ring > MAX_RING) {
        return fail_context(live);
    }
    live->n_ring = n_ring;
    return true;
}

bool good_reversed_check_first(Live *live, unsigned n_ring)
{
    if (MAX_RING < n_ring) {
        return false;
    }
    live->n_ring = n_ring;
    return true;
}

bool good_literal_reset(Live *live, unsigned n)
{
    live->n_ring = 0u;
    if (n > MAX_RING)
        return false;
    live->n_ring = n;
    return true;
}

int good_branch(Live *live, unsigned planes)
{
    live->n_planes = planes;
    if (live->n_planes > 1u) {
        const int err = check();
        if (err)
            return err;
    }
    return 0;
}

int good_no_exit(Live *live, unsigned n_ring)
{
    int err = 0;
    live->n_ring = n_ring;
    if (n_ring > MAX_RING) {
        err = -1;
    }
    return err;
}

bool good_reset_on_failure(Live *live, unsigned n_ring)
{
    live->n_ring = n_ring;
    if (n_ring > MAX_RING) {
        live->n_ring = 0;
        return fail_context(live);
    }
    return true;
}

bool good_reset_on_failure_arrow_goto(Live *live, unsigned n_ring)
{
    live->n_ring = n_ring;
    if (n_ring > MAX_RING) {
        live->n_ring = 0u;
        goto fail;
    }
    return true;
fail:
    return fail_context(live);
}

bool good_reset_on_failure_dot(Live live, unsigned n_ring)
{
    live.n_ring = n_ring;
    if (n_ring > MAX_RING) {
        live.n_ring = 0;
        return fail_context(&live);
    }
    return true;
}

bool good_reset_on_failure_dot_goto(Live live, unsigned n_ring)
{
    live.n_ring = n_ring;
    if (n_ring > MAX_RING) {
        live.n_ring = 0u;
        goto fail;
    }
    return true;
fail:
    return fail_context(&live);
}

bool good_unrelated_before_reset(Live *live, unsigned n_ring)
{
    live->n_ring = n_ring;
    if (n_ring > MAX_RING) {
        log_error("ring too large");
        live->n_ring = 0;
        return fail_context(live);
    }
    return true;
}
"""

PLANTED_HITS = tuple(n for n, line in enumerate(PLANTED_C.splitlines(), 1) if "/* reported" in line)


class SemgrepStoreBeforeBound(unittest.TestCase):
    def setUp(self) -> None:
        self.launcher = vendored_scope.SemgrepVendoredScope("setUp")
        self.launcher.setUp()

    def scan(
        self, project: Path, targets: list[str], excludes: list[str] | None = None
    ) -> dict[str, list[int]]:
        """Return finding lines per path for this rule, from the production rule file."""
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
        ]
        if excludes:
            for exc in excludes:
                argv.append(f"--exclude={exc}")
        argv.extend(targets)
        env = dict(os.environ)
        env_dir = Path(tempfile.gettempdir()) / "vmafx-semgrep-env"
        env_dir.mkdir(parents=True, exist_ok=True)
        env.setdefault("XDG_CONFIG_HOME", str(env_dir))
        env.setdefault("SEMGREP_SETTINGS_FILE", str(env_dir / "semgrep-settings.yaml"))
        env.setdefault("SEMGREP_LOG_FILE", str(env_dir / "semgrep.log"))
        proc = run_command(
            argv,
            allowed_executables=(executable,),
            cwd=project,
            env=env,
            capture_output=True,
            text=True,
            timeout_seconds=300,
            max_output_bytes=16 * 1_048_576,
        )
        self.assertIn(proc.returncode, (0, 1), proc.stderr)
        report = json.loads(proc.stdout)
        # A rule Semgrep cannot load reports no findings at all; fail on it by name.
        rule_errors = [
            error
            for error in report.get("errors", [])
            if "rule" in str(error.get("type", "")).lower()
        ]
        self.assertEqual(rule_errors, [], "Semgrep could not load .semgrep.yml")
        lines: dict[str, list[int]] = {}
        for result in report.get("results", []):
            if result["check_id"].rsplit(".", 1)[-1] == RULE:
                path = Path(result["path"])
                if path.is_absolute():
                    with contextlib.suppress(ValueError):
                        path = path.relative_to(project)
                lines.setdefault(path.as_posix(), []).append(result["start"]["line"])
        return {path: sorted(found) for path, found in lines.items()}

    def planted_project(self) -> Path:
        temp = tempfile.TemporaryDirectory(prefix="vmafx-semgrep-sbb-")
        self.addCleanup(temp.cleanup)
        project = Path(temp.name)
        shutil.copyfile(ROOT / ".semgrep.yml", project / ".semgrep.yml")
        target = project / IN_SCOPE
        target.write_text(PLANTED_C, encoding="utf-8")
        return project

    def test_planted_defect_recall(self) -> None:
        project = self.planted_project()
        lines = self.scan(project, [IN_SCOPE])
        self.assertEqual(
            lines.get(IN_SCOPE),
            list(PLANTED_HITS),
            "the rule must report each store before bound check and none of the "
            "check-first, literal reset, branch, or no-exit cases",
        )

    def test_real_tree_zero_findings(self) -> None:
        lines = self.scan(
            ROOT,
            ["."],
            excludes=["subprojects", "core/test/data"],
        )
        self.assertEqual(
            lines,
            {},
            f"unexpected findings for rule {RULE} in real tree: {lines}",
        )


if __name__ == "__main__":
    unittest.main()
