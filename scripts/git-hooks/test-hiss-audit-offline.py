#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check that ``hiss-audit.sh`` keeps a hook off the forge where it can.

praetor 6c772713a133 reads the live Actions permissions and workflow runs
during ``audit`` unless it gets ``--offline``. The wrapper must pass the flag
to an engine that has it and leave an engine without it (the pre-bump pin)
alone.
"""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("hiss-audit.sh").resolve()
BASH = Path(shutil.which("bash") or "/bin/bash")
GIT = Path(shutil.which("git") or "/usr/bin/git")
TIMEOUT_S = 30

# A stand-in engine: `audit -h` prints the usage of one engine generation,
# any other `audit` call records its arguments.
FAKE_ENGINE = """#!/bin/sh
if [ "$2" = "-h" ]; then
  printf '%s' "$FAKE_USAGE" >&2
  exit 2
fi
printf '%s\\n' "$*" > "$FAKE_LOG"
"""
USAGE_WITH_OFFLINE = "Usage of audit:\n  -offline\n    \tRead nothing from the forge\n"
USAGE_WITHOUT_OFFLINE = "Usage of audit:\n  -touched string\n    \tTouched files\n"


def run_wrapper(usage: str) -> str:
    """Run the wrapper against a fake engine and return the audit arguments it passed."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        engine = root / "praetorctl"
        engine.write_text(FAKE_ENGINE, encoding="utf-8")
        engine.chmod(engine.stat().st_mode | stat.S_IXUSR)
        log = root / "args.log"
        repo = root / "repo"
        # A git hook exports GIT_DIR and GIT_INDEX_FILE; with them `git init` and the
        # script under test would act on the caller's repository, not on `repo`.
        env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        subprocess.run(  # noqa: S603 -- resolved git path, fixed arguments, no shell
            [str(GIT), "init", "-q", str(repo)], check=True, timeout=TIMEOUT_S, env=env
        )
        env["PATH"] = f"{root}:{os.environ['PATH']}"
        env.update(FAKE_USAGE=usage, FAKE_LOG=str(log))
        done = subprocess.run(  # noqa: S603 -- resolved bash path and the repository's own script, no shell
            [str(BASH), str(SCRIPT)],
            cwd=repo,
            env=env,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_S,
            check=False,
        )
        if done.returncode != 0:
            raise AssertionError(done.stderr)
        return log.read_text(encoding="utf-8").strip()


class HissAuditOfflineTest(unittest.TestCase):
    def test_positive_engine_with_the_flag_runs_offline(self) -> None:
        self.assertEqual(run_wrapper(USAGE_WITH_OFFLINE), "audit --offline")

    def test_negative_engine_without_the_flag_is_run_as_before(self) -> None:
        self.assertEqual(run_wrapper(USAGE_WITHOUT_OFFLINE), "audit")

    def test_boundary_empty_usage_is_run_as_before(self) -> None:
        self.assertEqual(run_wrapper(""), "audit")


if __name__ == "__main__":
    unittest.main()
