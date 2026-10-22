#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/ci/go-fetch-retry.sh retries a failing Go module fetch a bounded number of times.

Each case runs the real script with GO_BIN pointing at a stub that fails a given
number of times before it succeeds, and GO_FETCH_DELAY=0 so no case sleeps.
The go-ci.yml case holds the workflow to the wrapper on both fetching steps.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "scripts" / "ci" / "go-fetch-retry.sh"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "go-ci.yml"
BASH = shutil.which("bash") or "/bin/bash"

STUB = """#!/usr/bin/env bash
set -euo pipefail
count_file="$STUB_DIR/count"
n=$(( $(cat "$count_file" 2>/dev/null || echo 0) + 1 ))
echo "$n" > "$count_file"
echo "$*" >> "$STUB_DIR/args"
if [ "$n" -le "$STUB_FAILS" ]; then exit "$STUB_STATUS"; fi
exit 0
"""


class GoFetchRetryTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        stub = self.dir / "go"
        stub.write_text(STUB, encoding="utf-8")
        stub.chmod(0o755)
        self.stub = stub

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_script(self, fails: int, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
        full_env = dict(os.environ)
        full_env.update(
            GO_BIN=str(self.stub),
            GO_FETCH_DELAY="0",
            STUB_DIR=str(self.dir),
            STUB_FAILS=str(fails),
            STUB_STATUS="1",
        )
        full_env.update(env)
        return subprocess.run(  # noqa: S603 -- script under test; fixed executable, stub go
            [BASH, str(SCRIPT), *args],
            env=full_env,
            capture_output=True,
            text=True,
            check=False,
            timeout=60,
        )

    def calls(self) -> int:
        count = self.dir / "count"
        return int(count.read_text(encoding="utf-8")) if count.exists() else 0

    def test_success_first_attempt_runs_once(self) -> None:
        result = self.run_script(0, "mod", "download")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), 1)
        self.assertEqual((self.dir / "args").read_text(encoding="utf-8"), "mod download\n")

    def test_transient_failure_is_retried(self) -> None:
        result = self.run_script(2, "install", "example.com/tool@v1.0.0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.calls(), 3)
        self.assertIn("attempt 2 of 3 failed", result.stderr)

    def test_persistent_failure_fails_after_three_attempts(self) -> None:
        result = self.run_script(99, "mod", "download", STUB_STATUS="7")
        self.assertEqual(result.returncode, 7)
        self.assertEqual(self.calls(), 3)
        self.assertIn("failed 3 times, last status 7", result.stderr)

    def test_attempt_bound_is_configurable_boundary_one(self) -> None:
        result = self.run_script(1, "mod", "download", GO_FETCH_ATTEMPTS="1")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.calls(), 1)

    def test_usage_errors(self) -> None:
        self.assertEqual(self.run_script(0).returncode, 2)
        self.assertEqual(self.run_script(0, "mod", GO_FETCH_ATTEMPTS="0").returncode, 2)
        self.assertEqual(self.run_script(0, "mod", GO_FETCH_ATTEMPTS="x").returncode, 2)
        self.assertEqual(self.run_script(0, "mod", GO_FETCH_ATTEMPTS="11").returncode, 2)
        self.assertEqual(self.run_script(0, "mod", GO_FETCH_DELAY="-1").returncode, 2)
        self.assertEqual(self.calls(), 0)

    def test_workflow_fetches_through_the_wrapper(self) -> None:
        text = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("bash scripts/ci/go-fetch-retry.sh mod download", text)
        self.assertIn("bash scripts/ci/go-fetch-retry.sh install github.com/securego/gosec/", text)
        self.assertNotIn("run: go install github.com/securego/gosec/", text)


if __name__ == "__main__":
    unittest.main()
