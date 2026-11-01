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
        """Positive: a fork file with an owner-only mode and no direct call passes."""
        self.assertEqual(
            self._run({"python/test/e_test.py": FORK + "import os\nos.chmod('x', 0o700)\n"}), 0
        )


if __name__ == "__main__":
    unittest.main()
