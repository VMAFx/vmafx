#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise AOT target selection with a standalone fake ocloc."""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ocloc_aot_targets as probe

from scripts.lib.safe_subprocess import run as run_command


class AotTargetsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stub = self.root / "ocloc"
        self.stub.write_text(
            f"#!{sys.executable}\n"
            "import sys, time\n"
            "target = sys.argv[2]\n"
            "if target == 'slow': time.sleep(2)\n"
            "if target == 'bad': print('unknown device'); sys.exit(1)\n"
            "if target == 'noip': print('ids: unknown'); sys.exit(0)\n"
            "print('device information'); print('ids: 12.1.0')\n",
            encoding="utf-8",
        )
        self.stub.chmod(0o755)
        self.output = self.root / "output"

    def run_selection(
        self, targets: list[str], expected: set[str], executable: str = ""
    ) -> tuple[int, str]:
        text = io.StringIO()
        with contextlib.redirect_stdout(text):
            rc = probe.select(executable or str(self.stub), targets, expected, self.output)
        return rc, text.getvalue()

    def test_matching_refusals_and_order(self) -> None:
        rc, text = self.run_selection(["a", "bad", "b", "noip"], {"bad", "noip"})
        self.assertEqual(rc, 0)
        self.assertEqual(self.output.read_text(), "targets=a,b\n")
        self.assertIn("a: accepted: device information", text)
        self.assertIn("::notice::Dropped AOT targets: bad, noip", text)

    def test_extra_refusal(self) -> None:
        rc, text = self.run_selection(["a", "bad", "noip"], {"bad"})
        self.assertEqual(rc, 1)
        self.assertIn("measured=['bad', 'noip'] expected=['bad']", text)
        self.assertFalse(self.output.exists())

    def test_missing_refusal(self) -> None:
        rc, _ = self.run_selection(["a", "bad"], {"bad", "noip"})
        self.assertEqual(rc, 1)
        self.assertFalse(self.output.exists())

    def test_empty_accepted(self) -> None:
        rc, text = self.run_selection(["bad"], {"bad"})
        self.assertEqual(rc, 1)
        self.assertIn("No accepted AOT targets", text)
        self.assertFalse(self.output.exists())

    def test_missing_ocloc(self) -> None:
        rc, _ = self.run_selection(["a"], set(), str(self.root / "missing"))
        self.assertEqual(rc, 1)

    def test_one_target(self) -> None:
        rc, _ = self.run_selection(["a"], set())
        self.assertEqual(rc, 0)
        self.assertEqual(self.output.read_text(), "targets=a\n")

    def test_timeout(self) -> None:
        accepted, line, failed = probe.probe(str(self.stub), "slow", timeout=0.1)
        self.assertFalse(accepted)
        self.assertTrue(failed)
        self.assertIn("timed out", line)

    def test_default_from_meson(self) -> None:
        options = self.root / "options"
        options.write_text("option('sycl_icpx_aot_targets', type: 'string', value: 'a,b')")
        self.assertEqual(probe.default_targets(options), ["a", "b"])
        options.write_text("option('other', type: 'string', value: 'a')")
        with self.assertRaises(ValueError):
            probe.default_targets(options)

    def test_cli_writes_default_targets(self) -> None:
        result = run_command(
            [
                sys.executable,
                str(Path(probe.__file__)),
                str(self.stub),
                "--expect-refused",
                "",
                "--github-output",
                str(self.output),
            ],
            allowed_executables=[sys.executable],
            capture_output=True,
            text=True,
            timeout_seconds=20,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.read_text(), f"targets={','.join(probe.default_targets())}\n")
        self.assertIn("::notice::Dropped AOT targets: (none)", result.stdout)

    def test_registered_in_tooling_suite(self) -> None:
        sys.path.insert(0, str(probe.ROOT))
        from scripts.ci.suite_registry import load_registry, suite_files  # noqa: PLC0415

        path = Path(__file__).resolve().relative_to(probe.ROOT).as_posix()
        self.assertEqual(suite_files(load_registry(probe.ROOT), [path], "tooling"), [path])
