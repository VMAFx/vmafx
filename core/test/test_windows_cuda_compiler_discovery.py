#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Execute the shipped Windows compiler-discovery block in a tiny Meson project.

Run on a POSIX build host with Python and Meson; no Windows SDK or GPU is needed.
Only external PowerShell/cl responses are stubbed. Meson evaluates the original
branch and checks that the selected compiler also supplies the include root.
"""

from __future__ import annotations

import importlib.util
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


def _meson_command() -> list[str]:
    """Resolve the Meson that configures the fixture project.

    Meson ships as an ordinary Python package, so the interpreter running this
    test can almost always run it directly. Under ``meson test`` that package
    *is* the Meson driving the suite, which is exactly the version the fixture
    has to be configured with, and asking the interpreter for it needs no
    hand-off from the build system at all. A PATH lookup covers the remaining
    case of a test interpreter that cannot import Meson.

    Deriving the command here rather than reading a build-supplied environment
    variable also keeps an attacker-settable value out of the ``subprocess``
    argv, which is what Semgrep's ``dangerous-subprocess-use-tainted-env-args``
    objects to (the rule treats ``os.environ`` *and* ``sys.argv`` as sources,
    so passing the path as a test argument would not have helped).
    """
    try:
        spec = importlib.util.find_spec("mesonbuild.mesonmain")
    except (ImportError, ValueError):
        spec = None
    if spec is not None:
        return [sys.executable, "-m", "mesonbuild.mesonmain"]
    on_path = shutil.which("meson")
    return [] if on_path is None else [on_path]


MESON_COMMAND = _meson_command()


def discovery_block() -> str:
    source = SOURCE.read_text(encoding="utf-8")
    marker = "# default C compiler. Use vswhere + powershell"
    start = source.index("        if host_machine.system() == 'windows'", source.index(marker))
    end = source.index("        # Detect CUDA version from nvcc directly", start)
    return textwrap.dedent(source[start:end])


DISCOVERED_COMPILER = "C:/Visual Studio/VC/Tools/MSVC/14.99/bin/HostX64/x64/cl.exe"
MSVC_ROOT = "C:/Visual Studio/VC/Tools/MSVC/14.99"
SDK_ROOT = "C:/Windows Kits/10/Include/10.0.26100.0"


BUILD_COMPILER = "C:/Visual Studio/VC/Tools/MSVC/14.51/bin/HostX64/x64/cl.exe"


def _selected(compiler: Path, vswhere: str, build_msvc: str) -> str:
    if build_msvc:
        return build_msvc
    return DISCOVERED_COMPILER if vswhere == "found" else str(compiler)


def _fixture_responses(compiler: Path, vswhere: str, build_msvc: str = "") -> dict[str, str]:
    """Canned PowerShell answers for one discovery case."""
    return {
        "vswhere": vswhere,
        "discovered": DISCOVERED_COMPILER,
        "selected": _selected(compiler, vswhere, build_msvc),
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
                if cfg['vswhere'] == 'forbidden':
                    sys.exit('searched for a compiler although the build compiles with MSVC')
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


def _write_project(root: Path, expected_compiler: str, build_msvc: str = "") -> None:
    """Write the tiny Meson project that runs the shipped discovery block.

    ``nvcc_build_msvc`` is what core/src/meson.build derives from the build's
    C++ compiler when that compiler is MSVC; a project without languages cannot
    hold a compiler object, so the fixture sets the variable directly.
    """
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
        "project('windows-cuda-discovery')\n"
        + f"nvcc_build_msvc = '{build_msvc}'\n"
        + discovery_block()
        + checks,
        encoding="utf-8",
    )


def _build_fixture(
    root: Path, *, vswhere: str, path_compiler: bool, build_msvc: str = ""
) -> tuple[Path, Path]:
    """Materialise one discovery case under ``root``.

    Returns the stub binary directory and the cross file.
    """
    binary_dir = root / "bin"
    binary_dir.mkdir()
    compiler = binary_dir / "cl"
    if path_compiler:
        compiler.write_text(f"#!{sys.executable}\n", encoding="utf-8")
        compiler.chmod(0o700)
    responses = _fixture_responses(compiler, vswhere, build_msvc)
    (root / "responses.json").write_text(json.dumps(responses), encoding="utf-8")
    _write_powershell_stub(binary_dir)
    # The compiler selected by the branch is checked against the exact
    # executable found by Meson, including the PATH fallback.
    if build_msvc:
        expected_compiler = f"'{build_msvc}'"
    elif vswhere == "found":
        expected_compiler = f"'{DISCOVERED_COMPILER}'"
    else:
        expected_compiler = "find_program('cl').full_path()"
    _write_project(root, expected_compiler, build_msvc)
    return binary_dir, _write_cross_file(root)


def _run_meson(root: Path, binary_dir: Path, cross: Path) -> subprocess.CompletedProcess[str]:
    """Configure the fixture project with only the stub binaries on PATH."""
    # No real cl/PowerShell may leak into the missing-tool cases. Repointing
    # PATH at this test's own fixture directory is the point of the case; the
    # child never reaches it to resolve argv[0], which is an absolute path.
    environment = {**os.environ, "PATH": str(binary_dir)}
    # argv is the interpreter-resolved Meson command plus literals and paths
    # this test just created under its own tmpdir. No shell is involved.
    return subprocess.run(  # noqa: S603
        [
            *MESON_COMMAND,
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
    def configure(
        self, *, vswhere: str, path_compiler: bool, build_msvc: str = ""
    ) -> subprocess.CompletedProcess[str]:
        self.assertTrue(MESON_COMMAND, "Meson is required for this configure regression")
        with tempfile.TemporaryDirectory(prefix="vmafx-windows-discovery-") as temporary:
            root = Path(temporary)
            binary_dir, cross = _build_fixture(
                root, vswhere=vswhere, path_compiler=path_compiler, build_msvc=build_msvc
            )
            return _run_meson(root, binary_dir, cross)

    def test_build_msvc_is_the_nvcc_host_compiler(self) -> None:
        """The build compiles C++ with MSVC: nvcc gets that cl.exe and no search runs.

        A second toolset compiles the host half of every .cu against another STL
        (T-WINDOWS-NVCC-CCBIN-OLDEST-TOOLSET-2026-10-06).
        """
        result = self.configure(vswhere="forbidden", path_compiler=True, build_msvc=BUILD_COMPILER)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_build_msvc_comes_from_the_cpp_compiler(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")
        marker = source.index("# default C compiler. Use vswhere + powershell")
        derivation = source[
            marker : source.index("        if host_machine.system() == 'windows'", marker)
        ]
        self.assertIn("if cxx.get_id() == 'msvc'", derivation)
        self.assertIn("nvcc_build_msvc = find_program(cxx.cmd_array()[0]).full_path()", derivation)

    def test_search_takes_the_newest_toolset(self) -> None:
        """The first cl.exe of a recursive walk was the oldest toolset (14.29 in VS 18)."""
        block = discovery_block()
        self.assertNotIn("-Recurse -Filter cl.exe", block)
        self.assertIn("Sort-Object { [version]$_.Name } -Descending", block)
        self.assertIn('Join-Path $_.FullName "bin/HostX64/x64/cl.exe"', block)

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
