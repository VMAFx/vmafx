#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Cases for scripts/ci/check_fork_python_subprocess.py."""

from __future__ import annotations

import importlib.util
import os
import shutil
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "check_fork_python_subprocess.py"
SPEC = importlib.util.spec_from_file_location("check_fork_python_subprocess", SCRIPT)
assert SPEC and SPEC.loader
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)

TAG = "SPDX-License-" + "Identifier"  # split: REUSE reads the literal
FORK = f"# Copyright 2026 Lusoris\n# {TAG}: EUPL-1.2\n"
NETFLIX = f"# {TAG}: BSD-2-Clause-Patent\n"
CALL = "import subprocess\nsubprocess.run(['vmaf'], check=False)\n"
CHMOD = "import os\nos.chmod('x', 0o755)\n"


@unittest.skipIf(shutil.which("ruff") is None, "ruff is not on PATH")
class ForkPythonSubprocess(unittest.TestCase):
    def _run(self, files: dict[str, str]) -> int:
        """Run the check from a temporary tree holding the given files."""
        with tempfile.TemporaryDirectory() as tmp:
            cwd = Path.cwd()
            os.chdir(tmp)
            try:
                for rel, text in files.items():
                    Path(rel).parent.mkdir(parents=True, exist_ok=True)
                    Path(rel).write_text(text, encoding="utf-8")
                return int(MOD.main(list(files)))
            finally:
                os.chdir(cwd)

    def test_fork_subprocess_call_fails(self) -> None:
        """Negative: a fork test under python/ calling subprocess directly fails."""
        self.assertEqual(self._run({"python/test/a_test.py": FORK + CALL}), 1)

    def test_fork_permissive_chmod_fails(self) -> None:
        """Negative: a fork file under testdata/ granting 0o755 fails."""
        self.assertEqual(self._run({"testdata/b.py": FORK + CHMOD}), 1)

    def test_netflix_file_is_left_alone(self) -> None:
        """Boundary: an upstream-licensed file under python/ is not checked."""
        self.assertEqual(self._run({"python/vmaf/c.py": NETFLIX + CALL}), 0)

    def test_file_outside_the_scopes_is_left_alone(self) -> None:
        """Boundary: scripts/ is covered by the normal ruff hook, not this one."""
        self.assertEqual(self._run({"scripts/d.py": FORK + CALL}), 0)

    def test_clean_fork_file_passes(self) -> None:
        """Positive: owner read-execute and a 0o644 data file stay under Semgrep's threshold."""
        body = "import os\nos.chmod('x', 0o500)\nos.chmod('y', 0o644)\n"
        self.assertEqual(self._run({"python/test/e_test.py": FORK + body}), 0)

    def test_owner_only_0o700_fails(self) -> None:
        """Negative: Semgrep reports 0o700 (>= 0o650) although ruff S103 accepts it."""
        body = "import os\nos.chmod('x', 0o700)\n"
        self.assertEqual(self._run({"python/test/f_test.py": FORK + body}), 1)

    def test_mode_thresholds(self) -> None:
        """Boundary: the Semgrep rule's edges, flags and the | 0o111 form."""
        cases = {
            "0o647": False,
            "0o650": True,
            "0o77777": True,
            "0o100644": False,
            "0o100650": True,
            "stat.S_IRUSR | stat.S_IXGRP": True,
            "m | 0o111": True,
            "stat.S_IRUSR | stat.S_IWUSR": False,
        }
        with tempfile.TemporaryDirectory() as tmp:
            for mode, wide in cases.items():
                path = Path(tmp) / "m.py"
                path.write_text(f"import os, stat\nos.chmod('x', {mode})\n", encoding="utf-8")
                self.assertEqual(bool(MOD.mode_findings(str(path))), wide, mode)


if __name__ == "__main__":
    unittest.main()
