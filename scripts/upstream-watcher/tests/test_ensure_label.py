#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/upstream-watcher/ensure_label.sh ensures a label exists idempotently."""

from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "ensure_label.sh"

STUB = """#!/usr/bin/env bash
printf '%s\\n' "$*" >> "$STUB_LOG"
case "$1 $2" in
  "label create")
    if [ "${STUB_FAIL_CREATE:-0}" = "1" ]; then
      exit 1
    fi
    exit 0
    ;;
  *) echo "unexpected: $*" >&2; exit 9 ;;
esac
"""


class EnsureLabelTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.log = root / "calls.log"
        self.stub = root / "gh"
        self.stub.write_text(STUB, encoding="utf-8")
        self.stub.chmod(self.stub.stat().st_mode | stat.S_IXUSR)

    def run_script(
        self,
        *args: str,
        fail_create: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        env = {
            **os.environ,
            "UPSTREAM_WATCHER_GH": str(self.stub),
            "STUB_LOG": str(self.log),
            "STUB_FAIL_CREATE": "1" if fail_create else "0",
        }
        return subprocess.run(  # noqa: S603 - fixed argv: bash, the script under test
            ["bash", str(SCRIPT), *args],  # noqa: S607
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

    def calls(self) -> list[str]:
        return self.log.read_text(encoding="utf-8").splitlines() if self.log.exists() else []

    def test_creates_label_with_force_and_exits_zero(self) -> None:
        result = self.run_script(
            "VMAFx/vmafx",
            "upstream-blocked",
            "e99695",
            "Waiting on an upstream change",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        self.assertEqual(len(calls), 1)
        self.assertIn("label create upstream-blocked", calls[0])
        self.assertIn("--repo VMAFx/vmafx", calls[0])
        self.assertIn("--color e99695", calls[0])
        self.assertIn("--description Waiting on an upstream change", calls[0])
        self.assertIn("--force", calls[0])

    def test_fails_closed_when_gh_label_create_fails(self) -> None:
        result = self.run_script(
            "VMAFx/vmafx",
            "upstream-blocked",
            "e99695",
            "Waiting on an upstream change",
            fail_create=True,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("upstream-blocked", result.stderr)
        self.assertIn("could not be created", result.stderr)

    def test_rejects_missing_or_malformed_arguments(self) -> None:
        test_cases = (
            (),
            ("VMAFx/vmafx",),
            ("VMAFx/vmafx", "upstream-blocked"),
            ("VMAFx/vmafx", "upstream-blocked", "e99695"),
            ("invalid-repo", "upstream-blocked", "e99695", "desc"),
            ("", "upstream-blocked", "e99695", "desc"),
        )
        for args in test_cases:
            with self.subTest(args=args):
                result = self.run_script(*args)
                self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
