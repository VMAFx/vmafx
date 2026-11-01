#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Cases for scripts/ci/run_ffmpeg_patch_contract_tests.py."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from collections.abc import Mapping
from pathlib import Path
from types import MappingProxyType
from typing import Final

SCRIPT = Path(__file__).resolve().parents[1] / "run_ffmpeg_patch_contract_tests.py"
SPEC = importlib.util.spec_from_file_location("run_ffmpeg_patch_contract_tests", SCRIPT)
assert SPEC and SPEC.loader
MOD = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MOD
SPEC.loader.exec_module(MOD)

GREEN_SCRIPT = "import sys\nPATH = 'ffmpeg-patches/src/x.c'\nsys.exit(0)\n"
RED_SCRIPT = "import sys\nPATH = 'ffmpeg-patches/src/x.c'\nprint('broken')\nsys.exit(1)\n"

LAVFI_JOB = (
    "jobs:\n  f:\n    steps:\n      - run: ./configure --disable-indevs{extra}\n"
    "      - run: bash ffmpeg-patches/test/check-x.sh\n"
)
LAVFI_FILES: Final = MappingProxyType(
    {
        "ffmpeg-patches/test/check-x.sh": '. "${HERE}/lib.sh"\n',
        "ffmpeg-patches/test/lib.sh": "ffmpeg -f lavfi -i testsrc2\n",
    }
)


QSV_CMD = (
    "ffmpeg -hwaccel qsv -hwaccel_output_format qsv -i d.mp4 \\\n"
    "  -lavfi '[0:v]{}hwmap=derive_device=drm,format=drm_prime[d]' -f null -\n"
)


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

    def _lavfi(self, extra: str, files: Mapping[str, str] | None = None) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            workflow = {".github/workflows/w.yml": LAVFI_JOB.format(extra=extra)}
            root = self._tree(tmp, {**workflow, **(files if files is not None else LAVFI_FILES)})
            found: list[str] = MOD.lavfi_without_indev(root)
            return found

    def test_lavfi_script_on_build_without_the_device_fails(self) -> None:
        """Negative: a sourced helper reading -f lavfi on a --disable-indevs build is refused."""
        found = self._lavfi("")
        self.assertEqual(len(found), 1)
        self.assertIn("w.yml:f: runs ffmpeg-patches/test/check-x.sh", found[0])

    def test_lavfi_script_with_the_device_passes(self) -> None:
        """Positive: --enable-indev=lavfi after --disable-indevs keeps the device."""
        self.assertEqual(self._lavfi(" --enable-indev=lavfi"), [])

    def test_script_without_lavfi_passes(self) -> None:
        """Boundary: a script that reads files, not lavfi, needs no input device."""
        files = {"ffmpeg-patches/test/check-x.sh": "ffmpeg -i in.nut\n"}
        self.assertEqual(self._lavfi("", files), [])

    def test_repository_workflows_keep_lavfi(self) -> None:
        """The repository's own workflows build the device their tests read."""
        self.assertEqual(MOD.lavfi_without_indev(SCRIPT.parents[2]), [])

    def test_qsv_direct_drm_map_fails(self) -> None:
        """Negative: QSV frames mapped straight to DRM PRIME in a doc example are reported."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._tree(tmp, {"docs/usage/x.md": "```bash\n" + QSV_CMD.format("") + "```\n"})
            self.assertEqual(len(MOD.qsv_direct_drm_maps(root)), 1)
            self.assertIn("docs/usage/x.md:3:", MOD.qsv_direct_drm_maps(root)[0])

    def test_qsv_map_through_vaapi_passes(self) -> None:
        """Positive: the VAAPI step before the DRM PRIME map is no finding."""
        with tempfile.TemporaryDirectory() as tmp:
            via = "hwmap=derive_device=vaapi,format=vaapi,"
            root = self._tree(tmp, {"scripts/test/x.sh": QSV_CMD.format(via)})
            self.assertEqual(MOD.qsv_direct_drm_maps(root), [])

    def test_qsv_prose_and_vaapi_decode_pass(self) -> None:
        """Boundary: prose outside a fence, and VAAPI frames mapped directly, are no finding."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._tree(
                tmp,
                {
                    "docs/usage/x.md": QSV_CMD.format(""),
                    "testdata/x.py": "'-hwaccel vaapi'\n'[0:v]hwmap=derive_device=drm[d]'\n",
                },
            )
            self.assertEqual(MOD.qsv_direct_drm_maps(root), [])

    def test_repository_has_no_qsv_direct_drm_map(self) -> None:
        """Positive: the repository's own examples map QSV frames through VAAPI."""
        self.assertEqual(MOD.qsv_direct_drm_maps(SCRIPT.parents[2]), [])

    def test_no_contract_script_fails(self) -> None:
        """Boundary: a tree with no contract script refuses instead of passing empty."""
        with tempfile.TemporaryDirectory() as tmp:
            root = self._tree(tmp, {"core/test/test_other.py": "x = 1\n"})
            self.assertEqual(MOD.main(["--root", str(root)]), 1)


if __name__ == "__main__":
    unittest.main()
