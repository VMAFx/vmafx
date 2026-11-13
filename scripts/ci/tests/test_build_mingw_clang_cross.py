# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Refusal-path tests for the Linux -> Windows clang cross build script."""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))

from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

SCRIPT = REPO / "scripts/ci/build-mingw-clang-cross.sh"


def _write_executable(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")
    path.chmod(0o755)


@unittest.skipIf(os.name == "nt", "the cross script runs on POSIX hosts")
class BuildMingwClangCrossRefusalTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.toolchain = self.root / "toolchain"
        self.prefix = self.root / "prefix"
        self.stage = self.root / "artifact"

    def run_script(self, *, compiler: bool = True, runtime_dlls: bool = True) -> TextCommandResult:
        if compiler:
            _write_executable(self.bin / "x86_64-w64-mingw32-clang", "#!/bin/sh\nexit 0\n")
        if runtime_dlls:
            runtime = self.toolchain / "x86_64-w64-mingw32/bin"
            runtime.mkdir(parents=True, exist_ok=True)
            for name in ("libc++.dll", "libunwind.dll", "libwinpthread-1.dll"):
                (runtime / name).write_bytes(b"dll")
        else:
            runtime = self.toolchain / "x86_64-w64-mingw32/bin"
            runtime.mkdir(parents=True, exist_ok=True)
            for name in ("libunwind.dll", "libwinpthread-1.dll"):
                (runtime / name).write_bytes(b"dll")

        (self.prefix / "bin").mkdir(parents=True, exist_ok=True)
        (self.prefix / "bin/vmafx.exe").write_bytes(b"exe")
        (self.prefix / "bin/libvmafx-1.dll").write_bytes(b"dll")

        # The script reaches meson only after the compiler check. These stubs
        # keep the refusal under test from depending on a real toolchain.
        _write_executable(self.bin / "meson", "#!/bin/sh\nexit 0\n")
        _write_executable(self.bin / "python3", "#!/bin/sh\nexit 0\n")

        env = {
            "PATH": f"{self.bin}:/usr/bin:/bin",
            "LLVM_MINGW_ROOT": str(self.toolchain),
            "STAGE_DIR": str(self.stage),
        }
        bash = shutil.which("bash")
        self.assertIsNotNone(bash)
        assert bash is not None
        return run_command(
            [bash, str(SCRIPT), str(self.root / "build"), str(self.prefix)],
            allowed_executables=(bash,),
            check=False,
            capture_output=True,
            text=True,
            timeout_seconds=30,
            env=env,
        )

    def test_missing_compiler_is_refused(self) -> None:
        result = self.run_script(compiler=False)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("x86_64-w64-mingw32-clang not found", result.stderr)

    def test_missing_runtime_dll_is_refused(self) -> None:
        result = self.run_script(runtime_dlls=False)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("libc++.dll not found", result.stderr)
        self.assertFalse((self.stage / "libunwind.dll").exists())

    def test_all_runtime_dlls_are_staged(self) -> None:
        result = self.run_script(runtime_dlls=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in (
            "vmafx.exe",
            "libvmafx-1.dll",
            "libc++.dll",
            "libunwind.dll",
            "libwinpthread-1.dll",
        ):
            self.assertTrue((self.stage / name).is_file(), name)
