#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Cases for scripts/ci/run_ffmpeg_patch_contract_tests.py."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "run_ffmpeg_patch_contract_tests.py"
SPEC = importlib.util.spec_from_file_location("run_ffmpeg_patch_contract_tests", SCRIPT)
assert SPEC and SPEC.loader
MOD = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MOD
SPEC.loader.exec_module(MOD)

GREEN_SCRIPT = "import sys\nPATH = 'ffmpeg-patches/src/x.c'\nsys.exit(0)\n"
RED_SCRIPT = "import sys\nPATH = 'ffmpeg-patches/src/x.c'\nprint('broken')\nsys.exit(1)\n"


class PatchContractRunner(unittest.TestCase):
    def _tree(self, tmp: str, files: dict[str, str]) -> Path:
        root = Path(tmp)
        (root / "ffmpeg-patches").mkdir()
        (root / "ffmpeg-patches" / "0001-live.patch").write_text("x\n")
        (root / "core" / "test").mkdir(parents=True)
        for rel, text in files.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text)
        return root

    def test_live_reference_passes(self) -> None:
        """Positive: a live file naming an existing patch is no finding."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._tree(tmp, {"dev/Containerfile": "# ffmpeg-patches/0001-live.patch\n"})
            self.assertEqual(MOD.stale_patch_refs(root, MOD.live_files(root)), [])

    def test_retired_patch_in_live_file_fails(self) -> None:
        """Negative: an AGENTS.md naming a retired patch is reported."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._tree(tmp, {"core/src/cuda/AGENTS.md": "ffmpeg-patches/0010-gone.patch\n"})
            self.assertEqual(
                MOD.stale_patch_refs(root, MOD.live_files(root)),
                [
                    "core/src/cuda/AGENTS.md: names ffmpeg-patches/0010-gone.patch, "
                    "which the series no longer has"
                ],
            )

    def test_history_and_fixtures_may_name_retired_patches(self) -> None:
        """Boundary: docs/ history and scripts/ test fixtures are not live files."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._tree(
                tmp,
                {
                    "docs/rebase-notes.md": "ffmpeg-patches/0013-old.patch\n",
                    "scripts/ci/tests/test_x.py": "'ffmpeg-patches/0018-example.patch'\n",
                    "scripts/ci/test_y.py": "'ffmpeg-patches/0002-broken.patch'\n",
                },
            )
            self.assertEqual(MOD.stale_patch_refs(root, MOD.live_files(root)), [])

    def test_failing_contract_script_is_reported(self) -> None:
        """Negative: a core/test script naming the series that exits 1 is a failure."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._tree(
                tmp, {"core/test/test_bad.py": RED_SCRIPT, "core/test/test_good.py": GREEN_SCRIPT}
            )
            scripts = MOD.contract_scripts(root)
            self.assertEqual([p.name for p in scripts], ["test_bad.py", "test_good.py"])
            found = MOD.failures(root, scripts)
            self.assertEqual(len(found), 1)
            self.assertIn("core/test/test_bad.py (exit 1)", found[0])
            self.assertIn("broken", found[0])

    def test_no_contract_script_fails(self) -> None:
        """Boundary: a tree with no contract script refuses instead of passing empty."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._tree(tmp, {"core/test/test_other.py": "x = 1\n"})
            self.assertEqual(MOD.main(["--root", str(root)]), 1)


if __name__ == "__main__":
    unittest.main()
