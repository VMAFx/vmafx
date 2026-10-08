#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A workflow step that runs the Meson tests through sudo keeps the runner's PATH.

`sudo` replaces PATH with its `secure_path`. The tests of `core/test` start the
compiler Meson found, by name (`test_compat_library_gates.py`,
`test_libvmaf_deprecation.py` take `--cc=<word>`); on the Intel LLVM leg of
`build.yml` that is `icx`, which the setup step puts on the runner's PATH
(`$GITHUB_PATH`) and which sudo's PATH does not have, so both tests failed with
`FileNotFoundError: 'icx'` on master (run 37772201709). A step in a workflow
whose matrix names `icx` as the compiler runs `sudo -E env "PATH=$PATH" ...`.

Positive, negative and boundary cases run on synthetic steps; the last test
scans `build.yml`.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = ROOT / ".github" / "workflows" / "build.yml"
SUDO_TEST = re.compile(r"\bsudo\b[^\n]*\n?[^\n]*run_meson_test\.py")
KEEPS_PATH = re.compile(r'\bsudo\s+-E\s+env\s+"PATH=\$PATH"')


def steps(text: str) -> list[str]:
    """Split a workflow into its `- name:` steps."""
    return re.split(r"(?m)^\s*- name:", text)[1:]


def offenders(text: str) -> list[str]:
    bad = []
    for step in steps(text):
        if SUDO_TEST.search(step) and not KEEPS_PATH.search(step):
            bad.append(step.strip().splitlines()[0])
    return bad


class SudoMesonTestKeepsPathTest(unittest.TestCase):
    def test_build_workflow_keeps_path(self) -> None:
        text = WORKFLOW.read_text(encoding="utf-8")
        self.assertTrue(any(SUDO_TEST.search(s) for s in steps(text)), "no sudo test step found")
        self.assertEqual(offenders(text), [])

    def test_plain_sudo_is_refused(self) -> None:
        step = '- name: Run\n  run: >-\n    sudo "$(command -v python3)" scripts/ci/run_meson_test.py\n'
        self.assertEqual(offenders(step), ["Run"])

    def test_path_kept_is_accepted(self) -> None:
        step = (
            '- name: Run\n  run: >-\n    sudo -E env "PATH=$PATH" "$(command -v python3)"\n'
            "    scripts/ci/run_meson_test.py\n"
        )
        self.assertEqual(offenders(step), [])

    def test_step_without_sudo_is_ignored(self) -> None:
        step = "- name: Run\n  run: python3 scripts/ci/run_meson_test.py\n"
        self.assertEqual(offenders(step), [])


if __name__ == "__main__":
    unittest.main()
