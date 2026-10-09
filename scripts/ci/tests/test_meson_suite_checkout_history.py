#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A CI job that runs the Meson suite checks out the full history.

Two tests of the fast suite compare the tree with the merge base of HEAD and
origin/master: test_vmafx_api_abi_append_only (the public API only grows,
HISS-14) and test_crd_compat (a custom resource only grows within v1). Without
that ref they exit 77, which Meson reports as a skip and the job as a pass. The
`Windows ARM64 MSVC` leg checked out one commit (`fetch-depth: 1`) and skipped
both on every run (T-CI-COMPAT-CHECKS-SHALLOW-CHECKOUT-2026-10-09).

The contract: every job with a step that runs scripts/ci/run_meson_test.py over
the fast suite or the whole suite (no `--suite` other than fast, not a list of
named tests) checks out with `fetch-depth: 0`, or with an expression that gives
0 whenever the job does work.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github" / "workflows"
SUITE = re.compile(r"--suite[= ](\S+)")
NAMED_TEST = re.compile(r"(?<![\w$])test_[a-z0-9_]+")


def runs_suite(run: str) -> bool:
    """Whether a step's script runs the fast suite or the whole suite."""
    if "run_meson_test.py" not in run:
        return False
    suites = SUITE.findall(run)
    if suites and "fast" not in suites:
        return False
    return bool(suites) or not NAMED_TEST.search(run)


def full_history(depth: Any) -> bool:
    """`fetch-depth` 0, or an expression that falls back to 0 (`... || 0`)."""
    if depth is None:
        return False
    text = str(depth).strip()
    return text == "0" or bool(re.search(r"\|\|\s*0\s*\}\}$", text))


def offenders(workflow: dict[str, Any], name: str) -> list[str]:
    out = []
    for job_id, job in (workflow.get("jobs") or {}).items():
        steps = job.get("steps") or []
        if not any(runs_suite(str(s.get("run", ""))) for s in steps):
            continue
        checkouts = [s for s in steps if str(s.get("uses", "")).startswith("actions/checkout@")]
        if not checkouts or not all(
            full_history((c.get("with") or {}).get("fetch-depth")) for c in checkouts
        ):
            out.append(f"{name}: job {job_id}")
    return out


def all_offenders() -> list[str]:
    found = []
    for path in sorted(WORKFLOWS.glob("*.yml")):
        workflow = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        found += offenders(workflow, path.name)
    return found


class MesonSuiteCheckoutHistory(unittest.TestCase):
    def test_every_suite_job_checks_out_full_history(self) -> None:
        self.assertEqual(all_offenders(), [])

    def test_some_suite_jobs_are_found(self) -> None:
        # The contract must look at something: the build matrix's legs run the suite.
        workflow = yaml.safe_load((WORKFLOWS / "libvmaf-build-matrix.yml").read_text())
        suite_jobs = [
            job_id
            for job_id, job in workflow["jobs"].items()
            if any(runs_suite(str(s.get("run", ""))) for s in job.get("steps") or [])
        ]
        self.assertIn("windows-arm64", suite_jobs)
        self.assertIn("libvmaf-build", suite_jobs)

    def test_planted_shallow_suite_job_is_refused(self) -> None:
        checkout = {"uses": "actions/checkout@0000000000000000000000000000000000000000"}
        suite = {"run": "python scripts/ci/run_meson_test.py -- -C core/build --suite fast"}
        named = {
            "run": "python scripts/ci/run_meson_test.py -- -C core/build test_sycl_coff_anchor"
        }
        rust = {"run": "python3 scripts/ci/run_meson_test.py -- -C build-rust --suite rust"}
        shallow = {"with": {"fetch-depth": 1}, **checkout}
        planted = {
            "jobs": {
                "shallow-suite": {"steps": [shallow, suite]},
                "no-depth-suite": {"steps": [checkout, suite]},
                "shallow-named": {"steps": [shallow, named]},
                "shallow-rust": {"steps": [shallow, rust]},
                "full-suite": {"steps": [{"with": {"fetch-depth": 0}, **checkout}, suite]},
                "leg-suite": {
                    "steps": [
                        {
                            "with": {
                                "fetch-depth": "${{ steps.leg.outputs.run != 'true' && 1 || 0 }}"
                            },
                            **checkout,
                        },
                        suite,
                    ]
                },
            }
        }
        self.assertEqual(
            offenders(planted, "planted.yml"),
            ["planted.yml: job shallow-suite", "planted.yml: job no-depth-suite"],
        )


if __name__ == "__main__":
    unittest.main()
