# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for scripts/ci/pull-dev-image.sh, the bounded image pull retry."""

from __future__ import annotations

import os
import secrets
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "pull-dev-image.sh"
BASH = shutil.which("bash") or "/bin/bash"
IMAGE = "ghcr.io/vmafx/dev@sha256:" + "0" * 64

STUB = """#!/usr/bin/env bash
# Stand-in docker: login succeeds, pull fails until FAILS pulls have failed.
cat >/dev/null || true
if [ "$1" = pull ]; then
  n=$(cat "$STATE" 2>/dev/null || echo 0)
  echo $((n + 1)) > "$STATE"
  [ "$n" -ge "$FAILS" ] || exit 1
fi
exit 0
"""


def run(fails: int, image: str = IMAGE) -> tuple[subprocess.CompletedProcess[str], int]:
    """Run the script against the stub; return the result and the pull count."""
    with tempfile.TemporaryDirectory() as tmp:
        stub = Path(tmp) / "docker"
        stub.write_text(STUB, encoding="utf-8")
        stub.chmod(0o755)
        state = Path(tmp) / "pulls"
        env = dict(
            os.environ,
            PULL_DOCKER=str(stub),
            PULL_SLEEP="true",
            STATE=str(state),
            FAILS=str(fails),
            REGISTRY_TOKEN=secrets.token_hex(8),
            GITHUB_ACTOR="a",
        )
        result = subprocess.run(  # noqa: S603 -- fixed bash executable and script argv
            [BASH, str(SCRIPT), image],
            env=env,
            text=True,
            capture_output=True,
            timeout=60,
            check=False,
        )
        pulls = int(state.read_text(encoding="utf-8")) if state.exists() else 0
        return result, pulls


class PullDevImageTest(unittest.TestCase):
    def test_first_attempt_succeeds(self) -> None:
        result, pulls = run(0)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(pulls, 1)

    def test_three_failures_then_success(self) -> None:
        result, pulls = run(3)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(pulls, 4)

    def test_four_failures_fail_closed(self) -> None:
        result, pulls = run(4)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(pulls, 4)
        self.assertIn("after 4 attempts", result.stdout)
        self.assertIn(IMAGE, result.stdout)

    def test_unpinned_image_refused(self) -> None:
        result, pulls = run(0, image="ghcr.io/vmafx/dev:latest")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(pulls, 0)
        self.assertIn("pinned by digest", result.stdout)


if __name__ == "__main__":
    unittest.main()
