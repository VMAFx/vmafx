#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Execute the shipped Windows compiler-discovery block in a tiny Meson project.

Run on a POSIX build host with Python and Meson; no Windows SDK or GPU is needed.
Only external PowerShell/cl responses are stubbed. Meson evaluates the original
branch and checks that the selected compiler also supplies the include root.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1] / "src" / "meson.build"
MESON = os.environ.get("VMAFX_TEST_MESON") or shutil.which("meson")


def discovery_block() -> str:
    source = SOURCE.read_text(encoding="utf-8")
    marker = "# default C compiler. Use vswhere + powershell"
    start = source.index("        if host_machine.system() == 'windows'", source.index(marker))
    end = source.index("        # Detect CUDA version from nvcc directly", start)
    return textwrap.dedent(source[start:end])


DISCOVERED_COMPILER = "C:/Visual Studio/VC/Tools/MSVC/14.99/bin/HostX64/x64/cl.exe"
MSVC_ROOT = "C:/Visual Studio/VC/Tools/MSVC/14.99"
SDK_ROOT = "C:/Windows Kits/10/Include/10.0.26100.0"


def _fixture_responses(compiler: Path, vswhere: str) -> dict[str, str]:
    """Canned PowerShell answers for one discovery case."""
    return {
        "vswhere": vswhere,
        "discovered": DISCOVERED_COMPILER,
        "selected": DISCOVERED_COMPILER if vswhere == "found" else str(compiler),
        "msvc_root": MSVC_ROOT,
        "sdk_root": SDK_ROOT,
    }


def _write_powershell_stub(binary_dir: Path) -> None:
    """Install the PowerShell stand-in that replays responses.json."""
    powershell = binary_dir / "powershell"
    powershell.write_text(
        f"#!{sys.executable}\n" + textwrap.dedent("""\
            import json
            import re
            import sys
            from pathlib import Path

            cfg = json.loads((Path(__file__).parents[1] / 'responses.json').read_text())
            command = sys.argv[-1]
            if 'vswhere.exe' in command:
                if cfg['vswhere'] == 'error':
                    sys.exit(1)
                if cfg['vswhere'] == 'found':
                    print(cfg['discovered'])
            elif '$clPath =' in command:
                selected = re.search(r'\\$clPath = "([^"]+)"', command).group(1)
                if selected != cfg['selected']:
                    sys.exit('include discovery received a different compiler')
                print(cfg['msvc_root'])
            elif 'Windows Kits/10/Include' in command:
                print(cfg['sdk_root'])
            else:
                sys.exit('unexpected PowerShell command')
            """),
        encoding="utf-8",
    )
    powershell.chmod(0o700)


def _write_cross_file(root: Path) -> Path:
    """Write the cross file that makes Meson evaluate the Windows branch."""
    cross = root / "windows.ini"
    cross.write_text(
        "[host_machine]\nsystem = 'windows'\ncpu_family = 'x86_64'\n"
        "cpu = 'x86_64'\nendian = 'little'\n",
        encoding="utf-8",
    )
    return cross


def _write_project(root: Path, expected_compiler: str) -> None:
    """Write the tiny Meson project that runs the shipped discovery block."""
    checks = (
        f"assert(cl_path == {expected_compiler}, 'wrong compiler selected')\n"
        "assert(nvcc_ccbin_flags == ['--allow-unsupported-compiler', '-ccbin', cl_path])\n"
        "assert(nvcc_host_includes == [\n"
        f"  '-I', '{MSVC_ROOT}/include',\n"
        f"  '-I', '{SDK_ROOT}/ucrt',\n"
        f"  '-I', '{SDK_ROOT}/shared',\n"
        f"  '-I', '{SDK_ROOT}/um'])\n"
    )
    (root / "meson.build").write_text(
        "project('windows-cuda-discovery')\n" + discovery_block() + checks,
        encoding="utf-8",
    )


def _build_fixture(root: Path, *, vswhere: str, path_compiler: bool) -> tuple[Path, Path]:
    """Materialise one discovery case under ``root``.

    Returns the stub binary directory and the cross file.
    """
    binary_dir = root / "bin"
    binary_dir.mkdir()
    compiler = binary_dir / "cl"
    if path_compiler:
        compiler.write_text(f"#!{sys.executable}\n", encoding="utf-8")
        compiler.chmod(0o700)
    responses = _fixture_responses(compiler, vswhere)
    (root / "responses.json").write_text(json.dumps(responses), encoding="utf-8")
    _write_powershell_stub(binary_dir)
    # The compiler selected by the branch is checked against the exact
    # executable found by Meson, including the PATH fallback.
    expected_compiler = (
        f"'{DISCOVERED_COMPILER}'" if vswhere == "found" else "find_program('cl').full_path()"
    )
    _write_project(root, expected_compiler)
    return binary_dir, _write_cross_file(root)


def _run_meson(root: Path, binary_dir: Path, cross: Path) -> subprocess.CompletedProcess[str]:
    """Configure the fixture project with only the stub binaries on PATH."""
    # No real cl/PowerShell may leak into the missing-tool cases.
    environment = {**os.environ, "PATH": str(binary_dir)}
    # Controlled Meson executable and test-owned paths; no shell. The
    # environment is os.environ with PATH repointed at this test's own
    # fixture directory, which is the point of the case, not tainted
    # input: argv is a fixed list of literals and paths this test just
    # created under its own tmpdir.
    return subprocess.run(  # noqa: S603
        # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-tainted-env-args.dangerous-subprocess-use-tainted-env-args
        [
            str(MESON),
            "setup",
            str(root / "build"),
            str(root),
            "--cross-file",
            str(cross),
            "--backend=none",
        ],
        env=environment,
        capture_output=True,
        text=True,
        check=False,
        timeout=20,
    )


class WindowsCudaCompilerDiscovery(unittest.TestCase):
    def configure(self, *, vswhere: str, path_compiler: bool) -> subprocess.CompletedProcess[str]:
        self.assertIsNotNone(MESON, "Meson is required for this configure regression")
        with tempfile.TemporaryDirectory(prefix="vmafx-windows-discovery-") as temporary:
            root = Path(temporary)
            binary_dir, cross = _build_fixture(root, vswhere=vswhere, path_compiler=path_compiler)
            return _run_meson(root, binary_dir, cross)

    def test_vswhere_success(self) -> None:
        result = self.configure(vswhere="found", path_compiler=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_empty_vswhere_uses_path_compiler(self) -> None:
        result = self.configure(vswhere="empty", path_compiler=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_failed_vswhere_uses_path_compiler(self) -> None:
        result = self.configure(vswhere="error", path_compiler=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_no_compiler_reports_actionable_error(self) -> None:
        result = self.configure(vswhere="empty", path_compiler=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("nvcc on Windows requires Visual Studio Build Tools", result.stdout)
        self.assertNotIn("Unknown variable", result.stdout)


if __name__ == "__main__":
    unittest.main()
