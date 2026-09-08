#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
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


class WindowsCudaCompilerDiscovery(unittest.TestCase):
    def configure(self, *, vswhere: str, path_compiler: bool) -> subprocess.CompletedProcess[str]:
        self.assertIsNotNone(MESON, "Meson is required for this configure regression")
        with tempfile.TemporaryDirectory(prefix="vmafx-windows-discovery-") as temporary:
            root = Path(temporary)
            binary_dir = root / "bin"
            binary_dir.mkdir()
            compiler = binary_dir / "cl"
            if path_compiler:
                compiler.write_text(f"#!{sys.executable}\n", encoding="utf-8")
                compiler.chmod(0o700)
            discovered = "C:/Visual Studio/VC/Tools/MSVC/14.99/bin/HostX64/x64/cl.exe"
            selected = discovered if vswhere == "found" else str(compiler)
            responses = {
                "vswhere": vswhere,
                "discovered": discovered,
                "selected": selected,
                "msvc_root": "C:/Visual Studio/VC/Tools/MSVC/14.99",
                "sdk_root": "C:/Windows Kits/10/Include/10.0.26100.0",
            }
            (root / "responses.json").write_text(json.dumps(responses), encoding="utf-8")
            powershell = binary_dir / "powershell"
            powershell.write_text(
                f"#!{sys.executable}\n"
                + textwrap.dedent(
                    """\
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
                    """
                ),
                encoding="utf-8",
            )
            powershell.chmod(0o700)
            cross = root / "windows.ini"
            cross.write_text(
                "[host_machine]\nsystem = 'windows'\ncpu_family = 'x86_64'\n"
                "cpu = 'x86_64'\nendian = 'little'\n",
                encoding="utf-8",
            )
            # The compiler selected by the branch is checked against the exact
            # executable found by Meson, including the PATH fallback.
            expected_compiler = (
                f"'{discovered}'" if vswhere == "found" else "find_program('cl').full_path()"
            )
            checks = (
                f"assert(cl_path == {expected_compiler}, 'wrong compiler selected')\n"
                "assert(nvcc_ccbin_flags == ['--allow-unsupported-compiler', '-ccbin', cl_path])\n"
                "assert(nvcc_host_includes == [\n"
                "  '-I', 'C:/Visual Studio/VC/Tools/MSVC/14.99/include',\n"
                "  '-I', 'C:/Windows Kits/10/Include/10.0.26100.0/ucrt',\n"
                "  '-I', 'C:/Windows Kits/10/Include/10.0.26100.0/shared',\n"
                "  '-I', 'C:/Windows Kits/10/Include/10.0.26100.0/um'])\n"
            )
            (root / "meson.build").write_text(
                "project('windows-cuda-discovery')\n" + discovery_block() + checks,
                encoding="utf-8",
            )
            # No real cl/PowerShell may leak into the missing-tool cases.
            environment = {**os.environ, "PATH": str(binary_dir)}
            # Controlled Meson executable and test-owned paths; no shell.
            return subprocess.run(  # noqa: S603
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
